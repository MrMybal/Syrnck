"""Compare paged pack indexes to the former in-memory map on identical fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--objects", type=int, default=1048576)
    parser.add_argument("--queries", type=int, default=20000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.objects <= 1048576 or not 1 <= args.queries <= 1000000 or not 1 <= args.repetitions <= 100:
        parser.error("Invalid fixture/query/repetition count")
    exe = args.exe.resolve(strict=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix="syrnck-index-benchmark-") as temporary:
        root = Path(temporary) / "fixture"
        prepared = subprocess.run([str(exe), "prepare", str(root), str(args.objects)], capture_output=True,
                                  text=True, check=True)
        descriptor = json.loads(prepared.stdout)
        modes = [("map", 0), ("paged", 0), ("paged", 4), ("paged", 64)]
        for repetition in range(args.repetitions):
            order = modes if repetition % 2 == 0 else list(reversed(modes))
            for pattern in ("random", "sequential"):
                for mode, budget in order:
                    result = subprocess.run([str(exe), mode, str(root), str(args.queries), str(budget), pattern],
                                            capture_output=True, text=True, check=True)
                    row = json.loads(result.stdout)
                    if (not row["all_results_verified"] or row["hits"] != (args.queries + 1) // 2 or
                            row["warm_hits"] != row["hits"]):
                        raise RuntimeError("Lookup results differ")
                    if row["index_cached_bytes"] > budget * 1024 * 1024:
                        raise RuntimeError("Index page cache exceeded budget")
                    row["repetition"] = repetition + 1
                    rows.append(row)
    summary = []
    for pattern in ("random", "sequential"):
        for mode, budget in modes:
            matching = [r for r in rows if r["pattern"] == pattern and r["mode"] == mode and r["cache_mib"] == budget]
            item = {"pattern": pattern, "mode": mode, "cache_mib": budget,
                    "median_load_seconds": statistics.median(r["load_seconds"] for r in matching),
                    "median_queries_per_second": statistics.median(r["queries_per_second"] for r in matching),
                    "median_warm_queries_per_second": statistics.median(r["warm_queries_per_second"] for r in matching),
                    "median_index_page_reads": statistics.median(r["index_page_reads"] for r in matching),
                    "median_warm_index_page_reads": statistics.median(r["warm_index_page_reads"] for r in matching)}
            for name in ("working_set_bytes", "peak_working_set_bytes", "private_bytes"):
                item["median_" + name] = (statistics.median(r["memory_after_queries"][name] for r in matching)
                                          if matching[0]["memory_after_queries"] else None)
            summary.append(item)
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "objects": args.objects, "queries": args.queries, "repetitions": args.repetitions,
              "two_passes_of_identical_queries_per_run": True,
              "synthetic_index_only_no_codec_or_reconstruction": True,
              "map_is_comparison_implementation_in_same_binary": True,
              "fixture_generation_in_separate_process": True, "os_cache_controlled": False,
              "all_lookup_results_verified": True, "fixture_descriptor": descriptor,
              "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
