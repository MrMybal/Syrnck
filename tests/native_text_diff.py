"""Native text diff, canonical JSON and unified output checked with Git apply."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import native_workflows as base


class NativeTextDiff(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def apply_patch(self, patch, name, before, after):
        with tempfile.TemporaryDirectory(prefix="syrnck-diff-apply-") as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True, capture_output=True)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if before is not None:
                path.write_bytes(before)
            applied = subprocess.run(["git", "-c", "core.autocrlf=false", "apply", "-"],
                                     cwd=root, input=patch, capture_output=True)
            self.assertEqual(applied.returncode, 0, applied.stderr.decode("utf-8", "replace"))
            if after is None:
                self.assertFalse(path.exists())
            else:
                self.assertEqual(path.read_bytes(), after)

    def test_revisions_json_languages_progress_and_unified_patch(self):
        path = self.root / "Édit source.cpp"
        old = b"one\r\ntwo\r\nthree"
        new = b"one\r\nchanged\r\nthree\r\n"
        path.write_bytes(old)
        first = self.commit("old")
        path.write_bytes(new)
        second = self.commit("new")
        before = self.snapshot()
        run = self.call("native", "diff-text", first, "HEAD", path.name, "--progress-json")
        report = json.loads(run.stdout)
        self.assertEqual(report["target_revision"], second)
        self.assertTrue(report["objects_verified"])
        self.assertFalse(report["equal"])
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "diff-text", first, "main", path.name).stdout), report)
        self.assertTrue(json.loads(run.stderr.splitlines()[-1])["complete"])
        patch = self.call("native", "diff-text", first, second, path.name, "--unified").stdout
        self.assertIn(b"\\ No newline at end of file\n", patch)
        self.assertEqual(patch, self.call("--lang", "fr", "native", "diff-text", first, second, path.name, "--unified").stdout)
        self.apply_patch(patch, path.name, old, new)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(self.call("native", "diff-text", second, second, path.name, "--unified").stdout, b"")
        self.call("native", "diff-text", first, second, path.name, "--context", "101", ok=False)
        self.call("native", "diff-text", first, second, "../unsafe", ok=False)

    def test_added_removed_and_binary_files(self):
        deleted = self.root / "deleted.cpp"
        deleted.write_bytes(b"removed\n")
        first = self.commit("before")
        deleted.unlink()
        (self.root / "added.cpp").write_bytes(b"added\n")
        (self.root / "asset.bin").write_bytes(b"binary\0payload")
        second = self.commit("after")
        for name, old, new in [("added.cpp", None, b"added\n"), ("deleted.cpp", b"removed\n", None)]:
            patch = self.call("native", "diff-text", first, second, name, "--unified").stdout
            self.apply_patch(patch, name, old, new)
        report = json.loads(self.call("native", "diff-text", first, second, "asset.bin").stdout)
        self.assertEqual(report["diff"]["status"], "binary")
        self.call("native", "diff-text", first, second, "asset.bin", "--unified", ok=False)
        self.call("native", "diff-text", first, second, "absent", ok=False)

    def test_default_byte_limit_is_explicit(self):
        first = self.commit("empty")
        (self.root / "large.cpp").write_bytes(b"x" * (1024 * 1024 + 1))
        second = self.commit("large")
        report = json.loads(self.call("native", "diff-text", first, second, "large.cpp").stdout)
        self.assertEqual(report["diff"]["status"], "limited")
        self.assertEqual(report["diff"]["reason"], "file_limit")
        self.assertFalse(report["objects_verified"])
        self.call("native", "diff-text", first, second, "large.cpp", "--unified", ok=False)


if __name__ == "__main__":
    unittest.main()
