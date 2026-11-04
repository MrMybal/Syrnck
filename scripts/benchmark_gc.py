"""Compare GC inventory memory using valid packed objects and complete file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile


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
    with tempfile.TemporaryDirectory(prefix="syrnck-gc-benchmark-") as temporary:
        root = Path(temporary) / "store"
        fixture = run("prepare", root, args.chunks)
        reference = None
        fields = ("live_objects", "unreachable_objects", "reclaimable_bytes", "packs_to_rewrite",
                  "packs_to_drop", "pack_bytes_to_write")
        for repetition in range(args.repetitions):
            for mode in (("materialized", "stream") if repetition % 2 == 0 else ("stream", "materialized")):
                row = run(mode, root)
                counters = {name: row["result"][name] for name in fields}
                if reference is None:
                    reference = counters
                if reference != counters or not row["all_live_files_verified"]:
                    raise RuntimeError("GC plan or file validation differs")
                row["repetition"] = repetition + 1
                rows.append(row)
                print(f"{mode} {repetition + 1}/{args.repetitions}: {row['seconds']:.3f}s", file=sys.stderr)
        deletion = run("delete", root)
        if not deletion["post_delete_audit"] or any(deletion["result"][name] != reference[name] for name in fields):
            raise RuntimeError("Deletion or reconstruction audit differs")
    summary = []
    for mode in ("materialized", "stream"):
        matching = [r for r in rows if r["mode"] == mode]
        summary.append({"mode": mode, "median_seconds": statistics.median(r["seconds"] for r in matching),
                        "median_peak_working_set_bytes": (statistics.median(
                            r["memory_after"]["peak_working_set_bytes"] for r in matching)
                            if matching[0]["memory_after"] else None)})
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "repetitions": args.repetitions,
              "fixture": fixture, "fixture_generation_in_separate_process": True,
              "materialized_is_comparison_implementation_in_same_binary": True,
              "real_valid_objects_and_complete_file_hashes": True, "encrypted": False,
              "decoded_cache_bytes": 0, "verified_cache_entries": 0, "index_cache_bytes": 4194304,
              "comparison_helper_index_cache_bytes": 0,
              "os_cache_controlled": False, "all_plans_equal": True, "summary_medians": summary,
              "runs": rows, "delete_run": deletion}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
