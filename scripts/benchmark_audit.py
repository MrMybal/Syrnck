"""Compare fsck inventory memory while verifying all objects and complete file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile


def snapshot(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--chunks", type=int, default=131072)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 4096 <= args.chunks <= 524288 or args.chunks % 4096 or not 1 <= args.repetitions <= 100:
        parser.error("Chunks must be a multiple of 4096 in 4096..524288; repetitions in 1..100")
    exe = args.exe.resolve(strict=True)

    def run(*arguments):
        completed = subprocess.run([str(exe), *map(str, arguments)], capture_output=True, text=True)
        if completed.returncode:
            raise RuntimeError(completed.stderr.strip() or "Benchmark failed without diagnostics")
        return json.loads(completed.stdout)

    rows = []
    with tempfile.TemporaryDirectory(prefix="syrnck-audit-benchmark-") as temporary:
        root = Path(temporary) / "store"
        fixture = run("prepare", root, args.chunks)
        before = snapshot(root)
        for repetition in range(args.repetitions):
            modes = ("audit-materialized", "audit-stream")
            for mode in modes if repetition % 2 == 0 else reversed(modes):
                row = run(mode, root)
                if (row["result"]["objects"] != fixture["objects"]
                        or row["io"]["object_reads"] != fixture["objects"]
                        or row["io"]["decoded_objects"] != fixture["objects"]
                        or not row["all_file_hashes_verified"]):
                    raise RuntimeError("Object counts, reads or complete file verification differ")
                if snapshot(root) != before:
                    raise RuntimeError("Audit changed the store")
                row["repetition"] = repetition + 1
                rows.append(row)
                print(f"{mode} {repetition + 1}/{args.repetitions}: {row['seconds']:.3f}s", file=sys.stderr)
    summary = []
    for mode in ("audit-materialized", "audit-stream"):
        matching = [row for row in rows if row["mode"] == mode]
        summary.append({"mode": mode,
                        "median_seconds": statistics.median(row["seconds"] for row in matching),
                        "median_peak_working_set_bytes": (statistics.median(
                            row["memory_after"]["peak_working_set_bytes"] for row in matching)
                            if matching[0]["memory_after"] else None)})
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "repetitions": args.repetitions, "fixture": fixture,
              "fixture_generation_in_separate_process": True,
              "materialized_is_comparison_implementation_in_same_binary": True,
              "real_valid_objects_and_complete_file_hashes": True,
              "unreachable_files_also_verified": True, "store_unchanged_after_every_run": True,
              "encrypted": False, "decoded_cache_bytes": 0, "verified_cache_entries": 0,
              "index_cache_bytes": 4194304, "comparison_helper_index_cache_bytes": 0,
              "os_cache_controlled": False, "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
