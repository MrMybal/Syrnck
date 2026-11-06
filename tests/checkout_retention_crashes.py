"""Terminate checkout retirement at each atomic publication boundary and resume independently."""
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
ENV = {k: v for k, v in os.environ.items() if k.upper() not in
       ("SYRNCK_KEY", "SYRNCK_BACKUP_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}
PHASES = ("atomic.temporary_opened", "atomic.writer_finished", "atomic.file_synced", "atomic.published", "atomic.durable")


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and p.name != "store.lock"}


class CheckoutRetirementCrashes(unittest.TestCase):
    def command(self, mode, root, *args):
        result = subprocess.run([READER, mode, str(root), *map(str, args)], env=ENV, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_atomic_receipt_target_and_optional_source_group(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-retirement-crash-") as temporary:
            parent = Path(temporary)
            count = 0
            verified = 0
            for encrypted in (False, True):
                for sources in (False, True):
                    for retained in (False, True):
                        for phase in PHASES:
                            with self.subTest(encrypted=encrypted, sources=sources, retained=retained, phase=phase):
                                root = parent / f"case-{count}"
                                count += 1
                                # Receipts bind canonical workspace paths. Prepare each
                                # local fixture at its actual recovery location.
                                self.command("prepare", root, "encrypted" if encrypted else "plain",
                                             "sources" if sources else "targets", "retained" if retained else "receipts")
                                process = subprocess.Popen([PROBE, "run", str(root), phase], env=ENV,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                lines = queue.Queue()
                                thread = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                                thread.start()
                                try:
                                    line = lines.get(timeout=15)
                                    self.assertTrue(line, "retirement checkpoint not reached: " + (process.stderr.read().decode("utf-8", "replace") if not line else ""))
                                    self.assertEqual(json.loads(line), {"phase": phase})
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
                                self.assertEqual(snapshot(root), before, "fresh reader modified recovery evidence")
                                self.assertTrue(self.command("resume", root)["verified"])
                                verified += 1
            print(f"{verified}/{count} checkout-retirement process termination boundaries verified", flush=True)


if __name__ == "__main__":
    unittest.main()
