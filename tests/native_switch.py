"""Symbolic native HEAD, branch switching and defaults in the production CLI."""
import json
import subprocess
import unittest

import native_workflows as base
import workflows


class NativeSwitch(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, branch, ok=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "switch-plan", branch],
                                cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout)

    def branches(self):
        return {name: value for name, value in json.loads(self.cmd("refs")).items()
                if name.startswith("heads/")}

    def fixture(self):
        (self.root / "asset.bin").write_bytes(b"original")
        first = self.commit("main baseline")
        (self.root / "asset.bin").write_bytes(b"feature")
        (self.root / "Éditeur.cpp").write_bytes(b"source\n")
        feature = self.call("native", "commit", "--branch", "feature", "-m", "feature baseline").stdout.decode().strip()
        (self.root / "asset.bin").write_bytes(b"original")
        (self.root / "Éditeur.cpp").unlink()
        return first, feature

    def test_switch_and_current_branch_defaults(self):
        first, feature = self.fixture()
        before = self.snapshot()
        plan = json.loads(self.call("native", "switch-plan", "feature", "--verify").stdout)
        self.assertTrue(plan["ready"] and plan["objects_verified"])
        self.assertEqual((plan["branch"], plan["destination_branch"], plan["source"], plan["target"]),
                         ("main", "feature", first, feature))
        self.assertEqual(self.snapshot(), before)
        branches = self.branches()
        switched = self.call("--lang", "fr", "native", "switch", "feature", "--progress-json")
        report = json.loads(switched.stdout)
        self.assertEqual(report["state"], "committed")
        self.assertEqual(self.branches(), branches)
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"feature")
        head = json.loads(self.call("native", "head").stdout)
        self.assertTrue(head["explicit"])
        self.assertEqual(head["branch"], "feature")
        self.assertEqual(head["revision"], feature)
        self.assertEqual(head, json.loads(self.call("--lang", "fr", "native", "head").stdout))
        self.assertTrue(json.loads(self.call("native", "status").stdout)["clean"])
        self.assertIn(b"feature baseline", self.call("native", "log").stdout)
        (self.root / "asset.bin").write_bytes(b"prepared")
        self.call("native", "add", "asset.bin")
        stage = json.loads(self.call("native", "staged").stdout)
        self.assertEqual(stage["branch"], "feature")
        self.assertEqual(stage["base"], feature)
        combined = json.loads(self.call("native", "status", "--combined").stdout)
        self.assertEqual(combined["branch"], "feature")
        self.call("native", "reset")
        self.assertIsNone(json.loads(self.call("native", "staged").stdout)["snapshot"])
        self.call("native", "add", "asset.bin")
        (self.root / "asset.bin").write_bytes(b"later edit")
        staged_commit = self.call("native", "commit", "--staged", "-m", "selected feature").stdout.decode().strip()
        self.assertEqual(json.loads(self.call("native", "head").stdout)["revision"], staged_commit)
        self.assertEqual(self.branches()["heads/main"]["id"], first)
        latest = self.commit("whole feature")
        self.assertEqual(json.loads(self.call("native", "head").stdout)["revision"], latest)
        checkout_plan = json.loads(self.call("native", "checkout-plan", feature).stdout)
        self.assertEqual(checkout_plan["branch"], "feature")
        self.call("native", "checkout", feature)
        self.assertEqual(json.loads(self.call("native", "head").stdout)["revision"], feature)
        detached = self.call("native", "detach-history", "-m", "feature root").stdout.decode().strip()
        self.assertEqual(json.loads(self.call("native", "head").stdout)["revision"], detached)
        self.assertEqual(self.branches()["heads/main"]["id"], first)
        self.call("native", "switch", "main")
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"original")
        self.assertFalse((self.root / "Éditeur.cpp").exists())
        self.assertEqual(json.loads(self.call("native", "head").stdout)["branch"], "main")
        self.assertIn(b"main baseline", self.call("native", "log").stdout)
        self.call("gc", "--delete")
        receipt = json.loads(self.call("native", "checkout-status", "--id", report["id"]).stdout)
        self.assertEqual(receipt["destination_branch"], "feature")

    def test_unborn_main_and_identical_revision_switches(self):
        head = json.loads(self.call("native", "head").stdout)
        self.assertEqual(head, {"version": 1, "branch": "main", "revision": None, "explicit": False, "directory": None})
        (self.root / "empty").write_bytes(b"")
        saved = self.call("native", "commit", "--branch", "saved", "-m", "empty").stdout.decode().strip()
        (self.root / "empty").unlink()
        self.call("native", "switch", "saved")
        self.assertEqual((self.root / "empty").read_bytes(), b"")
        self.assertNotIn("heads/main", self.branches())
        self.call("native", "branch", "twin", saved)
        self.call("native", "switch", "twin")
        self.assertEqual(json.loads(self.call("native", "head").stdout)["branch"], "twin")
        before = self.snapshot()
        self.assertEqual(json.loads(self.call("native", "switch", "twin").stdout)["state"], "unchanged")
        self.assertEqual(self.snapshot(), before)
        self.call("native", "switch", "main", ok=False)  # No implicit creation of unborn destinations.

    def test_blockers_and_invalid_arguments_do_not_change_head(self):
        first, feature = self.fixture()
        (self.root / "asset.bin").write_bytes(b"local edit")
        before = self.snapshot()
        self.assertFalse(self.plan("feature", ok=False)["ready"])
        self.call("native", "switch", "feature", ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / "asset.bin").write_bytes(b"original")
        self.call("native", "add", "asset.bin", "--branch", "feature")
        before = self.snapshot()
        self.assertIn("target_staging_present", [row["reason"] for row in self.plan("feature", ok=False)["conflicts"]])
        self.call("native", "switch", "feature", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.call("native", "reset", "--branch", "feature")
        for args in (("head", "extra"), ("head", "--branch", "feature"), ("switch",),
                     ("switch", "missing"), ("switch", "feature", "--force"),
                     ("switch", "feature", "--branch", "main"), ("switch", "feature", "--verify"),
                     ("switch-plan", "missing")):
            self.call("native", *args, ok=False)
        self.call("native", "switch", "feature")
        before = self.snapshot()
        self.call("native", "status", "--directory", self.root.parent, ok=False)
        self.call("native", "switch", "main", "--directory", self.root.parent, ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.branches()["heads/main"]["id"], first)
        self.assertEqual(self.branches()["heads/feature"]["id"], feature)


if __name__ == "__main__":
    unittest.main()
