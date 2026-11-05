"""External patch CLI, native/Git interoperability, plan binding and retention."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import native_workflows as base
import workflows

PATCH = b"--- a/code.cpp\n+++ b/code.cpp\n@@ -1 +1 @@\n-old\n+new\n"


class NativePatch(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, path="code.cpp", *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "add-patch-plan", path,
                                 "--patch", "change.patch", *map(str, args)], cwd=self.root, env=self.env,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        value = json.loads(result.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def fixture(self):
        (self.root / "code.cpp").write_bytes(b"old\n")
        (self.root / "other.txt").write_bytes(b"original")
        head = self.commit("baseline")
        (self.root / "change.patch").write_bytes(PATCH)
        return head

    def test_native_generated_patch_preserves_preparations_workspace_and_archive(self):
        code = self.root / "Source/Edited source.cpp"
        code.parent.mkdir()
        old, new = b"one\r\ntwo\r\nlast", b"one\r\nchanged\r\nlast\n"
        code.write_bytes(old)
        (self.root / "other.txt").write_bytes(b"original")
        first = self.commit("old")
        code.write_bytes(new)
        second = self.commit("new")
        patch = self.call("native", "diff-text", first, second, "Source/Edited source.cpp", "--unified").stdout
        self.call("pin", "heads/main", "revision", first)
        (self.root / "other.txt").write_bytes(b"prepared other")
        previous = self.call("native", "add", "other.txt").stdout.decode().strip()
        (self.root / "change.patch").write_bytes(patch)
        code.unlink()
        before = self.snapshot()
        plan = self.plan("Source/Edited source.cpp")
        self.assertEqual(plan, self.plan("Source/Edited source.cpp", "--lang", "fr"))
        self.assertEqual(before, self.snapshot())
        result = self.call("--lang", "fr", "native", "add-patch", "Source/Edited source.cpp", "--patch", "change.patch",
                           "--expect-plan", plan["plan_id"], "--progress-json")
        applied = json.loads(result.stdout)
        self.assertTrue(any(json.loads(row)["phase"] == "publishing" for row in result.stderr.splitlines()))
        self.assertEqual(applied["previous_index"], previous)
        self.call("gc", "--delete")
        self.call("native", "export", applied["index"], "frozen")
        self.assertEqual((self.root / "frozen/Source/Edited source.cpp").read_bytes(), new)
        self.assertEqual((self.root / "frozen/other.txt").read_bytes(), b"prepared other")
        self.call("native", "archive-show", "archive/stage-index/" + previous, "--verify")
        self.call("native", "commit", "--staged", "-m", "external patch")
        self.call("gc", "--delete")
        self.call("native", "export", previous, "previous")
        self.assertEqual((self.root / "previous/Source/Edited source.cpp").read_bytes(), old)
        self.assertFalse(code.exists())

    def git_patch(self, name, old, new):
        with tempfile.TemporaryDirectory(prefix="syrnck-git-patch-") as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True, capture_output=True)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(old)
            subprocess.run(["git", "-c", "core.autocrlf=false", "add", "--", name], cwd=root, check=True, capture_output=True)
            path.write_bytes(new)
            diff = subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.quotepath=false", "diff",
                                   "--no-ext-diff", "--no-textconv", "--", name], cwd=root, check=True, capture_output=True)
            return diff.stdout

    def test_git_generated_patches_unicode_spaces_empty_contents_and_exact_newlines(self):
        cases = [("Source/\u00c9dit source.cpp", b"old\r\nlast", b"new\r\nlast\n"),
                 ("empty.cpp", b"", b"first\nsecond\r\n"),
                 ("cleared.cpp", b"original\r\n", b""),
                 ("final.cpp", b"same\n", b"same"),
                 ("Source/insert.cpp", b"one\ntwo\nthree\n", b"zero\none\nthree\nlast\n")]
        for name, old, new in cases:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(old)
        self.commit("all old versions")
        for n, (name, old, new) in enumerate(cases):
            with self.subTest(name=name):
                patch = self.git_patch(name, old, new)
                self.assertTrue(patch)
                (self.root / "change.patch").write_bytes(patch)
                (self.root / name).write_bytes(b"binary\0working edit")
                plan = self.plan(name)
                applied = json.loads(self.call("native", "add-patch", name, "--patch", "change.patch", "--expect-plan", plan["plan_id"]).stdout)
                self.call("native", "export", applied["index"], "frozen-" + str(n))
                self.assertEqual((self.root / ("frozen-" + str(n)) / name).read_bytes(), new)
                self.assertEqual((self.root / name).read_bytes(), b"binary\0working edit")
        committed = self.call("native", "commit", "--staged", "-m", "Git patches").stdout.decode().strip()
        self.call("gc", "--delete")
        self.call("native", "export", committed, "complete")
        for name, old, new in cases:
            self.assertEqual((self.root / "complete" / name).read_bytes(), new)

    def test_invalid_patch_input_options_drift_and_stale_index_do_not_write(self):
        head = self.fixture()
        plan = self.plan()
        patchfile = self.root / "change.patch"
        before = self.snapshot()
        self.call("native", "add-patch", "code.cpp", "--patch", "change.patch", ok=False)
        self.call("native", "add-patch-plan", "code.cpp", ok=False)
        for option in (("--context", "0"), ("--directory", "."), ("--max-file-bytes", "1")):
            self.call("native", "add-patch", "code.cpp", "--patch", "change.patch", "--expect-plan", plan["plan_id"], *option, ok=False)
        patchfile.write_bytes(PATCH.replace(b"+new", b"+another"))
        self.call("native", "add-patch", "code.cpp", "--patch", "change.patch", "--expect-plan", plan["plan_id"], ok=False)
        for bad in (PATCH[:-1], PATCH.replace(b"-old", b"-wrong"), PATCH + PATCH,
                    PATCH.replace(b"a/code.cpp", b"a/../code.cpp"), PATCH.replace(b"@@ -1 +1", b"@@ -1 +2")):
            patchfile.write_bytes(bad)
            self.call("native", "add-patch-plan", "code.cpp", "--patch", "change.patch", ok=False)
        self.assertEqual(before, self.snapshot())
        patchfile.write_bytes(PATCH)
        (self.root / "other.txt").write_bytes(b"prepared other")
        self.call("native", "add", "other.txt")
        before = self.snapshot()
        self.call("native", "add-patch", "code.cpp", "--patch", "change.patch", "--expect-plan", plan["plan_id"], ok=False)
        self.assertEqual(before, self.snapshot())
        second = self.call("native", "commit", "--staged", "-m", "advance").stdout.decode().strip()
        self.call("pin", "heads/main", "revision", head)
        self.call("native", "add", "other.txt")
        self.call("pin", "heads/main", "revision", second)
        blocked = self.plan(ready=False)
        self.assertIn("stale_staging", [b["reason"] for b in blocked["blockers"]])
        before = self.snapshot()
        self.call("--lang", "fr", "native", "add-patch", "code.cpp", "--patch", "change.patch", "--expect-plan", blocked["plan_id"], ok=False)
        self.assertEqual(before, self.snapshot())

    def test_explicit_branch_and_noop_plan(self):
        head = self.fixture()
        self.call("native", "branch", "feature", head)
        plan = self.plan("code.cpp", "--branch", "feature", "--context", "0")
        applied = json.loads(self.call("native", "add-patch", "code.cpp", "--patch", "change.patch", "--expect-plan", plan["plan_id"],
                                      "--branch", "feature", "--context", "0").stdout)
        self.call("native", "export", applied["index"], "feature")
        self.assertEqual((self.root / "feature/code.cpp").read_bytes(), b"new\n")
        self.assertIsNone(json.loads(self.call("native", "staged").stdout)["snapshot"])
        (self.root / "change.patch").write_bytes(PATCH.replace(b"+new", b"+old"))
        self.assertIn("no_text_changes", [b["reason"] for b in self.plan(ready=False)["blockers"]])


if __name__ == "__main__":
    unittest.main()
