"""Measure complete Store::pack compaction with one harness linked to two engine revisions."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-exe", type=Path, required=True)
    parser.add_argument("--after-exe", type=Path, required=True)
    parser.add_argument("--fixture-exe", type=Path, required=True)
    parser.add_argument("--before-revision", required=True)
    parser.add_argument("--chunks", type=int, default=131072)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 4096 <= args.chunks <= 524288 or args.chunks % 4096 or not 1 <= args.repetitions <= 100:
        parser.error("Chunks must be a multiple of 4096 in 4096..524288; repetitions in 1..100")
    executables = {mode: path.resolve(strict=True) for mode, path in
                   (("before", args.before_exe), ("after", args.after_exe))}
    fixture_exe = args.fixture_exe.resolve(strict=True)

    def run(exe, *arguments):
        completed = subprocess.run([str(exe), *map(str, arguments)], capture_output=True, text=True)
        if completed.returncode:
            raise RuntimeError(completed.stderr.strip() or "Benchmark failed without diagnostics")
        return json.loads(completed.stdout)

    rows = []
    reference = None
    with tempfile.TemporaryDirectory(prefix="syrnck-compact-benchmark-") as temporary:
        base = Path(temporary)
        fixture = run(fixture_exe, "prepare", base / "fixture", args.chunks)
        for repetition in range(args.repetitions):
            for mode in ("before", "after") if repetition % 2 == 0 else ("after", "before"):
                root = base / f"{mode}-{repetition}"
                shutil.copytree(base / "fixture", root)
                row = run(executables[mode], root)
                result = row["result"]
                if reference is None:
                    reference = result
                if (result != reference or result["objects"] != fixture["objects"]
                        or not row["all_file_hashes_verified_after_compaction"]
                        or not row["references_unchanged"]):
                    raise RuntimeError("Compaction counters, file hashes or references differ")
                row.update(mode=mode, repetition=repetition + 1)
                rows.append(row)
                print(f"{mode} {repetition + 1}/{args.repetitions}: {row['seconds']:.3f}s", file=sys.stderr)
    summary = []
    for mode in ("before", "after"):
        matching = [row for row in rows if row["mode"] == mode]
        summary.append({"mode": mode, "median_seconds": statistics.median(r["seconds"] for r in matching),
                        "median_peak_working_set_bytes": (statistics.median(
                            r["memory_after"]["peak_working_set_bytes"] for r in matching)
                            if matching[0]["memory_after"] else None)})
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "before_revision": args.before_revision,
              "binary_sha256": {mode: hashlib.sha256(exe.read_bytes()).hexdigest()
                                 for mode, exe in executables.items()},
              "fixture_binary_sha256": hashlib.sha256(fixture_exe.read_bytes()).hexdigest(),
              "same_harness_linked_to_two_engine_revisions": True, "fixture": fixture,
              "fixture_generation_and_copy_outside_measurement": True,
              "real_valid_objects_and_complete_file_hashes": True, "encrypted": False,
              "decoded_cache_bytes": 0, "verified_cache_entries": 0, "index_cache_bytes": 4194304,
              "max_objects_per_pack": 65536, "max_pack_bytes": 536870912,
              "post_audit_excluded_from_time_and_memory": True, "os_cache_controlled": False,
              "repetitions": args.repetitions, "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
