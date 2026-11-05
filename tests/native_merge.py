"""Native merge topology, conflict previews and recoverable application."""
import json
import unittest

import native_history as history
import native_workflows as base


class NativeMerge(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit
    replay_plan = history.NativeHistory.replay_plan

    def test_merge_application_preserves_both_histories_and_source_branch(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"original")
        first = self.commit("root")
        asset.write_bytes(b"incoming")
        source = self.commit("feature")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        (self.root / "own.txt").write_bytes(b"our work")
        current = self.commit("ours")
        before = self.snapshot()
        self.replay_plan("merge-plan", source, "--ff-only", ready=False)
        self.call("native", "merge", source, "--ff-only", ok=False)
        self.call("native", "merge", source, "--expect", first, ok=False)
        self.assertEqual(self.snapshot(), before)
        result = json.loads(self.call("native", "merge", "feature", "--expect", current, "-m", "Merge feature").stdout)
        self.assertEqual(result["mode"], "merge")
        self.assertEqual(result["state"], "committed")
        self.assertEqual(asset.read_bytes(), b"incoming")
        self.assertEqual((self.root / "own.txt").read_bytes(), b"our work")
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/feature"]["id"], source)
        report = json.loads(self.call("native", "ancestry", source, result["target"]).stdout)
        self.assertTrue(report["source_is_ancestor"])
        before = self.snapshot()
        unchanged = json.loads(self.call("native", "merge", source).stdout)
        self.assertEqual(unchanged["state"], "unchanged")
        self.assertEqual(self.snapshot(), before)
        self.call("gc", "--delete")
        self.call("native", "revert", result["target"], "--mainline", "1", "-m", "Revert merge")
        self.assertEqual(asset.read_bytes(), b"original")
        self.assertEqual((self.root / "own.txt").read_bytes(), b"our work")
        self.call("native", "export", source, self.root / "source-copy")
        self.assertEqual((self.root / "source-copy/asset.bin").read_bytes(), b"incoming")

    def test_fast_forward_no_ff_and_staging_guard(self):
        (self.root / "asset.bin").write_bytes(b"root")
        first = self.commit("root")
        (self.root / "asset.bin").write_bytes(b"next")
        source = self.commit("next")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        self.call("native", "add", "asset.bin")
        before = self.snapshot()
        self.call("native", "merge", source, ok=False)
        self.assertEqual(self.snapshot(), before)
        self.call("native", "reset")
        forward = json.loads(self.call("native", "merge", "feature", "--ff-only").stdout)
        self.assertEqual(forward["target"], source)
        self.assertEqual(forward["mode"], "fast_forward")
        self.call("native", "checkout", first)
        explicit = json.loads(self.call("native", "merge", source, "--no-ff").stdout)
        self.assertEqual(explicit["mode"], "merge")
        self.assertNotEqual(explicit["target"], source)
        self.call("native", "merge-plan", source, "--no-ff", "--ff-only", ok=False)

    def test_merge_topology_plans_are_read_only_and_protect_dirty_files(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"original")
        first = self.commit("root")
        asset.write_bytes(b"incoming")
        source = self.commit("feature")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        before = self.snapshot()
        forward = self.replay_plan("merge-plan", "feature", "--verify")
        self.assertEqual(forward["mode"], "fast_forward")
        self.assertTrue(forward["objects_verified"])
        self.assertEqual(self.snapshot(), before)
        (self.root / "own.txt").write_bytes(b"our work")
        current = self.commit("our branch")
        before = self.snapshot()
        plan = self.replay_plan("merge-plan", "feature")
        self.assertEqual(plan["mode"], "merge")
        self.assertEqual(plan["head"], current)
        self.assertEqual(plan["base"], first)
        self.assertEqual(plan["merge_bases"], [first])
        self.assertEqual(plan, self.replay_plan("merge-plan", source, "--lang", "fr"))
        self.assertEqual(asset.read_bytes(), b"original")
        self.assertEqual(self.snapshot(), before)
        asset.write_bytes(b"uncommitted")
        self.replay_plan("merge-plan", source, ready=False)
        self.assertEqual(self.snapshot(), before)
        asset.write_bytes(b"ours")
        self.commit("conflicting branch")
        before = self.snapshot()
        conflict = self.replay_plan("merge-plan", source, ready=False)
        self.assertEqual(conflict["conflicts"][0]["reason"], "content")
        self.assertIsNone(conflict["workspace"])
        self.call("native", "merge-plan", source, "--base", current, ok=False)
        self.call("native", "merge-plan", source, "--max-revisions", "1", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.call("native", "checkout", source)
        self.assertEqual(self.replay_plan("merge-plan", first)["mode"], "up_to_date")


if __name__ == "__main__":
    unittest.main()
