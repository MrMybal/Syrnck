"""Recoverable namespace transitions through the production bilingual CLI."""
import json
import unittest
import native_workflows as base
import native_workspace


class NativeNamespace(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit
    plan = native_workspace.NativeWorkspace.plan

    def versions(self):
        (self.root / "Case").mkdir()
        (self.root / "Case/Leaf").write_bytes(b"same case bytes")
        (self.root / "Flat").write_bytes(b"old flat")
        (self.root / "Tree").mkdir()
        (self.root / "Tree/child").write_bytes(b"old tree")
        (self.root / "\u00c9dit.cpp").write_bytes(b"unicode case")
        source = self.commit("source namespace")
        (self.root / "Case").rename(self.root / "temporary-case")
        (self.root / "temporary-case").rename(self.root / "case")
        (self.root / "Flat").unlink()
        (self.root / "Flat").mkdir()
        (self.root / "Flat/new").write_bytes(b"new nested")
        (self.root / "Tree/child").unlink()
        (self.root / "Tree").rmdir()
        (self.root / "Tree").write_bytes(b"new flat")
        (self.root / "\u00c9dit.cpp").rename(self.root / "temporary-file")
        (self.root / "temporary-file").rename(self.root / "\u00e9dit.cpp")
        target = self.commit("target namespace")
        self.call("native", "branch", "target", target)
        self.call("native", "checkout", source)
        return source, target

    def paths(self):
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*")
                      if p.is_file() and not p.is_relative_to(self.store))

    def test_read_only_plan_exact_names_round_trip_retention_and_switch(self):
        source, target = self.versions()
        before = self.snapshot()
        plan, _ = self.plan(target, "--verify")
        french, _ = self.plan("target", "--verify", "--lang", "fr")
        self.assertEqual(plan, french)
        self.assertTrue(plan["namespace_transition"])
        self.assertEqual(plan["remove_directories"], ["Case", "Tree"])
        self.assertEqual(self.snapshot(), before)
        applied = json.loads(self.call("--lang", "fr", "native", "checkout", "target", "--progress-json").stdout)
        self.assertEqual(applied["version"], 3)
        self.assertEqual(self.paths(), ["Flat/new", "Tree", "case/Leaf", "\u00e9dit.cpp"])
        self.assertEqual((self.root / "Tree").read_bytes(), b"new flat")
        self.call("gc", "--delete")
        self.call("native", "checkout", source)
        self.assertEqual(self.paths(), ["Case/Leaf", "Flat", "Tree/child", "\u00c9dit.cpp"])
        selected = json.loads(self.call("native", "switch", "target").stdout)
        self.assertEqual(selected["version"], 4)
        refs = json.loads(self.call("refs").stdout)
        self.assertEqual(refs["heads/main"]["id"], source)
        self.assertEqual(refs["heads/target"]["id"], target)
        self.call("native", "switch", "main")
        self.assertEqual(self.paths(), ["Case/Leaf", "Flat", "Tree/child", "\u00c9dit.cpp"])

    def test_untracked_empty_directories_ignored_descendants_and_dirty_files_block(self):
        _, target = self.versions()
        (self.root / "Tree/empty-untracked").mkdir()
        self.plan(target, ready=False)
        before = self.snapshot()
        self.call("native", "checkout", target, ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / "Tree/empty-untracked").rmdir()
        (self.root / ".syrnckignore").write_bytes(b"*.tmp\n")
        # Keep the ignore file outside the tracked snapshot: it is unrelated to
        # this transition but must not hide the descendant collision.
        (self.root / "Tree/local.tmp").write_bytes(b"preserve")
        plan, _ = self.plan(target, ready=False)
        self.assertTrue(any(row["path"] == "Tree/local.tmp" for row in plan["conflicts"]))
        self.call("native", "checkout", target, ok=False)
        self.assertEqual((self.root / "Tree/local.tmp").read_bytes(), b"preserve")
        (self.root / "Tree/local.tmp").unlink()
        (self.root / "Flat").write_bytes(b"dirty")
        self.plan(target, ready=False)
        self.call("native", "checkout", target, ok=False)
        self.assertEqual((self.root / "Flat").read_bytes(), b"dirty")


if __name__ == "__main__":
    unittest.main(verbosity=2)
