"""Compare loose and packed CLI reconstruction; verify IDs, bytes and file counts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--mib", type=int, default=8)
    parser.add_argument("--block-kib", type=int, default=4)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.mib <= 1024 or not 1 <= args.block_kib <= 16384 or not 1 <= args.repetitions <= 100:
        parser.error("Invalid corpus, block size or repetition count")
    exe = args.exe.resolve(strict=True)
    size = args.mib * 1024 * 1024
    source_bytes = hashlib.shake_256(b"syrnck-pack-benchmark-v1").digest(size)
    source_hash = hashlib.sha256(source_bytes).hexdigest()
    rows = []
    with tempfile.TemporaryDirectory(prefix="syrnck-pack-benchmark-") as temporary:
        directory = Path(temporary)
        source = directory / "source.bin"
        source.write_bytes(source_bytes)
        for encrypted in (False, True):
            env = {k: v for k, v in os.environ.items() if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN")}
            if encrypted:
                env["SYRNCK_KEY"] = "71" * 32  # Public benchmark-only key.
            stores, manifests, file_counts = {}, {}, {}

            def command(store, *arguments):
                return subprocess.run([str(exe), "--store", str(store), "--cache-mib", "0", *map(str, arguments)],
                                      capture_output=True, env=env, check=True).stdout

            for layout in ("loose", "packed"):
                store = directory / f"{layout}-{int(encrypted)}"
                stores[layout] = store
                command(store, "init", *(["--encrypted"] if encrypted else []))
                value = json.loads(command(store, "put", source, "--fixed-size", args.block_kib * 1024))
                manifests[layout] = value["manifest"]
                if layout == "packed":
                    started = time.perf_counter()
                    pack_result = json.loads(command(store, "pack"))
                    pack_seconds = time.perf_counter() - started
                    if pack_result["loose_objects_removed"] != pack_result["objects"]:
                        raise RuntimeError("Packing retained loose files unexpectedly")
                file_counts[layout] = sum(p.is_file() for base in (store / "objects", store / "packs")
                                          if base.exists() for p in base.rglob("*"))
                command(store, "fsck")
            if manifests["loose"] != manifests["packed"] or file_counts["packed"] != 2:
                raise RuntimeError("Object IDs or physical file count changed unexpectedly")
            # One untimed read per layout warms OS data/metadata; decoded cache is disabled.
            for layout in ("loose", "packed"):
                output = directory / "warmup.bin"
                command(stores[layout], "get", manifests[layout], output)
                if hashlib.sha256(output.read_bytes()).hexdigest() != source_hash:
                    raise RuntimeError("Warmup reconstruction differs")
                output.unlink()
            for repetition in range(args.repetitions):
                order = ("loose", "packed") if repetition % 2 == 0 else ("packed", "loose")
                for layout in order:
                    output = directory / "output.bin"
                    started = time.perf_counter()
                    command(stores[layout], "get", manifests[layout], output)
                    seconds = time.perf_counter() - started
                    if hashlib.sha256(output.read_bytes()).hexdigest() != source_hash:
                        raise RuntimeError("Reconstruction differs")
                    output.unlink()
                    rows.append({"layout": layout, "encrypted": encrypted, "repetition": repetition + 1,
                                 "seconds": seconds, "mib_per_second": args.mib / seconds,
                                 "manifest": manifests[layout], "object_files": file_counts[layout]})
            for row in rows:
                if row["encrypted"] == encrypted and row["layout"] == "packed":
                    row["pack_seconds"] = pack_seconds
                    row["pack_result"] = pack_result
    summary = []
    for encrypted in (False, True):
        item = {"encrypted": encrypted}
        for layout in ("loose", "packed"):
            matching = [r for r in rows if r["encrypted"] == encrypted and r["layout"] == layout]
            item[layout] = {"median_mib_per_second": statistics.median(r["mib_per_second"] for r in matching),
                            "object_files": matching[0]["object_files"]}
        item["speedup"] = item["packed"]["median_mib_per_second"] / item["loose"]["median_mib_per_second"]
        summary.append(item)
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "mib": args.mib, "block_kib": args.block_kib, "repetitions": args.repetitions,
              "decoded_cache_enabled": False, "os_cache_warmed": True,
              "cli_startup_index_loading_output_flush_included": True,
              "matching_object_ids": True, "byte_exact_restore": True,
              "network_measured": False, "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
