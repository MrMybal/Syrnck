"""Terminate detached HEAD entry, exit and movement at every observed IO boundary."""
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

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
LINKED = "--linked" in sys.argv
if LINKED:
    sys.argv.remove("--linked")
SHARD = CrashShard.from_argv()
ENV = {key: value for key, value in os.environ.items() if key.upper() not in
       ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and p.name != "store.lock"}


class DetachedCrashes(unittest.TestCase):
    def command(self, executable, mode, root, *arguments):
        result = subprocess.run([executable, mode, str(root), *map(str, arguments)], env=ENV,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout) if result.stdout else None

    def test_all_observed_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-detached-crash-") as temporary:
            parent = Path(temporary)
            count = 0
            verified = 0
            actions = [(action, operation) for action in ("enter", "leave", "move")
                       for operation in ("apply", "abort")]
            if LINKED:
                actions.append(("register", "apply"))
            scenarios = [(encrypted, action, operation) for encrypted in (False, True)
                         for action, operation in actions]
            for encrypted, action, operation in SHARD.select(scenarios):
                seed = parent / f"seed-{encrypted}-{action}-{operation}"
                self.command(READER, "prepare", seed, "encrypted" if encrypted else "plain", action, operation,
                             *(["linked"] if LINKED else []))
                trace = parent / f"trace-{encrypted}-{action}-{operation}"
                shutil.copytree(seed, trace)
                self.command(READER, "bind", trace)
                events = self.command(PROBE, "trace", trace)
                self.assertTrue(events)
                self.assertTrue(all(e["kind"] in {"refs", "config", "chunk", "manifest", "revision", "new", "old", "workspace", "checkout"}
                                    for e in events), events)
                coordinates = {(e["phase"], e["kind"], e["occurrence"]) for e in events}
                self.assertEqual(len(coordinates), len(events))
                if action != "register":
                    self.assertTrue(any(e["phase"] == "checkout.cleaned" for e in events))
                for event in events:
                    with self.subTest(encrypted=encrypted, action=action, operation=operation, event=event):
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
                            line = lines.get(timeout=15)
                            self.assertTrue(line, "detached probe missed checkpoint")
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
            print(f"{verified}/{count} detached process termination boundaries verified", flush=True)


if __name__ == "__main__":
    unittest.main()
