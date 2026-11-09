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

    def test_bound_resolution_documents_and_custom_replay_files(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"base")
        first = self.commit("base")
        asset.write_bytes(b"incoming")
        source = self.commit("feature")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        asset.write_bytes(b"ours")
        current = self.commit("ours")
        blocked = self.replay_plan("merge-plan", source, ready=False)
        choices = blocked["resolution_context"]
        choices["files"] = {"asset.bin": {"choice": "incoming"}}
        document = self.store / "choices.json"
        good = json.dumps(choices, indent=2)
        document.write_text(good, encoding="utf-8")
        before = self.snapshot()
        resolved = self.replay_plan("merge-plan", source, "--resolutions", document, "--verify")
        self.assertEqual(resolved["resolved"][0]["choice"], "incoming")
        self.assertEqual(resolved["conflicts"], [])
        self.assertEqual(self.snapshot(), before)
        document.write_text(good[:-1] + ',"head":"' + current + '"}', encoding="utf-8")
        error = self.call("--lang", "fr", "native", "merge-plan", source,
                          "--resolutions", document, ok=False).stderr.decode("utf-8")
        self.assertIn("champ de résolution native dupliqué", error)
        document.write_text(good, encoding="utf-8")
        merged = json.loads(self.call("native", "merge", source, "--resolutions", document).stdout)
        self.assertEqual(asset.read_bytes(), b"incoming")
        before = self.snapshot()
        self.call("native", "merge", source, "--expect", merged["target"], "--resolutions", document, ok=False)
        self.assertEqual(self.snapshot(), before)
        self.call("native", "checkout", current)
        custom = self.store / "combined.bin"
        custom.write_bytes(b"manual combination")
        imported = json.loads(self.call("put", custom).stdout)
        replay = self.replay_plan("cherry-pick-plan", source, ready=False)["resolution_context"]
        replay["files"] = {"asset.bin": {"choice": "file", "manifest": imported["manifest"], "executable": False}}
        document.write_text(json.dumps(replay), encoding="utf-8")
        self.replay_plan("cherry-pick-plan", source, "--resolutions", document, "--verify")
        picked = json.loads(self.call("native", "cherry-pick", source, "--resolutions", document,
                                     "-m", "Manual resolution").stdout)
        self.assertEqual(picked["previous_revision"], current)
        self.assertEqual(asset.read_bytes(), b"manual combination")
        self.call("gc", "--delete")
        self.call("native", "export", picked["target"], self.root / "custom-copy")
        self.assertEqual((self.root / "custom-copy/asset.bin").read_bytes(), b"manual combination")

    def test_recursive_bases_and_explicit_ambiguity_are_bilingual_and_read_only(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"base")
        root = self.commit("root")
        asset.write_bytes(b"one")
        a = self.commit("a")
        self.call("native", "branch", "arm-a", a)
        self.call("native", "checkout", root)
        asset.write_bytes(b"two")
        b = self.commit("b")
        self.call("native", "branch", "arm-b", b)
        document = self.store / "choices.json"

        def make_merge(head, source):
            self.call("native", "checkout", head)
            context = self.replay_plan("merge-plan", source, ready=False)["resolution_context"]
            context["files"] = {"asset.bin": {"choice": "current"}}
            document.write_text(json.dumps(context), encoding="utf-8")
            return json.loads(self.call("native", "merge", source, "--resolutions", document).stdout)["target"]

        left = make_merge(a, "arm-b")
        right = make_merge(b, "arm-a")
        self.call("native", "checkout", left)
        before = self.snapshot()
        plan = self.replay_plan("merge-plan", right, "--text-merge", "--rename-similarity", "50", ready=False)
        self.assertTrue(plan["base_virtual"])
        self.assertEqual(sorted(plan["merge_bases"]), sorted([a, b]))
        self.assertEqual(plan["base_synthesis"]["ambiguous_paths"], ["asset.bin"])
        self.assertEqual(plan["conflicts"][0]["reason"], "ambiguous_base")
        self.assertTrue(plan["renames_suppressed"])
        self.assertEqual(plan, self.replay_plan("merge-plan", right, "--text-merge", "--rename-similarity", "50",
                                              "--lang", "fr", ready=False))
        strict = self.replay_plan("merge-plan", right, "--no-recursive-bases", ready=False)
        self.assertEqual(strict["blockers"], [{"reason": "multiple_merge_bases"}])
        self.assertIsNone(strict["base_synthesis"])
        self.assertFalse(self.replay_plan("merge-plan", right, "--base", a)["base_virtual"])
        error = self.call("--lang", "fr", "native", "merge-plan", right,
                          "--base-max-work", "1", ok=False).stderr.decode("utf-8")
        self.assertIn("limite de travail des bases de fusion natives dépassée", error)
        for flag, number in (("--base-max-count", "1"), ("--base-max-merges", "0"),
                             ("--base-max-depth", "129"), ("--base-max-input-bytes", "1023"),
                             ("--base-max-work", "4294967296")):
            self.call("native", "merge-plan", right, flag, number, ok=False)
        self.assertEqual(self.snapshot(), before)
        context = plan["resolution_context"]
        context["files"] = {"asset.bin": {"choice": "base"}}
        document.write_text(json.dumps(context), encoding="utf-8")
        self.call("native", "merge-plan", right, "--resolutions", document, "--text-merge",
                  "--rename-similarity", "50", ok=False)
        context["files"]["asset.bin"]["choice"] = "incoming"
        document.write_text(json.dumps(context), encoding="utf-8")
        merged = json.loads(self.call("native", "merge", right, "--resolutions", document,
                                      "--text-merge", "--rename-similarity", "50").stdout)
        self.assertTrue(merged["base_virtual"])
        self.assertEqual(merged["base"], plan["base"])
        self.assertEqual(asset.read_bytes(), b"two")
        revision = json.loads(self.call("inspect", "revision", merged["target"]).stdout)
        self.assertEqual(revision["parents"], [left, right])
        self.call("gc", "--delete")
        self.call("fsck")

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
