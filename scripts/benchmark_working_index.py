"""Compare full and indexed native status on one synthetic large asset."""
import argparse
import json
from pathlib import Path
import os
import platform
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--asset-mib", type=int, default=64)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--linked", action="store_true", help="Measure a registered external workspace")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.asset_mib <= 4096 or not 1 <= args.iterations <= 100:
        parser.error("asset-mib must be in 1..4096 and iterations in 1..100")
    cli = str(args.cli.resolve(strict=True))
    env = {k: v for k, v in os.environ.items() if k.upper() not in
           ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_SKIP_SMUDGE")}
    with tempfile.TemporaryDirectory(prefix="syrnck-working-index-bench-") as temporary:
        project = Path(temporary) / "primary"
        project.mkdir()
        workspace, context = project, []
        def command(*words):
            result = subprocess.run([cli, "--store", str(project / ".syrnck"), *context, *words],
                                    cwd=workspace, env=env, capture_output=True, check=True, timeout=300)
            return result.stdout
        command("init")
        path = project / "asset.bin"
        block = bytes(range(256)) * 4096
        with path.open("wb") as output:
            for _ in range(args.asset_mib):
                output.write(block)
        revision = command("native", "commit", "-m", "Synthetic asset").decode().strip()
        if args.linked:
            linked = Path(temporary) / "linked"
            command("native", "workspace-add", "benchmark", str(linked), "--revision", revision)
            workspace, context = linked, ["--workspace", "benchmark"]
            path = workspace / "asset.bin"
        command("native", "status", "--indexed")
        time.sleep(3.1)  # Recent-file guard, including second-resolution clocks.
        records = {}
        expected = None
        for name, flags in (("full", []), ("indexed", ["--indexed"])):
            times, reports = [], []
            for _ in range(args.iterations):
                start = time.perf_counter()
                report = json.loads(command("native", "status", *flags))
                times.append(time.perf_counter() - start)
                assert report["clean"] and not report["changes"]
                comparison = {k: v for k, v in report.items() if k not in ("hashed_bytes", "working_index")}
                if expected is None:
                    expected = comparison
                assert comparison == expected
                reports.append(report)
            records[name] = {"seconds": times, "median_seconds": statistics.median(times),
                             "hashed_bytes": [r["hashed_bytes"] for r in reports],
                             "reused_files": [r.get("working_index", {}).get("reused_files", 0) for r in reports]}
        before = path.stat()
        with path.open("r+b") as output:
            output.write(b"changed!")
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        changed = json.loads(command("native", "status", "--indexed"))
        assert not changed["clean"] and changed["hashed_bytes"] >= before.st_size
        result = {"version": 1, "platform": platform.system(), "architecture": platform.machine(),
                  "workspace": "linked" if args.linked else "primary",
                  "asset_bytes": before.st_size, "iterations": args.iterations, "results": records,
                  "same_size_same_mtime_edit_detected": True}
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")


if __name__ == "__main__":
    main()
