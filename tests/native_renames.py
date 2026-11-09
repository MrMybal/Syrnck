"""Exercise edited rename CLI workflows and kill processes with prepared targets."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

HELPER = str(Path(sys.argv.pop(2)).resolve())
import native_workflows as base


def source():
    return "".join(f"int value_{i} = {i};\n" for i in range(100)).encode()


class NativeRenames(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    snapshot = base.NativeWorkflows.snapshot

    def test_cli_diff_history_merge_and_limits(self):
        original = source()
        old, new = "Old-Édit.cpp", "New-édit.cpp"
        (self.root / old).write_bytes(original)
        first = self.cmd("native", "commit", "-m", "base").decode().strip()
        (self.root / old).rename(self.root / new)
        ours = original.replace(b"= 20;", b"= 98;")
        (self.root / new).write_bytes(ours)
        current = self.cmd("native", "commit", "-m", "rename and edit").decode().strip()
        before = self.snapshot()
        report = json.loads(self.call("native", "diff", first, current, "--rename-similarity", 50).stdout)
        self.assertEqual(report["counts"]["renamed"], 1)
        self.assertTrue(report["changes"][0]["content_changed"])
        history = json.loads(self.call("native", "log-file", current, new, "--rename-similarity", 50).stdout)
        self.assertEqual([row["revision"] for row in history["entries"]], [current, first])
        blame = json.loads(self.call("native", "blame", current, new, "--rename-similarity", 50).stdout)
        self.assertEqual(blame["status"], "complete")
        self.assertEqual([line["revision"] for line in blame["lines"]],
                         [current if i == 20 else first for i in range(100)])
        for flags in [("--rename-similarity", 0), ("--rename-similarity", 4294967346),
                      ("--rename-max-work", 10), ("--rename-similarity", 50, "--rename-max-work", 1)]:
            self.call("native", "diff", first, current, *flags, ok=False)
        french = self.call("--lang", "fr", "native", "diff", first, current, "--rename-similarity", 0, ok=False)
        self.assertIn("similarité de renommage".encode(), french.stderr)
        self.assertEqual(before, self.snapshot())
        self.call("native", "checkout", first)
        (self.root / old).write_bytes(original.replace(b"= 70;", b"= 99;"))
        incoming = self.cmd("native", "commit", "-m", "edit old path").decode().strip()
        self.call("native", "checkout", current)
        plan = json.loads(self.call("native", "merge-plan", incoming, "--rename-similarity", 50, "--text-merge").stdout)
        self.assertTrue(plan["ready"])
        self.assertEqual(plan["renames"]["renames"][0]["old_path"], old)
        self.call("native", "merge", incoming, "--rename-similarity", 50, "--text-merge", "-m", "merged")
        self.assertFalse((self.root / old).exists())
        self.assertEqual((self.root / new).read_bytes(), ours.replace(b"= 70;", b"= 99;"))

    def test_process_kill_retains_rename_targets_for_resume_and_abort(self):
        original = source()
        for encrypted in (False, True):
            for kind in ("cherry-pick", "revert"):
                for abort in (False, True):
                    with self.subTest(encrypted=encrypted, kind=kind, abort=abort):
                        root = self.root / f"crash-{encrypted}-{kind}-{abort}"
                        environment = {name: value for name, value in self.env.items() if name.upper() not in
                            {"SYRNCK_KEY_FILE", "SYRNCK_KEY_PASSWORD", "SYRNCK_NEW_KEY_PASSWORD"}}
                        if encrypted:
                            environment["SYRNCK_KEY"] = bytes([75] * 32).hex()
                        else:
                            environment.pop("SYRNCK_KEY", None)
                        startup = {}
                        if os.name == "nt":
                            startup["creationflags"] = subprocess.CREATE_NO_WINDOW
                        with tempfile.TemporaryFile() as diagnostics:
                            process = subprocess.Popen([HELPER, "--crash-probe", str(root), kind,
                                "encrypted" if encrypted else "plain"], stdout=subprocess.PIPE, stderr=diagnostics,
                                env=environment, **startup)
                            events = queue.Queue()
                            def read_events():
                                for line in process.stdout:
                                    events.put(line)
                                events.put(b"")
                            reader = threading.Thread(target=read_events, daemon=True)
                            reader.start()
                            try:
                                prepared = json.loads(events.get(timeout=20))
                                checkpoint = json.loads(events.get(timeout=20))
                                self.assertEqual(prepared["event"], "prepared")
                                self.assertEqual(checkpoint["event"], "checkpoint")
                                self.assertIsNone(process.poll())
                                process.kill()
                                self.assertNotEqual(process.wait(timeout=10), 0)
                            finally:
                                if process.poll() is None:
                                    process.kill()
                                process.wait(timeout=10)
                                reader.join(timeout=5)
                                process.stdout.close()
                            def cli(*args, ok=True):
                                result = subprocess.run([base.base.CLI, "--store", str(root / ".syrnck"), *args],
                                    cwd=root, env=environment, capture_output=True, timeout=20)
                                self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
                                if not ok:
                                    return result.stderr
                                if args[0] == "fsck":
                                    self.assertIn(b"Verified", result.stdout)
                                    return result.stdout
                                return json.loads(result.stdout)
                            self.assertTrue(cli("native", "checkout-status")["pending"])
                            self.assertIn(b"native checkout is pending", cli("gc", "--delete", ok=False))
                            cli("native", "checkout-recover", *(["--abort"] if abort else []))
                            self.assertFalse(cli("native", "checkout-status")["pending"])
                            inverse = kind == "revert"
                            before = (original.replace(b"= 70;", b"= 99;") if inverse else original).replace(b"= 20;", b"= 98;")
                            expected = before if abort else (original if inverse else original.replace(b"= 70;", b"= 99;")).replace(b"= 20;", b"= 98;")
                            name = ("new.cpp" if inverse else "old.cpp") if abort else ("old.cpp" if inverse else "new.cpp")
                            self.assertEqual((root / name).read_bytes(), expected)
                            refs = cli("refs")
                            self.assertEqual(refs["heads/main"]["id"] == prepared["head"], abort)
                            cli("gc", "--delete")
                            cli("fsck")


if __name__ == "__main__":
    unittest.main()
