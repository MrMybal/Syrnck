"""Kill at every traced storage-repair write boundary; audit with production readers."""
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from crash_shard import CrashShard
from fixture_timing import case_directory

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
SHARD = CrashShard.from_argv()
SCENARIOS = [(encrypted, layout) for encrypted in (False, True) for layout in range(3)]
ENV = {name: value for name, value in os.environ.items() if not name.upper().startswith("SYRNCK_")}

if __name__ == "__main__" and "--list-scenarios" in sys.argv:
    print(json.dumps({"version": 1, "index": SHARD.index, "count": SHARD.count, "scenarios": SHARD.select(SCENARIOS)}))
    raise SystemExit(0)


def snapshot(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file() and path.name != "store.lock"}


class StorageRepairCrashes(unittest.TestCase):
    def run_reader(self, mode, root, *args):
        result = subprocess.run([READER, mode, str(root), *map(str, args)], env=ENV, capture_output=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_every_traced_boundary(self):
        count = 0
        with tempfile.TemporaryDirectory(prefix="syrnck-storage-repair-crash-") as temporary:
            parent = Path(temporary)
            for encrypted, layout in SHARD.select(SCENARIOS):
                seed = parent / f"seed-{encrypted}-{layout}"
                self.run_reader("prepare", seed, "encrypted" if encrypted else "plain", layout)
                with case_directory(parent, f"trace-{encrypted}-{layout}") as trace:
                    shutil.copytree(seed, trace)
                    result = subprocess.run([PROBE, "trace", str(trace)], env=ENV, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
                    events = [item for line in result.stdout.splitlines() if "phase" in (item := json.loads(line))]
                    self.assertTrue(events)
                    self.assertEqual([event["index"] for event in events], list(range(1, len(events) + 1)))
                    self.assertTrue(any(event["phase"] == "atomic.published" for event in events))
                    if layout:
                        self.assertTrue(any(event["phase"] == "repair_storage.catalog_published" for event in events))
                        self.assertTrue(any(event["phase"] == "repair_storage.old_pack_removed" for event in events))
                for event in events:
                    with self.subTest(encrypted=encrypted, layout=layout, event=event):
                        with case_directory(parent, f"case-{count}") as root:
                            count += 1
                            shutil.copytree(seed, root)
                            process = subprocess.Popen([PROBE, "run", str(root), str(event["index"])], env=ENV,
                                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                            lines = queue.Queue()
                            thread = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                            thread.start()
                            try:
                                reached = json.loads(lines.get(timeout=20))
                                self.assertEqual(reached, event)
                                self.assertIsNone(process.poll())
                                process.kill(); process.communicate(timeout=10)
                            finally:
                                if process.poll() is None: process.kill(); process.communicate(timeout=10)
                                thread.join(timeout=5)
                            before = snapshot(root)
                            self.assertTrue(self.run_reader("verify", root)["verified"])
                            self.assertEqual(snapshot(root), before, "production reader modified crash evidence")
                            self.assertTrue(self.run_reader("resume", root)["verified"])
            print(f"{count} storage repair hard-termination boundaries verified", flush=True)


if __name__ == "__main__": unittest.main()
