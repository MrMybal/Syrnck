package syncthing

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

const MaxCatalogFiles = 65536

var (
	ErrCatalogStale    = errors.New("syrnck BEP: catalog changed; reload and replan")
	ErrCatalogConflict = errors.New("syrnck BEP: concurrent versions require explicit resolution")
)

// CatalogOptions binds durable state to one folder and the full local and
// explicitly paired device identities. It is not a discovery or pairing API.
type CatalogOptions struct {
	Folder      string
	LocalDevice protocol.DeviceID
	Peers       []protocol.DeviceID
}

// CatalogEntry retains immutable data when Manifest is nonempty. A remote file
// may be announced before its data is downloaded. Directories, symlinks and
// tombstones have no manifest. No working path is interpreted by this catalog.
type CatalogEntry struct {
	Info         protocol.FileInfo `json:"info"`
	Manifest     string            `json:"manifest"`
	CipherBlocks []CipherBlock     `json:"cipher_blocks,omitempty"`
	Origin       string            `json:"origin,omitempty"`
}

type catalogPeer struct {
	Device   string         `json:"device"`
	IndexID  uint64         `json:"index_id"`
	Sequence int64          `json:"sequence"`
	Files    []CatalogEntry `json:"files"`
}

type catalogState struct {
	Version       int            `json:"version"`
	Folder        string         `json:"folder"`
	Device        string         `json:"device"`
	IndexID       uint64         `json:"index_id"`
	Sequence      int64          `json:"sequence"`
	Files         []CatalogEntry `json:"files"`
	Peers         []catalogPeer  `json:"peers"`
	PasswordToken []byte         `json:"password_token,omitempty"`
}

// Catalog is a caller-serialized draft. It holds no native handle or network
// connection. Open it with a short-lived Store, edit a draft, Commit before
// releasing any prepared unpinned content, then close the Store before waiting
// for network IO. Catalog copies must not be mutated concurrently.
type Catalog struct {
	state    catalogState
	tip      syrnck.ObjectKey
	original []byte
}

func catalogRef(device, folder string) string {
	hash := sha256.Sum256([]byte(device + "\x00" + folder))
	return "ffi/bep/" + hex.EncodeToString(hash[:])
}

func catalogPath(device, name string) string {
	hash := sha256.Sum256([]byte(device + "\x00" + name))
	return "data/" + hex.EncodeToString(hash[:])
}

func validManifest(id string) bool {
	if len(id) != 64 || strings.ToLower(id) != id {
		return false
	}
	_, err := hex.DecodeString(id)
	return err == nil
}

func strictCatalogJSON(data []byte, value any) error {
	if len(data) > syrnck.MaxObjectSize {
		return errors.New("syrnck BEP: catalog exceeds 16 MiB")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return err
	}
	canonical, err := json.Marshal(value)
	if err != nil {
		return err
	}
	// Besides a stable codec, this rejects duplicate keys, null/empty aliases,
	// trailing documents and unknown fields nested in protocol metadata.
	if !bytes.Equal(data, canonical) {
		return errors.New("syrnck BEP: noncanonical catalog document")
	}
	return nil
}

func catalogConfiguration(options CatalogOptions) (catalogState, error) {
	state := catalogState{Version: 1, Folder: options.Folder, Device: options.LocalDevice.String(),
		Files: []CatalogEntry{}, Peers: []catalogPeer{}}
	if !folderValid(options.Folder) || !pairedDevice(options.LocalDevice) || len(options.Peers) > 256 {
		return state, errors.New("syrnck BEP: invalid catalog identity or peer limit")
	}
	shorts := map[protocol.ShortID]bool{options.LocalDevice.Short(): true}
	for _, device := range options.Peers {
		if !pairedDevice(device) || shorts[device.Short()] {
			return state, errors.New("syrnck BEP: duplicate, local or short-ID-colliding peer")
		}
		shorts[device.Short()] = true
		state.Peers = append(state.Peers, catalogPeer{Device: device.String(), Files: []CatalogEntry{}})
	}
	sort.Slice(state.Peers, func(i, j int) bool { return state.Peers[i].Device < state.Peers[j].Device })
	return state, nil
}

type catalogRevisionFile struct {
	Manifest   string `json:"manifest"`
	Executable bool   `json:"executable"`
}

type catalogRevision struct {
	Version   int                            `json:"version"`
	Parents   []string                       `json:"parents"`
	Message   string                         `json:"message"`
	Timestamp uint64                         `json:"timestamp"`
	Files     map[string]catalogRevisionFile `json:"files"`
}

// OpenCatalog loads a verified immutable state snapshot or creates an unpublished
// empty draft. Device membership must exactly match the caller's trusted config.
// A fresh nonzero local IndexID is published only by Commit.
func OpenCatalog(ctx context.Context, store *syrnck.Store, options CatalogOptions) (*Catalog, error) {
	return openCatalog(ctx, store, options, false)
}

// OpenEncryptedCatalog opens the distinct ciphertext-only BEP catalog. It has no
// folder password, local scanner or workspace. Never pass plaintext FileInfo.
func OpenEncryptedCatalog(ctx context.Context, store *syrnck.Store, options CatalogOptions) (*Catalog, error) {
	return openCatalog(ctx, store, options, true)
}
func openCatalog(ctx context.Context, store *syrnck.Store, options CatalogOptions, encrypted bool) (*Catalog, error) {
	configuration, err := catalogConfiguration(options)
	if encrypted {
		configuration.Version = 2
	}
	if err != nil {
		return nil, err
	}
	if store == nil {
		return nil, errors.New("syrnck BEP: catalog Store required")
	}
	refs, err := store.RefsContext(ctx)
	if err != nil {
		return nil, err
	}
	tip, found := refs[catalogStateRef(configuration)]
	if !found {
		var random [8]byte
		for configuration.IndexID == 0 {
			if _, err := rand.Read(random[:]); err != nil {
				return nil, err
			}
			configuration.IndexID = binary.BigEndian.Uint64(random[:])
		}
		return &Catalog{state: configuration}, nil
	}
	if tip.Kind != "revision" {
		return nil, errors.New("syrnck BEP: catalog root is not a revision")
	}
	data, err := store.Get(tip)
	if err != nil {
		return nil, err
	}
	var revision catalogRevision
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&revision); err != nil {
		return nil, err
	}
	if revision.Version != 1 || revision.Message != catalogMessage(configuration.Version) || revision.Timestamp != 0 || len(revision.Parents) > 1 {
		return nil, errors.New("syrnck BEP: invalid catalog revision")
	}
	stateFile, found := revision.Files["catalog.json"]
	if !found || stateFile.Executable {
		return nil, errors.New("syrnck BEP: catalog state missing")
	}
	manifest, err := store.Get(syrnck.ObjectKey{Kind: "manifest", ID: stateFile.Manifest})
	if err != nil {
		return nil, err
	}
	var metadata struct {
		Size uint64 `json:"size"`
	}
	if err := json.Unmarshal(manifest, &metadata); err != nil {
		return nil, err
	}
	if metadata.Size > syrnck.MaxObjectSize {
		return nil, errors.New("syrnck BEP: catalog exceeds 16 MiB")
	}
	if err := store.VerifyFile(stateFile.Manifest); err != nil {
		return nil, err
	}
	data, err = store.ReadRange(stateFile.Manifest, 0, metadata.Size)
	if err != nil {
		return nil, err
	}
	var state catalogState
	if err := strictCatalogJSON(data, &state); err != nil {
		return nil, err
	}
	if state.Version != configuration.Version || state.Device != configuration.Device || state.Folder != configuration.Folder || len(state.Peers) != len(configuration.Peers) {
		return nil, errors.New("syrnck BEP: catalog identity or membership differs")
	}
	for i := range state.Peers {
		if state.Peers[i].Device != configuration.Peers[i].Device {
			return nil, errors.New("syrnck BEP: catalog peer membership differs")
		}
	}
	if err := validateCatalogState(state); err != nil {
		return nil, err
	}
	expected := catalogDataFiles(state)
	expected["catalog.json"] = stateFile
	if len(expected) != len(revision.Files) {
		return nil, errors.New("syrnck BEP: catalog retained-data table differs")
	}
	for name, file := range expected {
		if revision.Files[name] != file {
			return nil, errors.New("syrnck BEP: catalog retained-data binding differs")
		}
	}
	return &Catalog{state: state, tip: tip, original: bytes.Clone(data)}, nil
}

func catalogDataFiles(state catalogState) map[string]catalogRevisionFile {
	files := make(map[string]catalogRevisionFile)
	add := func(device string, entries []CatalogEntry) {
		for _, entry := range entries {
			if entry.Manifest != "" {
				files[catalogPath(device, entry.Info.Name)] = catalogRevisionFile{Manifest: entry.Manifest}
			}
			for number, block := range entry.CipherBlocks {
				for part, manifest := range block.Parts {
					files[catalogPath(device, fmt.Sprintf("%s/cipher/%d/%d", entry.Info.Name, number, part))] = catalogRevisionFile{Manifest: manifest}
				}
			}
		}
	}
	add(state.Device, state.Files)
	for _, peer := range state.Peers {
		add(peer.Device, peer.Files)
	}
	return files
}

func validateCatalogInfo(folder string, info protocol.FileInfo) error {
	if !nameValid(info.Name) || info.Size < 0 || info.Sequence <= 0 || info.ModifiedNs < 0 || info.ModifiedNs >= 1e9 ||
		info.LocalFlags != 0 || info.New || info.EncryptionTrailerSize != 0 || len(info.Encrypted) != 0 ||
		len(info.Version.Counters) == 0 || len(info.Version.Counters) > 256 || info.IsInvalid() {
		return errors.New("syrnck BEP: invalid catalog file metadata")
	}
	for i, counter := range info.Version.Counters {
		if counter.Value == 0 || i > 0 && counter.ID <= info.Version.Counters[i-1].ID {
			return errors.New("syrnck BEP: invalid, duplicate or unsorted version vector")
		}
	}
	if info.Deleted {
		if info.Type < protocol.FileInfoTypeFile || info.Type > protocol.FileInfoTypeSymlink || len(info.Blocks) != 0 || len(info.BlocksHash) != 0 || len(info.SymlinkTarget) != 0 {
			return errors.New("syrnck BEP: invalid tombstone")
		}
		return nil
	}
	switch info.Type {
	case protocol.FileInfoTypeFile:
		return validateIndex(folder, info)
	case protocol.FileInfoTypeDirectory:
		if info.Size == 0 && len(info.Blocks) == 0 && len(info.BlocksHash) == 0 && len(info.SymlinkTarget) == 0 {
			return nil
		}
	case protocol.FileInfoTypeSymlinkFile, protocol.FileInfoTypeSymlinkDirectory, protocol.FileInfoTypeSymlink:
		if info.Size == 0 && len(info.Blocks) == 0 && len(info.BlocksHash) == 0 && len(info.SymlinkTarget) > 0 && len(info.SymlinkTarget) <= 4096 && bytes.IndexByte(info.SymlinkTarget, 0) < 0 {
			return nil
		}
	}
	return errors.New("syrnck BEP: invalid catalog file type or payload")
}

func validateCatalogState(state catalogState) error {
	if (state.Version != 1 && state.Version != 2) || state.IndexID == 0 || state.Sequence < 0 || state.Files == nil || state.Peers == nil {
		return errors.New("syrnck BEP: invalid catalog state")
	}
	if state.Version == 1 && len(state.PasswordToken) != 0 || state.Version == 2 && len(state.PasswordToken) != 0 && len(state.PasswordToken) != len(state.Folder)+25 {
		return errors.New("syrnck BEP: invalid encrypted folder token")
	}
	if state.Version == 2 && len(state.PasswordToken) == 0 {
		for _, peer := range state.Peers {
			if len(peer.Files) != 0 {
				return errors.New("syrnck BEP: encrypted folder token missing")
			}
		}
		if len(state.Files) != 0 {
			return errors.New("syrnck BEP: encrypted folder token missing")
		}
	}
	count := 0
	check := func(entries []CatalogEntry, sequence int64, local bool) error {
		if entries == nil || sequence < 0 {
			return errors.New("syrnck BEP: invalid catalog index")
		}
		sequences := make(map[int64]bool)
		for i, entry := range entries {
			count++
			if count > MaxCatalogFiles || i > 0 && entries[i-1].Info.Name >= entry.Info.Name || entry.Info.Sequence > sequence || sequences[entry.Info.Sequence] {
				return errors.New("syrnck BEP: duplicate catalog name/sequence or size limit")
			}
			sequences[entry.Info.Sequence] = true
			if err := validateCatalogEntry(state, entry, local); err != nil {
				return err
			}

		}
		return nil
	}
	if err := check(state.Files, state.Sequence, true); err != nil {
		return err
	}
	for _, peer := range state.Peers {
		if peer.IndexID == 0 && (peer.Sequence != 0 || len(peer.Files) != 0) {
			return errors.New("syrnck BEP: peer index generation missing")
		}
		if err := check(peer.Files, peer.Sequence, false); err != nil {
			return err
		}
	}
	return nil
}

func cloneCatalogState(state catalogState) catalogState {
	cloneEntries := func(input []CatalogEntry) []CatalogEntry {
		if input == nil {
			return nil
		}
		output := make([]CatalogEntry, len(input))
		for i, entry := range input {
			output[i] = copyCatalogEntry(entry)
		}
		return output
	}
	state.PasswordToken = bytes.Clone(state.PasswordToken)
	state.Files = cloneEntries(state.Files)
	peers := make([]catalogPeer, len(state.Peers))
	for i, peer := range state.Peers {
		peer.Files = cloneEntries(peer.Files)
		peers[i] = peer
	}
	state.Peers = peers
	return state
}

// LocalIndex returns an independent snapshot, ordered by local sequence for BEP
// transmission. A stored peer index is never implicitly promoted to local data.
func (c *Catalog) LocalIndex() (uint64, int64, []CatalogEntry) {
	state := cloneCatalogState(c.state)
	sort.Slice(state.Files, func(i, j int) bool { return state.Files[i].Info.Sequence < state.Files[j].Info.Sequence })
	return state.IndexID, state.Sequence, state.Files
}

func (c *Catalog) PeerIndex(device protocol.DeviceID) (uint64, int64, []CatalogEntry, error) {
	state := cloneCatalogState(c.state)
	for _, peer := range state.Peers {
		if peer.Device == device.String() {
			return peer.IndexID, peer.Sequence, peer.Files, nil
		}
	}
	return 0, 0, nil, errors.New("syrnck BEP: unpaired catalog peer")
}

func (c *Catalog) Revision() syrnck.ObjectKey { return c.tip }

func (c *Catalog) replace(state catalogState) error {
	if err := validateCatalogState(state); err != nil {
		return err
	}
	data, err := json.Marshal(state)
	if err != nil {
		return err
	}
	if len(data) > syrnck.MaxObjectSize {
		return errors.New("syrnck BEP: catalog exceeds 16 MiB")
	}
	c.state = state
	return nil
}

func findCatalogEntry(entries []CatalogEntry, name string) (int, bool) {
	i := sort.Search(len(entries), func(i int) bool { return entries[i].Info.Name >= name })
	return i, i < len(entries) && entries[i].Info.Name == name
}

func setCatalogEntry(entries []CatalogEntry, entry CatalogEntry) []CatalogEntry {
	i, found := findCatalogEntry(entries, entry.Info.Name)
	if found {
		entries[i] = entry
		return entries
	}
	entries = append(entries, CatalogEntry{})
	copy(entries[i+1:], entries[i:])
	entries[i] = entry
	return entries
}

// RecordLocal records one explicit scanner change. Version/Sequence/ModifiedBy
// must be unset: the catalog derives them from its previous local version and
// checks overflow. A deletion is an explicit tombstone, never a missing path.
func (c *Catalog) RecordLocal(entry CatalogEntry) error {
	return c.RecordLocalChanges([]CatalogEntry{entry})
}

// RecordLocalChanges records a bounded scanner batch with one draft copy and
// one name sort, avoiding repeated insertion/copy work for a large folder scan.
// A bad entry leaves the entire draft unchanged. Input order assigns sequences.
func (c *Catalog) RecordLocalChanges(changes []CatalogEntry) error {
	if c.state.Version != 1 {
		return errors.New("syrnck BEP: encrypted receiver cannot create local metadata")
	}
	if len(changes) > MaxCatalogFiles {
		return errors.New("syrnck BEP: local batch exceeds limit")
	}
	if len(changes) == 0 {
		return nil
	}
	state := cloneCatalogState(c.state)
	local, _ := protocol.DeviceIDFromString(state.Device)
	if int64(len(changes)) > math.MaxInt64-state.Sequence {
		return errors.New("syrnck BEP: local sequence overflow")
	}
	entries := make(map[string]CatalogEntry, len(state.Files)+len(changes))
	for _, entry := range state.Files {
		entries[entry.Info.Name] = entry
	}
	seen := make(map[string]bool, len(changes))
	for _, entry := range changes {
		if entry.Info.Sequence != 0 || len(entry.Info.Version.Counters) != 0 || entry.Info.ModifiedBy != 0 {
			return errors.New("syrnck BEP: local causal fields must be unset")
		}
		entry.Info = copyInfo(entry.Info)
		entry.Info.Name = protocolName(entry.Info.Name)
		if seen[entry.Info.Name] {
			return errors.New("syrnck BEP: duplicate local batch name")
		}
		seen[entry.Info.Name] = true
		vector := entries[entry.Info.Name].Info.Version
		if vector.Counter(local.Short()) == math.MaxUint64 {
			return errors.New("syrnck BEP: local vector overflow")
		}
		entry.Info.Version = vector.Update(local.Short())
		state.Sequence++
		entry.Info.Sequence, entry.Info.ModifiedBy = state.Sequence, local.Short()
		entries[entry.Info.Name] = entry
	}
	state.Files = sortedCatalogEntries(entries)
	return c.replace(state)
}

func sortedCatalogEntries(entries map[string]CatalogEntry) []CatalogEntry {
	output := make([]CatalogEntry, 0, len(entries))
	for _, entry := range entries {
		output = append(output, entry)
	}
	sort.Slice(output, func(i, j int) bool { return output[i].Info.Name < output[j].Info.Name })
	return output
}

// CatalogIndex is a sequence-bounded full Index (Reset) or IndexUpdate. A new
// peer IndexID requires Reset. Chunks of a full index after the first are deltas.
type CatalogIndex struct {
	Device                     protocol.DeviceID
	IndexID                    uint64
	Reset                      bool
	PrevSequence, LastSequence int64
	Files                      []protocol.FileInfo
}

// ReceiveIndex records an authorized peer announcement. It neither downloads
// bytes nor changes the local index. Concurrent and deleted versions remain in
// durable peer state, while historical roots retain their earlier file data.
func (c *Catalog) ReceiveIndex(batch CatalogIndex) error {
	if batch.IndexID == 0 || batch.PrevSequence < 0 || batch.LastSequence < batch.PrevSequence || len(batch.Files) > MaxCatalogFiles {
		return errors.New("syrnck BEP: invalid index generation or bounds")
	}
	state := cloneCatalogState(c.state)
	peerIndex := -1
	for i := range state.Peers {
		if state.Peers[i].Device == batch.Device.String() {
			peerIndex = i
			break
		}
	}
	if peerIndex < 0 {
		return errors.New("syrnck BEP: unpaired catalog peer")
	}
	peer := &state.Peers[peerIndex]
	if batch.Reset {
		if batch.PrevSequence != 0 {
			return errors.New("syrnck BEP: full index must start at zero")
		}
	} else if peer.IndexID != batch.IndexID || batch.PrevSequence != peer.Sequence {
		return errors.New("syrnck BEP: delta index generation or sequence gap")
	}
	old := c.state.Peers[peerIndex].Files
	entries := make(map[string]CatalogEntry, len(peer.Files)+len(batch.Files))
	if !batch.Reset {
		for _, entry := range peer.Files {
			entries[entry.Info.Name] = entry
		}
	}
	seen := make(map[string]bool)
	previous := batch.PrevSequence
	for _, incoming := range batch.Files {
		incoming.Name = protocolName(incoming.Name)
		if incoming.Sequence <= previous || incoming.Sequence > batch.LastSequence || seen[incoming.Name] {
			return errors.New("syrnck BEP: duplicate or unordered index entry")
		}
		previous = incoming.Sequence
		seen[incoming.Name] = true
		if err := validateCatalogInfoMode(state, incoming); err != nil {
			return err
		}
		incoming = copyInfo(incoming)
		entry := CatalogEntry{Info: incoming}
		if state.Version == 2 && incoming.Type == protocol.FileInfoTypeFile && !incoming.Deleted {
			entry.CipherBlocks = make([]CipherBlock, len(incoming.Blocks))
		}
		if i, found := findCatalogEntry(old, incoming.Name); found {
			prior := old[i]
			if state.Version == 2 {
				reuseCipherBlocks(&entry, prior)
				// Metadata uses fresh nonces on every announcement. Preserve an
				// equivalent retained envelope instead of manufacturing new history.
				if sameEncryptedPublicInfo(prior.Info, incoming) {
					entry.Info.Encrypted = bytes.Clone(prior.Info.Encrypted)
				}
			} else if prior.Info.Version.Equal(incoming.Version) {
				before, after := prior.Info, incoming
				before.Sequence, after.Sequence = 0, 0
				a, _ := json.Marshal(before)
				b, _ := json.Marshal(after)
				if !bytes.Equal(a, b) {
					return errors.New("syrnck BEP: equal vector describes different file metadata")
				}
				entry.Manifest = prior.Manifest
			}
		}
		entries[incoming.Name] = entry
	}
	if len(batch.Files) > 0 && previous != batch.LastSequence || len(batch.Files) == 0 && batch.LastSequence != batch.PrevSequence {
		return errors.New("syrnck BEP: index last sequence mismatch")
	}
	peer.IndexID, peer.Sequence = batch.IndexID, batch.LastSequence
	peer.Files = sortedCatalogEntries(entries)
	return c.replace(state)
}

// Attach binds downloaded verified data to the exact persisted peer version.
// The block index is compared against the complete immutable native manifest.
func (c *Catalog) Attach(ctx context.Context, store *syrnck.Store, device protocol.DeviceID, name, manifest string) error {
	state := cloneCatalogState(c.state)
	name = protocolName(name)
	for p := range state.Peers {
		if state.Peers[p].Device != device.String() {
			continue
		}
		i, found := findCatalogEntry(state.Peers[p].Files, name)
		if !found {
			return protocol.ErrNoSuchFile
		}
		entry := &state.Peers[p].Files[i]
		if err := verifyCatalogData(ctx, store, state.Folder, entry.Info, manifest); err != nil {
			return err
		}
		entry.Manifest = manifest
		return c.replace(state)
	}
	return errors.New("syrnck BEP: unpaired catalog peer")
}

func verifyCatalogData(ctx context.Context, store *syrnck.Store, folder string, info protocol.FileInfo, manifest string) error {
	if store == nil || info.Deleted || info.Type != protocol.FileInfoTypeFile || len(info.Encrypted) != 0 || !validManifest(manifest) {
		return errors.New("syrnck BEP: invalid file data binding")
	}
	view, err := NewFile(ctx, store, manifest, folder, info.Name, info.BlockSize())
	if err != nil {
		return err
	}
	if view.info.Size != info.Size || !bytes.Equal(view.info.BlocksHash, protocol.BlocksHash(info.Blocks)) {
		return errors.New("syrnck BEP: manifest does not match announced blocks")
	}
	return nil
}

// Adopt promotes an already verified peer version to the local advertised
// index, preserving its causal vector. Concurrent versions are reported without
// choosing a winner or deleting either side. Working-file application is separate.
func (c *Catalog) Adopt(device protocol.DeviceID, name string) error {
	if c.state.Version != 1 {
		return errors.New("syrnck BEP: use encrypted receiver reconciliation")
	}
	name = protocolName(name)
	for _, peer := range c.state.Peers {
		if peer.Device != device.String() {
			continue
		}
		i, found := findCatalogEntry(peer.Files, name)
		if !found {
			return protocol.ErrNoSuchFile
		}
		entry := peer.Files[i]
		if !entry.Info.Deleted && entry.Info.Type == protocol.FileInfoTypeFile && entry.Manifest == "" {
			return errors.New("syrnck BEP: peer file data has not been verified")
		}
		if j, found := findCatalogEntry(c.state.Files, name); found {
			existing := c.state.Files[j]
			switch entry.Info.Version.Compare(existing.Info.Version) {
			case protocol.Lesser:
				return errors.New("syrnck BEP: cannot adopt an older version")
			case protocol.ConcurrentLesser, protocol.ConcurrentGreater:
				return ErrCatalogConflict
			case protocol.Equal:
				a, b := entry.Info, existing.Info
				a.Sequence, b.Sequence = 0, 0
				x, _ := json.Marshal(a)
				y, _ := json.Marshal(b)
				// Native manifests may use different chunk boundaries for the
				// same verified BEP block stream. Their IDs are not file identities.
				if !bytes.Equal(x, y) {
					return errors.New("syrnck BEP: equal vector has unequal data")
				}
				return nil
			}
		}
		if c.state.Sequence == math.MaxInt64 {
			return errors.New("syrnck BEP: local sequence overflow")
		}
		// Most controller passes encounter already acknowledged entries. Clone
		// only when adopting a new version, not the full catalog per idle file.
		state := cloneCatalogState(c.state)
		entry.Info = copyInfo(entry.Info)
		state.Sequence++
		entry.Info.Sequence = state.Sequence
		state.Files = setCatalogEntry(state.Files, entry)
		return c.replace(state)
	}
	return errors.New("syrnck BEP: unpaired catalog peer")
}

// Commit verifies every retained file against its BEP blocks, then atomically
// publishes protocol state and its complete data closure under one guarded root.
// Previous catalog revisions are native parents: GC preserves old versions even
// after peer reset, rename or deletion. No implicit per-file ffi/ pins are used.
// An IO error is an unknown publication outcome: reload before any retry.
func (c *Catalog) Commit(ctx context.Context, store *syrnck.Store) error {
	return c.CommitWithRefs(ctx, store, nil)
}

// CommitWithRefs publishes the catalog and additional expected-value reference
// updates in one native CAS. This couples a service checkpoint to its exact
// catalog/data closure. The catalog reference itself cannot appear in updates.
// On any error reload every affected root to establish the publication outcome.
func (c *Catalog) CommitWithRefs(ctx context.Context, store *syrnck.Store, updates []syrnck.RefUpdate) error {
	name := catalogStateRef(c.state)
	for _, update := range updates {
		if update.Name == name {
			return errors.New("syrnck BEP: duplicate catalog publication root")
		}
	}
	key, data, err := c.prepare(ctx, store)
	if err != nil {
		return err
	}
	if bytes.Equal(data, c.original) && len(updates) == 0 {
		return nil
	}
	var expected *syrnck.ObjectKey
	if c.tip.ID != "" {
		old := c.tip
		expected = &old
	}
	group := make([]syrnck.RefUpdate, 0, len(updates)+1)
	group = append(group, syrnck.RefUpdate{Name: name, Expected: expected, Value: &key})
	group = append(group, updates...)
	applied, err := store.CompareRefsContext(ctx, group)
	if err != nil {
		return fmt.Errorf("syrnck BEP catalog publication (reload to establish outcome): %w", err)
	}
	if !applied {
		return ErrCatalogStale
	}
	c.tip, c.original = key, bytes.Clone(data)
	return nil
}

func (c *Catalog) prepare(ctx context.Context, store *syrnck.Store) (syrnck.ObjectKey, []byte, error) {
	if store == nil {
		return syrnck.ObjectKey{}, nil, errors.New("syrnck BEP: catalog Store required")
	}
	if err := validateCatalogState(c.state); err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	data, err := json.Marshal(c.state)
	if err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	if len(data) > syrnck.MaxObjectSize {
		return syrnck.ObjectKey{}, nil, errors.New("syrnck BEP: catalog exceeds 16 MiB")
	}
	refs, err := store.RefsContext(ctx)
	if err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	name := catalogStateRef(c.state)
	current, present := refs[name]
	if present != (c.tip.ID != "") || present && current != c.tip {
		return syrnck.ObjectKey{}, nil, ErrCatalogStale
	}
	if bytes.Equal(data, c.original) {
		return c.tip, data, nil
	}
	groups := [][]CatalogEntry{c.state.Files}
	for _, peer := range c.state.Peers {
		groups = append(groups, peer.Files)
	}
	for _, entries := range groups {
		for _, entry := range entries {
			if err := verifyCatalogEntryData(ctx, store, c.state, entry); err != nil {
				return syrnck.ObjectKey{}, nil, err
			}
		}
	}
	stateID, err := store.PrepareBytesContext(ctx, data, syrnck.Chunking{})
	if err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	revision := catalogRevision{Version: 1, Parents: []string{}, Message: catalogMessage(c.state.Version), Files: catalogDataFiles(c.state)}
	if c.tip.ID != "" {
		old := c.tip
		revision.Parents = append(revision.Parents, old.ID)
	}
	revision.Files["catalog.json"] = catalogRevisionFile{Manifest: stateID}
	encoded, err := json.Marshal(revision)
	if err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	key, err := store.PutContext(ctx, "revision", encoded)
	if err != nil {
		return syrnck.ObjectKey{}, nil, err
	}
	return key, data, nil
}
