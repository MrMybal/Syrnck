"""Terminate signed native publications and recover without a signing private key."""
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
import time
import unittest
from crash_shard import CrashShard

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
SHARD = CrashShard.from_argv()
ENV = {key: value for key, value in os.environ.items() if key.upper() not in
       ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_USER_NAME", "SYRNCK_USER_EMAIL",
        "SYRNCK_USER_TIMEZONE", "SYRNCK_SIGNING_SEED")}


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and p.name != "store.lock"}


class AuthorshipCrashes(unittest.TestCase):
    def command(self, executable, mode, root, *arguments):
        result = subprocess.run([executable, mode, str(root), *map(str, arguments)], env=ENV,
                                capture_output=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_all_observed_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-authorship-crash-") as temporary:
            parent = Path(temporary)
            count = verified = 0
            scenarios = [(encrypted, scope, action) for encrypted in (False, True)
                         for scope in ("primary", "linked", "partial", "detached")
                         for action in ("commit", "amend", "rebase", "abort")]
            for encrypted, scope, action in SHARD.select(scenarios):
                label = f"{encrypted}-{scope}-{action}"
                seed = parent / ("seed-" + label)
                self.command(READER, "prepare", seed, "encrypted" if encrypted else "plain",
                             scope, action)
                trace = parent / ("trace-" + label)
                shutil.copytree(seed, trace)
                self.command(READER, "bind", trace)
                events = self.command(PROBE, "trace", trace)
                self.assertTrue(events)
                self.assertTrue(all(e["kind"] in {"refs", "config", "chunk", "manifest", "revision", "filesystem"}
                                    for e in events), events)
                self.assertEqual(len({(e["phase"], e["kind"], e["occurrence"]) for e in events}), len(events))
                self.assertTrue(any(e["kind"] == "refs" for e in events))
                for event in events:
                    with self.subTest(encrypted=encrypted, scope=scope, action=action, event=event):
                        root = parent / f"case-{count}"
                        count += 1
                        shutil.copytree(seed, root)
                        self.command(READER, "bind", root)
                        process = subprocess.Popen([PROBE, "run", str(root), event["phase"],
                            event["kind"], str(event["occurrence"])], env=ENV,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                        lines = queue.Queue()
                        reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                        reader.start()
                        try:
                            line = lines.get(timeout=20)
                            if not line:
                                _, error = process.communicate(timeout=8)
                                saved = Path(__file__).resolve().parents[1] / ".scratch" / "authorship-crash-failures" / str(time.time_ns())
                                try:
                                    shutil.copytree(root, saved)
                                    saved = str(saved)
                                except OSError as copy_error:
                                    saved = "copy failed: " + str(copy_error)
                                self.fail("authorship probe missed checkpoint: exit=" + str(process.returncode) +
                                          "; " + error.decode("utf-8", "replace") + "; fixture=" + saved)
                            self.assertEqual(json.loads(line), event)
                            self.assertIsNone(process.poll())
                            process.kill()
                            process.communicate(timeout=8)
                            self.assertNotEqual(process.returncode, 0)
                        finally:
                            if process.poll() is None:
                                process.kill()
                            process.communicate(timeout=8)
                            reader.join(timeout=5)
                        before = snapshot(root)
                        self.assertTrue(self.command(READER, "verify", root)["verified"])
                        self.assertEqual(snapshot(root), before, "fresh reader modified recovery evidence")
                        self.assertTrue(self.command(READER, "resume", root)["verified"])
                        verified += 1
            print(f"{verified}/{count} authorship process termination boundaries verified", flush=True)


if __name__ == "__main__":
    unittest.main()
