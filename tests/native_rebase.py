"""Explicit linear native rebase plans and application."""
import json
import unittest
import native_history as history
import native_workflows as base


class NativeRebase(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit
    plan = history.NativeHistory.replay_plan

    def setup_range(self):
        (self.root / "asset.bin").write_bytes(b"base")
        root = self.commit("base")
        (self.root / "extra").write_bytes(b"upstream")
        onto = self.commit("onto")
        self.call("native", "branch", "upstream", onto)
        self.call("native", "checkout", root)
        (self.root / "asset.bin").write_bytes(b"changed")
        first = self.commit("first")
        (self.root / "own").write_bytes(b"second")
        head = self.commit("second")
        return root, onto, first, head

    def test_readonly_virtual_plan_languages_topology_and_limits(self):
        root, onto, first, head = self.setup_range()
        before = self.snapshot()
        result = self.plan("rebase-plan", root, "--onto", "upstream", "--verify")
        self.assertEqual(result["commits"], [first, head])
        self.assertEqual(result["result_files"], 3)
        self.assertEqual(result, self.plan("rebase-plan", root, "--onto", onto, "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        self.call("native", "rebase-plan", root, ok=False)
        self.call("native", "rebase-plan", root, "--onto", onto, "--max-commits", 1, ok=False)
        self.assertEqual(self.plan("rebase-plan", root, "--onto", root)["target"], head)
        self.assertEqual(self.plan("rebase-plan", head, "--onto", onto)["mode"], "move")
        (self.root / "asset.bin").write_bytes(b"dirty")
        self.plan("rebase-plan", root, "--onto", onto, ready=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
