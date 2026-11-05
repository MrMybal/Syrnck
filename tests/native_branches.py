"""Native branch inventory and retained branch lifecycle through the CLI."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows


class NativeBranches(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def deletion_plan(self, name, *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "branch-delete-plan", name,
                                 *map(str, args)], cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        value = json.loads(result.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def test_archived_deletion_retains_staging_and_historical_bytes(self):
        asset = self.root / "asset"
        asset.write_bytes(b"old")
        old = self.commit("old")
        asset.write_bytes(b"main")
        head = self.commit("main")
        self.call("native", "branch", "feature", old)
        asset.write_bytes(b"prepared")
        index = self.call("native", "add", "asset", "--branch", "feature").stdout.decode().strip()
        asset.write_bytes(b"later dirty bytes")
        before = self.snapshot()
        self.deletion_plan("main", ready=False)
        self.deletion_plan("feature", ready=False)
        plan = self.deletion_plan("feature", "--archive-index", "--verify")
        self.assertEqual(plan, self.deletion_plan("feature", "--archive-index", "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        self.call("native", "branch-delete", "feature", ok=False)
        self.call("native", "branch-delete", "feature", "--expect", head, "--archive-index", ok=False)
        self.call("native", "branch-delete", "feature", "--expect", old, ok=False)
        result = json.loads(self.call("native", "branch-delete", "feature", "--expect", old,
                                      "--archive-index", "--expect-index", index).stdout)
        self.assertEqual(result["archive"], plan["archive"])
        self.assertEqual(asset.read_bytes(), b"later dirty bytes")
        refs = json.loads(self.call("refs").stdout)
        self.assertNotIn("heads/feature", refs)
        self.assertNotIn("staging/feature", refs)
        self.call("gc", "--delete")
        archive = json.loads(self.call("native", "branch-archive", result["archive"], "--verify").stdout)
        self.assertEqual(archive["archive"]["index"], index)
        listing = json.loads(self.call("native", "branch-archives", "--verify").stdout)
        self.assertEqual(listing["archives"], [archive["archive"]])
        self.call("native", "export", index, self.store / "saved-index")
        self.assertEqual((self.store / "saved-index/asset").read_bytes(), b"prepared")

    def test_pages_verified_creation_and_language_parity(self):
        self.assertEqual(json.loads(self.call("native", "branches").stdout)["branches"], [])
        (self.root / "asset").write_bytes(b"base")
        first = self.commit("base")
        self.assertEqual(self.call("native", "branch", "feature/topic", first).stdout, b"")
        (self.root / "asset").write_bytes(b"frozen")
        index = self.call("native", "add", "asset", "--branch", "feature/topic").stdout.decode().strip()
        before = self.snapshot()
        page = json.loads(self.call("native", "branches", "--limit", 1, "--verify").stdout)
        self.assertEqual(page, json.loads(self.call("--lang", "fr", "native", "branches", "--limit", 1, "--verify").stdout))
        self.assertEqual(page["branches"][0]["index"], index)
        other = json.loads(self.call("native", "branches", "--after", page["next_after"]).stdout)
        self.assertEqual(other["branches"][0]["name"], "main")
        self.assertTrue(other["branches"][0]["current"])
        self.assertEqual(self.snapshot(), before)
        self.call("native", "branch", "feature/topic", first, ok=False)
        self.call("native", "branches", "--limit", 0, ok=False)
        self.call("native", "branches", "--after", "../bad", ok=False)


if __name__ == "__main__":
    unittest.main()
