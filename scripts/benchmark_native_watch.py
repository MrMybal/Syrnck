"""Measure native watch edits/namespace changes beside an unrelated synthetic asset."""
import argparse
import json
import os
from pathlib import Path
import platform
import queue
import statistics
import subprocess
import tempfile
import threading
import time


def semantic(report):
    return {k: v for k, v in report.items() if k not in ("hashed_bytes", "working_index", "observation")}


class Resident:
    def __init__(self, command, root, env):
        self.lines = queue.Queue()
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, cwd=root, env=env, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=self.stderr)

        def read():
            try:
                for line in self.process.stdout:
                    self.lines.put(json.loads(line))
            except Exception as error:
                self.lines.put(error)
            finally:
                self.lines.put(None)

        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def until(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                frame = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if frame is None:
                self.stderr.seek(0)
                raise RuntimeError("watch exited: " + self.stderr.read().decode("utf-8", "replace"))
            if isinstance(frame, Exception):
                raise RuntimeError("invalid watch output") from frame
            if frame["type"] == "error":
                raise RuntimeError("watch observation failed: " + frame["message"])
            if predicate(frame):
                return frame
        raise RuntimeError("timed out waiting for expected watch status")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.reader.join(timeout=5)
        self.process.stdout.close()
        self.stderr.close()


def emit(result, output):
    document = json.dumps(result, indent=2) + "\n"
    if output:
        output.write_text(document, encoding="utf-8")
    print(document, end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--asset-mib", type=int, default=64)
    parser.add_argument("--files", type=int, default=1000)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--debounce-ms", type=int, default=10)
    parser.add_argument("--journal", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not (1 <= args.asset_mib <= 4096 and 0 <= args.files <= 50000 and 1 <= args.iterations <= 100
            and 0 <= args.debounce_ms <= 5000):
        parser.error("asset-mib: 1..4096; files: 0..50000; iterations: 1..100; debounce-ms: 0..5000")
    cli = str(args.cli.resolve(strict=True))
    env = {k: v for k, v in os.environ.items() if k.upper() not in
           ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_SKIP_SMUDGE")}
    result = {"version": 1, "platform": platform.system(), "architecture": platform.machine(),
              "asset_bytes": args.asset_mib * 1024 * 1024, "source_files": args.files,
              "iterations": args.iterations, "debounce_ms": args.debounce_ms, "journal": args.journal}
    with tempfile.TemporaryDirectory(prefix="syrnck-watch-bench-") as temporary:
        project = Path(temporary)
        prefix = [cli, "--store", str(project / ".syrnck")]

        def command(*words):
            return subprocess.run([*prefix, *words], cwd=project, env=env, capture_output=True,
                                  check=True, timeout=300).stdout

        command("init")
        asset = project / "assets/large.bin"
        asset.parent.mkdir()
        block = bytes(range(256)) * 4096
        with asset.open("wb") as output:
            for _ in range(args.asset_mib):
                output.write(block)
        source_bytes = 0
        for number in range(args.files):
            path = project / f"source/d{number // 100:04d}/file{number:05d}.cpp"
            path.parent.mkdir(parents=True, exist_ok=True)
            content = f"int asset_{number} = {number};\n".encode("ascii")
            path.write_bytes(content)
            source_bytes += len(content)
        command("native", "commit", "-m", "Synthetic watch corpus")
        started = time.perf_counter()
        run = Resident([*prefix, "native", "watch", "--audit-ms", "3600000", "--debounce-ms", str(args.debounce_ms),
                        *(["--journal"] if args.journal else [])], project, env)
        records = {name: [] for name in ("create", "edit", "rename", "delete")}
        last = None
        try:
            initial = run.until(lambda f: f["type"] == "status", timeout=300)
            result.update(backend=initial["backend"], initial_seconds=time.perf_counter() - started)
            if initial["backend"] == "polling":
                result.update(supported=False, reason="native_notifications_unavailable")
            else:
                result["supported"] = True
                assert initial["status"]["clean"] and initial["full_scan"]
                assert initial["status"]["hashed_bytes"] == result["asset_bytes"] + source_bytes
                last = initial["status"]
                for number in range(args.iterations):
                    current = project / "change.txt"
                    renamed = project / "renamed.txt"
                    old_hash = None
                    payload = b""
                    for name in records:
                        if name in ("create", "edit"):
                            payload = f"{name}-{number}".encode("ascii")
                        before = time.perf_counter()
                        if name in ("create", "edit"):
                            current.write_bytes(payload)
                        elif name == "rename":
                            current.rename(renamed)
                        else:
                            renamed.unlink()
                        wanted = "renamed.txt" if name == "rename" else "change.txt"

                        def matches(frame):
                            status = frame["status"]
                            if name == "delete":
                                return status["clean"]
                            rows = status["changes"]
                            return (len(rows) == 1 and rows[0]["path"] == wanted and
                                    (name != "edit" or rows[0]["target"]["file_hash"] != old_hash))

                        frame = run.until(matches)
                        elapsed = time.perf_counter() - before
                        last = frame["status"]
                        assert not frame["full_scan"] and last["observation"]["mode"] == "targeted"
                        assert last["hashed_bytes"] == (0 if name == "delete" else len(payload))
                        assert last["workspace_files"] == args.files + 1 + (name != "delete")
                        if name != "delete":
                            assert last["changes"][0]["status"] == "untracked"
                            assert last["changes"][0]["target"]["size"] == len(payload)
                            if name == "rename":
                                assert last["changes"][0]["target"]["file_hash"] == old_hash
                            old_hash = last["changes"][0]["target"]["file_hash"]
                        if args.journal:
                            assert frame["journal"]["durable"]
                        records[name].append({"seconds": elapsed, "hashed_bytes": last["hashed_bytes"]})
        finally:
            # No full-status subprocess competes for the resident Store lock.
            # Baseline timings are collected only after the observer has exited.
            run.close()
        if result["supported"]:
            full = []
            for _ in range(args.iterations):
                before = time.perf_counter()
                report = json.loads(command("native", "status"))
                elapsed = time.perf_counter() - before
                assert semantic(report) == semantic(last)
                assert report["hashed_bytes"] == result["asset_bytes"] + source_bytes
                full.append({"seconds": elapsed, "hashed_bytes": report["hashed_bytes"]})
            result["operations"] = {name: {"samples": rows, "median_seconds": statistics.median(r["seconds"] for r in rows)}
                                    for name, rows in records.items()}
            result["full_status"] = {"samples": full, "median_seconds": statistics.median(r["seconds"] for r in full)}
            result["final_semantic_status_matches_full"] = True
    emit(result, args.output)


if __name__ == "__main__":
    main()
