"""Measure complete versus partial status on a synthetic code/assets project."""
import argparse
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
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--asset-mib", type=int, default=128, help="Total size of two synthetic assets")
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 2 <= args.asset_mib <= 4096 or not 1 <= args.iterations <= 100:
        parser.error("asset-mib must be in 2..4096 and iterations in 1..100")
    cli = str(args.cli.resolve(strict=True))
    environment = {k: v for k, v in os.environ.items() if k.upper() not in
                   ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_SKIP_SMUDGE")}
    with tempfile.TemporaryDirectory(prefix="syrnck-partial-bench-") as temporary:
        parent = Path(temporary)
        root = parent / "primary"
        root.mkdir()
        storage = root / ".syrnck"

        def command(*words, context=None):
            arguments = [cli, "--store", str(storage)]
            if context:
                arguments.extend(["--workspace", context])
            result = subprocess.run([*arguments, *map(str, words)], cwd=root, env=environment,
                                    capture_output=True, check=True, timeout=300)
            return result.stdout

        command("init")
        (root / "Source").mkdir()
        (root / "Content").mkdir()
        code = b"int main() { return 0; }\n"
        (root / "Source/main.cpp").write_bytes(code)
        for number, size in enumerate((args.asset_mib // 2, args.asset_mib - args.asset_mib // 2)):
            block = bytes((value + number) % 256 for value in range(256)) * 4096
            with (root / f"Content/asset-{number}.uasset").open("wb") as output:
                for _ in range(size):
                    output.write(block)
        revision = command("native", "commit", "-m", "Synthetic partial benchmark").decode().strip()
        partial = parent / "code"
        command("native", "workspace-add", "code", partial, "--revision", revision, "--include", "Source")
        assert not (partial / "Content").exists()
        records = {}
        for name, context, expected_bytes, expected_files in (
                ("complete", None, args.asset_mib * 1024 * 1024 + len(code), 3),
                ("partial", "code", len(code), 1)):
            times, reports = [], []
            for _ in range(args.iterations):
                start = time.perf_counter()
                report = json.loads(command("native", "status", context=context))
                times.append(time.perf_counter() - start)
                assert report["clean"] and not report["changes"]
                assert report["hashed_bytes"] == expected_bytes and report["workspace_files"] == expected_files
                reports.append(report)
            records[name] = {"seconds": times, "median_seconds": statistics.median(times),
                             "hashed_bytes": [r["hashed_bytes"] for r in reports],
                             "workspace_files": [r["workspace_files"] for r in reports]}
        result = {"version": 1, "platform": platform.platform(), "architecture": platform.machine(),
                  "asset_bytes": args.asset_mib * 1024 * 1024, "code_bytes": len(code),
                  "iterations": args.iterations, "fixture": "synthetic repeated bytes; no OS cache flush; no indexed cache",
                  "results": records}
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")


if __name__ == "__main__":
    main()
