"""Targeted native working diffs, namespace guards and exact unified output."""
import json
import os
import subprocess
import unittest
import native_text_diff as base


class NativeWorkingDiff(unittest.TestCase):
    setUp = base.NativeTextDiff.setUp
    tearDown = base.NativeTextDiff.tearDown
    cmd = base.NativeTextDiff.cmd
    call = base.NativeTextDiff.call
    snapshot = base.NativeTextDiff.snapshot
    commit = base.NativeTextDiff.commit
    apply_patch = base.NativeTextDiff.apply_patch

    def test_selected_branch_index_and_explicit_head(self):
        name = "Source/Édit name.cpp"
        path = self.root / name
        path.parent.mkdir()
        path.write_bytes(b"old\n")
        # Some filesystems normalize Unicode names; use their actual spelling.
        path = next(path.parent.iterdir())
        name = path.relative_to(self.root).as_posix()
        first = self.commit("baseline")
        self.call("native", "branch", "feature", first)
        self.call("native", "switch", "feature")
        path.write_bytes(b"prepared\n")
        index = self.call("native", "add", name).stdout.decode().strip()
        path.write_bytes(b"working\r\n")
        before = self.snapshot()
        run = self.call("native", "diff-text", "--working", name, "--progress-json")
        report = json.loads(run.stdout)
        self.assertEqual(report["branch"], "feature")
        self.assertEqual(report["index"], index)
        self.assertEqual(report["source_revision"], index)
        self.assertTrue(report["workspace_verified"])
        self.assertEqual(report, json.loads(self.call("--lang", "fr", "native", "diff-text", "--working", name).stdout))
        events = [json.loads(line) for line in run.stderr.splitlines()]
        self.assertTrue(any(e["phase"] == "verifying" and e["path"] == name for e in events))
        self.assertTrue(events[-1]["complete"])
        patch = self.call("native", "diff-text", "--working", name, "--unified").stdout
        self.apply_patch(patch, name, b"prepared\n", b"working\r\n")
        explicit = json.loads(self.call("native", "diff-text", "--working", name, "--revision", "HEAD").stdout)
        self.assertEqual(explicit["source_revision"], first)
        self.assertNotIn("index", explicit)
        self.apply_patch(self.call("native", "diff-text", "--working", name, "--revision", "HEAD", "--unified").stdout,
                         name, b"old\n", b"working\r\n")
        other = json.loads(self.call("native", "diff-text", "--working", name, "--branch", "main").stdout)
        self.assertIsNone(other["index"])
        self.assertEqual(other["source_revision"], first)
        self.assertEqual(before, self.snapshot())
        self.call("native", "diff-text", "--working", "--staged", name, ok=False)
        self.call("native", "diff-text", "--working", name, "--revision", "HEAD", "--branch", "main", ok=False)

    def test_unborn_untracked_and_removed_files(self):
        path = self.root / "new.cpp"
        path.write_bytes(b"new\n")
        report = json.loads(self.call("native", "diff-text", "--working", path.name).stdout)
        self.assertIsNone(report["source_revision"])
        self.assertIsNone(report["source"])
        self.apply_patch(self.call("native", "diff-text", "--working", path.name, "--unified").stdout,
                         path.name, None, b"new\n")
        self.commit("tracked")
        path.unlink()
        before = self.snapshot()
        report = json.loads(self.call("native", "diff-text", "--working", path.name).stdout)
        self.assertIsNone(report["target"])
        self.assertTrue(report["workspace_verified"])
        self.apply_patch(self.call("native", "diff-text", "--working", path.name, "--unified").stdout,
                         path.name, b"new\n", None)
        self.assertEqual(before, self.snapshot())

    def test_ignored_paths_case_aliases_and_obstructions(self):
        path = self.root / "Source/code.cpp"
        path.parent.mkdir()
        path.write_bytes(b"old\n")
        self.commit("baseline")
        path.write_bytes(b"working\n")
        (self.root / ".syrnckignore").write_bytes(b"Source/\n")
        self.call("native", "diff-text", "--working", "Source/code.cpp", ok=False)
        # Snapshot comparisons remain independent of working ignore rules.
        self.call("native", "diff-text", "HEAD", "HEAD", "Source/code.cpp")
        (self.root / ".syrnckignore").unlink()
        self.call("native", "diff-text", "--working", "source/code.cpp", ok=False)
        self.call("native", "diff-text", "--working", "Source", ok=False)
        self.call("native", "diff-text", "--working", "Source/code.cpp/child", ok=False)
        self.call("native", "diff-text", "--working", ".syrnck/config.json", ok=False)
        self.call("native", "diff-text", "--working", "../unsafe", ok=False)

    def test_binary_and_large_files_are_explicit(self):
        path = self.root / "file.cpp"
        path.write_bytes(b"old\n")
        self.commit("baseline")
        before = self.snapshot()
        path.write_bytes(b"binary\0bytes")
        report = json.loads(self.call("native", "diff-text", "--working", path.name).stdout)
        self.assertEqual(report["diff"]["status"], "binary")
        self.assertTrue(report["workspace_verified"])
        self.call("native", "diff-text", "--working", path.name, "--unified", ok=False)
        path.write_bytes(b"x" * (1024 * 1024 + 1))
        report = json.loads(self.call("native", "diff-text", "--working", path.name).stdout)
        self.assertEqual(report["diff"]["reason"], "file_limit")
        self.assertFalse(report["workspace_verified"])
        self.assertIsNone(report["target"]["file_hash"])
        self.assertTrue(report["content_changed"])
        self.call("native", "diff-text", "--working", path.name, "--unified", ok=False)
        # Raising the input budget allows verification, while a separate output
        # budget still prevents an incomplete or unexpectedly huge patch.
        report = json.loads(self.call("native", "diff-text", "--working", path.name,
                                      "--max-file-bytes", "2097152", "--max-output-bytes", "2048").stdout)
        self.assertTrue(report["workspace_verified"])
        self.assertTrue(report["objects_verified"])
        self.assertEqual(report["diff"]["reason"], "output_limit")
        self.assertEqual(report["diff"]["hunks"], [])
        self.assertIsNotNone(report["target"]["file_hash"])
        self.call("native", "diff-text", "--working", path.name, "--max-file-bytes", "2097152",
                  "--max-output-bytes", "2048", "--unified", ok=False)
        self.assertEqual(before, self.snapshot())

    def test_links_are_opaque_and_unrelated_subtrees_are_not_scanned(self):
        path = self.root / "code.cpp"
        path.write_bytes(b"old\n")
        self.commit("baseline")
        path.write_bytes(b"working\n")
        destination = self.root / "destination"
        destination.mkdir()
        (destination / "payload.txt").write_bytes(b"outside\n")
        link = self.root / "Other"
        if os.name == "nt":
            result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(destination)],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        else:
            os.symlink(destination, link, target_is_directory=True)
        try:
            before = self.snapshot()
            self.call("native", "diff-text", "--working", path.name)
            self.call("native", "diff-text", "--working", "Other/payload.txt", ok=False)
            self.assertEqual(before, self.snapshot())
        finally:
            if os.name == "nt":
                os.rmdir(link)
            else:
                link.unlink()
        self.assertEqual((destination / "payload.txt").read_bytes(), b"outside\n")


if __name__ == "__main__":
    unittest.main()
