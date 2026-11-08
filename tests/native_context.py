"""Shared object storage with isolated native workspace selection and indexes."""
import json
import shutil
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

    def test_partial_creation_commit_watch_and_empty_projection(self):
        (self.root / "Source").mkdir()
        (self.root / "Content").mkdir()
        (self.root / "Données").mkdir()
        (self.root / "Source/code.cpp").write_bytes(b"first\n")
        (self.root / "Content/asset").write_bytes(b"hidden" * 2048)
        (self.root / "Données/config").write_bytes(b"settings")
        (self.root / ".syrnckignore").write_bytes(b"*.tmp\n")
        first = self.commit("complete tree")
        self.cmd("native", "branch", "partial", first)
        added = json.loads(self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "partial",
                                    "--include", "Source/", "--include", "Données"))
        self.assertEqual(added["checkout"]["version"], 9)
        self.assertEqual(added["checkout"]["source_materialization"], ["Données", "Source"])
        self.assertTrue((self.edit / ".syrnckignore").is_file())
        self.assertTrue((self.edit / "Données/config").is_file())
        self.assertFalse((self.edit / "Content").exists())
        before = self.snapshot()
        selected = json.loads(self.scoped("edit", "materialization").stdout)
        self.assertEqual(selected["prefixes"], ["Données", "Source"])
        self.assertFalse(selected["complete"])
        self.assertTrue(json.loads(self.call("native", "materialization").stdout)["complete"])
        self.assertEqual(self.snapshot(), before)
        error = self.call("--lang", "fr", "--workspace", "edit", "native", "commit",
                          "--branch", "absent", "-m", "no complete baseline", ok=False)
        self.assertIn("révision cible existante", error.stderr.decode("utf-8"))
        self.scoped("edit", "add", "--branch", "absent", ".", ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.edit / "Source/code.cpp").write_bytes(b"changed\n")
        self.scoped("edit", "add", ".")
        child = self.scoped("edit", "commit", "--staged", "-m", "partial edit").stdout.decode().strip()
        exported = Path(self.external.name) / "complete"
        self.cmd("gc", "--delete")
        self.cmd("native", "export", child, exported)
        self.assertEqual((exported / "Content/asset").read_bytes(), b"hidden" * 2048)
        watched = self.scoped("edit", "watch", "--polling", "--journal", "--max-updates", "1")
        frames = [json.loads(line) for line in watched.stdout.splitlines()]
        self.assertTrue(next(frame for frame in frames if frame["type"] == "status")["status"]["clean"])
        empty = Path(self.external.name) / "empty"
        self.cmd("native", "workspace-add", "empty", empty, "--revision", child, "--no-files")
        self.assertEqual(list(empty.iterdir()), [])
        self.assertEqual(json.loads(self.scoped("empty", "materialization").stdout)["prefixes"], [])
        empty_child = self.scoped("empty", "commit", "-m", "metadata only").stdout.decode().strip()
        self.assertTrue(json.loads(self.scoped("empty", "diff", child, empty_child).stdout)["equal"])
        invalid = Path(self.external.name) / "invalid"
        before = self.snapshot()
        self.call("native", "workspace-add", "invalid", invalid, "--no-files", "--include", "Source", ok=False)
        self.assertFalse(invalid.exists())
        self.assertEqual(self.snapshot(), before)

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

    def test_partial_hard_reset_keeps_full_history_index_and_excluded_work(self):
        (self.root / "Source").mkdir()
        (self.root / "Content").mkdir()
        (self.root / "Source/shape").write_bytes(b"target flat")
        (self.root / "Content/asset").write_bytes(b"old hidden asset")
        target = self.commit("target")
        (self.root / "Source/shape").unlink()
        (self.root / "Source/shape").mkdir()
        (self.root / "Source/shape/child").write_bytes(b"head child")
        (self.root / "Content/asset").write_bytes(b"new hidden asset")
        head = self.commit("head")
        self.cmd("native", "branch", "partial", head)
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "partial", "--include", "Source")
        (self.edit / "Source/shape/child").write_bytes(b"staged child")
        index = self.scoped("edit", "add", ".").stdout.decode().strip()
        (self.edit / "Source/shape/child").write_bytes(b"dirty child")
        (self.edit / "Source/shape/local").write_bytes(b"save obstruction")
        (self.edit / "Content").mkdir()
        (self.edit / "Content/local").write_bytes(b"keep excluded local data")
        plan = json.loads(self.scoped("edit", "reset-plan", target, "--hard", "--verify").stdout)
        self.assertEqual(plan["version"], 2)
        self.assertEqual(plan["materialization"], ["Source"])
        applied = json.loads(self.call("--lang", "fr", "--workspace", "edit", "native", "reset", target,
                                      "--hard", "--expect", head, "--expect-plan", plan["plan"]).stdout)
        self.assertEqual((applied["version"], applied["layout_version"]), (9, 5))
        self.assertEqual((self.edit / "Source/shape").read_bytes(), b"target flat")
        self.assertEqual((self.edit / "Content/local").read_bytes(), b"keep excluded local data")
        self.assertFalse((self.edit / "Content/asset").exists())
        self.cmd("gc", "--delete")
        for revision, folder in ((applied["source"], "work"), (index, "index"), (head, "history")):
            self.cmd("native", "export", revision, self.store / folder)
        self.assertEqual((self.store / "work/Source/shape/child").read_bytes(), b"dirty child")
        self.assertEqual((self.store / "work/Source/shape/local").read_bytes(), b"save obstruction")
        self.assertFalse((self.store / "work/Content").exists())
        self.assertEqual((self.store / "index/Content/asset").read_bytes(), b"new hidden asset")
        self.assertEqual((self.store / "index/Source/shape/child").read_bytes(), b"staged child")
        self.assertEqual((self.store / "history/Content/asset").read_bytes(), b"new hidden asset")
        self.assertEqual(json.loads(self.cmd("refs"))["heads/main"]["id"], head)

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

    def test_partial_stash_physical_ancestor_index_and_french_policy_refusal(self):
        for name, value in {"Source/code": b"base code", "Content/A/leaf": b"base leaf",
                            "Content/B": b"hidden asset"}.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        head = self.commit("base")
        (self.root / "Source/code").write_bytes(b"full stash work")
        full = json.loads(self.cmd("native", "stash", "push", "-m", "full"))["stash"]["id"]
        self.cmd("native", "branch", "partial", head)
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "partial",
                 "--include", "Source", "--include", "Content/A/leaf")
        before = self.snapshot()
        refused = self.call("--lang", "fr", "--workspace", "edit", "native", "stash", "apply-plan", full, ok=False)
        self.assertIn("diffère de celle du workspace", refused.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        (self.edit / "Source/code").write_bytes(b"staged code")
        index = self.scoped("edit", "add", ".").stdout.decode().strip()
        (self.edit / "Source/code").write_bytes(b"dirty code")
        (self.edit / "Source/new").write_bytes(b"selected new")
        shutil.rmtree(self.edit / "Content")
        (self.edit / "Content").write_bytes(b"physical ancestor")
        (self.edit / "outside").write_bytes(b"keep excluded")
        saved = json.loads(self.scoped("edit", "stash", "push", "--include-untracked", "-m", "partial").stdout)["stash"]
        shown = json.loads(self.scoped("edit", "stash", "show", saved["id"], "--verify").stdout)
        self.assertEqual((shown["version"], shown["materialization"]), (3, ["Content/A/leaf", "Source"]))
        self.assertEqual((self.edit / "Content/A/leaf").read_bytes(), b"base leaf")
        self.assertFalse((self.edit / "Content/B").exists())
        self.assertFalse((self.edit / "Source/new").exists())
        plan = json.loads(self.scoped("edit", "stash", "apply-plan", saved["id"], "--index", "--verify").stdout)
        self.assertEqual(plan["version"], 2)
        applied = json.loads(self.call("--lang", "fr", "--workspace", "edit", "native", "stash", "pop",
                                      saved["id"], "--index", "--expect-plan", plan["plan"]).stdout)
        self.assertEqual((applied["version"], applied["layout_version"]), (9, 6))
        self.assertEqual((self.edit / "Content").read_bytes(), b"physical ancestor")
        self.assertEqual((self.edit / "outside").read_bytes(), b"keep excluded")
        self.assertEqual((self.edit / "Source/code").read_bytes(), b"dirty code")
        self.assertEqual((self.edit / "Source/new").read_bytes(), b"selected new")
        self.cmd("gc", "--delete")
        self.cmd("native", "export", index, self.store / "saved-index")
        self.cmd("native", "export", shown["work"], self.store / "saved-work")
        self.assertEqual((self.store / "saved-index/Content/B").read_bytes(), b"hidden asset")
        self.assertEqual((self.store / "saved-index/Source/code").read_bytes(), b"staged code")
        self.assertEqual((self.store / "saved-work/Content").read_bytes(), b"physical ancestor")
        self.assertFalse((self.store / "saved-work/outside").exists())
        refs = json.loads(self.cmd("refs"))
        self.assertEqual((refs["heads/main"]["id"], refs["heads/partial"]["id"]), (head, head))

    def test_change_materialization_primary_linked_and_detached(self):
        for name, value in {"Source/code": b"code", "Content/B": b"asset",
                            ".syrnckignore": b"*.tmp\n"}.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        head = self.commit("complete")
        (self.root / "local.tmp").write_bytes(b"unrelated local")
        before = self.snapshot()
        plan = json.loads(self.cmd("native", "materialization-plan", "--include", "Source", "--verify"))
        self.assertTrue(plan["ready"])
        self.assertEqual(self.snapshot(), before)
        refused = self.call("--lang", "fr", "native", "materialization-set", "--include", "Source",
                            "--expect-plan", "0" * 64, ok=False)
        self.assertIn("a changé", refused.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        applied = json.loads(self.cmd("native", "materialization-set", "--include", "Source",
                                      "--expect", head, "--expect-plan", plan["plan"]))
        self.assertEqual((applied["version"], applied["layout_version"]), (10, 7))
        self.assertFalse((self.root / "Content/B").exists())
        self.assertEqual((self.root / "local.tmp").read_bytes(), b"unrelated local")
        self.assertEqual(json.loads(self.cmd("native", "head"))["revision"], head)
        self.call("native", "materialization-set", "--all-files", ok=False)
        self.cmd("native", "branch", "feature", head)
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "feature", "--include", "Source")
        grow = json.loads(self.scoped("edit", "materialization-plan", "--all-files").stdout)
        self.scoped("edit", "materialization-set", "--all-files", "--expect-plan", grow["plan"])
        self.assertEqual((self.edit / "Content/B").read_bytes(), b"asset")
        self.assertFalse((self.root / "Content/B").exists())
        self.scoped("edit", "switch", "--detach", head)
        empty = json.loads(self.scoped("edit", "materialization-plan", "--no-files").stdout)
        self.scoped("edit", "materialization-set", "--no-files", "--expect-plan", empty["plan"])
        self.assertFalse((self.edit / "Source/code").exists())
        self.assertFalse((self.edit / "Content/B").exists())
        self.assertIsNone(json.loads(self.scoped("edit", "head").stdout)["branch"])
        self.assertTrue(json.loads(self.scoped("edit", "status", "--combined").stdout)["clean"])
        self.cmd("gc", "--delete")
        self.cmd("fsck")

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

    def test_workspace_archive_keeps_dirty_files_and_independent_partial_index(self):
        (self.root / "Source").mkdir()
        (self.root / "Content").mkdir()
        (self.root / "Source/code").write_bytes(b"base")
        (self.root / "Content/asset").write_bytes(b"complete hidden asset")
        (self.root / ".syrnckignore").write_bytes(b"*.tmp\n")
        head = self.commit("complete baseline")
        self.cmd("native", "branch", "feature", head)
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "feature", "--include", "Source")
        (self.edit / "Source/code").write_bytes(b"prepared")
        index = self.scoped("edit", "add", "Source").stdout.decode().strip()
        (self.edit / "Source/code").write_bytes(b"dirty")
        (self.edit / "Source/new").write_bytes(b"selected untracked")
        (self.edit / "outside").write_bytes(b"excluded local")
        (self.edit / "ignored.tmp").write_bytes(b"ignored local")
        original_files = {p.relative_to(self.edit): p.read_bytes() for p in self.edit.rglob("*") if p.is_file()}
        before = self.snapshot()
        plan = json.loads(self.cmd("native", "workspace-unregister-plan", "edit", "--include-untracked", "--verify"))
        self.assertTrue(plan["ready"])
        self.assertTrue(plan["directory_preserved"])
        self.assertEqual(plan["index"], index)
        self.assertEqual(self.snapshot(), before)
        missing = self.call("--lang", "fr", "native", "workspace-unregister", "edit", ok=False)
        self.assertIn("exige --expect-plan", missing.stderr.decode("utf-8"))
        archived = json.loads(self.cmd("native", "workspace-unregister", "edit", "--include-untracked",
                                       "--expect-plan", plan["plan"]))
        identifier = archived["archive"]
        self.assertEqual(archived["state"], "unregistered")
        self.assertEqual({p.relative_to(self.edit): p.read_bytes() for p in self.edit.rglob("*") if p.is_file()}, original_files)
        self.scoped("edit", "head", ok=False)
        before = self.snapshot()
        shown = json.loads(self.cmd("native", "workspace-archive", identifier, "--verify"))
        self.assertEqual(shown["index"], index)
        self.assertEqual(shown["revision"], head)
        self.assertEqual(shown["materialization"], ["Source"])
        inventory = json.loads(self.cmd("native", "workspace-archives", "--limit", "1", "--verify"))
        self.assertEqual(inventory["archives"][0]["id"], identifier)
        self.assertEqual(json.loads(self.cmd("native", "workspace-archives", "--after", identifier))["archives"], [])
        self.assertEqual(self.snapshot(), before)
        destination = Path(self.external.name) / "archived-edit"
        restore_args = (identifier, "restored", destination, "--detach")
        restore_plan = json.loads(self.cmd("native", "workspace-archive-restore-plan", *restore_args, "--verify"))
        restored = json.loads(self.cmd("native", "workspace-archive-restore", *restore_args, "--expect-plan", restore_plan["plan"]))
        self.assertEqual(restored["state"], "restored")
        self.assertEqual((destination / "Source/code").read_bytes(), b"dirty")
        self.assertEqual((destination / "Source/new").read_bytes(), b"selected untracked")
        self.assertFalse((destination / "outside").exists())
        self.assertFalse((destination / "ignored.tmp").exists())
        self.assertFalse((destination / "Content").exists())
        self.assertEqual(json.loads(self.scoped("restored", "head").stdout)["revision"], head)
        staged = json.loads(self.scoped("restored", "status", "--combined").stdout)
        self.assertEqual(staged["staged"]["base"], head)
        self.assertEqual(staged["unstaged"]["base"], restored["index"])
        frozen = Path(self.external.name) / "frozen"
        self.cmd("native", "export", restored["index"], frozen)
        self.assertEqual((frozen / "Source/code").read_bytes(), b"prepared")
        self.assertEqual((frozen / "Content/asset").read_bytes(), b"complete hidden asset")
        before = self.snapshot()
        self.cmd("native", "workspace-archive-restore", *restore_args, "--expect-plan", restore_plan["plan"])
        self.assertEqual(self.snapshot(), before)
        backup = Path(self.external.name) / "archive-backup"
        self.cmd("backup", "create", backup)
        group = "archive/workspaces/" + identifier
        retirement = json.loads(self.cmd("backup", "retire-plan", group, "--from", backup))
        self.assertEqual(retirement["references"][0]["kind"], "workspace-group")
        self.cmd("backup", "retire", group, "--from", backup, "--expect-plan", retirement["plan"])
        self.cmd("gc", "--delete")
        self.cmd("fsck")
        self.assertEqual(json.loads(self.cmd("native", "workspace-archives"))["archives"], [])
        self.assertEqual(json.loads(self.cmd("native", "workspace-archive", identifier, "--verify", store=backup))["index"], index)

    def test_whole_workspace_relocation_and_case_change_preserve_partial_preparation(self):
        (self.root / "Source").mkdir()
        (self.root / "Content").mkdir()
        (self.root / "Source/code").write_bytes(b"base")
        (self.root / "Content/asset").write_bytes(b"hidden")
        head = self.commit("complete")
        self.cmd("native", "workspace-add", "edit", self.edit, "--revision", head, "--include", "Source")
        (self.edit / "Source/code").write_bytes(b"prepared")
        index = self.scoped("edit", "add", "Source").stdout.decode().strip()
        (self.edit / "Source/code").write_bytes(b"dirty")
        (self.edit / "outside").write_bytes(b"excluded but preserved")
        (self.edit / "empty").mkdir()
        destination = Path(self.external.name) / "moved"
        before = self.snapshot()
        plan = json.loads(self.cmd("native", "workspace-relocate-plan", "edit", destination))
        self.assertEqual(self.snapshot(), before)
        missing = self.call("--lang", "fr", "native", "workspace-relocate", "edit", destination, ok=False)
        self.assertIn("exige --expect-plan", missing.stderr.decode("utf-8"))
        result = json.loads(self.cmd("native", "workspace-relocate", "edit", destination, "--expect-plan", plan["plan"]))
        self.assertEqual(result["state"], "completed")
        self.assertFalse(self.edit.exists())
        self.assertEqual((destination / "Source/code").read_bytes(), b"dirty")
        self.assertEqual((destination / "outside").read_bytes(), b"excluded but preserved")
        self.assertTrue((destination / "empty").is_dir())
        selected = json.loads(self.scoped("edit", "head").stdout)
        self.assertEqual(selected["revision"], head)
        self.assertIsNone(selected["branch"])
        status = json.loads(self.scoped("edit", "status", "--combined").stdout)
        self.assertEqual(status["staged"]["snapshot"], index)
        before = self.snapshot()
        self.cmd("native", "workspace-relocate", "edit", destination, "--expect-plan", plan["plan"])
        self.assertEqual(self.snapshot(), before)
        upper = Path(self.external.name) / "MOVED"
        next_plan = json.loads(self.cmd("native", "workspace-relocate-plan", "edit", upper))
        self.cmd("native", "workspace-relocate", "edit", upper, "--expect-plan", next_plan["plan"])
        self.assertIn("MOVED", [p.name for p in upper.parent.iterdir()])
        self.assertNotIn("moved", [p.name for p in upper.parent.iterdir()])
        self.assertEqual((upper / "outside").read_bytes(), b"excluded but preserved")
        self.assertFalse(json.loads(self.cmd("native", "workspace-relocation-status"))["pending"])
        self.assertEqual(json.loads(self.cmd("native", "workspace-relocation-recover"))["state"], "none")
        self.cmd("gc", "--delete")
        self.cmd("fsck")

    def test_workspace_archive_stale_plan_and_foreign_directory_are_refused(self):
        head, _ = self.fixture()
        self.cmd("native", "workspace-add", "edit", self.edit, "--branch", "feature")
        plan = json.loads(self.cmd("native", "workspace-unregister-plan", "edit"))
        (self.edit / "asset").write_bytes(b"dirty")
        before = self.snapshot()
        stale = self.call("--lang", "fr", "native", "workspace-unregister", "edit", "--expect-plan", plan["plan"], ok=False)
        self.assertIn("plan de désenregistrement", stale.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        plan = json.loads(self.cmd("native", "workspace-unregister-plan", "edit"))
        archived = json.loads(self.cmd("native", "workspace-unregister", "edit", "--expect-plan", plan["plan"]))
        occupied = Path(self.external.name) / "occupied"
        occupied.mkdir()
        (occupied / "unrelated").write_bytes(b"keep")
        before = self.snapshot()
        self.call("native", "workspace-archive-restore-plan", archived["archive"], "restored", occupied, "--detach", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual((occupied / "unrelated").read_bytes(), b"keep")
        self.assertEqual((self.edit / "asset").read_bytes(), b"dirty")
        self.assertEqual(json.loads(self.cmd("refs"))["heads/feature"]["id"], head)


if __name__ == "__main__":
    unittest.main()
