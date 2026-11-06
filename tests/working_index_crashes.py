"""Kill cache publication processes and reopen with the production reader."""
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
PHASES = ("atomic.temporary_opened", "atomic.writer_finished", "atomic.file_synced", "atomic.published", "atomic.durable")


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and p.name != "store.lock"}


class WorkingIndexCrashes(unittest.TestCase):
    def command(self, mode, root, *args):
        result = subprocess.run([READER, mode, str(root), *args], capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_object_and_pointer_boundaries(self):
        count = 0
        with tempfile.TemporaryDirectory(prefix="syrnck-working-index-crashes-") as temporary:
            for encrypted in (False, True):
                for previous in (False, True):
                    for part in ("object", "pointer"):
                        for phase in PHASES:
                            with self.subTest(encrypted=encrypted, previous=previous, part=part, phase=phase):
                                root = Path(temporary) / f"case-{count}"
                                count += 1
                                self.command("prepare", root, "encrypted" if encrypted else "plain",
                                             "previous" if previous else "missing")
                                process = subprocess.Popen([PROBE, "run", str(root), phase, part],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                lines = queue.Queue()
                                thread = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                                thread.start()
                                try:
                                    line = lines.get(timeout=15)
                                    self.assertTrue(line, process.stderr.read().decode("utf-8", "replace") if not line else "")
                                    self.assertEqual(json.loads(line), {"phase": phase, "part": part})
                                    self.assertIsNone(process.poll())
                                    process.kill()
                                    process.communicate(timeout=8)
                                finally:
                                    if process.poll() is None:
                                        process.kill()
                                        process.communicate(timeout=8)
                                    thread.join(timeout=5)
                                before = snapshot(root)
                                self.assertTrue(self.command("verify", root)["verified"])
                                self.assertEqual(snapshot(root), before)
                                self.assertTrue(self.command("resume", root)["verified"])
        print(f"{count}/{count} working-index process termination boundaries verified", flush=True)


if __name__ == "__main__":
    unittest.main()
