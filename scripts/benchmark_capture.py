"""Measure complete native captures with/without session-local manifest reuse."""
import argparse
import json
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--mib", type=int, default=64)
    parser.add_argument("--files", type=int, default=32)
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--encrypted", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.mib <= 4096 or not 1 <= args.files <= 4096 or not 1 <= args.iterations <= 100:
        parser.error("mib/files must be 1..4096 and iterations 1..100")
    result = subprocess.run([str(args.exe.resolve(strict=True)), str(args.mib), str(args.files),
                             str(args.iterations), str(int(args.encrypted))],
                            capture_output=True, text=True, check=True)
    report = json.loads(result.stdout)
    report.update(platform=platform.system(), architecture=platform.machine())
    report["summary"] = {}
    for mode in ("full", "incremental"):
        rows = [r for r in report["rows"] if r["mode"] == mode]
        assert len(rows) == args.iterations and all(r["byte_exact_capture"] for r in rows)
        report["summary"][mode] = {"median_seconds": statistics.median(r["seconds"] for r in rows),
                                    "source_bytes_read": [r["source_bytes_read"] for r in rows],
                                    "object_reads": [r["store_io"]["object_reads"] for r in rows]}
    report["summary"]["elapsed_speedup"] = report["summary"]["full"]["median_seconds"] / report["summary"]["incremental"]["median_seconds"]
    report["scope"] = "Synthetic local files; complete object closure verification included. No Unreal-project throughput guarantee."
    text = json.dumps(report, indent=2) + "\n"
    args.output.write_text(text, encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
