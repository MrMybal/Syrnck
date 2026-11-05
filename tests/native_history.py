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

    def test_cherry_pick_and_revert_publish_children_and_keep_old_bytes(self):
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
        for args in (("--expect", first, "-m", "bad tip"), ("--expect", "HEAD", "-m", "bad ID"), ()):
            self.call("native", "cherry-pick", feature, *args, ok=False)
        self.assertEqual(self.snapshot(), before)
        result = self.call("native", "cherry-pick", "feature", "--expect", current, "-m", "Picked fix", "--progress-json")
        picked = json.loads(result.stdout)
        self.assertEqual(picked["operation"], "cherry-pick")
        self.assertEqual(picked["previous_revision"], current)
        self.assertEqual(picked["replayed_revision"], feature)
        self.assertEqual(picked["state"], "committed")
        self.assertEqual(asset.read_bytes(), b"feature")
        self.assertEqual((self.root / "ours.txt").read_bytes(), b"independent work")
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/main"]["id"], picked["target"])
        self.assertEqual(refs["heads/feature"]["id"], feature)
        self.assertEqual(json.loads(self.call("native", "head").stdout)["branch"], "main")
        before = self.snapshot()
        self.call("native", "cherry-pick", feature, "--expect", current, "-m", "retry", ok=False)
        self.call("native", "cherry-pick", feature, "-m", "already applied", ok=False)
        self.assertEqual(self.snapshot(), before)
        reverted = json.loads(self.call("--lang", "fr", "native", "revert", feature, "-m", "Annulation").stdout)
        self.assertEqual(reverted["operation"], "revert")
        self.assertEqual(reverted["previous_revision"], picked["target"])
        self.assertEqual(asset.read_bytes(), b"base")
        self.assertEqual((self.root / "ours.txt").read_bytes(), b"independent work")
        self.call("gc", "--delete")
        self.call("native", "export", picked["target"], self.root / "old-picked")
        self.assertEqual((self.root / "old-picked/asset.bin").read_bytes(), b"feature")
        log = self.call("native", "log").stdout.decode("utf-8").splitlines()
        self.assertEqual(len(log), 4)
        self.assertTrue(log[0].endswith("Annulation"))
        self.assertTrue(log[1].endswith("Picked fix"))
    def test_root_revert_and_cherry_pick_restore_every_file(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"root bytes")
        root = self.commit("root")
        reverted = json.loads(self.call("native", "revert", root, "-m", "Remove root files").stdout)
        self.assertFalse(asset.exists())
        self.assertIsNone(reverted["mainline"])
        restored = json.loads(self.call("native", "cherry-pick", root, "-m", "Restore root files").stdout)
        self.assertEqual(asset.read_bytes(), b"root bytes")
        self.assertEqual(restored["previous_revision"], reverted["target"])
        self.assertIsNone(restored["mainline"])
        self.assertEqual(len(self.call("native", "log").stdout.splitlines()), 3)

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
