"""Native ancestry and history operations through the executable."""
import json
import subprocess
import unittest

import native_workflows as base
import workflows


class NativeHistory(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def replay_plan(self, operation, source, *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", operation,
                                 source, *map(str, args)], cwd=self.root, env=self.env,
                                capture_output=True, timeout=15)
        report = json.loads(result.stdout)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        self.assertEqual(report["ready"], ready)
        return report

    def test_replay_plans_preserve_later_changes_and_never_write(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"base")
        first = self.commit("base")
        asset.write_bytes(b"feature")
        feature = self.commit("feature")
        self.call("native", "branch", "feature", feature)
        self.call("native", "checkout", first)
        (self.root / "ours.txt").write_bytes(b"independent work")
        current = self.commit("ours")
        before = self.snapshot()
        plan = self.replay_plan("cherry-pick-plan", "feature", "--verify")
        self.assertEqual(plan["head"], current)
        self.assertEqual(plan["source"], feature)
        self.assertEqual(plan["parent"], first)
        self.assertTrue(plan["objects_verified"])
        self.assertEqual([row["path"] for row in plan["changes"]], ["asset.bin"])
        self.assertTrue(plan["workspace"]["ready"])
        self.assertEqual(asset.read_bytes(), b"base")
        self.assertEqual(self.snapshot(), before)
        asset.write_bytes(b"uncommitted work")
        blocked = self.replay_plan("cherry-pick-plan", feature, ready=False)
        self.assertFalse(blocked["workspace"]["ready"])
        self.assertEqual(self.snapshot(), before)
        asset.write_bytes(b"conflicting committed work")
        self.commit("conflict")
        before = self.snapshot()
        conflict = self.replay_plan("cherry-pick-plan", feature, ready=False)
        self.assertEqual(conflict["conflicts"][0]["reason"], "content")
        self.assertIsNone(conflict["workspace"])
        self.assertEqual(self.snapshot(), before)
        self.call("native", "checkout", feature)
        before = self.snapshot()
        reverted = self.replay_plan("revert-plan", feature)
        self.assertEqual(reverted["operation"], "revert")
        self.assertEqual(reverted["changes"][0]["path"], "asset.bin")
        self.replay_plan("cherry-pick-plan", feature, ready=False)
        for options in (("--mainline", "0"), ("--mainline", "2"), ("--mainline", "4294967296")):
            self.call("native", "revert-plan", feature, *options, ok=False)
        self.assertEqual(self.snapshot(), before)

    def test_ancestry_selectors_budgets_progress_and_read_only_queries(self):
        (self.root / "asset.bin").write_bytes(b"first")
        first = self.commit("first")
        self.call("native", "tag", "v1")
        (self.root / "asset.bin").write_bytes(b"second")
        second = self.commit("second")
        self.call("native", "branch", "feature", first)
        before = self.snapshot()
        result = self.call("native", "ancestry", "tags/v1", "HEAD", "--progress-json")
        report = json.loads(result.stdout)
        self.assertEqual(report["source"], first)
        self.assertEqual(report["target"], second)
        self.assertEqual(report["merge_bases"], [first])
        self.assertTrue(report["source_is_ancestor"])
        self.assertFalse(report["target_is_ancestor"])
        self.assertEqual(report["visited_revisions"], 2)
        self.assertEqual(report["parent_links"], 1)
        self.assertTrue([json.loads(line) for line in result.stderr.splitlines()][-1]["complete"])
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "ancestry", "feature", second).stdout), report)
        reverse = json.loads(self.call("native", "ancestry", "HEAD", "v1").stdout)
        self.assertTrue(reverse["target_is_ancestor"])
        self.assertFalse(reverse["source_is_ancestor"])
        for args in (("--max-revisions", "1"), ("--max-links", "0"),
                     ("--max-revisions", "0"), ("--max-links", "10000001")):
            self.call("native", "ancestry", first, second, *args, ok=False)
        self.call("native", "ancestry", first, ok=False)
        error = self.call("--lang", "fr", "native", "ancestry", first, second,
                          "--max-revisions", "1", ok=False).stderr.decode("utf-8")
        self.assertIn("limite de révisions du graphe natif dépassée", error)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
