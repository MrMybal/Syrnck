"""Measure verified incremental pack maintenance versus full compaction."""
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
    parser.add_argument("--mib", type=int, default=32)
    parser.add_argument("--block-kib", type=int, default=64)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.mib <= 1024 or not 1 <= args.block_kib <= 16384 or not 1 <= args.repetitions <= 100:
        parser.error("Invalid corpus, block size or repetition count")
    size, block = args.mib * 1024 * 1024, args.block_kib * 1024
    if size % block:
        parser.error("The source must contain a whole number of fixed blocks")
    exe = args.exe.resolve(strict=True)
    data = hashlib.shake_256(b"syrnck-incremental-packs-v1").digest(size + block)
    expected_hash = hashlib.sha256(data).hexdigest()
    rows, identities = [], set()
    with tempfile.TemporaryDirectory(prefix="syrnck-incremental-packs-") as temporary:
        root = Path(temporary)
        original, changed = root / "original.bin", root / "changed.bin"
        original.write_bytes(data[:size])
        changed.write_bytes(data)
        for repetition in range(args.repetitions):
            for encrypted in (False, True):
                env = {k: v for k, v in os.environ.items() if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN")}
                if encrypted:
                    env["SYRNCK_KEY"] = "73" * 32  # Public benchmark key only.
                for mode in (("incremental", "compact") if repetition % 2 == 0 else ("compact", "incremental")):
                    store = root / f"{mode}-{int(encrypted)}-{repetition}"

                    def command(*arguments):
                        return subprocess.run([str(exe), "--store", str(store), "--cache-mib", "0", *map(str, arguments)],
                                              env=env, capture_output=True, check=True).stdout

                    command("init", *(["--encrypted"] if encrypted else []))
                    first = json.loads(command("put", original, "--fixed-size", block))
                    command("pack")
                    before = {p: (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                              for p in (store / "packs").iterdir()}
                    second = json.loads(command("put", changed, "--fixed-size", block))
                    identities.add((first["manifest"], second["manifest"]))
                    started = time.perf_counter()
                    result = json.loads(command("pack", *(["--compact"] if mode == "compact" else [])))
                    seconds = time.perf_counter() - started
                    after = set((store / "packs").iterdir())
                    created = after - before.keys()
                    actual_pack_bytes = sum(p.stat().st_size for p in created if p.suffix == ".syp")
                    actual_index_bytes = sum(p.stat().st_size for p in created if p.suffix == ".syi")
                    if (actual_pack_bytes, actual_index_bytes) != (result["pack_bytes_written"], result["index_bytes_written"]):
                        raise RuntimeError("Reported write volume differs from newly published files")
                    if mode == "incremental":
                        if result["objects_written"] != 2:
                            raise RuntimeError("Unchanged blocks were repacked")
                        for path, fingerprint in before.items():
                            if (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns) != fingerprint:
                                raise RuntimeError("Incremental maintenance changed an existing pack")
                    output = root / "restored.bin"
                    command("get", second["manifest"], output)
                    if hashlib.sha256(output.read_bytes()).hexdigest() != expected_hash:
                        raise RuntimeError("Reconstruction differs")
                    output.unlink()
                    command("fsck")
                    rows.append({"mode": mode, "encrypted": encrypted, "repetition": repetition + 1,
                                 "seconds": seconds, "first_manifest": first["manifest"],
                                 "changed_manifest": second["manifest"], "result": result,
                                 "actual_new_pack_bytes": actual_pack_bytes, "actual_new_index_bytes": actual_index_bytes})
    if len(identities) != 1:
        raise RuntimeError("Logical object IDs differ across modes")
    summary = []
    for encrypted in (False, True):
        item = {"encrypted": encrypted}
        for mode in ("incremental", "compact"):
            matching = [r for r in rows if r["mode"] == mode and r["encrypted"] == encrypted]
            item[mode] = {"median_seconds": statistics.median(r["seconds"] for r in matching),
                          "pack_bytes_written": matching[0]["actual_new_pack_bytes"],
                          "index_bytes_written": matching[0]["actual_new_index_bytes"],
                          "objects_written": matching[0]["result"]["objects_written"]}
        item["speedup"] = item["compact"]["median_seconds"] / item["incremental"]["median_seconds"]
        summary.append(item)
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "source_mib": args.mib, "block_kib": args.block_kib, "appended_bytes": block,
              "repetitions": args.repetitions, "decoded_cache_enabled": False,
              "full_validation_and_publication_included": True,
              "os_cache_controlled": False, "byte_exact_restore": True, "matching_object_ids": True,
              "existing_pack_bytes_and_mtime_preserved_in_incremental_mode": True,
              "write_counters_match_new_physical_files": True,
              "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
