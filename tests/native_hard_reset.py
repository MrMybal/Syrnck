"""Hard reset keeps recoverable dirty/index versions and uses exact content fences."""
import json
import os
import unittest
import native_workflows as base


class HardReset(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def fixture(self):
        for name, content in {"a": b"target a", "missing": b"restore missing", "shape": b"target flat"}.items():
            (self.root / name).write_bytes(content)
        target = self.commit("target")
        (self.root / "shape").unlink()
        (self.root / "shape").mkdir()
        (self.root / "shape/child").write_bytes(b"head nested")
        (self.root / "a").write_bytes(b"head a")
        head = self.commit("head")
        (self.root / "a").write_bytes(b"staged a")
        (self.root / "new").write_bytes(b"staged new")
        index = self.call("native", "add", "a", "new").stdout.decode().strip()
        (self.root / "a").write_bytes(b"working a")
        (self.root / "new").write_bytes(b"working new")
        (self.root / "missing").unlink()
        (self.root / "shape/untracked").write_bytes(b"save obstruction")
        (self.root / "shape/empty/deeper").mkdir(parents=True)
        (self.root / "keep.tmp").write_bytes(b"keep untracked")
        (self.root / ".syrnckignore").write_text("a\nshape\n*.tmp\n", encoding="utf-8")
        return target, head, index

    def test_plan_apply_french_and_all_recovery_versions_after_gc(self):
        target, head, index = self.fixture()
        before = self.snapshot()
        plan = json.loads(self.call("native", "reset-plan", target, "--hard", "--verify").stdout)
        self.assertTrue(plan["ready"])
        self.assertEqual(self.snapshot(), before)
        result = json.loads(self.call("--lang", "fr", "native", "reset", target, "--hard",
                                      "--expect", head, "--expect-plan", plan["plan"]).stdout)
        self.assertEqual(result["version"], 5)
        self.assertEqual(result["original_head"], head)
        self.assertEqual(result["previous_index"], index)
        self.assertEqual((self.root / "a").read_bytes(), b"target a")
        self.assertEqual((self.root / "shape").read_bytes(), b"target flat")
        self.assertEqual((self.root / "missing").read_bytes(), b"restore missing")
        self.assertFalse((self.root / "new").exists())
        self.assertEqual((self.root / "keep.tmp").read_bytes(), b"keep untracked")
        refs = json.loads(self.cmd("refs"))
        self.assertNotIn("staging/main", refs)
        self.assertEqual(refs[result["archive"]]["id"], result["recovery_revision"])
        self.call("gc", "--delete")
        for revision, name, content in ((result["recovery_revision"], "working", b"working a"),
                                        (index, "staged", b"staged a"), (head, "head", b"head a")):
            self.call("native", "export", revision, self.store / name)
            self.assertEqual((self.store / name / "a").read_bytes(), content)
        self.assertEqual((self.store / "working/shape/untracked").read_bytes(), b"save obstruction")
        self.assertEqual((self.store / "working/new").read_bytes(), b"working new")
        before = self.snapshot()
        same = json.loads(self.call("native", "reset", "HEAD", "--hard").stdout)
        self.assertEqual(same["state"], "unchanged")
        self.assertEqual(self.snapshot(), before)

    def test_stale_plan_checks_bytes_even_with_restored_size_and_timestamp(self):
        target, head, _ = self.fixture()
        plan = json.loads(self.call("native", "reset-plan", target, "--hard").stdout)
        file = self.root / "a"
        stat = file.stat()
        file.write_bytes(b"Working a")
        os.utime(file, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        before = self.snapshot()
        result = self.call("--lang", "fr", "native", "reset", target, "--hard",
                           "--expect", head, "--expect-plan", plan["plan"], ok=False)
        self.assertIn("plan", result.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(file.read_bytes(), b"Working a")
        self.call("native", "reset", target, "--hard", "--expect", target, ok=False)
        self.assertEqual(self.snapshot(), before)

    def test_unsafe_entries_and_wrong_selected_branch_are_refused_before_write(self):
        target, _, _ = self.fixture()
        self.call("native", "branch", "other", target)
        before = self.snapshot()
        self.call("native", "reset", target, "--hard", "--branch", "other", ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / "shape/.git").mkdir()
        (self.root / "shape/.git/config").write_bytes(b"protected Git data")
        before = self.snapshot()
        self.call("native", "reset", target, "--hard", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual((self.root / "shape/.git/config").read_bytes(), b"protected Git data")
        self.call("native", "reset", target, "--hard", "--soft", ok=False)

    def test_unicode_case_transition_captures_obstructing_untracked_descendants(self):
        (self.root / "Étage").mkdir()
        (self.root / "Étage/Map").write_bytes(b"target")
        target = self.commit("target case")
        (self.root / "Étage/Map").write_bytes(b"head")
        head = self.commit("head case")
        (self.root / "Étage").rename(self.root / "temporary-case")
        (self.root / "temporary-case").rename(self.root / "étage")
        (self.root / "étage/Map").write_bytes(b"dirty")
        (self.root / "étage/untracked").write_bytes(b"preserve in recovery")
        result = json.loads(self.call("native", "reset", target, "--hard", "--expect", head).stdout)
        self.assertIn("Étage", [p.name for p in self.root.iterdir()])
        self.assertNotIn("étage", [p.name for p in self.root.iterdir()])
        self.assertEqual((self.root / "Étage/Map").read_bytes(), b"target")
        self.call("gc", "--delete")
        self.call("native", "export", result["recovery_revision"], self.store / "case-recovery")
        self.assertEqual((self.store / "case-recovery/étage/Map").read_bytes(), b"dirty")
        self.assertEqual((self.store / "case-recovery/étage/untracked").read_bytes(), b"preserve in recovery")


if __name__ == "__main__":
    unittest.main()
