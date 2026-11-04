"""Compare sequential/parallel ingestion with alternating run order and verified IDs."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--mib", type=int, default=64)
parser.add_argument("--repetitions", type=int, default=5)
parser.add_argument("--workers", type=int, default=4)
parser.add_argument("--buffer-mib", type=int, default=32)
parser.add_argument("--pattern", choices=["random", "compressible"], default="random")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
if not 1 <= args.repetitions <= 100 or not 2 <= args.workers <= 64:
    parser.error("Use 1..100 repetitions and 2..64 parallel workers")
exe = args.exe.resolve(strict=True)
rows = []
identities = {}
for repetition in range(args.repetitions):
    for encrypted in (False, True):
        order = (1, args.workers) if repetition % 2 == 0 else (args.workers, 1)
        for workers in order:
            result = subprocess.run([str(exe), str(args.mib), str(workers), str(int(encrypted)),
                                     str(args.buffer_mib), args.pattern], capture_output=True, text=True, check=True)
            values = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            if len(values) != 2:
                raise RuntimeError("Expected results for both chunking policies")
            for value in values:
                identity = (value["initial_manifest"], value["edited_manifest"])
                if identities.setdefault(value["policy"], identity) != identity or not value["byte_exact_restore"]:
                    raise RuntimeError("Sequential/parallel/encrypted object identity or reconstruction differs")
                for stage in ("initial", "delta"):
                    peak = value[stage]["peak_in_flight_bytes"]
                    tasks = value[stage]["peak_in_flight_blocks"]
                    if peak > args.buffer_mib * 1024 * 1024 or tasks > 2 * workers:
                        raise RuntimeError("Pipeline exceeded its configured bounds")
                value["repetition"] = repetition + 1
                rows.append(value)
summary = []
for encrypted in (False, True):
    for policy in ("fastcdc2020", "fixed-1mib"):
        item = {"encrypted": encrypted, "policy": policy}
        for workers in (1, args.workers):
            matching = [r for r in rows if r["workers"] == workers and r["encrypted"] == encrypted and r["policy"] == policy]
            item[str(workers)] = {name: statistics.median(r[name] for r in matching) for name in
                                 ("initial_mib_per_second", "edited_mib_per_second", "restore_mib_per_second")}
        item["initial_speedup"] = item[str(args.workers)]["initial_mib_per_second"] / item["1"]["initial_mib_per_second"]
        summary.append(item)
report = {"platform": platform.system(), "architecture": platform.machine(),
          "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
          "mib": args.mib, "pattern": args.pattern, "repetitions": args.repetitions,
          "parallel_workers": args.workers, "buffer_mib": args.buffer_mib,
          "includes_reference_publication": False, "includes_network": False,
          "byte_exact_restore": True, "matching_object_ids": True,
          "summary_medians": summary, "runs": rows}
encoded = json.dumps(report, indent=2) + "\n"
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(encoded, encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))
else:
    print(encoded, end="")
