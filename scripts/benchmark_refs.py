"""Compare complete CLI reference listing with two engine binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-exe", type=Path, required=True)
    parser.add_argument("--after-exe", type=Path, required=True)
    parser.add_argument("--before-revision", required=True)
    parser.add_argument("--references", type=int, nargs="+", default=[8192, 32768])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (not 1 <= args.repetitions <= 100 or len(set(args.references)) != len(args.references)
            or any(not 1 <= count <= 65536 for count in args.references)):
        parser.error("Choose distinct reference counts in 1..65536 and repetitions in 1..100")
    executables = {mode: path.resolve(strict=True) for mode, path in
                   (("before", args.before_exe), ("after", args.after_exe))}
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}

    def run(exe, store, *arguments):
        started = time.perf_counter()
        completed = subprocess.run([str(exe), "--store", str(store), *map(str, arguments)],
                                   env=env, capture_output=True, timeout=300)
        elapsed = time.perf_counter() - started
        if completed.returncode:
            raise RuntimeError(completed.stderr.decode("utf-8", "replace"))
        return completed.stdout, elapsed

    rows, fixtures = [], []
    with tempfile.TemporaryDirectory(prefix="syrnck-refs-benchmark-") as temporary:
        base = Path(temporary)
        store = base / "store"
        run(executables["after"], store, "init")
        asset = base / "asset.bin"
        asset.write_bytes(b"complete retained file version\n" * 32)
        output, _ = run(executables["after"], store, "put", asset)
        key = {"kind": "manifest", "id": json.loads(output)["manifest"]}
        for count in args.references:
            expected = {f"git-live/repository/{i:08d}": key for i in range(count)}
            data = json.dumps(expected, separators=(",", ":")).encode()
            (store / "refs.json").write_bytes(data)
            fixtures.append({"references": count, "refs_json_bytes": len(data)})
            snapshot = {p.relative_to(store): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in store.rglob("*") if p.is_file() and p.name != "store.lock"}
            reference_bytes = None
            for repetition in range(args.repetitions):
                for mode in (("before", "after") if repetition % 2 == 0 else ("after", "before")):
                    output, elapsed = run(executables[mode], store, "refs")
                    if json.loads(output) != expected:
                        raise RuntimeError("Reference listing differs from complete fixture")
                    if reference_bytes is None:
                        reference_bytes = output
                    if output != reference_bytes:
                        raise RuntimeError("Reference output bytes differ between binaries")
                    rows.append({"mode": mode, "references": count, "repetition": repetition + 1,
                                 "seconds": elapsed, "stdout_bytes": len(output),
                                 "stdout_sha256": hashlib.sha256(output).hexdigest()})
                    print(f"{mode}, {count} refs, run {repetition + 1}: {elapsed:.3f}s", file=sys.stderr)
            for exe in executables.values():
                run(exe, store, "fsck")  # Full retained-file audit outside measurement.
            after = {p.relative_to(store): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in store.rglob("*") if p.is_file() and p.name != "store.lock"}
            if snapshot != after:
                raise RuntimeError("Read-only benchmark changed the Store")
    summary = [{"references": count, "mode": mode,
                "median_seconds": statistics.median(row["seconds"] for row in rows
                                    if row["mode"] == mode and row["references"] == count)}
               for count in args.references for mode in ("before", "after")]
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "before_revision": args.before_revision,
              "binary_sha256": {mode: hashlib.sha256(exe.read_bytes()).hexdigest()
                                 for mode, exe in executables.items()},
              "operation": "CLI refs: process startup, Store open validation, second refs read, JSON output",
              "fixture": fixtures, "unique_complete_files": 1, "encrypted": False,
              "fixture_generation_and_audit_outside_measurement": True,
              "python_output_validation_outside_measurement": True,
              "stdout_byte_identical": True, "store_unchanged": True,
              "full_file_audit_after_runs": True, "os_cache_controlled": False,
              "fresh_process_per_run": True, "repetitions": args.repetitions,
              "summary_medians": summary, "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "summary_medians": summary}, indent=2))


if __name__ == "__main__":
    main()
