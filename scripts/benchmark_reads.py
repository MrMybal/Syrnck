"""Measure verified warm reads with cache enabled/disabled and alternating order."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--mib", type=int, default=32)
parser.add_argument("--cache-mib", type=int, default=64)
parser.add_argument("--repetitions", type=int, default=5)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.repetitions <= 100 or not 1 <= args.cache_mib <= 1024:
    parser.error("Use 1..100 repetitions and 1..1024 MiB cache")
exe = args.exe.resolve(strict=True)
rows, identities = [], {}
for repetition in range(args.repetitions):
    for pattern in ("random", "repeated"):
        for encrypted in (False, True):
            order = (0, args.cache_mib) if repetition % 2 == 0 else (args.cache_mib, 0)
            for cache_mib in order:
                result = subprocess.run([str(exe), str(args.mib), str(cache_mib), str(int(encrypted)), pattern],
                                        capture_output=True, text=True, check=True)
                value = json.loads(result.stdout)
                if identities.setdefault(pattern, value["manifest"]) != value["manifest"] or not value["byte_exact_restore"]:
                    raise RuntimeError("Object identity or reconstruction changed")
                for stage in value["stages"].values():
                    if stage["io"]["cached_bytes"] > cache_mib * 1024 * 1024:
                        raise RuntimeError("Decoded cache exceeded its budget")
                value["repetition"] = repetition + 1
                rows.append(value)
summary = []
for pattern in ("random", "repeated"):
    for encrypted in (False, True):
        for stage in ("presence", "reingest", "publication", "reconstruction", "fsck"):
            item = {"pattern": pattern, "encrypted": encrypted, "stage": stage}
            for cache_mib in (0, args.cache_mib):
                matching = [r["stages"][stage] for r in rows if r["pattern"] == pattern and
                            r["encrypted"] == encrypted and r["cache_mib"] == cache_mib]
                item[str(cache_mib)] = {"mib_per_second": statistics.median(r["mib_per_second"] for r in matching),
                    "decoded_objects": statistics.median(r["io"]["decoded_objects"] for r in matching),
                    "object_reads": statistics.median(r["io"]["object_reads"] for r in matching),
                    "envelope_bytes_read": statistics.median(r["io"]["envelope_bytes_read"] for r in matching)}
            item["speedup"] = item[str(args.cache_mib)]["mib_per_second"] / item["0"]["mib_per_second"]
            summary.append(item)
report = {"platform": platform.system(), "architecture": platform.machine(),
          "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
          "mib": args.mib, "cache_mib": args.cache_mib, "repetitions": args.repetitions,
          "cache_warmed_in_same_store_handle": True, "network_measured": False,
          "matching_object_ids": True, "byte_exact_restore": True,
          "summary_medians": summary, "runs": rows}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))
