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

    def test_frozen_staging_ignores_later_working_changes_and_preserves_stale_base(self):
        path = self.root / "code.cpp"
        path.write_bytes(b"old\n")
        first = self.commit("base")
        self.call("native", "branch", "feature", first)
        path.write_bytes(b"prepared\n")
        index = self.call("native", "add", path.name, "--branch", "feature").stdout.decode().strip()
        path.write_bytes(b"later working bytes\n")
        before = self.snapshot()
        report = json.loads(self.call("native", "diff-text", "--staged", path.name, "--branch", "feature").stdout)
        self.assertEqual(report["base"], first)
        self.assertEqual(report["index"], index)
        self.assertFalse(report["stale"])
        self.assertEqual(report, json.loads(self.call("--lang", "fr", "native", "diff-text", "--staged", path.name, "--branch", "feature").stdout))
        patch = self.call("native", "diff-text", "--staged", path.name, "--branch", "feature", "--unified").stdout
        self.apply_patch(patch, path.name, b"old\n", b"prepared\n")
        self.assertNotIn(b"later working", patch)
        self.assertEqual(before, self.snapshot())
        main = json.loads(self.call("native", "diff-text", "--staged", path.name).stdout)
        self.assertIsNone(main["index"])
        self.assertTrue(main["equal"])
        second = self.commit("main advances")
        # Advance the head through the generic pin API, leaving the frozen
        # index on its original baseline to exercise stale inspection.
        self.call("pin", "heads/feature", "revision", second)
        stale = json.loads(self.call("native", "diff-text", "--staged", path.name, "--branch", "feature").stdout)
        self.assertTrue(stale["stale"])
        self.assertEqual(stale["base"], first)
        self.assertEqual(stale["head"], second)
        self.assertEqual(stale["diff"], report["diff"])
        self.assertEqual(path.read_bytes(), b"later working bytes\n")

    def test_initial_staging_and_prepared_deletion(self):
        path = self.root / "initial.cpp"
        path.write_bytes(b"initial\n")
        self.call("native", "add", path.name)
        report = json.loads(self.call("native", "diff-text", "--staged", path.name).stdout)
        self.assertIsNone(report["base"])
        self.assertIsNone(report["source"])
        self.assertFalse(report["stale"])
        self.apply_patch(self.call("native", "diff-text", "--staged", path.name, "--unified").stdout,
                         path.name, None, b"initial\n")
        self.call("native", "commit", "--staged", "-m", "initial")
        path.unlink()
        self.call("native", "add", path.name)
        before = self.snapshot()
        removed = json.loads(self.call("native", "diff-text", "--staged", path.name).stdout)
        self.assertIsNone(removed["target"])
        self.apply_patch(self.call("native", "diff-text", "--staged", path.name, "--unified").stdout,
                         path.name, b"initial\n", None)
        self.assertEqual(before, self.snapshot())

    def test_cli_diff_budgets_apply_to_revisions_and_staging(self):
        path = self.root / "code.cpp"
        path.write_bytes(b"old\n")
        first = self.commit("base")
        path.write_bytes(b"changed\nsecond\n")
        self.call("native", "add", path.name)
        before = self.snapshot()
        staged = json.loads(self.call("native", "diff-text", "--staged", path.name, "--max-lines", "1").stdout)
        self.assertEqual(staged["diff"]["reason"], "line_limit")
        self.assertEqual(before, self.snapshot())
        second = self.call("native", "commit", "--staged", "-m", "changed").stdout.decode().strip()
        before = self.snapshot()
        for option, value, reason in [("--max-file-bytes", "1", "file_limit"),
                                      ("--max-lines", "1", "line_limit"),
                                      ("--max-work", "1", "work_limit")]:
            result = json.loads(self.call("native", "diff-text", first, second, path.name, option, value).stdout)
            self.assertEqual(result["diff"]["reason"], reason)
            self.assertEqual(result["diff"]["hunks"], [])
        for option, value in [("--max-file-bytes", "4194305"), ("--max-lines", "0"),
                              ("--max-work", "0"), ("--max-output-bytes", "1023")]:
            self.call("--lang", "fr", "native", "diff-text", first, second, path.name, option, value, ok=False)
        self.assertEqual(before, self.snapshot())


if __name__ == "__main__":
    unittest.main()
