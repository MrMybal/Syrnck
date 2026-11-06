package syncthing

import (
	"bytes"
	"context"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
)

// Synthetic warm-store comparisons, not a real Unreal corpus or a measure of
// isolated cgo/syscall overhead. Both paths verify native object integrity.
func BenchmarkVerifiedRead(b *testing.B) {
	store := openStore(b, false)
	data := bytes.Join([][]byte{block(1), block(2), block(3), block(4)}, nil)
	manifest := ingest(b, store, data, true)
	f := file(b, store, manifest)
	req := &protocol.Request{Folder: "project", Name: f.info.Name, Size: protocol.MinBlockSize, Hash: f.info.Blocks[0].Hash}
	b.Run("native_range", func(b *testing.B) {
		b.SetBytes(protocol.MinBlockSize)
		b.ReportAllocs()
		for b.Loop() {
			if _, err := store.ReadRange(manifest, 0, protocol.MinBlockSize); err != nil {
				b.Fatal(err)
			}
		}
	})
	b.Run("bep_range_and_sha256", func(b *testing.B) {
		b.SetBytes(protocol.MinBlockSize)
		b.ReportAllocs()
		for b.Loop() {
			if _, err := f.ReadRequest(context.Background(), req); err != nil {
				b.Fatal(err)
			}
		}
	})
}

func BenchmarkDifferentialFetch(b *testing.B) {
	source, destination := openStore(b, false), openStore(b, false)
	before := bytes.Join([][]byte{block(1), block(2), block(3), block(4)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(5), block(4)}, nil)
	reuse := ingest(b, destination, before, true)
	incoming := file(b, source, ingest(b, source, after, false))
	var last FetchStats
	b.ReportAllocs()
	b.SetBytes(int64(len(after)))
	for b.Loop() {
		result, err := Fetch(context.Background(), destination, "project", incoming.Info(), incoming.ReadRequest, FetchOptions{ReuseManifest: reuse})
		if err != nil {
			b.Fatal(err)
		}
		last = result.Stats
	}
	b.ReportMetric(float64(last.TransferredBytes), "network_payload_B/op")
	b.ReportMetric(float64(last.ReusedBytes), "reused_B/op")
}
