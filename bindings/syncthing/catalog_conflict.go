package syncthing

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"path"
	"strings"
	"unicode/utf8"

	"github.com/syncthing/syncthing/lib/protocol"
	"golang.org/x/text/cases"
	"golang.org/x/text/unicode/norm"
	syrnck "syrnck.local/bindings/go"
)

// CatalogConflictResolution describes a draft change, never a filesystem
// receipt. Commit or the native Folder transaction must publish it first.
type CatalogConflictResolution struct {
	Name      string `json:"name"`
	Winner    string `json:"winner"` // local or peer; snapshot origin, not authorship
	Copy      string `json:"copy,omitempty"`
	Coalesced bool   `json:"coalesced"`
}

func conflictCopyName(name string, version protocol.Vector, modifiedBy protocol.ShortID, content string, attempt int) string {
	identity, _ := json.Marshal(struct {
		Name, Content string
		Version       protocol.Vector
		Attempt       int
	}{name, content, version, attempt})
	digest := sha256.Sum256(identity)
	leaf := path.Base(name)
	extension := path.Ext(leaf)
	if len(norm.NFD.String(extension)) > 24 {
		extension = ""
	}
	base := strings.TrimSuffix(leaf, extension)
	suffix := ".sync-conflict-" + modifiedBy.String() + "-" + hex.EncodeToString(digest[:16]) + extension
	for len(base) > 0 && len(norm.NFD.String(base))+len(norm.NFD.String(suffix)) > 240 {
		_, size := utf8.DecodeLastRuneInString(base)
		base = base[:len(base)-size]
	}
	if base == "" {
		base = "file"
	}
	return path.Join(path.Dir(name), base+suffix)
}

func conflictNameOccupied(files []CatalogEntry, name string) bool {
	fold := cases.Fold()
	wanted := fold.String(name)
	for _, entry := range files {
		// Reserve tombstones as well: a new copy must not resurrect an unrelated
		// historical path or collide with its case/normalization spelling.
		existing := fold.String(protocolName(entry.Info.Name))
		if existing == wanted || strings.HasPrefix(existing, wanted+"/") || strings.HasPrefix(wanted, existing+"/") {
			return true
		}
	}
	return false
}

// ResolveConcurrent explicitly merges two verified regular-file versions using
// the upstream Syncthing conflict ordering. Different losing bytes become an
// additional stable file; equal content coalesces without another copy. Both
// inputs remain in catalog history. Default Adopt still refuses conflicts.
// No working files or retained references change until the draft is published.
func (c *Catalog) ResolveConcurrent(ctx context.Context, store *syrnck.Store, device protocol.DeviceID, name string) (CatalogConflictResolution, error) {
	result := CatalogConflictResolution{Name: protocolName(name)}
	if ctx == nil || store == nil {
		return result, errors.New("syrnck BEP: conflict context and Store required")
	}
	if err := ctx.Err(); err != nil {
		return result, err
	}
	state := cloneCatalogState(c.state)
	localIndex, found := findCatalogEntry(state.Files, result.Name)
	if !found {
		return result, errors.New("syrnck BEP: local conflict version missing")
	}
	local := state.Files[localIndex]
	var incoming CatalogEntry
	paired := false
	for _, peer := range state.Peers {
		if peer.Device != device.String() {
			continue
		}
		if index, found := findCatalogEntry(peer.Files, result.Name); found {
			incoming = peer.Files[index]
			paired = true
		}
		break
	}
	if !paired {
		return result, errors.New("syrnck BEP: paired conflict version missing")
	}
	order := local.Info.Version.Compare(incoming.Info.Version)
	if order != protocol.ConcurrentLesser && order != protocol.ConcurrentGreater {
		return result, errors.New("syrnck BEP: concurrent versions required")
	}
	if local.Info.Type != protocol.FileInfoTypeFile || incoming.Info.Type != protocol.FileInfoTypeFile {
		return result, errors.New("syrnck BEP: conflict resolution requires regular-file versions")
	}
	hashes := make([]string, 2)
	for i, entry := range []CatalogEntry{local, incoming} {
		if entry.Info.Deleted {
			continue
		}
		if err := verifyCatalogData(ctx, store, state.Folder, entry.Info, entry.Manifest); err != nil {
			return result, err
		}
		hash, err := manifestHash(store, entry.Manifest)
		if err != nil {
			return result, err
		}
		hashes[i] = hash
	}
	winner, loser := local, incoming
	loserHash := hashes[1]
	result.Winner = "local"
	if incoming.Info.WinsConflict(local.Info) {
		winner, loser = incoming, local
		loserHash = hashes[0]
		result.Winner = "peer"
	}
	merged := protocol.Vector{Counters: append([]protocol.Counter(nil), local.Info.Version.Counters...)}.Merge(incoming.Info.Version)
	if len(merged.Counters) > 256 {
		return result, errors.New("syrnck BEP: merged conflict vector exceeds limit")
	}
	winner.Info.Version = merged
	result.Coalesced = local.Info.Deleted && incoming.Info.Deleted || !local.Info.Deleted && !incoming.Info.Deleted && hashes[0] == hashes[1]
	var copy *CatalogEntry
	if !result.Coalesced && !loser.Info.Deleted {
		preserved := loser
		for attempt := 0; ; attempt++ {
			if attempt == 128 {
				return result, errors.New("syrnck BEP: conflict copy namespace exhausted")
			}
			candidate := conflictCopyName(result.Name, loser.Info.Version, loser.Info.ModifiedBy, loserHash, attempt)
			if !nameValid(candidate) {
				return result, errors.New("syrnck BEP: conflict copy path exceeds limit")
			}
			if !conflictNameOccupied(state.Files, candidate) {
				preserved.Info.Name = candidate
				break
			}
		}
		copy = &preserved
		result.Copy = preserved.Info.Name
	}
	count := int64(1)
	if copy != nil {
		count++
	}
	if state.Sequence > math.MaxInt64-count {
		return result, errors.New("syrnck BEP: local sequence overflow")
	}
	state.Sequence++
	winner.Info.Sequence = state.Sequence
	state.Files = setCatalogEntry(state.Files, winner)
	if copy != nil {
		state.Sequence++
		copy.Info.Sequence = state.Sequence
		state.Files = setCatalogEntry(state.Files, *copy)
	}
	if err := ctx.Err(); err != nil {
		return result, err
	}
	return result, c.replace(state)
}
