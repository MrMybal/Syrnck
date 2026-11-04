"""CLI progress framing, binary filters, and cooperative OS interruption."""
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())
RELAY = str(Path(sys.argv.pop(1)).resolve()) if os.name == "nt" else None


class CliOperations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="syrnck-cli-operations-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_LANG", "SYRNCK_TOKEN")}
        self.run_cli("init")

    def run_cli(self, *args, lang="en", data=None, ok=True):
        result = subprocess.run([CLI, "--lang", lang, *map(str, args)], cwd=self.root,
                                env=self.env, input=data, capture_output=True, timeout=20)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        return result

    def events(self, result):
        events = [json.loads(line) for line in result.stderr.splitlines()]
        self.assertTrue(events)
        for event in events:
            self.assertEqual(set(event), {"phase", "path", "bytes", "total_bytes", "complete", "buffered_blocks"})
        return events

    def test_file_progress_and_translations(self):
        source = self.root / "données.bin"
        data = bytes(range(256)) * 17000
        source.write_bytes(data)
        plain = self.run_cli("put", source)
        self.assertEqual(plain.stderr, b"")
        with_progress = self.run_cli("put", source, "--progress-json", "--workers", 4)
        manifest = json.loads(with_progress.stdout)["manifest"]
        self.assertEqual(manifest, json.loads(plain.stdout)["manifest"])
        events = self.events(with_progress)
        self.assertTrue(all(Path(e["path"]) == source and e["total_bytes"] == len(data) for e in events))
        self.assertEqual(events[-1]["bytes"], len(data))
        self.assertTrue(events[-1]["complete"])
        restored = self.root / "restored.bin"
        result = self.run_cli("get", manifest, restored, "--progress-json", lang="fr")
        self.assertEqual(result.stdout, b"")
        self.assertEqual(restored.read_bytes(), data)
        self.assertEqual(self.events(result)[-1]["phase"], "reconstructing")
        for lang, expected in (("en", "Progress: Ingesting"), ("fr", "Progression : Importation")):
            result = self.run_cli("put", source, "--progress", lang=lang)
            self.assertIn(expected, result.stderr.decode("utf-8"))
            self.assertEqual(json.loads(result.stdout)["manifest"], manifest)
        for args in (("put", source, "--progress", "--progress-json"),
                     ("put", source, "--progress", "--progress"), ("fsck", "--progress-json"),
                     ("pack", "--progress"), ("native", "export", "--progress")):
            self.run_cli(*args, ok=False)

    def test_native_commit_and_filter_stdout(self):
        project = self.root
        data = bytes(range(256)) * 4000
        (project / "asset.bin").write_bytes(data)
        result = self.run_cli("native", "commit", "--directory", project, "-m", "Snapshot",
                              "--workers", 4, "--progress-json")
        self.assertEqual(len(result.stdout.strip()), 64)
        events = self.events(result)
        self.assertTrue(any(e["path"] == "asset.bin" and e["total_bytes"] == len(data) and e["complete"]
                            for e in events))
        self.assertEqual(events[-1]["phase"], "publishing")
        pointer = self.run_cli("git", "clean", data=data).stdout
        result = self.run_cli("git", "clean", "--progress-json", data=data, lang="fr")
        self.assertEqual(result.stdout, pointer)
        self.assertTrue(self.events(result)[-1]["complete"])
        result = self.run_cli("git", "smudge", "--progress-json", data=pointer)
        self.assertEqual(result.stdout, data)
        self.assertTrue(self.events(result)[-1]["complete"])
        passthrough = b"ordinary source\x00\r\n" * 1000
        self.assertEqual(self.run_cli("git", "smudge", "--progress-json", data=passthrough).stdout, passthrough)
        self.run_cli("fsck")

    def test_real_signal_cancels_without_publishing_a_pin(self):
        previous = self.run_cli("git", "clean", data=b"retained previous version").stdout
        refs = self.run_cli("refs").stdout
        for iteration, signum in enumerate((signal.SIGINT, signal.SIGTERM) if os.name != "nt" else (None,)):
            with self.subTest(signal=signum):
                request = self.root / f"interrupt-{iteration}"
                command = [CLI, "--lang", "fr", "git", "clean", "--workers", "4", "--progress-json"]
                startup = {}
                if RELAY:
                    command = [RELAY, str(request), *command]
                    info = subprocess.STARTUPINFO()
                    info.dwFlags = subprocess.STARTF_USESHOWWINDOW
                    info.wShowWindow = subprocess.SW_HIDE
                    startup = {"creationflags": subprocess.CREATE_NEW_CONSOLE, "startupinfo": info}
                child = subprocess.Popen(command, cwd=self.root, env=self.env, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, **startup)
                first_line = queue.Queue()
                reader = threading.Thread(target=lambda: first_line.put(child.stderr.readline()), daemon=True)
                reader.start()
                try:
                    # Allow pointer detection to finish, then keep the first CDC
                    # read blocked until the real signal has reached the process.
                    child.stdin.write(b"x" * 8192)
                    child.stdin.flush()
                    line = first_line.get(timeout=10)
                    reader.join(timeout=1)
                    self.assertEqual(json.loads(line)["phase"], "ingesting", line)
                    if RELAY:
                        request.touch()
                    else:
                        child.send_signal(signum)
                    time.sleep(0.3)
                    child.stdin.close()
                    child.stdin = None
                    output, error = child.communicate(timeout=10)
                    self.assertEqual(child.returncode, 130, (output, line + error))
                    self.assertEqual(output, b"")
                    self.assertIn("opération annulée", error.decode("utf-8"))
                finally:
                    if child.poll() is None:
                        child.kill()  # Relay job object also kills its CLI child.
                        child.communicate(timeout=5)
                    reader.join(timeout=1)
                self.assertEqual(self.run_cli("refs").stdout, refs)
                self.assertEqual(self.run_cli("git", "smudge", data=previous).stdout, b"retained previous version")
                self.assertFalse(any(".tmp" in p.name for p in (self.root / ".syrnck").rglob("*")))
                self.run_cli("fsck")
        retry = self.run_cli("git", "clean", "--workers", 4, data=b"retry after cancellation").stdout
        self.assertEqual(self.run_cli("git", "smudge", data=retry).stdout, b"retry after cancellation")


if __name__ == "__main__":
    unittest.main()
