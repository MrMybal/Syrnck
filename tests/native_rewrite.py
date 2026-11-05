"""Native branch reset, retained history and rewrite CLI contracts."""
import json
import unittest

import native_history as history
import native_workflows as base


class NativeRewrite(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit
    plan = history.NativeHistory.replay_plan

    def test_soft_mixed_and_legacy_unstage_preserve_working_files_and_archives(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"base")
        root = self.commit("base")
        asset.write_bytes(b"committed")
        head = self.commit("head")
        asset.write_bytes(b"prepared")
        index = self.call("native", "add", "asset.bin").stdout.decode().strip()
        asset.write_bytes(b"later")
        before = self.snapshot()
        preview = self.plan("reset-plan", root, "--soft", "--verify")
        self.assertEqual(preview["index"], index)
        self.assertEqual(preview, self.plan("reset-plan", root, "--soft", "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        self.call("native", "reset", root, "--soft", "--expect", root, ok=False)
        self.assertEqual(self.snapshot(), before)
        reset = json.loads(self.call("native", "reset", root, "--soft", "--expect", head).stdout)
        self.assertEqual(asset.read_bytes(), b"later")
        self.assertEqual(reset["previous_index"], index)
        staged = json.loads(self.call("native", "staged").stdout)
        self.assertEqual(staged["base"], root)
        self.assertFalse(staged["stale"])
        revised = self.call("native", "commit", "--staged", "-m", "recommit").stdout.decode().strip()
        self.call("native", "add", "asset.bin")
        self.call("native", "reset", "asset.bin")  # Existing path reset is unchanged.
        self.assertTrue(json.loads(self.call("native", "staged").stdout)["clean"])
        prepared = self.call("native", "add", "asset.bin").stdout.decode().strip()
        self.call("native", "reset", root, "--mixed", "--expect", revised)
        self.assertEqual(asset.read_bytes(), b"later")
        self.assertIsNone(json.loads(self.call("native", "staged").stdout)["snapshot"])
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["archive/reset/" + head]["id"], head)
        self.assertEqual(refs["archive/reset-index/" + prepared]["id"], prepared)
        self.call("gc", "--delete")
        for revision, content in ((head, b"committed"), (index, b"prepared"), (prepared, b"later")):
            destination = self.store / ("export-" + revision)
            self.call("native", "export", revision, destination)
            self.assertEqual((destination / "asset.bin").read_bytes(), content)

    def test_squash_preserves_exact_tip_and_retains_removed_file_versions(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"root")
        root = self.commit("root")
        asset.write_bytes(b"middle")
        middle = self.commit("middle")
        asset.write_bytes(b"tip")
        head = self.commit("head")
        asset.write_bytes(b"dirty workspace")
        before = self.snapshot()
        preview = self.plan("squash-plan", "2", "--verify")
        self.assertEqual(preview["revisions"], [head, middle])
        self.assertEqual(preview["parents"], [root])
        self.assertEqual(preview, self.plan("squash-plan", "2", "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        self.call("native", "squash", "2", "-m", "wrong", "--expect", root, ok=False)
        result = json.loads(self.call("native", "squash", "2", "-m", "Combined", "--expect", head).stdout)
        revision = json.loads(self.call("inspect", "revision", result["target"]).stdout)
        self.assertEqual(revision["parents"], [root])
        self.assertEqual(revision["files"], json.loads(self.call("inspect", "revision", head).stdout)["files"])
        self.assertEqual(asset.read_bytes(), b"dirty workspace")
        self.call("gc", "--delete")
        self.call("native", "export", middle, self.store / "middle-copy")
        self.assertEqual((self.store / "middle-copy/asset.bin").read_bytes(), b"middle")
        self.plan("squash-plan", "3", ready=False)
        self.call("native", "squash", "1", "-m", "invalid count", ok=False)
        self.call("native", "squash", "2", ok=False)

    def test_archive_pages_inspection_and_export_after_gc(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"old")
        root = self.commit("old")
        asset.write_bytes(b"new")
        head = self.commit("new")
        self.call("native", "reset", root, "--soft")
        self.call("native", "reset", root, "--mixed")
        before = self.snapshot()
        page = json.loads(self.call("native", "archives", "--limit", 1, "--verify").stdout)
        self.assertTrue(page["has_more"])
        self.assertTrue(page["objects_verified"])
        second = json.loads(self.call("native", "archives", "--after", page["next_after"], "--limit", 100).stdout)
        rows = page["archives"] + second["archives"]
        self.assertEqual(len(rows), 3)
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "archives", "--limit", 1, "--verify").stdout), page)
        self.assertEqual(self.snapshot(), before)
        saved = json.loads(self.call("native", "archive-show", "archive/reset/" + head, "--verify").stdout)
        self.assertEqual(saved["archive"]["revision"], head)
        self.call("native", "archives", "--kind", "invalid", ok=False)
        self.call("gc", "--delete")
        self.call("native", "export", saved["archive"]["revision"], self.store / "old-copy")
        self.assertEqual((self.store / "old-copy/asset.bin").read_bytes(), b"new")

    def test_explicit_modes_validation_and_selected_branch(self):
        (self.root / "a").write_bytes(b"first")
        first = self.commit("first")
        (self.root / "a").write_bytes(b"second")
        second = self.commit("second")
        self.call("native", "branch", "feature", second)
        self.call("native", "reset-plan", first, ok=False)
        self.call("native", "reset", first, "--soft", "--mixed", ok=False)
        self.call("native", "reset", first, "--hard", ok=False)
        self.call("native", "reset", first, "--mixed", "--branch", "feature")
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/main"]["id"], second)
        self.assertEqual(refs["heads/feature"]["id"], first)
        self.assertEqual(json.loads(self.call("native", "head").stdout)["branch"], "main")


if __name__ == "__main__":
    unittest.main(verbosity=2)
