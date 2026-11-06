"""Opt-in persisted workspace hashes, CLI parity and safe fallback."""
import json
import os
import subprocess
import time
import unittest
import native_workflows as base
from workflows import CLI


class WorkingIndex(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    commit = base.NativeWorkflows.commit

    def indexed(self, *args):
        return json.loads(self.call("native", "status", "--indexed", *args).stdout)

    def assert_parity(self, indexed, full):
        indexed = dict(indexed)
        indexed.pop("working_index")
        indexed["hashed_bytes"] = full["hashed_bytes"]
        self.assertEqual(indexed, full)

    def test_persistence_same_time_edit_and_bilingual_output(self):
        path = self.root / "Éditeur.asset"
        path.write_bytes(b"a" * (1024 * 1024 + 7))
        self.commit("asset baseline")
        first = self.indexed()
        self.assertEqual(first["working_index"]["previous"], "missing")
        self.assertEqual(first["working_index"]["reused_files"], 0)
        time.sleep(3.1)
        second = self.indexed()
        self.assertEqual(second["working_index"]["previous"], "ready")
        if second["working_index"]["supported_files"] == 1:
            self.assertEqual(second["hashed_bytes"], 0)
            self.assertEqual(second["working_index"]["reused_files"], 1)
        self.assert_parity(second, json.loads(self.call("native", "status").stdout))
        french = json.loads(self.call("--lang", "fr", "native", "status", "--indexed").stdout)
        self.assertEqual(french, second)
        stat = path.stat()
        path.write_bytes(b"b" * path.stat().st_size)
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        changed = self.indexed("--progress-json")
        self.assertFalse(changed["clean"])
        self.assertGreaterEqual(changed["hashed_bytes"], path.stat().st_size)
        self.assert_parity(changed, json.loads(self.call("native", "status").stdout))

    def test_combined_uses_frozen_index(self):
        path = self.root / "file"
        path.write_bytes(b"base")
        self.commit("base")
        path.write_bytes(b"prepared")
        prepared = self.call("native", "add", ".").stdout.decode().strip()
        path.write_bytes(b"later")
        combined = self.indexed("--combined")
        full = json.loads(self.call("native", "status", "--combined").stdout)
        self.assertEqual(combined["staged"], full["staged"])
        self.assertEqual(combined["staged"]["snapshot"], prepared)
        self.assert_parity(combined["unstaged"], full["unstaged"])
        self.assertFalse(combined["clean"])

    def test_rebuild_clear_and_obstructed_pointer(self):
        (self.root / "file").write_bytes(b"contents")
        self.commit("base")
        self.indexed()
        refs = json.loads(self.cmd("refs"))
        pointer = self.store / "working-index.json"
        pointer.write_bytes(b"{}")
        rebuilt = self.indexed()
        self.assertEqual(rebuilt["working_index"]["previous"], "invalid")
        self.assertEqual(rebuilt["working_index"]["reused_files"], 0)
        self.assertEqual(json.loads(self.call("native", "index-clear").stdout)["state"], "cleared")
        self.assertEqual(json.loads(self.call("native", "index-clear").stdout)["state"], "absent")
        self.assertEqual(json.loads(self.cmd("refs")), refs)
        pointer.mkdir()
        result = self.call("--lang", "fr", "native", "status", "--indexed", ok=False)
        self.assertIn("pointeur de l'index de travail invalide", result.stderr.decode("utf-8"))
        self.assertTrue(pointer.is_dir())
        self.call("native", "index-clear", ok=False)

    def test_checkout_still_hashes_every_tracked_file(self):
        path = self.root / "asset"
        path.write_bytes(b"base")
        self.commit("base")
        path.write_bytes(b"next")
        target = self.call("native", "commit", "--branch", "target", "-m", "target").stdout.decode().strip()
        path.write_bytes(b"base")
        self.indexed()
        stat = path.stat()
        path.write_bytes(b"edit")
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        result = subprocess.run([CLI, "--store", str(self.store), "native", "checkout-plan", target],
                                cwd=self.root, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr.decode("utf-8", "replace"))
        plan = json.loads(result.stdout)
        self.assertFalse(plan["ready"])
        self.assertEqual(plan["hashed_bytes"], 4)
        self.assertTrue(any(row["reason"] == "tracked_modified" for row in plan["conflicts"]))
        self.assertEqual(path.read_bytes(), b"edit")


if __name__ == "__main__":
    unittest.main()
