"""Native CLI status/diff, snapshot selection and read-only contracts."""
import hashlib
import json
import os
import subprocess
import unittest

import workflows as base


class NativeWorkflows(unittest.TestCase):
    setUp = base.Workflows.setUp
    tearDown = base.Workflows.tearDown
    cmd = base.Workflows.cmd

    def call(self, *args, ok=True):
        result = subprocess.run([base.CLI, "--store", str(self.store), *map(str, args)],
                                cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        if not ok:
            self.assertEqual(result.stdout, b"")
        return result

    def snapshot(self):
        return {p.relative_to(self.store): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.store.rglob("*") if p.is_file() and p.name != "store.lock"}

    def commit(self, message):
        return self.cmd("native", "commit", "-m", message).decode().strip()

    def test_unborn_status_content_changes_diff_progress_and_languages(self):
        initial = json.loads(self.call("native", "status").stdout)
        self.assertTrue(initial["clean"])
        self.assertIsNone(initial["base"])
        self.assertEqual(initial["workspace_files"], 0)
        (self.root / "edited.bin").write_bytes(b"old\x00data")
        (self.root / "removed.txt").write_text("removed", encoding="utf-8")
        (self.root / "Éditeur.cpp").write_text("source\n", encoding="utf-8")
        first = self.commit("first")
        self.assertTrue(json.loads(self.call("native", "status").stdout)["clean"])
        stat = (self.root / "edited.bin").stat()
        (self.root / "edited.bin").write_bytes(b"new\x00data")
        os.utime(self.root / "edited.bin", ns=(stat.st_atime_ns, stat.st_mtime_ns))
        (self.root / "removed.txt").unlink()
        (self.root / "added.txt").write_text("new", encoding="utf-8")
        before = self.snapshot()
        run = self.call("native", "status", "--progress-json")
        report = json.loads(run.stdout)
        self.assertEqual(report["base"], first)
        self.assertEqual(report["counts"], {"untracked": 1, "removed": 1, "modified": 1, "unchanged": 1})
        self.assertEqual([r["path"] for r in report["changes"]], ["added.txt", "edited.bin", "removed.txt"])
        events = [json.loads(line) for line in run.stderr.splitlines()]
        self.assertTrue(any(e["phase"] == "scanning" for e in events))
        self.assertTrue(any(e["phase"] == "verifying" and e["path"] == "edited.bin" for e in events))
        self.assertEqual(events[-1]["phase"], "planning")
        self.assertTrue(events[-1]["complete"])
        french = self.call("--lang", "fr", "native", "status", "--revision", first)
        self.assertEqual(json.loads(french.stdout), report)
        unborn = json.loads(self.call("native", "status", "--branch", "future").stdout)
        self.assertIsNone(unborn["base"])
        self.assertEqual(unborn["counts"]["untracked"], 3)
        self.assertEqual(before, self.snapshot())
        second = self.commit("second")
        before = self.snapshot()
        delta = self.call("native", "diff", first, second, "--progress-json")
        comparison = json.loads(delta.stdout)
        self.assertEqual(comparison["counts"], {"added": 1, "removed": 1, "modified": 1, "unchanged": 1})
        self.assertFalse(comparison["equal"])
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "diff", first, second).stdout), comparison)
        self.assertTrue(json.loads(self.call("native", "diff", first, first).stdout)["equal"])
        self.assertEqual(before, self.snapshot())

    def test_nested_ignores_match_commit_and_tracked_files_can_be_excluded(self):
        (self.root / ".syrnckignore").write_text("*.tmp\n!keep.tmp\n", encoding="utf-8")
        (self.root / "nested").mkdir()
        (self.root / "nested/.syrnckignore").write_text("*.txt\n!include.txt\n", encoding="utf-8")
        for name in ("keep.tmp", "ignored.tmp", "tracked.txt", "nested/excluded.txt", "nested/include.txt"):
            (self.root / name).write_text(name, encoding="utf-8")
        first = self.commit("ignore rules")
        self.assertTrue(json.loads(self.call("native", "status").stdout)["clean"])
        (self.root / ".syrnckignore").write_text("*.tmp\n!keep.tmp\ntracked.txt\n", encoding="utf-8")
        result = json.loads(self.call("native", "status").stdout)
        self.assertEqual([(r["path"], r["status"]) for r in result["changes"]],
                         [(".syrnckignore", "modified"), ("tracked.txt", "removed")])
        second = self.commit("exclude tracked file")
        comparison = json.loads(self.call("native", "diff", first, second).stdout)
        self.assertEqual([(r["path"], r["status"]) for r in comparison["changes"]],
                         [(".syrnckignore", "modified"), ("tracked.txt", "removed")])
        self.assertTrue((self.root / "tracked.txt").exists())

    def test_empty_snapshot_and_empty_file_addition(self):
        first = self.commit("empty snapshot")
        report = json.loads(self.call("native", "status").stdout)
        self.assertEqual(report["base"], first)
        self.assertTrue(report["clean"])
        (self.root / "empty").write_bytes(b"")
        report = json.loads(self.call("native", "status").stdout)
        self.assertEqual(report["hashed_bytes"], 0)
        self.assertEqual(report["workspace_files"], 1)
        self.assertEqual(report["changes"][0]["target"]["size"], 0)
        self.assertEqual(report["changes"][0]["status"], "untracked")
        second = self.commit("empty file added")
        report = json.loads(self.call("native", "diff", first, second).stdout)
        self.assertEqual(report["counts"], {"added": 1, "removed": 0, "modified": 0, "unchanged": 0})

    def test_ignore_character_classes_and_invalid_ranges_are_portable(self):
        for name in ("a", "b", "z", "-", "payload"):
            (self.root / name).write_bytes(b"retained")
        ignore = self.root / ".syrnckignore"
        for pattern, expected in (("[a-c]", {"-", "z", "payload"}),
                                  ("[!a-c]", {"a", "b", "payload"}),
                                  ("[a-cx-z]", {"-", "payload"}),
                                  ("[-ab]", {"z", "payload"}),
                                  ("[ab-]", {"z", "payload"})):
            with self.subTest(pattern=pattern):
                ignore.write_text(pattern + "\n", encoding="utf-8")
                report = json.loads(self.call("native", "status").stdout)
                self.assertEqual({entry["path"] for entry in report["changes"]}, expected | {".syrnckignore"})
        before = self.snapshot()
        for pattern in ("[z-a]", "[9-0]", "[!z-a]", "[]", "[!]"):
            with self.subTest(pattern=pattern):
                ignore.write_text(pattern + "\n", encoding="utf-8")
                for command in (("native", "status"), ("native", "commit", "-m", "invalid ignore")):
                    error = self.call(*command, ok=False)
                    self.assertIn(b"invalid ignore pattern", error.stderr)
                self.assertEqual(self.snapshot(), before)

    def test_argument_errors_missing_metadata_and_wrong_branch_kind(self):
        (self.root / "file").write_bytes(b"retained")
        revision = self.commit("first")
        value = json.loads(self.cmd("inspect", "revision", revision))
        manifest = value["files"]["file"]["manifest"]
        self.cmd("pin", "heads/wrong", "manifest", manifest)
        before = self.snapshot()
        error = self.call("--lang", "fr", "native", "status", "--branch", "main", "--revision", revision, ok=False)
        self.assertIn("choisissez --branch ou --revision", error.stderr.decode("utf-8"))
        self.call("native", "status", "--branch", "wrong", ok=False)
        self.call("native", "status", "--revision", "invalid", ok=False)
        self.call("native", "status", "--revision", "0" * 64, ok=False)
        self.call("native", "diff", revision, "0" * 64, ok=False)
        self.call("native", "diff", revision, ok=False)
        self.call("native", "status", "--directory", self.root / "absent", ok=False)
        self.assertEqual(before, self.snapshot())

    def test_file_directory_transitions_are_reported_per_path(self):
        (self.root / "file").write_bytes(b"old file")
        (self.root / "directory").mkdir()
        (self.root / "directory/child").write_bytes(b"old child")
        first = self.commit("before path type changes")
        (self.root / "file").unlink()
        (self.root / "file").mkdir()
        (self.root / "file/child").write_bytes(b"new child")
        (self.root / "directory/child").unlink()
        (self.root / "directory").rmdir()
        (self.root / "directory").write_bytes(b"new file")
        report = json.loads(self.call("native", "status").stdout)
        self.assertEqual([(r["path"], r["status"]) for r in report["changes"]],
                         [("directory", "untracked"), ("directory/child", "removed"),
                          ("file", "removed"), ("file/child", "untracked")])
        second = self.commit("after path type changes")
        report = json.loads(self.call("native", "diff", first, second).stdout)
        self.assertEqual(report["counts"], {"added": 2, "removed": 2, "modified": 0, "unchanged": 0})

    def test_symlinks_are_rejected_unless_excluded(self):
        target, link = self.root / "target", self.root / "link"
        target.write_bytes(b"target")
        try:
            link.symlink_to(target)
        except OSError:
            self.skipTest("symlink creation requires an unavailable platform privilege")
        self.call("native", "status", ok=False)
        (self.root / ".syrnckignore").write_text("link\n", encoding="utf-8")
        result = json.loads(self.call("native", "status").stdout)
        self.assertEqual([r["path"] for r in result["changes"]], [".syrnckignore", "target"])

    def test_ignore_configuration_must_be_a_regular_file(self):
        ignore = self.root / ".syrnckignore"
        ignore.mkdir()
        before = self.snapshot()
        self.call("native", "status", ok=False)
        self.call("native", "commit", "-m", "invalid ignore directory", ok=False)
        ignore.rmdir()
        if hasattr(os, "mkfifo"):
            os.mkfifo(ignore)
            self.call("native", "status", ok=False)
            self.call("native", "commit", "-m", "invalid ignore FIFO", ok=False)
            ignore.unlink()
        self.assertEqual(before, self.snapshot())

    def test_portable_unicode_file_directory_collision(self):
        composed, decomposed = self.root / "é", self.root / "e\u0301"
        composed.write_bytes(b"file")
        if decomposed.exists():
            self.skipTest("filesystem already folds canonically equivalent names")
        decomposed.mkdir()
        (decomposed / "child").write_bytes(b"child")
        before = self.snapshot()
        error = self.call("native", "status", ok=False)
        self.assertIn(b"file/directory path collision", error.stderr)
        self.assertEqual(before, self.snapshot())
        self.call("native", "commit", "-m", "invalid portable paths", ok=False)
        self.assertEqual(json.loads(self.cmd("refs")), {})


if __name__ == "__main__":
    unittest.main()
