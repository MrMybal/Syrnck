"""Shared object storage with isolated native workspace selection and indexes."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import native_workflows as base
import workflows


class NativeContext(unittest.TestCase):
    def setUp(self):
        base.NativeWorkflows.setUp(self)
        self.external = tempfile.TemporaryDirectory(prefix="syrnck-linked-cli-")
        self.addCleanup(self.external.cleanup)
        self.edit = Path(self.external.name) / "edit"

    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    commit = base.NativeWorkflows.commit
    snapshot = base.NativeWorkflows.snapshot

    def scoped(self, name, *arguments, ok=True):
        return self.call("--workspace", name, "native", *arguments, ok=ok)

    def fixture(self):
        (self.root / "asset").write_bytes(b"first")
        first = self.commit("first")
        self.cmd("native", "branch", "feature", first)
        (self.root / "asset").write_bytes(b"main")
        main = self.commit("main")
        return first, main

    def test_isolated_heads_indexes_and_foreign_branch_preflight(self):
        first, main = self.fixture()
        (self.root / "asset").write_bytes(b"primary prepared")
        primary_index = self.cmd("native", "add", "asset").decode().strip()
        added = json.loads(self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "feature"))
        self.assertEqual((added["state"], added["revision"], added["head"]["workspace"]), ("ready", first, "edit"))
        (self.edit / "asset").write_bytes(b"linked prepared")
        self.scoped("edit", "add", "asset")
        child = self.scoped("edit", "commit", "--staged", "-m", "linked child").stdout.decode().strip()
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/main"]["id"], main)
        self.assertEqual(refs["staging/main"]["id"], primary_index)
        self.assertEqual(refs["heads/feature"]["id"], child)
        self.assertTrue(json.loads(self.scoped("edit", "status", "--combined").stdout)["clean"])
        before = self.snapshot()
        plan = subprocess.run([workflows.CLI, "--store", str(self.store), "--workspace", "edit", "native", "switch-plan", "main"],
                              cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(plan.returncode, 1, plan.stderr)
        self.assertIn("branch_in_other_workspace", [row["reason"] for row in json.loads(plan.stdout)["conflicts"]])
        self.scoped("edit", "switch", "main", ok=False)
        error = self.call("--lang", "fr", "native", "commit", "--branch", "feature", "-m", "foreign", ok=False)
        self.assertIn("autre workspace", error.stderr.decode("utf-8"))
        self.assertEqual(before, self.snapshot())
        receipt = json.loads(self.scoped("edit", "checkout-receipts", "--verify").stdout)["transactions"][0]["id"]
        self.scoped("edit", "checkout-status", "--id", receipt)
        self.call("native", "checkout-status", "--id", receipt, ok=False)
        self.cmd("gc", "--delete")
        self.cmd("native", "export", first, self.store / "saved")
        self.assertEqual((self.store / "saved/asset").read_bytes(), b"first")
        self.assertEqual((self.root / "asset").read_bytes(), b"primary prepared")

    def test_maximum_names_and_default_linked_watch_directory(self):
        first, main = self.fixture()
        branch, name = "topic/" + "b" * 180, "a" * 64
        self.cmd("native", "branch", branch, first)
        self.cmd("native", "workspace-add", name, self.edit, "--branch", branch)
        self.assertEqual(json.loads(self.scoped(name, "head").stdout)["branch"], branch)
        (self.edit / "asset").write_bytes(b"new long branch data")
        self.scoped(name, "add", "asset")
        self.scoped(name, "commit", "--staged", "-m", "long branch")
        watched = self.scoped(name, "watch", "--polling", "--journal", "--max-updates", "1", "--duration-ms", "2000")
        frames = [json.loads(line) for line in watched.stdout.splitlines()]
        self.assertTrue(any(frame.get("type") == "status" for frame in frames), frames)
        self.assertTrue((self.store / "workspaces" / name / "watch-journal").is_file())
        self.assertFalse((self.store / "watch-journal").exists())
        self.assertEqual(json.loads(self.cmd("refs"))["heads/main"]["id"], main)

    def test_portable_backup_restore_and_preserved_frozen_index(self):
        first, main = self.fixture()
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "feature")
        (self.edit / "asset").write_bytes(b"frozen backup bytes")
        original_index = self.scoped("edit", "add", "asset").stdout.decode().strip()
        (self.edit / "asset").write_bytes(b"not stored dirty bytes")
        backup = Path(self.external.name) / "backup"
        self.cmd("backup", "create", backup)
        checked = json.loads(self.cmd("backup", "verify", store=backup))
        self.assertIn("workspaces/edit/registration", checked["excluded_refs"])
        backup_refs = json.loads(self.cmd("refs", store=backup))
        self.assertNotIn("workspaces/edit/workspace/HEAD", backup_refs)
        recovered = Path(self.external.name) / "recovered"
        recovered.mkdir()
        restored_store = recovered / ".syrnck"
        self.cmd("backup", "restore", restored_store, store=backup)
        destination = Path(self.external.name) / "restored-edit"
        report = json.loads(self.cmd("native", "workspace-restore", "edit", destination,
                                    "--directory", recovered, "--detach", store=restored_store))
        self.assertIsNone(report["branch"])
        self.assertEqual(report["revision"], first)
        self.assertNotEqual(report["index"], original_index)
        self.assertEqual((destination / "asset").read_bytes(), b"frozen backup bytes")
        refs = json.loads(self.cmd("refs", store=restored_store))
        self.assertEqual(refs["archive/reset-index/" + original_index]["id"], original_index)
        self.assertEqual(refs["heads/main"]["id"], main)
        self.cmd("gc", "--delete", store=restored_store)
        self.cmd("fsck", store=restored_store)

    def test_invalid_names_overlaps_and_context_failures_preserve_existing_files(self):
        self.fixture()
        self.cmd("native", "workspace-add", "edit", self.edit)
        before = self.snapshot()
        for name in ("primary", "con", "lpt1", "bad/name", "Upper", "-prefix", "trailing-", "x" * 65):
            self.call("native", "workspace-add", name, Path(self.external.name) / "invalid", ok=False)
        self.call("native", "workspace-add", "nested", self.root / "nested", ok=False)
        self.call("native", "workspace-add", "nested", self.edit / "nested", ok=False)
        self.scoped("missing", "head", ok=False)
        self.scoped("edit", "status", "--directory", self.root, ok=False)
        self.assertEqual(before, self.snapshot())
        self.assertFalse((self.edit / "nested").exists())
        self.assertFalse((self.root / "nested").exists())


if __name__ == "__main__":
    unittest.main()
