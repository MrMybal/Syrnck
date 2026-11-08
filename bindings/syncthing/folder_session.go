package syncthing

import (
	"bytes"
	"context"
	"errors"
	"net"
	"sync"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

type FolderSessionOptions struct {
	Device       DeviceOptions
	PollInterval time.Duration // default 250ms, range 50ms..one minute
	QueueBytes   int64         // default 32 MiB, range 1..128 MiB including processing
}

type folderIndexFrame struct {
	batch CatalogIndex
	bytes int64
}

// FolderSession authenticates one paired connection, ingests indexes on a
// separate bounded worker and continuously captures/publishes/applies file data.
// Folder remains caller-owned. Close the session and wait before closing Folder.
type FolderSession struct {
	folder               *Folder
	connection           *DeviceConnection
	options              FolderSessionOptions
	ctx                  context.Context
	cancel               context.CancelFunc
	mu                   sync.Mutex
	generation           uint64
	knownLocal           PeerFrontier
	authorized           bool
	queuedBytes          int64
	advertised, previous map[string]CatalogEntry
	sentGeneration       uint64
	sentSequence         int64
	configured           chan struct{}
	configureOnce        sync.Once
	frames               chan folderIndexFrame
	closed               chan struct{}
	closedOnce           sync.Once
	workers              sync.WaitGroup
	done                 chan struct{}
	reports              chan FolderStatus
	errors               chan error
}

// OpenFolderSession owns transport even on failure. It checks the local
// certificate's full identity against the folder configuration and authenticates
// the explicit peer with OpenDeviceConnection. Workers own no idle Store.
func OpenFolderSession(ctx context.Context, folder *Folder, transport net.Conn, options FolderSessionOptions) (session *FolderSession, err error) {
	accepted := false
	defer func() {
		if !accepted && transport != nil {
			_ = transport.Close()
		}
	}()
	if ctx == nil || folder == nil || transport == nil {
		return nil, errors.New("syrnck BEP: folder, context and connected transport required")
	}
	if options.PollInterval == 0 {
		options.PollInterval = 250 * time.Millisecond
	}
	if options.PollInterval < 50*time.Millisecond || options.PollInterval > time.Minute {
		return nil, errors.New("syrnck BEP: invalid folder polling interval")
	}
	if options.QueueBytes == 0 {
		options.QueueBytes = 32 * 1024 * 1024
	}
	if options.QueueBytes < 1024*1024 || options.QueueBytes > 128*1024*1024 {
		return nil, errors.New("syrnck BEP: invalid folder index queue budget")
	}
	paired := false
	for _, peer := range folder.options.Catalog.Peers {
		paired = paired || peer == options.Device.RemoteDevice
	}
	if !paired {
		return nil, errors.New("syrnck BEP: unpaired folder session peer")
	}
	local, err := validateDevice(&options.Device)
	if err != nil {
		return nil, err
	}
	if local != folder.options.Catalog.LocalDevice {
		return nil, errors.New("syrnck BEP: folder local certificate identity differs")
	}
	if _, err := folder.Refresh(ctx); err != nil {
		return nil, err
	}
	snapshot, err := folder.Snapshot(ctx)
	if err != nil {
		return nil, err
	}
	child, cancel := context.WithCancel(ctx)
	session = &FolderSession{folder: folder, options: options, ctx: child, cancel: cancel, configured: make(chan struct{}), frames: make(chan folderIndexFrame, 256), closed: make(chan struct{}), done: make(chan struct{}), reports: make(chan FolderStatus, 16), errors: make(chan error, 16)}
	connection, err := OpenDeviceConnection(child, transport, options.Device, session)
	if err != nil {
		cancel()
		return nil, err
	}
	session.connection = connection
	connection.Start()
	peer := snapshot.Peers[options.Device.RemoteDevice]
	connection.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: folder.options.Catalog.Folder, Type: protocol.FolderTypeSendReceive,
		Devices: []protocol.Device{{ID: folder.options.Catalog.LocalDevice, IndexID: protocol.IndexID(snapshot.IndexID), MaxSequence: snapshot.Sequence},
			{ID: options.Device.RemoteDevice, IndexID: protocol.IndexID(peer.IndexID), MaxSequence: peer.Sequence}}}}}, nil)
	session.workers.Add(2)
	go session.indexLoop()
	go session.syncLoop()
	go func() { session.workers.Wait(); close(session.done) }()
	accepted = true
	return session, nil
}

func (s *FolderSession) Reports() <-chan FolderStatus { return s.reports }
func (s *FolderSession) Errors() <-chan error         { return s.errors }
func (s *FolderSession) Done() <-chan struct{}        { return s.done }
func (s *FolderSession) Close() error {
	s.cancel()
	s.connection.Close(nil)
	<-s.done
	select {
	case <-s.closed:
	case <-time.After(time.Second):
		return errors.New("syrnck BEP: model close callback did not finish")
	}
	return nil
}

func (s *FolderSession) fail(err error) {
	select {
	case s.errors <- err:
	default:
	}
	s.cancel()
	s.connection.Close(err)
}
func (s *FolderSession) report(status FolderStatus, err error) {
	select {
	case s.reports <- status:
	default:
	}
	if err != nil {
		select {
		case s.errors <- err:
		default:
		}
	}
}

func (s *FolderSession) ClusterConfig(connection protocol.Connection, configuration *protocol.ClusterConfig) error {
	if connection.DeviceID() != s.options.Device.RemoteDevice {
		return errors.New("syrnck BEP: folder connection identity differs")
	}
	var selected *protocol.Folder
	for i := range configuration.Folders {
		if configuration.Folders[i].ID == s.folder.options.Catalog.Folder {
			if selected != nil {
				return errors.New("syrnck BEP: duplicate shared folder")
			}
			selected = &configuration.Folders[i]
		}
	}
	if selected == nil || !selected.IsRunning() || selected.Type == protocol.FolderTypeReceiveEncrypted {
		return errors.New("syrnck BEP: folder not shared, paused or requires unsupported encrypted reception")
	}
	var own, remote *protocol.Device
	for i := range selected.Devices {
		device := &selected.Devices[i]
		if device.ID == s.folder.options.Catalog.LocalDevice {
			if own != nil {
				return errors.New("syrnck BEP: duplicate local cluster device")
			}
			own = device
		}
		if device.ID == s.options.Device.RemoteDevice {
			if remote != nil {
				return errors.New("syrnck BEP: duplicate remote cluster device")
			}
			remote = device
		}
	}
	if own == nil || remote == nil || remote.IndexID == 0 || own.MaxSequence < 0 || remote.MaxSequence < 0 || len(own.EncryptionPasswordToken) != 0 || len(remote.EncryptionPasswordToken) != 0 {
		return errors.New("syrnck BEP: invalid shared device generation or encryption agreement")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authorized && s.generation != uint64(remote.IndexID) {
		return errors.New("syrnck BEP: peer changed index generation during its session")
	}
	s.generation = uint64(remote.IndexID)
	s.knownLocal = PeerFrontier{uint64(own.IndexID), own.MaxSequence}
	s.authorized = true
	s.configureOnce.Do(func() { close(s.configured) })
	return nil
}

func (s *FolderSession) Index(connection protocol.Connection, index *protocol.Index) error {
	return s.enqueue(connection, index.Folder, CatalogIndex{Reset: true, LastSequence: index.LastSequence, Files: index.Files})
}
func (s *FolderSession) IndexUpdate(connection protocol.Connection, index *protocol.IndexUpdate) error {
	return s.enqueue(connection, index.Folder, CatalogIndex{PrevSequence: index.PrevSequence, LastSequence: index.LastSequence, Files: index.Files})
}

func indexFrameBytes(folder string, files []protocol.FileInfo) (int64, error) {
	if len(files) > MaxCatalogFiles {
		return 0, errors.New("syrnck BEP: index frame entry limit exceeded")
	}
	size := int64(1024)
	for _, info := range files {
		info.Name = protocolName(info.Name)
		if err := validateCatalogInfo(folder, info); err != nil {
			return 0, err
		}
		size += 512 + 4*int64(len(info.Name)) + 128*int64(len(info.Blocks)) + 32*int64(len(info.Version.Counters)) + 2*int64(len(info.SymlinkTarget)+len(info.PreviousBlocksHash))
		if info.Platform.Unix != nil {
			size += 4 * int64(len(info.Platform.Unix.OwnerName)+len(info.Platform.Unix.GroupName))
		}
		if info.Platform.Windows != nil {
			size += 4 * int64(len(info.Platform.Windows.OwnerName))
		}
		for _, platform := range []*protocol.XattrData{info.Platform.Linux, info.Platform.Darwin, info.Platform.FreeBSD, info.Platform.NetBSD} {
			if platform != nil {
				for _, attribute := range platform.Xattrs {
					size += 64 + 4*int64(len(attribute.Name)) + 2*int64(len(attribute.Value))
					if size > syrnck.MaxObjectSize {
						return 0, errors.New("syrnck BEP: index frame metadata limit exceeded")
					}
				}
			}
		}
		if size > syrnck.MaxObjectSize {
			return 0, errors.New("syrnck BEP: index frame metadata limit exceeded")
		}
	}
	return size, nil
}

func (s *FolderSession) enqueue(connection protocol.Connection, folder string, batch CatalogIndex) error {
	if connection.DeviceID() != s.options.Device.RemoteDevice || folder != s.folder.options.Catalog.Folder {
		return protocol.ErrNoSuchFile
	}
	size, err := indexFrameBytes(folder, batch.Files)
	if err != nil {
		return err
	}
	s.mu.Lock()
	if !s.authorized || s.queuedBytes+size > s.options.QueueBytes {
		s.mu.Unlock()
		return errors.New("syrnck BEP: unauthorized or full folder index queue")
	}
	batch.Device = s.options.Device.RemoteDevice
	batch.IndexID = s.generation
	s.queuedBytes += size
	s.mu.Unlock()
	files := make([]protocol.FileInfo, len(batch.Files))
	for i, info := range batch.Files {
		files[i] = copyInfo(info)
	}
	batch.Files = files
	select {
	case s.frames <- folderIndexFrame{batch, size}:
		return nil
	default:
		s.mu.Lock()
		s.queuedBytes -= size
		s.mu.Unlock()
		return errors.New("syrnck BEP: full folder frame queue")
	}
}

func (s *FolderSession) indexLoop() {
	defer s.workers.Done()
	for {
		select {
		case <-s.ctx.Done():
			return
		case frame := <-s.frames:
			for {
				err := s.folder.ReceiveIndex(s.ctx, frame.batch)
				if errors.Is(err, ErrFolderPending) {
					_, recovery := s.folder.Refresh(s.ctx)
					if recovery == nil {
						continue
					}
					s.report(FolderStatus{State: "blocked"}, recovery)
					select {
					case <-s.ctx.Done():
						return
					case <-time.After(s.options.PollInterval):
					}
					continue
				}
				s.mu.Lock()
				s.queuedBytes -= frame.bytes
				s.mu.Unlock()
				if err != nil {
					s.fail(err)
					return
				}
				break
			}
		}
	}
}

func (s *FolderSession) syncLoop() {
	defer s.workers.Done()
	select {
	case <-s.configured:
	case <-s.ctx.Done():
		return
	}
	if err := s.sendSnapshot(true); err != nil {
		s.fail(err)
		return
	}
	ticker := time.NewTicker(s.options.PollInterval)
	defer ticker.Stop()
	for {
		status, err := s.folder.Step(s.ctx, func(device protocol.DeviceID) RequestFunc {
			if device == s.options.Device.RemoteDevice {
				return s.connection.Request
			}
			return nil
		})
		s.report(status, err)
		if snapshotError := s.sendSnapshot(false); snapshotError != nil && !errors.Is(snapshotError, ErrFolderPending) {
			s.fail(snapshotError)
			return
		}
		select {
		case <-s.ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (s *FolderSession) sendSnapshot(initial bool) error {
	snapshot, err := s.folder.Snapshot(s.ctx)
	if err != nil {
		return err
	}
	s.mu.Lock()
	known := s.knownLocal
	generation, previous := s.sentGeneration, s.sentSequence
	s.mu.Unlock()
	full := false
	if initial {
		full = known.IndexID != snapshot.IndexID
		previous = known.Sequence
		if !full && previous > snapshot.Sequence {
			return errors.New("syrnck BEP: peer retains a newer local index; restored state requires reconciliation")
		}
	}
	if !initial {
		if generation != snapshot.IndexID {
			return ErrFolderChanged
		}
		if previous == snapshot.Sequence {
			return nil
		}
	}
	var files []protocol.FileInfo
	for _, entry := range snapshot.Files {
		if full || entry.Info.Sequence > previous {
			files = append(files, entry.Info)
		}
	}
	advertised := make(map[string]CatalogEntry, len(snapshot.Files))
	for _, entry := range snapshot.Files {
		advertised[entry.Info.Name] = entry
	}
	s.mu.Lock()
	s.previous = s.advertised
	s.advertised = advertised
	s.mu.Unlock()
	if full {
		err = s.connection.Index(s.ctx, &protocol.Index{Folder: s.folder.options.Catalog.Folder, Files: files, LastSequence: snapshot.Sequence})
	} else if previous != snapshot.Sequence {
		err = s.connection.IndexUpdate(s.ctx, &protocol.IndexUpdate{Folder: s.folder.options.Catalog.Folder, Files: files, PrevSequence: previous, LastSequence: snapshot.Sequence})
	}
	if err != nil {
		return err
	}
	s.mu.Lock()
	s.sentGeneration, s.sentSequence = snapshot.IndexID, snapshot.Sequence
	s.mu.Unlock()
	return nil
}

func (s *FolderSession) Closed(_ protocol.Connection, err error) {
	s.closedOnce.Do(func() { s.report(FolderStatus{State: "disconnected"}, err); s.cancel(); close(s.closed) })
}
func (s *FolderSession) DownloadProgress(protocol.Connection, *protocol.DownloadProgress) error {
	return nil
}

type folderBlockResponse struct {
	data []byte
	done chan struct{}
	once sync.Once
}

func (r *folderBlockResponse) Data() []byte { return r.data }
func (r *folderBlockResponse) Close()       { r.once.Do(func() { close(r.done) }) }
func (r *folderBlockResponse) Wait()        { <-r.done }

func (s *FolderSession) Request(connection protocol.Connection, request *protocol.Request) (protocol.RequestResponse, error) {
	if connection.DeviceID() != s.options.Device.RemoteDevice || request == nil || request.Folder != s.folder.options.Catalog.Folder {
		return nil, protocol.ErrNoSuchFile
	}
	name := protocolName(request.Name)
	s.mu.Lock()
	authorized := s.authorized
	current, currentFound := s.advertised[name]
	previous, previousFound := s.previous[name]
	s.mu.Unlock()
	if !authorized {
		return nil, protocol.ErrNoSuchFile
	}
	var candidates []CatalogEntry
	if currentFound && !current.Info.Deleted {
		candidates = append(candidates, current)
	}
	if previousFound && !previous.Info.Deleted {
		candidates = append(candidates, previous)
	}
	if len(request.Hash) == 0 && len(candidates) > 1 && !bytes.Equal(protocol.BlocksHash(candidates[0].Info.Blocks), protocol.BlocksHash(candidates[1].Info.Blocks)) {
		return nil, protocol.ErrInvalid
	}
	for _, entry := range candidates {
		data, err := s.folder.readEntry(s.ctx, entry, request)
		if err == nil {
			return &folderBlockResponse{data: data, done: make(chan struct{})}, nil
		}
		if !errors.Is(err, protocol.ErrNoSuchFile) && !errors.Is(err, protocol.ErrInvalid) {
			return nil, err
		}
	}
	return nil, protocol.ErrNoSuchFile
}
