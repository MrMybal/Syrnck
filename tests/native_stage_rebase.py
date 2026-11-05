"""Stale native staging replay without reading or changing working files."""
import json
import unittest

import native_workflows as base


class NativeStageRebase(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_read_only_plan_with_dirty_workspace(self):
        asset = self.root / "asset"
        asset.write_bytes(b"base")
        old = self.commit("base")
        (self.root / "upstream").write_bytes(b"independent upstream")
        head = self.commit("head")
        self.call("native", "checkout", old)
        asset.write_bytes(b"prepared")
        index = self.call("native", "add", "asset").stdout.decode().strip()
        self.call("pin", "heads/main", "revision", head)
        asset.write_bytes(b"later dirty bytes")
        before = self.snapshot()
        plan = json.loads(self.call("native", "rebase-index-plan", "--verify").stdout)
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(plan["ready"])
        self.assertEqual(plan["index"], index)
        self.assertEqual(plan["head"], head)
        self.assertEqual(plan["base"], old)
        self.assertEqual(plan, json.loads(self.call("--lang", "fr", "native", "rebase-index-plan", "--verify").stdout))
        self.call("native", "rebase-index-plan", "--directory", self.root, ok=False)
        self.call("native", "rebase-index", "--expect", old, "--expect-index", index, ok=False)
        self.assertEqual(self.snapshot(), before)
        result = json.loads(self.call("native", "rebase-index", "--expect", head, "--expect-index", index).stdout)
        self.assertEqual(result["target"], plan["target"])
        self.assertEqual(asset.read_bytes(), b"later dirty bytes")
        self.assertFalse(json.loads(self.call("native", "staged").stdout)["stale"])
        stable = self.snapshot()
        self.assertEqual(json.loads(self.call("native", "rebase-index").stdout)["state"], "unchanged")
        self.assertEqual(self.snapshot(), stable)
        committed = self.call("native", "commit", "--staged", "-m", "rebased staged bytes").stdout.decode().strip()
        revision = json.loads(self.call("inspect", "revision", committed).stdout)
        self.assertEqual(revision["parents"], [head])
        self.assertIn("upstream", revision["files"])
        self.call("gc", "--delete")
        self.call("native", "export", index, self.store / "old-index")
        self.assertEqual((self.store / "old-index/asset").read_bytes(), b"prepared")
        archive = json.loads(self.call("native", "archives", "--kind", "rebase-index", "--verify").stdout)
        self.assertEqual(archive["archives"][0]["revision"], index)
        self.call("native", "reset")
        self.call("native", "rebase-index-plan", ok=False)


if __name__ == "__main__":
    unittest.main()
