package syncthing

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"sync"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// EncryptedReceiverOptions uses an initialized native Store as a ciphertext-only
// backend. There is no working directory, scanner, plaintext folder password or
// implicit native HEAD. OpenStore transfers ownership of each short-lived Store.
type EncryptedReceiverOptions struct {
	Catalog   CatalogOptions
	Storage   string
	OpenStore func(context.Context) (*syrnck.Store, error)
}
type EncryptedReceiver struct {
	options EncryptedReceiverOptions
	mu      sync.Mutex
	closed  bool
	ctx     context.Context
	cancel  context.CancelFunc
	active  map[protocol.DeviceID]bool
	workers sync.WaitGroup
}
type EncryptedReceiverSnapshot struct {
	FolderSnapshot
	PasswordToken []byte
}

func NewEncryptedReceiver(ctx context.Context, options EncryptedReceiverOptions) (*EncryptedReceiver, error) {
	if ctx == nil {
		return nil, errors.New("syrnck BEP: receiver context required")
	}
	if _, err := catalogConfiguration(options.Catalog); err != nil {
		return nil, err
	}
	if len(options.Catalog.Peers) == 0 {
		return nil, errors.New("syrnck BEP: encrypted receiver requires paired peers")
	}
	options.Catalog.Peers = append([]protocol.DeviceID(nil), options.Catalog.Peers...)
	if options.OpenStore == nil {
		if options.Storage == "" {
			return nil, errors.New("syrnck BEP: encrypted receiver Store required")
		}
		options.OpenStore = func(ctx context.Context) (*syrnck.Store, error) {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			return syrnck.Open(options.Storage, syrnck.Options{})
		}
	}
	child, cancel := context.WithCancel(ctx)
	r := &EncryptedReceiver{options: options, ctx: child, cancel: cancel, active: make(map[protocol.DeviceID]bool)}
	if err := r.withCatalog(ctx, func(store *syrnck.Store, catalog *Catalog) error { return catalog.Commit(ctx, store) }); err != nil {
		cancel()
		return nil, err
	}
	return r, nil
}
func (r *EncryptedReceiver) Close() error {
	r.mu.Lock()
	r.closed = true
	r.cancel()
	r.mu.Unlock()
	r.workers.Wait()
	return nil
}
func (r *EncryptedReceiver) withStore(ctx context.Context, action func(*syrnck.Store) error) (err error) {
	if ctx == nil {
		return errors.New("syrnck BEP: receiver context required")
	}
	if err = ctx.Err(); err != nil {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return errors.New("syrnck BEP: encrypted receiver closed")
	}
	store, err := r.options.OpenStore(ctx)
	if err != nil {
		return err
	}
	if store == nil {
		return errors.New("syrnck BEP: Store provider returned nil")
	}
	defer func() {
		closed := store.Close()
		if err == nil {
			err = closed
		}
	}()
	return action(store)
}
func (r *EncryptedReceiver) withCatalog(ctx context.Context, action func(*syrnck.Store, *Catalog) error) error {
	return r.withStore(ctx, func(store *syrnck.Store) error {
		catalog, err := OpenEncryptedCatalog(ctx, store, r.options.Catalog)
		if err != nil {
			return err
		}
		return action(store, catalog)
	})
}
func (r *EncryptedReceiver) BindToken(ctx context.Context, token []byte) error {
	return r.withCatalog(ctx, func(store *syrnck.Store, catalog *Catalog) error {
		if err := catalog.bindEncryptedToken(token); err != nil {
			return err
		}
		return catalog.Commit(ctx, store)
	})
}
func (r *EncryptedReceiver) Snapshot(ctx context.Context) (snapshot EncryptedReceiverSnapshot, err error) {
	err = r.withCatalog(ctx, func(_ *syrnck.Store, catalog *Catalog) error {
		snapshot.Catalog = catalog.tip
		snapshot.IndexID, snapshot.Sequence, snapshot.Files = catalog.LocalIndex()
		snapshot.Peers = make(map[protocol.DeviceID]PeerFrontier)
		for _, peer := range catalog.state.Peers {
			device, err := protocol.DeviceIDFromString(peer.Device)
			if err != nil {
				return err
			}
			snapshot.Peers[device] = PeerFrontier{peer.IndexID, peer.Sequence}
		}
		snapshot.PasswordToken = bytes.Clone(catalog.state.PasswordToken)
		return nil
	})
	return
}
func (r *EncryptedReceiver) ReceiveIndex(ctx context.Context, batch CatalogIndex) error {
	return r.withCatalog(ctx, func(store *syrnck.Store, catalog *Catalog) error {
		if err := catalog.ReceiveIndex(batch); err != nil {
			return err
		}
		if err := catalog.reconcileEncrypted(); err != nil {
			return err
		}
		return catalog.Commit(ctx, store)
	})
}

type cipherReceipt struct {
	Version int    `json:"version"`
	Folder  string `json:"folder"`
	Peer    string `json:"peer"`
	Name    string `json:"name"`
	Offset  int64  `json:"offset"`
	Size    int    `json:"size"`
	Token   []byte `json:"token"`
}

func (r *EncryptedReceiver) receipt(info protocol.FileInfo, peer protocol.DeviceID, number int) (string, []byte, error) {
	block := info.Blocks[number]
	data, err := json.Marshal(cipherReceipt{1, r.options.Catalog.Folder, peer.String(), info.Name, block.Offset, block.Size, block.Hash})
	if err != nil {
		return "", nil, err
	}
	digest := sha256.Sum256(data)
	prefix := sha256.Sum256([]byte(r.options.Catalog.LocalDevice.String() + "\x00" + r.options.Catalog.Folder))
	return "ffi/bep-receipts/" + hex.EncodeToString(prefix[:]) + "/" + hex.EncodeToString(digest[:]), data, nil
}
func readBoundedManifest(store *syrnck.Store, id string, maximum uint64) ([]byte, error) {
	data, err := store.Get(syrnck.ObjectKey{Kind: "manifest", ID: id})
	if err != nil {
		return nil, err
	}
	var manifest struct {
		Size uint64 `json:"size"`
	}
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, err
	}
	if manifest.Size > maximum {
		return nil, errors.New("syrnck BEP: receipt descriptor exceeds limit")
	}
	if err := store.VerifyFile(id); err != nil {
		return nil, err
	}
	return store.ReadRange(id, 0, manifest.Size)
}
func readCipherReceipt(store *syrnck.Store, root syrnck.ObjectKey, expected []byte) (CipherBlock, error) {
	if root.Kind != "revision" {
		return CipherBlock{}, errors.New("syrnck BEP: invalid encrypted block receipt")
	}
	data, err := store.Get(root)
	if err != nil {
		return CipherBlock{}, err
	}
	var revision catalogRevision
	if err := strictCatalogJSON(data, &revision); err != nil {
		return CipherBlock{}, err
	}
	if revision.Version != 1 || revision.Timestamp != 0 || len(revision.Parents) != 0 || revision.Message != "Syrnck encrypted BEP block v1" || len(revision.Files) < 2 || len(revision.Files) > 3 {
		return CipherBlock{}, errors.New("syrnck BEP: invalid encrypted block receipt")
	}
	file, found := revision.Files["receipt.json"]
	if !found || file.Executable {
		return CipherBlock{}, errors.New("syrnck BEP: invalid encrypted block receipt")
	}
	descriptor, err := readBoundedManifest(store, file.Manifest, 16384)
	if err != nil {
		return CipherBlock{}, err
	}
	if !bytes.Equal(descriptor, expected) {
		return CipherBlock{}, errors.New("syrnck BEP: encrypted block receipt binding differs")
	}
	var receipt cipherReceipt
	if err := strictCatalogJSON(descriptor, &receipt); err != nil {
		return CipherBlock{}, err
	}
	block := CipherBlock{}
	left := receipt.Size
	for i := 0; left > 0; i++ {
		file, found := revision.Files[fmt.Sprintf("part/%d", i)]
		if !found || file.Executable {
			return CipherBlock{}, errors.New("syrnck BEP: invalid encrypted block receipt")
		}
		length := min(left, syrnck.MaxObjectSize)
		part, err := readBoundedManifest(store, file.Manifest, uint64(length))
		if err != nil {
			return CipherBlock{}, err
		}
		if len(part) != length {
			return CipherBlock{}, errors.New("syrnck BEP: encrypted receipt part size differs")
		}
		block.Parts = append(block.Parts, file.Manifest)
		left -= length
	}
	if len(revision.Files) != len(block.Parts)+1 {
		return CipherBlock{}, errors.New("syrnck BEP: invalid encrypted block receipt")
	}
	return block, nil
}
func writeCipherReceipt(ctx context.Context, store *syrnck.Store, name string, descriptor, data []byte) (CipherBlock, syrnck.ObjectKey, error) {
	refs, err := store.RefsContext(ctx)
	if err != nil {
		return CipherBlock{}, syrnck.ObjectKey{}, err
	}
	if old, found := refs[name]; found {
		block, err := readCipherReceipt(store, old, descriptor)
		return block, old, err
	}
	metadata, err := store.PrepareBytesContext(ctx, descriptor, syrnck.Chunking{})
	if err != nil {
		return CipherBlock{}, syrnck.ObjectKey{}, err
	}
	revision := catalogRevision{Version: 1, Parents: []string{}, Message: "Syrnck encrypted BEP block v1", Files: map[string]catalogRevisionFile{"receipt.json": {Manifest: metadata}}}
	block := CipherBlock{}
	for at := 0; at < len(data); {
		end := min(at+syrnck.MaxObjectSize, len(data))
		id, err := store.PrepareBytesContext(ctx, data[at:end], syrnck.Chunking{})
		if err != nil {
			return CipherBlock{}, syrnck.ObjectKey{}, err
		}
		revision.Files[fmt.Sprintf("part/%d", len(block.Parts))] = catalogRevisionFile{Manifest: id}
		block.Parts = append(block.Parts, id)
		at = end
	}
	encoded, err := json.Marshal(revision)
	if err != nil {
		return CipherBlock{}, syrnck.ObjectKey{}, err
	}
	root, err := store.PutContext(ctx, "revision", encoded)
	if err != nil {
		return CipherBlock{}, root, err
	}
	applied, err := store.CompareRefsContext(ctx, []syrnck.RefUpdate{{Name: name, Value: &root}})
	if err == nil && !applied {
		err = ErrCatalogStale
	}
	return block, root, err
}

// Step retains each completed ciphertext block independently while waiting for
// the network. Only a complete current file enters the advertised local index.
// An interrupted file resumes its exact token-bound receipts, including after GC.
func (r *EncryptedReceiver) Step(ctx context.Context, requests func(protocol.DeviceID) RequestFunc) (status FolderStatus, err error) {
	if ctx == nil {
		return status, errors.New("syrnck BEP: receiver context required")
	}
	child, cancel := context.WithCancel(ctx)
	stop := context.AfterFunc(r.ctx, cancel)
	defer func() { stop(); cancel() }()
	ctx = child
	type plan struct {
		entry  CatalogEntry
		device protocol.DeviceID
	}
	var plans []plan
	err = r.withCatalog(ctx, func(_ *syrnck.Store, catalog *Catalog) error {
		status = FolderStatus{State: "synced", Catalog: catalog.tip.ID}
		for _, peer := range catalog.state.Peers {
			for _, entry := range peer.Files {
				if !encryptedEntryReady(entry) {
					device, err := protocol.DeviceIDFromString(peer.Device)
					if err != nil {
						return err
					}
					plans = append(plans, plan{copyCatalogEntry(entry), device})
					break
				}
			}
		}
		return nil
	})
	if err != nil || len(plans) == 0 {
		return status, err
	}
	if requests == nil {
		return status, errors.New("syrnck BEP: missing block requester")
	}
	var target *CatalogEntry
	var device protocol.DeviceID
	for _, candidate := range plans {
		if requests(candidate.device) == nil {
			continue
		}
		r.mu.Lock()
		if !r.closed && !r.active[candidate.device] {
			r.active[candidate.device] = true
			r.workers.Add(1)
			entry := candidate.entry
			target = &entry
			device = candidate.device
		}
		r.mu.Unlock()
		if target != nil {
			break
		}
	}
	if target == nil {
		status.State = "waiting_for_source"
		return status, ctx.Err()
	}
	defer func() { r.mu.Lock(); delete(r.active, device); r.mu.Unlock(); r.workers.Done() }()
	status.State = "receiving"
	parts := append([]CipherBlock(nil), target.CipherBlocks...)
	receipts := make(map[string]syrnck.ObjectKey)
	for number, block := range target.Info.Blocks {
		if err := ctx.Err(); err != nil {
			return status, err
		}
		if len(parts[number].Parts) != 0 {
			status.Transfer.ReusedBytes += uint64(block.Size)
			continue
		}
		name, descriptor, err := r.receipt(target.Info, device, number)
		if err != nil {
			return status, err
		}
		var root *syrnck.ObjectKey
		err = r.withStore(ctx, func(store *syrnck.Store) error {
			refs, err := store.RefsContext(ctx)
			if err != nil {
				return err
			}
			root = refKey(refs, name)
			if root != nil {
				parts[number], err = readCipherReceipt(store, *root, descriptor)
			}
			return err
		})
		if err != nil {
			return status, err
		}
		if root != nil {
			receipts[name] = *root
			status.Transfer.StagedBytes += uint64(block.Size)
			continue
		}
		if requests == nil {
			return status, errors.New("syrnck BEP: missing block requester")
		}
		request := requests(device)
		if request == nil {
			return status, errors.New("syrnck BEP: missing block requester")
		}
		data, err := request(ctx, &protocol.Request{Folder: r.options.Catalog.Folder, Name: target.Info.Name, Offset: block.Offset,
			Size: block.Size, Hash: bytes.Clone(block.Hash), BlockNo: number})
		if err != nil {
			return status, err
		}
		if len(data) != block.Size {
			return status, errors.New("syrnck BEP: encrypted block length mismatch")
		}
		status.Transfer.Requests++
		status.Transfer.TransferredBytes += uint64(len(data))
		err = r.withStore(ctx, func(store *syrnck.Store) error {
			var receipt syrnck.ObjectKey
			var err error
			parts[number], receipt, err = writeCipherReceipt(ctx, store, name, descriptor, data)
			if err == nil {
				receipts[name] = receipt
			}
			return err
		})
		if err != nil {
			return status, err
		}
	}
	err = r.withCatalog(ctx, func(store *syrnck.Store, catalog *Catalog) error {
		state := cloneCatalogState(catalog.state)
		found := false
		for p := range state.Peers {
			if state.Peers[p].Device == device.String() {
				i, present := findCatalogEntry(state.Peers[p].Files, target.Info.Name)
				if !present {
					return ErrCatalogStale
				}
				entry := &state.Peers[p].Files[i]
				if entry.Info.Deleted || len(entry.Info.Blocks) != len(target.Info.Blocks) {
					return ErrCatalogStale
				}
				for number, block := range entry.Info.Blocks {
					if !sameCipherToken(block, target.Info.Blocks[number]) {
						return ErrCatalogStale
					}
				}
				entry.CipherBlocks = parts
				found = true
				break
			}
		}
		if !found {
			return ErrCatalogStale
		}
		if err := catalog.replace(state); err != nil {
			return err
		}
		if err := catalog.reconcileEncrypted(); err != nil {
			return err
		}
		if err := catalog.Commit(ctx, store); err != nil {
			return err
		}
		status.State = "received"
		status.Catalog = catalog.tip.ID
		// Publication has made these manifests durable independently. Delete only
		// exact receipt roots, with a no-op CAS guarding the retaining catalog.
		group := []syrnck.RefUpdate{{Name: catalogStateRef(catalog.state), Expected: &catalog.tip, Value: &catalog.tip}}
		flush := func() error {
			if len(group) == 1 {
				return nil
			}
			applied, err := store.CompareRefsContext(ctx, group)
			if err == nil && !applied {
				err = ErrCatalogStale
			}
			group = group[:1]
			return err
		}
		for name, root := range receipts {
			copy := root
			group = append(group, syrnck.RefUpdate{Name: name, Expected: &copy})
			if len(group) == 65536 {
				if err := flush(); err != nil {
					return err
				}
			}
		}
		return flush()
	})
	return status, err
}
func readCipherBlock(store *syrnck.Store, block CipherBlock, size int) ([]byte, error) {
	data := make([]byte, 0, size)
	for _, part := range block.Parts {
		piece, err := readBoundedManifest(store, part, uint64(min(size-len(data), syrnck.MaxObjectSize)))
		if err != nil {
			return nil, err
		}
		data = append(data, piece...)
	}
	if len(data) != size {
		return nil, errors.New("syrnck BEP: encrypted block manifest size differs")
	}
	return data, nil
}
func (r *EncryptedReceiver) ReadRequest(ctx context.Context, entry CatalogEntry, request *protocol.Request) (data []byte, err error) {
	if request == nil || request.Folder != r.options.Catalog.Folder || protocolName(request.Name) != entry.Info.Name ||
		entry.Info.Deleted || entry.Info.Type != protocol.FileInfoTypeFile || request.Offset < 0 || len(request.Hash) != 48 {
		return nil, protocol.ErrNoSuchFile
	}
	number := request.BlockNo
	if number < 0 || number >= len(entry.Info.Blocks) || number >= len(entry.CipherBlocks) {
		return nil, protocol.ErrNoSuchFile
	}
	block := entry.Info.Blocks[number]
	if request.Offset != block.Offset || request.Size != block.Size || !bytes.Equal(request.Hash, block.Hash) {
		return nil, protocol.ErrInvalid
	}
	err = r.withStore(ctx, func(store *syrnck.Store) error {
		var err error
		data, err = readCipherBlock(store, entry.CipherBlocks[number], block.Size)
		return err
	})
	return
}
