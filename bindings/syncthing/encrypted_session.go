package syncthing

import (
	"bytes"
	"context"
	"errors"
	"net"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

// EncryptedReceiverSessionOptions pairs one trusted source/restore device with
// an opaque receiver. It intentionally exposes no folder-password option.
type EncryptedReceiverSessionOptions struct {
	Device       DeviceOptions
	PollInterval time.Duration
	QueueBytes   int64
}

func prepareReceiverSessionOptions(receiver *EncryptedReceiver, options *FolderSessionOptions) error {
	if receiver == nil {
		return errors.New("syrnck BEP: encrypted receiver required")
	}
	if err := prepareSessionBounds(options); err != nil {
		return err
	}
	paired := false
	for _, peer := range receiver.options.Catalog.Peers {
		paired = paired || peer == options.Device.RemoteDevice
	}
	if !paired {
		return errors.New("syrnck BEP: unpaired encrypted receiver peer")
	}
	local, err := validateDevice(&options.Device)
	if err != nil {
		return err
	}
	if local != receiver.options.Catalog.LocalDevice {
		return errors.New("syrnck BEP: receiver local certificate identity differs")
	}
	return nil
}

// OpenEncryptedReceiverSession owns transport on both success and failure.
// The caller owns receiver and closes all its sessions before closing it.
// Received metadata and blocks remain in the upstream encrypted wire format.
func OpenEncryptedReceiverSession(ctx context.Context, receiver *EncryptedReceiver, transport net.Conn, options EncryptedReceiverSessionOptions) (*FolderSession, error) {
	accepted := false
	defer func() {
		if !accepted && transport != nil {
			_ = transport.Close()
		}
	}()
	if ctx == nil || receiver == nil || transport == nil {
		return nil, errors.New("syrnck BEP: receiver, context and connected transport required")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	prepared := FolderSessionOptions{Device: options.Device, PollInterval: options.PollInterval, QueueBytes: options.QueueBytes}
	if err := prepareReceiverSessionOptions(receiver, &prepared); err != nil {
		return nil, err
	}
	snapshot, err := receiver.Snapshot(ctx)
	if err != nil {
		return nil, err
	}
	session, err := startFolderSession(ctx, &FolderSession{receiver: receiver, options: prepared}, transport, snapshot.FolderSnapshot)
	if err != nil {
		return nil, err
	}
	accepted = true
	return session, nil
}

func (s *FolderSession) catalogOptions() CatalogOptions {
	if s.receiver != nil {
		return s.receiver.options.Catalog
	}
	return s.folder.options.Catalog
}
func (s *FolderSession) snapshot() (FolderSnapshot, error) {
	if s.receiver != nil {
		snapshot, err := s.receiver.Snapshot(s.ctx)
		return snapshot.FolderSnapshot, err
	}
	return s.folder.Snapshot(s.ctx)
}
func (s *FolderSession) receiveIndex(batch CatalogIndex) error {
	if s.receiver != nil {
		return s.receiver.ReceiveIndex(s.ctx, batch)
	}
	return s.folder.ReceiveIndex(s.ctx, batch)
}
func (s *FolderSession) step(requests func(protocol.DeviceID) RequestFunc) (FolderStatus, error) {
	if s.receiver != nil {
		return s.receiver.Step(s.ctx, requests)
	}
	return s.folder.Step(s.ctx, requests)
}
func (s *FolderSession) readEntry(entry CatalogEntry, request *protocol.Request) ([]byte, error) {
	if s.receiver != nil {
		return s.receiver.ReadRequest(s.ctx, entry, request)
	}
	return s.folder.readEntry(s.ctx, entry, request)
}

func (s *FolderSession) sendReceiverConfiguration() error {
	snapshot, err := s.receiver.Snapshot(s.ctx)
	if err != nil {
		return err
	}
	config := &protocol.ClusterConfig{}
	if len(snapshot.PasswordToken) != 0 {
		peer := snapshot.Peers[s.options.Device.RemoteDevice]
		config.Folders = []protocol.Folder{{ID: s.catalogOptions().Folder, Type: protocol.FolderTypeReceiveEncrypted,
			Devices: []protocol.Device{
				{ID: s.catalogOptions().LocalDevice, IndexID: protocol.IndexID(snapshot.IndexID), MaxSequence: snapshot.Sequence, EncryptionPasswordToken: bytes.Clone(snapshot.PasswordToken)},
				{ID: s.options.Device.RemoteDevice, IndexID: protocol.IndexID(peer.IndexID), MaxSequence: peer.Sequence},
			}}}
	}
	// Nil passwords preserve the ciphertext: the receiver never installs folder
	// keys in the upstream codec. An empty new receiver first learns the token
	// from a certificate-pinned peer, then echoes it before advertising data.
	s.connection.ClusterConfig(config, nil)
	return nil
}

func (s *FolderSession) receiverClusterConfig(connection protocol.Connection, configuration *protocol.ClusterConfig) error {
	if connection.DeviceID() != s.options.Device.RemoteDevice || configuration == nil {
		return errors.New("syrnck BEP: receiver connection identity differs")
	}
	var selected *protocol.Folder
	for i := range configuration.Folders {
		folder := &configuration.Folders[i]
		if folder.ID == s.catalogOptions().Folder {
			if selected != nil {
				return errors.New("syrnck BEP: duplicate shared folder")
			}
			selected = folder
		}
	}
	if selected == nil || !selected.IsRunning() || selected.Type == protocol.FolderTypeReceiveEncrypted {
		return errors.New("syrnck BEP: encrypted receiver requires a running trusted peer folder")
	}
	var own, remote *protocol.Device
	for i := range selected.Devices {
		device := &selected.Devices[i]
		if device.ID == s.catalogOptions().LocalDevice {
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
	if own == nil || remote == nil || remote.IndexID == 0 || own.MaxSequence < 0 || remote.MaxSequence < 0 ||
		len(own.EncryptionPasswordToken) == 0 || len(remote.EncryptionPasswordToken) != 0 {
		return errors.New("syrnck BEP: invalid encrypted receiver agreement")
	}
	s.mu.Lock()
	changed := s.authorized && s.generation != uint64(remote.IndexID)
	s.mu.Unlock()
	if changed {
		return errors.New("syrnck BEP: peer changed index generation during its session")
	}
	if err := s.receiver.BindToken(s.ctx, own.EncryptionPasswordToken); err != nil {
		return err
	}
	s.mu.Lock()
	s.generation = uint64(remote.IndexID)
	s.knownLocal = PeerFrontier{uint64(own.IndexID), own.MaxSequence}
	s.authorized = true
	s.configureOnce.Do(func() { close(s.configured) })
	s.mu.Unlock()
	return nil
}
