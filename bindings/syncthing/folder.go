package syncthing

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	stfs "github.com/syncthing/syncthing/lib/fs"
	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

var (
	ErrFolderPending = errors.New("syrnck BEP: workspace application is pending")
	ErrFolderChanged = errors.New("syrnck BEP: workspace binding or causal input changed")
)

// FolderOptions binds the file synchronization controller to an existing native
// workspace. OpenStore returns a newly opened handle owned and closed by Folder;
// it can obtain encryption keys from the embedding application's key provider.
type FolderOptions struct {
	Catalog                       CatalogOptions
	Directory, Storage, Workspace string
	OpenStore                     func(context.Context) (*syrnck.Store, error)
	UsePermissions                bool
	PreserveCheckoutEdits         bool
	AuditInterval                 time.Duration
}

type FolderStatus struct {
	State        string     `json:"state"`
	LocalChanges int        `json:"local_changes"`
	Revision     string     `json:"revision"`
	Catalog      string     `json:"catalog"`
	Transfer     FetchStats `json:"transfer"`
}

type PeerFrontier struct {
	IndexID  uint64
	Sequence int64
}
type FolderSnapshot struct {
	Catalog  syrnck.ObjectKey
	IndexID  uint64
	Sequence int64
	Files    []CatalogEntry
	Peers    map[protocol.DeviceID]PeerFrontier
}

type folderBinding struct {
	Version         int    `json:"version"`
	Device          string `json:"device"`
	Folder          string `json:"folder"`
	Workspace       string `json:"workspace"`
	Target          string `json:"target"`
	Materialization string `json:"materialization"`
}

type folderExpected struct {
	Target          string `json:"target"`
	Revision        string `json:"revision"`
	Materialization string `json:"materialization"`
}

func freezeNative(expected syrnck.NativeExpectation) folderExpected {
	return folderExpected{expected.Target, expected.Revision, expected.Materialization}
}
func (e folderExpected) native() syrnck.NativeExpectation {
	return syrnck.NativeExpectation{Target: e.Target, Revision: e.Revision, Materialization: e.Materialization}
}

type folderPending struct {
	Version               int            `json:"version"`
	Owner                 string         `json:"owner"`
	Binding               string         `json:"binding"`
	SourceCatalog         string         `json:"source_catalog"`
	Catalog               string         `json:"catalog"`
	Expected              folderExpected `json:"expected"`
	Candidate             string         `json:"candidate"`
	Applied               string         `json:"applied"`
	PreserveCheckoutEdits bool           `json:"preserve_checkout_edits,omitempty"`
}

type folderLoaded struct {
	catalog *Catalog
	native  syrnck.NativeState
	refs    map[string]syrnck.ObjectKey
	binding *syrnck.ObjectKey
	applied *syrnck.ObjectKey
	pending *folderPending
}

// Folder retains no idle Store. Native transactions and protocol checkpoints are
// serialized, but a block download releases that serialization and the Store so
// incoming indexes and immutable block requests can continue. Close after users
// have stopped; a failed operation is resumed from native roots on the next Step.
type Folder struct {
	options               FolderOptions
	workspace             *syrnck.Workspace
	mu, stores, stepping  sync.Mutex
	prefix, storagePrefix string
	closed                bool
	nextAudit             time.Time
}

func NewFolder(ctx context.Context, options FolderOptions) (*Folder, error) {
	if options.AuditInterval == 0 {
		options.AuditInterval = 5 * time.Minute
	}
	if options.AuditInterval < time.Second || options.AuditInterval > 24*time.Hour {
		return nil, errors.New("syrnck BEP: audit interval must be in 1 second..24 hours")
	}
	if _, err := catalogConfiguration(options.Catalog); err != nil {
		return nil, err
	}
	options.Catalog.Peers = append([]protocol.DeviceID(nil), options.Catalog.Peers...)
	if options.OpenStore == nil {
		options.OpenStore = func(ctx context.Context) (*syrnck.Store, error) {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			return syrnck.Open(options.Storage, syrnck.Options{})
		}
	}
	workspace, err := syrnck.OpenWorkspaceContext(ctx, options.Directory, options.Storage, syrnck.WorkspaceOptions{Name: options.Workspace})
	if err != nil {
		return nil, err
	}
	folder := &Folder{options: options, workspace: workspace, prefix: catalogRef(options.Catalog.LocalDevice.String(), options.Catalog.Folder) + "/workspace"}
	if relative, err := filepath.Rel(options.Directory, options.Storage); err == nil && relative != "." && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		folder.storagePrefix = filepath.ToSlash(relative)
	}
	err = folder.withStore(ctx, func(store *syrnck.Store) error {
		loaded, err := folder.load(ctx, store)
		if err != nil {
			return err
		}
		if loaded.pending != nil {
			return nil
		} // Step performs guarded recovery.
		_, err = folder.observeLocal(ctx, store, loaded)
		return err
	})
	if err != nil {
		_ = workspace.Close()
		return nil, err
	}
	return folder, nil
}

func (f *Folder) Close() error {
	f.stepping.Lock()
	defer f.stepping.Unlock()
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return nil
	}
	f.closed = true
	return f.workspace.Close()
}

// Invalidate requests a synchronous full audit on the next Step. Resident
// callers should also schedule audits independently of advisory notifications.
func (f *Folder) Invalidate() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return errors.New("syrnck BEP: folder closed")
	}
	return f.workspace.Invalidate()
}

func (f *Folder) withStore(ctx context.Context, action func(*syrnck.Store) error) (err error) {
	if err = ctx.Err(); err != nil {
		return err
	}
	f.stores.Lock()
	defer f.stores.Unlock()
	store, err := f.options.OpenStore(ctx)
	if err != nil {
		return err
	}
	if store == nil {
		return errors.New("syrnck BEP: Store provider returned nil")
	}
	defer func() {
		closeError := store.Close()
		if err == nil {
			err = closeError
		}
	}()
	if err = store.UseWorkspace(f.options.Workspace); err != nil {
		return err
	}
	return action(store)
}

func (f *Folder) managed(name string) bool {
	native := filepath.FromSlash(name)
	if stfs.IsInternal(native) || stfs.IsTemporary(native) {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if strings.EqualFold(part, ".git") || strings.EqualFold(part, ".syrnck") {
			return false
		}
	}
	return f.storagePrefix == "" || !(strings.EqualFold(name, f.storagePrefix) || strings.HasPrefix(strings.ToLower(name), strings.ToLower(f.storagePrefix)+"/"))
}

func refKey(refs map[string]syrnck.ObjectKey, name string) *syrnck.ObjectKey {
	if key, ok := refs[name]; ok {
		copy := key
		return &copy
	}
	return nil
}
func keyEqual(a, b *syrnck.ObjectKey) bool {
	return a == nil && b == nil || a != nil && b != nil && *a == *b
}
func folderOwner(binding folderBinding) string {
	data, _ := json.Marshal(binding)
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:])
}

func folderIdentity(value string) bool {
	decoded, err := hex.DecodeString(value)
	return err == nil && len(decoded) == 32 && value == strings.ToLower(value)
}

func (f *Folder) load(ctx context.Context, store *syrnck.Store) (*folderLoaded, error) {
	native, err := f.workspace.NativeStateContext(ctx, store)
	if err != nil {
		return nil, err
	}
	catalog, err := OpenCatalog(ctx, store, f.options.Catalog)
	if err != nil {
		return nil, err
	}
	refs, err := store.RefsContext(ctx)
	if err != nil {
		return nil, err
	}
	loaded := &folderLoaded{catalog: catalog, native: native, refs: refs, binding: refKey(refs, f.prefix+"/binding"), applied: refKey(refs, f.prefix+"/applied")}
	if (loaded.binding == nil) != (loaded.applied == nil) {
		return nil, errors.New("syrnck BEP: incomplete workspace checkpoint")
	}
	if loaded.binding != nil {
		if catalog.tip.ID == "" {
			return nil, errors.New("syrnck BEP: bound workspace catalog is missing")
		}
		if loaded.binding.Kind != "chunk" || loaded.applied.Kind != "revision" {
			return nil, errors.New("syrnck BEP: invalid workspace checkpoint types")
		}
		data, err := store.Get(*loaded.binding)
		if err != nil {
			return nil, err
		}
		var saved folderBinding
		if err := strictCatalogJSON(data, &saved); err != nil {
			return nil, err
		}
		if saved != f.binding(native) {
			return nil, ErrFolderChanged
		}
	}
	keys := []*syrnck.ObjectKey{refKey(refs, f.prefix+"/pending/state"), refKey(refs, f.prefix+"/pending/native"), refKey(refs, f.prefix+"/pending/catalog")}
	count := 0
	for _, key := range keys {
		if key != nil {
			count++
		}
	}
	if count != 0 {
		if count != 3 || loaded.binding == nil || keys[0].Kind != "chunk" || keys[1].Kind != "revision" || keys[2].Kind != "revision" {
			return nil, errors.New("syrnck BEP: incomplete pending workspace application")
		}
		data, err := store.Get(*keys[0])
		if err != nil {
			return nil, err
		}
		var pending folderPending
		if err := strictCatalogJSON(data, &pending); err != nil {
			return nil, err
		}
		if pending.Version != 1 || pending.Binding != folderOwner(f.binding(native)) || !folderIdentity(pending.Owner) || pending.SourceCatalog != catalog.tip.ID || pending.Catalog != keys[2].ID || pending.Candidate != keys[1].ID || pending.Applied != loaded.applied.ID || pending.Expected.Target != native.Target || pending.Expected.Materialization != native.Expected().Materialization {
			return nil, ErrFolderChanged
		}
		loaded.pending = &pending
	}
	return loaded, nil
}

func (f *Folder) binding(native syrnck.NativeState) folderBinding {
	return folderBinding{Version: 1, Device: f.options.Catalog.LocalDevice.String(), Folder: f.options.Catalog.Folder,
		Workspace: native.Workspace, Target: native.Target, Materialization: native.Expected().Materialization}
}

type nativeFolderTree struct {
	Timestamp uint64                         `json:"timestamp"`
	Files     map[string]catalogRevisionFile `json:"files"`
}

func nativeTree(store *syrnck.Store, key syrnck.ObjectKey) (nativeFolderTree, error) {
	var tree nativeFolderTree
	data, err := store.Get(key)
	if err != nil {
		return tree, err
	}
	err = json.Unmarshal(data, &tree)
	if err == nil && tree.Files == nil {
		err = errors.New("syrnck BEP: native revision lacks its file tree")
	}
	return tree, err
}

// Native trees retain physical spelling (including Darwin NFD). BEP names
// use NFC. Keep an explicit mapping so an echo or edit does not create a second
// native name or a false deletion for the same protocol file.
func nativeProtocolNames(files map[string]catalogRevisionFile) (map[string]string, error) {
	names := make(map[string]string, len(files))
	for name := range files {
		canonical := protocolName(name)
		if _, found := names[canonical]; found {
			return nil, errors.New("syrnck BEP: native names collide after protocol normalization")
		}
		names[canonical] = name
	}
	return names, nil
}

func manifestHash(store *syrnck.Store, id string) (string, error) {
	data, err := store.Get(syrnck.ObjectKey{Kind: "manifest", ID: id})
	if err != nil {
		return "", err
	}
	var value struct {
		Hash string `json:"file_hash"`
	}
	err = json.Unmarshal(data, &value)
	return value.Hash, err
}
func decodeWorking(data []byte) (bool, error) {
	var report struct {
		Clean bool `json:"clean"`
	}
	err := json.Unmarshal(data, &report)
	return report.Clean, err
}

func (f *Folder) observeLocal(ctx context.Context, store *syrnck.Store, loaded *folderLoaded) (int, error) {
	if loaded.pending != nil {
		return 0, ErrFolderPending
	}
	var pending struct {
		Pending bool `json:"pending"`
	}
	if err := json.Unmarshal(loaded.native.Checkout, &pending); err != nil {
		return 0, err
	}
	if pending.Pending || loaded.native.Staging {
		return 0, ErrFolderPending
	}
	if time.Now().After(f.nextAudit) {
		if err := f.workspace.Invalidate(); err != nil {
			return 0, err
		}
		f.nextAudit = time.Now().Add(f.options.AuditInterval)
	}
	data, err := f.workspace.StatusContext(ctx, store, syrnck.WorkspaceSelection{})
	if err != nil {
		return 0, err
	}
	clean, err := decodeWorking(data)
	if err != nil {
		return 0, err
	}
	if !clean || loaded.native.Revision == nil {
		if _, err := f.workspace.CaptureContext(ctx, store, loaded.native.Expected(), "Syrnck BEP local workspace snapshot"); err != nil {
			return 0, err
		}
		loaded.native, err = f.workspace.NativeStateContext(ctx, store)
		if err != nil {
			return 0, err
		}
	}
	if loaded.binding != nil && keyEqual(loaded.native.Revision, loaded.applied) {
		return 0, nil
	}
	tree, err := nativeTree(store, *loaded.native.Revision)
	if err != nil {
		return 0, err
	}
	if tree.Timestamp > math.MaxInt64 {
		return 0, errors.New("syrnck BEP: native timestamp exceeds int64")
	}
	nativeNames, err := nativeProtocolNames(tree.Files)
	if err != nil {
		return 0, err
	}
	var changes []CatalogEntry
	local := make(map[string]CatalogEntry, len(loaded.catalog.state.Files))
	for _, entry := range loaded.catalog.state.Files {
		if entry.Info.Type != protocol.FileInfoTypeFile || !f.managed(entry.Info.Name) {
			return 0, errors.New("syrnck BEP: existing local catalog is not a managed file workspace")
		}
		local[entry.Info.Name] = entry
	}
	for name, physical := range nativeNames {
		file := tree.Files[physical]
		if !f.managed(name) {
			continue
		}
		if prior, ok := local[name]; ok && !prior.Info.Deleted {
			sameContent := prior.Manifest == file.Manifest
			if !sameContent {
				oldHash, err := manifestHash(store, prior.Manifest)
				if err != nil {
					return 0, err
				}
				newHash, err := manifestHash(store, file.Manifest)
				if err != nil {
					return 0, err
				}
				sameContent = oldHash == newHash
			}
			oldExecutable := prior.Info.Permissions&0111 != 0
			if sameContent && (!f.options.UsePermissions || oldExecutable == file.Executable) {
				index, _ := findCatalogEntry(loaded.catalog.state.Files, name)
				loaded.catalog.state.Files[index].Manifest = file.Manifest
				continue
			}
		}
		view, err := NewFile(ctx, store, file.Manifest, f.options.Catalog.Folder, name, 0)
		if err != nil {
			return 0, err
		}
		info := view.Info()
		info.ModifiedS = int64(tree.Timestamp)
		if f.options.UsePermissions {
			info.NoPermissions = false
			info.Permissions = 0644
			if file.Executable {
				info.Permissions = 0755
			}
		}
		changes = append(changes, CatalogEntry{Info: info, Manifest: file.Manifest})
	}
	for name, entry := range local {
		if _, found := nativeNames[name]; !found && !entry.Info.Deleted {
			info := copyInfo(entry.Info)
			info.Deleted = true
			info.Size = 0
			info.Blocks = nil
			info.BlocksHash = nil
			info.Version = protocol.Vector{}
			info.Sequence = 0
			info.ModifiedBy = 0
			info.ModifiedS = int64(tree.Timestamp)
			changes = append(changes, CatalogEntry{Info: info})
		}
	}
	sort.Slice(changes, func(i, j int) bool { return changes[i].Info.Name < changes[j].Info.Name })
	if err := loaded.catalog.RecordLocalChanges(changes); err != nil {
		return 0, err
	}
	updates := []syrnck.RefUpdate{{Name: f.prefix + "/applied", Expected: loaded.applied, Value: loaded.native.Revision},
		{Name: loaded.native.Reference, Expected: loaded.native.Revision, Value: loaded.native.Revision}}
	if loaded.binding == nil {
		data, _ := json.Marshal(f.binding(loaded.native))
		key, err := store.PutContext(ctx, "chunk", data)
		if err != nil {
			return 0, err
		}
		updates = append(updates, syrnck.RefUpdate{Name: f.prefix + "/binding", Value: &key})
	}
	if err := loaded.catalog.CommitWithRefs(ctx, store, updates); err != nil {
		return 0, err
	}
	return len(changes), nil
}

// Refresh captures local changes and resumes an owned pending application
// without waiting for remote data. It can run while Step is downloading blocks.
func (f *Folder) Refresh(ctx context.Context) (status FolderStatus, err error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	status.State = "idle"
	if f.closed {
		return status, errors.New("syrnck BEP: folder closed")
	}
	err = f.withStore(ctx, func(store *syrnck.Store) error {
		loaded, e := f.load(ctx, store)
		if e != nil {
			return e
		}
		if loaded.pending != nil {
			if e = f.recoverPending(ctx, store, loaded); e != nil {
				return e
			}
			loaded, e = f.load(ctx, store)
			if e != nil {
				return e
			}
			status.State = "recovered"
		}
		status.LocalChanges, e = f.observeLocal(ctx, store, loaded)
		if e != nil {
			return e
		}
		loaded, e = f.load(ctx, store)
		if e != nil {
			return e
		}
		status.Catalog = loaded.catalog.tip.ID
		if loaded.native.Revision != nil {
			status.Revision = loaded.native.Revision.ID
		}
		if status.LocalChanges > 0 {
			status.State = "changed"
		}
		return nil
	})
	if err != nil {
		status.State = "blocked"
	}
	return
}

// ReceiveIndex records a sequence-bounded announcement from an authenticated
// paired device. The network model must supply its own connection identity and
// negotiated generation; payload data is fetched separately by Step.
func (f *Folder) ReceiveIndex(ctx context.Context, batch CatalogIndex) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return errors.New("syrnck BEP: folder closed")
	}
	return f.withStore(ctx, func(store *syrnck.Store) error {
		loaded, err := f.load(ctx, store)
		if err != nil {
			return err
		}
		if loaded.pending != nil {
			return ErrFolderPending
		}
		if err := loaded.catalog.ReceiveIndex(batch); err != nil {
			return err
		}
		return loaded.catalog.Commit(ctx, store)
	})
}

func (f *Folder) Snapshot(ctx context.Context) (snapshot FolderSnapshot, err error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return snapshot, errors.New("syrnck BEP: folder closed")
	}
	err = f.withStore(ctx, func(store *syrnck.Store) error {
		loaded, e := f.load(ctx, store)
		if e != nil {
			return e
		}
		if loaded.pending != nil {
			return ErrFolderPending
		}
		snapshot.Catalog = loaded.catalog.tip
		snapshot.IndexID, snapshot.Sequence, snapshot.Files = loaded.catalog.LocalIndex()
		snapshot.Peers = make(map[protocol.DeviceID]PeerFrontier)
		for _, peer := range loaded.catalog.state.Peers {
			device, e := protocol.DeviceIDFromString(peer.Device)
			if e != nil {
				return e
			}
			snapshot.Peers[device] = PeerFrontier{peer.IndexID, peer.Sequence}
		}
		return nil
	})
	return
}

func (f *Folder) readEntry(ctx context.Context, entry CatalogEntry, request *protocol.Request) (data []byte, err error) {
	err = f.withStore(ctx, func(store *syrnck.Store) error {
		view := File{store: store, manifest: entry.Manifest, folder: f.options.Catalog.Folder, info: entry.Info}
		var e error
		data, e = view.ReadRequest(ctx, request)
		return e
	})
	return
}

type folderDownload struct {
	Device  protocol.DeviceID
	IndexID uint64
	Info    protocol.FileInfo
	Reuse   *CatalogEntry
}

func reuseEntry(entries []CatalogEntry, info protocol.FileInfo) *CatalogEntry {
	wanted := make(map[blockKey]bool, len(info.Blocks))
	for _, block := range info.Blocks {
		wanted[identity(block)] = true
	}
	var best *CatalogEntry
	var score int64
	for _, entry := range entries {
		if entry.Manifest == "" || entry.Info.Deleted || entry.Info.Type != protocol.FileInfoTypeFile || entry.Info.BlockSize() != info.BlockSize() {
			continue
		}
		var matched int64
		for _, block := range entry.Info.Blocks {
			if wanted[identity(block)] {
				matched += int64(block.Size)
			}
		}
		if best == nil || matched > score || matched == score && entry.Info.Name == info.Name {
			copy := entry
			best = &copy
			score = matched
		}
	}
	return best
}

func sameInfo(a, b protocol.FileInfo) bool {
	left, _ := json.Marshal(a)
	right, _ := json.Marshal(b)
	return bytes.Equal(left, right)
}

func (f *Folder) selectIncoming(ctx context.Context, store *syrnck.Store, loaded *folderLoaded) (*folderDownload, *Catalog, error) {
	draft := &Catalog{state: cloneCatalogState(loaded.catalog.state), tip: loaded.catalog.tip, original: bytes.Clone(loaded.catalog.original)}
	for _, peer := range loaded.catalog.state.Peers {
		device, err := protocol.DeviceIDFromString(peer.Device)
		if err != nil {
			return nil, nil, err
		}
		for _, entry := range peer.Files {
			if entry.Info.Type != protocol.FileInfoTypeFile || !f.managed(entry.Info.Name) {
				continue
			}
			i, found := findCatalogEntry(draft.state.Files, entry.Info.Name)
			if found && draft.state.Files[i].Info.Version.Compare(entry.Info.Version) == protocol.Greater {
				continue
			}
			if !entry.Info.Deleted && entry.Manifest == "" {
				plan := &folderDownload{Device: device, IndexID: peer.IndexID, Info: copyInfo(entry.Info)}
				plan.Reuse = reuseEntry(loaded.catalog.state.Files, entry.Info)
				if plan.Reuse == nil && found && !draft.state.Files[i].Info.Deleted {
					prior := draft.state.Files[i]
					view, e := NewFile(ctx, store, prior.Manifest, f.options.Catalog.Folder, prior.Info.Name, entry.Info.BlockSize())
					if e != nil {
						return nil, nil, e
					}
					prior.Info = view.Info()
					plan.Reuse = &prior
				}
				if plan.Reuse == nil || plan.Reuse.Info.Size != entry.Info.Size || !bytes.Equal(protocol.BlocksHash(plan.Reuse.Info.Blocks), protocol.BlocksHash(entry.Info.Blocks)) {
					return plan, nil, nil
				}
				// Reuse an already verified immutable stream, including an exact
				// rename. Publication re-verifies its complete closure and blocks.
				for p := range draft.state.Peers {
					if draft.state.Peers[p].Device == peer.Device {
						j, _ := findCatalogEntry(draft.state.Peers[p].Files, entry.Info.Name)
						draft.state.Peers[p].Files[j].Manifest = plan.Reuse.Manifest
					}
				}
			}
			if err := draft.Adopt(device, entry.Info.Name); err != nil {
				return nil, nil, err
			}
		}
	}
	return nil, draft, nil
}

func (f *Folder) pendingUpdates(loaded *folderLoaded) []syrnck.RefUpdate {
	var updates []syrnck.RefUpdate
	for _, suffix := range []string{"state", "native", "catalog"} {
		name := f.prefix + "/pending/" + suffix
		updates = append(updates, syrnck.RefUpdate{Name: name, Expected: refKey(loaded.refs, name)})
	}
	return updates
}

func (f *Folder) recoverPending(ctx context.Context, store *syrnck.Store, loaded *folderLoaded) error {
	p := loaded.pending
	if p == nil {
		return nil
	}
	var journal struct {
		Pending bool   `json:"pending"`
		Owner   string `json:"owner"`
		Target  string `json:"target"`
	}
	if err := json.Unmarshal(loaded.native.Checkout, &journal); err != nil {
		return err
	}
	if journal.Pending {
		if journal.Owner != p.Owner || journal.Target != p.Candidate {
			return ErrFolderChanged
		}
		recover := f.workspace.RecoverOwnedContext
		if p.PreserveCheckoutEdits {
			recover = f.workspace.RecoverOwnedPreservingContext
		}
		if _, err := recover(ctx, store, p.Expected.Target, p.Candidate, p.Owner); err != nil {
			return err
		}
		var err error
		loaded.native, err = f.workspace.NativeStateContext(ctx, store)
		if err != nil {
			return err
		}
	}
	current := ""
	if loaded.native.Revision != nil {
		current = loaded.native.Revision.ID
	}
	if current != p.Candidate {
		if current != p.Expected.Revision || freezeNative(loaded.native.Expected()) != p.Expected {
			return ErrFolderChanged
		}
		if _, err := f.workspace.CheckoutOwnedContext(ctx, store, p.Expected.native(), p.Candidate, p.Owner); err != nil {
			// A local edit can arrive after the durable preparation but before
			// the core creates its journal. Retire only that demonstrably
			// unstarted intent; never retarget or abandon a published journal.
			state, inspection := f.workspace.NativeStateContext(ctx, store)
			var active struct {
				Pending bool `json:"pending"`
			}
			if inspection == nil {
				inspection = json.Unmarshal(state.Checkout, &active)
			}
			if inspection == nil && !active.Pending && freezeNative(state.Expected()) == p.Expected {
				updates := f.pendingUpdates(loaded)
				updates = append(updates, syrnck.RefUpdate{Name: state.Reference, Expected: state.Revision, Value: state.Revision},
					syrnck.RefUpdate{Name: catalogRef(f.options.Catalog.LocalDevice.String(), f.options.Catalog.Folder), Expected: &loaded.catalog.tip, Value: &loaded.catalog.tip})
				if _, retirement := store.CompareRefsContext(ctx, updates); retirement != nil {
					return retirement
				}
			}
			return err
		}
	}
	// A committed native receipt is durable evidence of application even if the
	// user edited working bytes after native publication but before this CAS.
	// HEAD alone is insufficient: a manual checkout must never acknowledge our
	// protocol transaction. Each pending operation has its own random owner.
	prefix := "checkouts/receipts/"
	if f.options.Workspace != "" {
		prefix = "workspaces/" + f.options.Workspace + "/" + prefix
	}
	refs, err := store.RefsContext(ctx)
	if err != nil {
		return err
	}
	proved := false
	for name := range refs {
		if !strings.HasPrefix(name, prefix) {
			continue
		}
		data, e := f.workspace.CheckoutStatusContext(ctx, store, strings.TrimPrefix(name, prefix))
		if e != nil {
			return e
		}
		var receipt struct {
			Pending bool   `json:"pending"`
			State   string `json:"state"`
			Owner   string `json:"owner"`
			Source  string `json:"source"`
			Target  string `json:"target"`
			Branch  string `json:"branch"`
		}
		if e = json.Unmarshal(data, &receipt); e != nil {
			return e
		}
		if !receipt.Pending && receipt.State == "committed" && receipt.Owner == p.Owner && receipt.Source == p.Expected.Revision && receipt.Target == p.Candidate && receipt.Branch == p.Expected.Target {
			proved = true
			break
		}
	}
	if !proved {
		return ErrFolderPending
	}
	if err = f.workspace.Invalidate(); err != nil {
		return err
	}
	prepared := syrnck.ObjectKey{Kind: "revision", ID: p.Catalog}
	candidate := syrnck.ObjectKey{Kind: "revision", ID: p.Candidate}
	updates := f.pendingUpdates(loaded)
	updates = append(updates, syrnck.RefUpdate{Name: catalogRef(f.options.Catalog.LocalDevice.String(), f.options.Catalog.Folder), Expected: &loaded.catalog.tip, Value: &prepared},
		syrnck.RefUpdate{Name: f.prefix + "/applied", Expected: loaded.applied, Value: &candidate},
		syrnck.RefUpdate{Name: loaded.native.Reference, Expected: &candidate, Value: &candidate})
	applied, err := store.CompareRefsContext(ctx, updates)
	if err != nil {
		return err
	}
	if !applied {
		return ErrFolderChanged
	}
	return nil
}

func (f *Folder) prepareApplication(ctx context.Context, store *syrnck.Store, loaded *folderLoaded, draft *Catalog) error {
	data, _ := json.Marshal(draft.state)
	if bytes.Equal(data, loaded.catalog.original) {
		return nil
	}
	tree, err := nativeTree(store, *loaded.native.Revision)
	if err != nil {
		return err
	}
	originalFiles := make(map[string]catalogRevisionFile, len(tree.Files))
	for name, file := range tree.Files {
		originalFiles[name] = file
	}
	nativeNames, err := nativeProtocolNames(originalFiles)
	if err != nil {
		return err
	}
	for _, entry := range loaded.catalog.state.Files {
		if f.managed(entry.Info.Name) {
			delete(tree.Files, nativeNames[entry.Info.Name])
		}
	}
	for _, entry := range draft.state.Files {
		if entry.Info.Deleted {
			continue
		}
		if entry.Info.Type != protocol.FileInfoTypeFile {
			return errors.New("syrnck BEP: unsupported working-file type")
		}
		physical := nativeNames[entry.Info.Name]
		if physical == "" {
			physical = entry.Info.Name
		}
		executable := entry.Info.Permissions&0111 != 0
		if !f.options.UsePermissions {
			executable = originalFiles[physical].Executable
		}
		tree.Files[physical] = catalogRevisionFile{Manifest: entry.Manifest, Executable: executable}
	}
	if len(tree.Files) == len(originalFiles) {
		same := true
		for name, file := range tree.Files {
			if originalFiles[name] != file {
				same = false
				break
			}
		}
		if same {
			return draft.CommitWithRefs(ctx, store, []syrnck.RefUpdate{{Name: f.prefix + "/applied", Expected: loaded.applied, Value: loaded.applied},
				{Name: loaded.native.Reference, Expected: loaded.native.Revision, Value: loaded.native.Revision}})
		}
	}
	parents := []string{loaded.native.Revision.ID}
	encoded, err := json.Marshal(catalogRevision{Version: 1, Parents: parents, Message: "Syrnck BEP synchronized workspace", Timestamp: uint64(time.Now().Unix()), Files: tree.Files})
	if err != nil {
		return err
	}
	candidate, err := store.PutContext(ctx, "revision", encoded)
	if err != nil {
		return err
	}
	plan, err := f.workspace.PlanCheckoutContext(ctx, store, loaded.native.Expected(), candidate.ID)
	if err != nil {
		return err
	}
	var ready struct {
		Ready bool `json:"ready"`
	}
	if err := json.Unmarshal(plan, &ready); err != nil {
		return err
	}
	if !ready.Ready {
		return errors.New("syrnck BEP: native workspace application is obstructed")
	}
	prepared, _, err := draft.prepare(ctx, store)
	if err != nil {
		return err
	}
	var nonce [32]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return err
	}
	pending := folderPending{Version: 1, Owner: hex.EncodeToString(nonce[:]), Binding: folderOwner(f.binding(loaded.native)), SourceCatalog: loaded.catalog.tip.ID, Catalog: prepared.ID,
		Expected: freezeNative(loaded.native.Expected()), Candidate: candidate.ID, Applied: loaded.applied.ID, PreserveCheckoutEdits: f.options.PreserveCheckoutEdits}
	encoded, _ = json.Marshal(pending)
	descriptor, err := store.PutContext(ctx, "chunk", encoded)
	if err != nil {
		return err
	}
	updates := []syrnck.RefUpdate{{Name: f.prefix + "/pending/state", Value: &descriptor}, {Name: f.prefix + "/pending/native", Value: &candidate}, {Name: f.prefix + "/pending/catalog", Value: &prepared},
		{Name: catalogRef(f.options.Catalog.LocalDevice.String(), f.options.Catalog.Folder), Expected: &loaded.catalog.tip, Value: &loaded.catalog.tip},
		{Name: f.prefix + "/applied", Expected: loaded.applied, Value: loaded.applied}, {Name: loaded.native.Reference, Expected: loaded.native.Revision, Value: loaded.native.Revision}}
	applied, err := store.CompareRefsContext(ctx, updates)
	if err != nil {
		return err
	}
	if !applied {
		return ErrFolderChanged
	}
	return nil
}

// Step captures local changes, fetches one missing peer file, then applies a
// complete verified candidate through the native owned journal. No network wait
// holds a Store. Concurrent incoming batches can advance while data downloads;
// their generation, vector and exact descriptors are rechecked before attachment.
func (f *Folder) Step(ctx context.Context, requestFor func(protocol.DeviceID) RequestFunc) (status FolderStatus, err error) {
	defer func() {
		if err != nil {
			status.State = "blocked"
		}
	}()
	f.stepping.Lock()
	defer f.stepping.Unlock()
	status.State = "idle"
	var download *folderDownload
	f.mu.Lock()
	if f.closed {
		f.mu.Unlock()
		return status, errors.New("syrnck BEP: folder closed")
	}
	err = f.withStore(ctx, func(store *syrnck.Store) error {
		loaded, e := f.load(ctx, store)
		if e != nil {
			return e
		}
		if loaded.pending != nil {
			if e = f.recoverPending(ctx, store, loaded); e != nil {
				return e
			}
			loaded, e = f.load(ctx, store)
			if e != nil {
				return e
			}
			status.State = "recovered"
		}
		status.LocalChanges, e = f.observeLocal(ctx, store, loaded)
		if e != nil {
			return e
		}
		loaded, e = f.load(ctx, store)
		if e != nil {
			return e
		}
		download, _, e = f.selectIncoming(ctx, store, loaded)
		return e
	})
	f.mu.Unlock()
	if err != nil {
		return status, err
	}
	if download != nil {
		if requestFor == nil {
			return status, errors.New("syrnck BEP: no peer requester")
		}
		request := requestFor(download.Device)
		if request == nil {
			return status, errors.New("syrnck BEP: peer offline")
		}
		options := DownloadOptions{}
		if download.Reuse != nil {
			options.ReuseIndex = &download.Reuse.Info
			options.ReadReuse = func(ctx context.Context, req *protocol.Request) ([]byte, error) {
				return f.readEntry(ctx, *download.Reuse, req)
			}
		}
		staged, e := DownloadFile(ctx, f.options.Catalog.Folder, download.Info, request, options)
		if e != nil {
			return status, e
		}
		defer staged.Close()
		status.Transfer = staged.Stats()
		f.mu.Lock()
		err = f.withStore(ctx, func(store *syrnck.Store) error {
			loaded, e := f.load(ctx, store)
			if e != nil {
				return e
			}
			if loaded.pending != nil {
				return ErrFolderPending
			}
			generation, _, entries, e := loaded.catalog.PeerIndex(download.Device)
			if e != nil {
				return e
			}
			i, found := findCatalogEntry(entries, download.Info.Name)
			if generation != download.IndexID || !found || !sameInfo(entries[i].Info, download.Info) {
				return ErrFolderChanged
			}
			result, e := staged.PrepareWithChunking(ctx, store, syrnck.Chunking{})
			if e != nil {
				return e
			}
			if e = loaded.catalog.Attach(ctx, store, download.Device, download.Info.Name, result.Manifest); e != nil {
				return e
			}
			return loaded.catalog.Commit(ctx, store)
		})
		f.mu.Unlock()
		if err != nil {
			return status, err
		}
		status.State = "downloaded"
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	err = f.withStore(ctx, func(store *syrnck.Store) error {
		loaded, e := f.load(ctx, store)
		if e != nil {
			return e
		}
		changes, e := f.observeLocal(ctx, store, loaded)
		if e != nil {
			return e
		}
		status.LocalChanges += changes
		loaded, e = f.load(ctx, store)
		if e != nil {
			return e
		}
		missing, draft, e := f.selectIncoming(ctx, store, loaded)
		if e != nil {
			return e
		}
		if missing == nil {
			if e = f.prepareApplication(ctx, store, loaded, draft); e != nil {
				return e
			}
			loaded, e = f.load(ctx, store)
			if e != nil {
				return e
			}
			if loaded.pending != nil {
				if e = f.recoverPending(ctx, store, loaded); e != nil {
					return e
				}
				loaded, e = f.load(ctx, store)
				if e != nil {
					return e
				}
			}
			postChanges, e := f.observeLocal(ctx, store, loaded)
			if e != nil {
				return e
			}
			status.LocalChanges += postChanges
		}
		loaded, e = f.load(ctx, store)
		if e != nil {
			return e
		}
		status.Catalog = loaded.catalog.tip.ID
		if loaded.native.Revision != nil {
			status.Revision = loaded.native.Revision.ID
		}
		if changes > 0 || status.LocalChanges > 0 {
			status.State = "changed"
		}
		return nil
	})
	if err != nil {
		status.State = "blocked"
		return status, fmt.Errorf("syrnck BEP folder: %w", err)
	}
	return
}
