package syncthing

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base32"
	"encoding/hex"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func opaqueTestInfo(label string, data [][]byte, clock uint64, sequence int64) protocol.FileInfo {
	digest := sha256.Sum256([]byte(label))
	encoded := base32.HexEncoding.WithPadding(base32.NoPadding).EncodeToString(digest[:])
	name := encoded[:1] + ".syncthing-enc/" + encoded[1:3] + "/" + encoded[3:]
	info := protocol.FileInfo{Name: name, Type: protocol.FileInfoTypeFile, Permissions: 0o644, ModifiedS: 1234567890,
		Version: protocol.Vector{Counters: []protocol.Counter{{ID: 1, Value: clock}}}, Sequence: sequence,
		Encrypted: bytes.Repeat([]byte{17}, 64), RawBlockSize: int32(protocol.MinBlockSize + 40)}
	for _, body := range data {
		hash := sha256.Sum256(body)
		token := append(bytes.Repeat([]byte{5}, 16), hash[:]...)
		info.Blocks = append(info.Blocks, protocol.BlockInfo{Offset: info.Size, Size: len(body), Hash: token})
		info.Size += int64(len(body))
	}
	return info
}
func opaqueTestReceiver(t *testing.T, encrypted bool, peers []protocol.DeviceID) (*EncryptedReceiver, func(context.Context) (*syrnck.Store, error)) {
	t.Helper()
	path := filepath.Join(tempDirectory(t), "store")
	var key []byte
	if encrypted {
		key = bytes.Repeat([]byte{93}, 32)
	}
	store, err := syrnck.Open(path, syrnck.Options{Create: true, Key: key})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	provider := func(ctx context.Context) (*syrnck.Store, error) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		return syrnck.Open(path, syrnck.Options{Key: key})
	}
	r, err := NewEncryptedReceiver(context.Background(), EncryptedReceiverOptions{Catalog: CatalogOptions{Folder: "project",
		LocalDevice: protocol.NewDeviceID([]byte("opaque receiver")), Peers: peers}, Storage: path, OpenStore: provider})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })
	if err := r.BindToken(context.Background(), bytes.Repeat([]byte{7}, 32)); err != nil {
		t.Fatal(err)
	}
	return r, provider
}
func opaqueTestGC(t *testing.T, r *EncryptedReceiver, encrypted bool) {
	t.Helper()
	cli := os.Getenv("SYRNCK_TEST_CLI")
	if cli == "" {
		t.Fatal("build native CLI for retention qualification")
	}
	command := exec.Command(cli, "--store", r.options.Storage, "gc", "--delete")
	for _, entry := range os.Environ() {
		name := strings.ToUpper(strings.SplitN(entry, "=", 2)[0])
		if !strings.HasPrefix(name, "SYRNCK_") {
			command.Env = append(command.Env, entry)
		}
	}
	if encrypted {
		command.Env = append(command.Env, "SYRNCK_KEY="+hex.EncodeToString(bytes.Repeat([]byte{93}, 32)))
	}
	hideReferenceProcess(command)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("receiver GC: %v %s", err, output)
	}
}
func TestEncryptedReceiverResumesBlocksAndRetainsHistory(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(map[bool]string{false: "plain-store", true: "encrypted-store"}[encrypted], func(t *testing.T) {
			ctx := context.Background()
			peer := protocol.NewDeviceID([]byte("opaque source"))
			r, provider := opaqueTestReceiver(t, encrypted, []protocol.DeviceID{peer})
			bodies := [][]byte{bytes.Repeat([]byte{10}, protocol.MinBlockSize+40), bytes.Repeat([]byte{11}, 1064)}
			info := opaqueTestInfo("private asset", bodies, 1, 1)
			if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peer, IndexID: 7, Reset: true, LastSequence: 1, Files: []protocol.FileInfo{info}}); err != nil {
				t.Fatal(err)
			}
			interrupt := errors.New("test network interrupted")
			_, err := r.Step(ctx, func(protocol.DeviceID) RequestFunc {
				return func(ctx context.Context, request *protocol.Request) ([]byte, error) {
					// Network waits hold neither the receiver mutex nor a Store lock.
					store, err := provider(ctx)
					if err != nil {
						t.Fatal("Store held during network", err)
					}
					store.Close()
					if request.BlockNo == 1 {
						return nil, interrupt
					}
					return bytes.Clone(bodies[0]), nil
				}
			})
			if !errors.Is(err, interrupt) {
				t.Fatal(err)
			}
			snapshot, err := r.Snapshot(ctx)
			if err != nil || len(snapshot.Files) != 0 {
				t.Fatal("partial file was advertised", err)
			}
			opaqueTestGC(t, r, encrypted)
			options := r.options
			r.Close()
			r, err = NewEncryptedReceiver(ctx, options)
			if err != nil {
				t.Fatal(err)
			}
			defer r.Close()
			var requested []int
			status, err := r.Step(ctx, func(protocol.DeviceID) RequestFunc {
				return func(_ context.Context, request *protocol.Request) ([]byte, error) {
					requested = append(requested, request.BlockNo)
					return bytes.Clone(bodies[request.BlockNo]), nil
				}
			})
			if err != nil || len(requested) != 1 || requested[0] != 1 || status.Transfer.StagedBytes != uint64(len(bodies[0])) {
				t.Fatal("resume retransferred retained block", status, requested, err)
			}
			first, err := r.Snapshot(ctx)
			if err != nil || len(first.Files) != 1 {
				t.Fatal(err)
			}
			if err := r.withStore(ctx, func(store *syrnck.Store) error {
				refs, err := store.Refs()
				if err != nil {
					return err
				}
				for name := range refs {
					if strings.HasPrefix(name, "ffi/bep-receipts/") {
						t.Fatal("completed receipt remained pinned")
					}
				}
				return nil
			}); err != nil {
				t.Fatal(err)
			}
			changed := [][]byte{bytes.Repeat([]byte{12}, protocol.MinBlockSize+40), bodies[1]}
			newInfo := opaqueTestInfo("private asset", changed, 2, 2)
			if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peer, IndexID: 7, PrevSequence: 1, LastSequence: 2, Files: []protocol.FileInfo{newInfo}}); err != nil {
				t.Fatal(err)
			}
			requested = nil
			status, err = r.Step(ctx, func(protocol.DeviceID) RequestFunc {
				return func(_ context.Context, request *protocol.Request) ([]byte, error) {
					requested = append(requested, request.BlockNo)
					return changed[request.BlockNo], nil
				}
			})
			if err != nil || len(requested) != 1 || requested[0] != 0 || status.Transfer.ReusedBytes != 1064 {
				t.Fatal("opaque differential reuse", status, requested, err)
			}
			newInfo.Deleted = true
			newInfo.Size = 0
			newInfo.Blocks = nil
			newInfo.Version.Counters[0].Value = 3
			newInfo.Sequence = 3
			if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peer, IndexID: 7, PrevSequence: 2, LastSequence: 3, Files: []protocol.FileInfo{newInfo}}); err != nil {
				t.Fatal(err)
			}
			deleted, err := r.Snapshot(ctx)
			if err != nil || !deleted.Files[0].Info.Deleted {
				t.Fatal("opaque tombstone was not retained", err)
			}
			opaqueTestGC(t, r, encrypted)
			for number, body := range bodies {
				data, err := r.ReadRequest(ctx, first.Files[0], &protocol.Request{Folder: "project", Name: info.Name,
					Offset: info.Blocks[number].Offset, Size: len(body), Hash: info.Blocks[number].Hash, BlockNo: number})
				if err != nil || !bytes.Equal(data, body) {
					t.Fatal("GC lost complete historical ciphertext", number, err)
				}
			}
			// A fresh nonce on an equivalent announcement must not discard reusable
			// ciphertext or grow catalog history forever on ordinary reconnection.
			before := deleted.Catalog
			newInfo.Encrypted = bytes.Repeat([]byte{18}, 64)
			if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peer, IndexID: 7, Reset: true, LastSequence: 3, Files: []protocol.FileInfo{newInfo}}); err != nil {
				t.Fatal(err)
			}
			after, err := r.Snapshot(ctx)
			if err != nil || before != after.Catalog {
				t.Fatal("equivalent encrypted announcement changed history", err)
			}
		})
	}
}
func TestEncryptedReceiverMaximumBlockAndPeerSelection(t *testing.T) {
	ctx := context.Background()
	peers := []protocol.DeviceID{protocol.NewDeviceID([]byte("first opaque peer")), protocol.NewDeviceID([]byte("second opaque peer"))}
	r, _ := opaqueTestReceiver(t, false, peers)
	for i, peer := range peers {
		body := bytes.Repeat([]byte{byte(i + 20)}, 1064)
		info := opaqueTestInfo("tie", [][]byte{body}, 1, 1)
		if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peer, IndexID: uint64(i + 7), Reset: true, LastSequence: 1, Files: []protocol.FileInfo{info}}); err != nil {
			t.Fatal(err)
		}
	}
	// A session for the second peer must progress even while another configured
	// peer is offline and first in the catalog's deterministic order.
	for _, chosen := range []protocol.DeviceID{peers[1], peers[0]} {
		_, err := r.Step(ctx, func(device protocol.DeviceID) RequestFunc {
			if device != chosen {
				return nil
			}
			return func(context.Context, *protocol.Request) ([]byte, error) {
				value := byte(20)
				if chosen == peers[1] {
					value = 21
				}
				return bytes.Repeat([]byte{value}, 1064), nil
			}
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	snapshot, err := r.Snapshot(ctx)
	if err != nil || len(snapshot.Files) != 1 {
		t.Fatal(err)
	}
	expected := peers[0].String()
	if peers[1].String() < expected {
		expected = peers[1].String()
	}
	if snapshot.Files[0].Origin != expected {
		t.Fatal("opaque tie break oscillated")
	}
	large := bytes.Repeat([]byte{31}, protocol.MaxBlockSize+40)
	info := opaqueTestInfo("maximum", [][]byte{large}, 2, 2)
	info.RawBlockSize = int32(protocol.MaxBlockSize + 40)
	if err := r.ReceiveIndex(ctx, CatalogIndex{Device: peers[0], IndexID: 7, PrevSequence: 1, LastSequence: 2, Files: []protocol.FileInfo{info}}); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Step(ctx, func(protocol.DeviceID) RequestFunc {
		return func(context.Context, *protocol.Request) ([]byte, error) { return large, nil }
	}); err != nil {
		t.Fatal(err)
	}
	snapshot, err = r.Snapshot(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range snapshot.Files {
		if entry.Info.Name == info.Name {
			if len(entry.CipherBlocks[0].Parts) != 2 {
				t.Fatal("maximum encrypted block was not split at native buffer bound")
			}
			data, err := r.ReadRequest(ctx, entry, &protocol.Request{Folder: "project", Name: info.Name, Size: len(large), Hash: info.Blocks[0].Hash})
			if err != nil || !bytes.Equal(data, large) {
				t.Fatal("maximum ciphertext block reconstruction", err)
			}
		}
	}
}
func TestEncryptedReceiverCloseCancelsNetworkWithoutHoldingStore(t *testing.T) {
	peer := protocol.NewDeviceID([]byte("cancellable opaque source"))
	r, _ := opaqueTestReceiver(t, false, []protocol.DeviceID{peer})
	info := opaqueTestInfo("cancel", [][]byte{bytes.Repeat([]byte{1}, 1064)}, 1, 1)
	if err := r.ReceiveIndex(context.Background(), CatalogIndex{Device: peer, IndexID: 7, Reset: true, LastSequence: 1, Files: []protocol.FileInfo{info}}); err != nil {
		t.Fatal(err)
	}
	entered, done := make(chan struct{}), make(chan error, 1)
	go func() {
		_, err := r.Step(context.Background(), func(protocol.DeviceID) RequestFunc {
			return func(ctx context.Context, _ *protocol.Request) ([]byte, error) {
				close(entered)
				<-ctx.Done()
				return nil, ctx.Err()
			}
		})
		done <- err
	}()
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("opaque request did not start")
	}
	if err := r.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("receiver did not join its network worker")
	}
}
