"""Terminate backup writers at durable IO boundaries; audit and resume in a fresh process."""
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

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
ENV = {k: v for k, v in os.environ.items() if k.upper() not in
       ("SYRNCK_KEY", "SYRNCK_BACKUP_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}
PHASES = ("atomic.temporary_opened", "atomic.writer_finished", "atomic.file_synced", "atomic.published", "atomic.durable")


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and p.name != "store.lock"}


class BackupCrashes(unittest.TestCase):
    def command(self, mode, root, *args):
        result = subprocess.run([READER, mode, str(root), *map(str, args)], env=ENV, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_process_kill_at_object_and_directory_publications(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-backup-crash-") as temporary:
            parent = Path(temporary)
            count = 0
            for encrypted in (False, True):
                for operation in ("create", "restore", "repair"):
                    seed = parent / f"seed-{encrypted}-{operation}"
                    self.command("prepare", seed, "encrypted" if encrypted else "plain", operation)
                    kinds = ("chunk", "manifest") if operation == "repair" else (
                        "chunk", "manifest", "revision", "refs", "config", "settings")
                    if operation == "create":
                        kinds += ("descriptor",)
                    cases = [(phase, kind, 2 if kind == "refs" else 1) for kind in kinds for phase in PHASES]
                    if os.name != "nt":
                        cases += [("atomic.linked", kind, 1) for kind in kinds if kind != "refs"]
                    if operation != "repair":
                        cases += [(phase, "backup" if operation == "create" else "restored", 1) for phase in
                                  ("directory.created", "directory.writer_finished", "directory.synced", "directory.published", "directory.durable")]
                    for phase, kind, occurrence in cases:
                        with self.subTest(encrypted=encrypted, operation=operation, phase=phase, kind=kind):
                            root = parent / f"case-{count}"
                            count += 1
                            shutil.copytree(seed, root)
                            process = subprocess.Popen([PROBE, "run", str(root), phase, kind, str(occurrence)],
                                env=ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                            lines = queue.Queue()
                            thread = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                            thread.start()
                            try:
                                line = lines.get(timeout=15)
                                self.assertTrue(line, "backup probe did not reach the requested checkpoint")
                                event = json.loads(line)
                                self.assertEqual((event["phase"], event["kind"]), (phase, kind))
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
                            self.assertEqual(snapshot(root), before, "fresh reader modified crash evidence")
                            self.assertTrue(self.command("resume", root)["verified"])
            print(f"{count} backup/restore/repair hard termination boundaries verified", flush=True)


if __name__ == "__main__":
    unittest.main()
