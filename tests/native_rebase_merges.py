"""Real CLI DAG replays, editable merges and guarded resolution contexts."""
import json
import subprocess
import unittest
import native_history as history
import native_workflows as base


class MergeRebase(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit
    plan = history.NativeHistory.replay_plan

    def fixture(self, conflict=False):
        (self.root / "asset").write_bytes(b"base")
        root = self.commit("root")
        (self.root / "upstream").write_bytes(b"upstream")
        onto = self.commit("onto")
        self.call("native", "branch", "upstream", onto)
        self.call("native", "checkout", root)
        if conflict:
            (self.root / "asset").write_bytes(b"left")
        else:
            (self.root / "left").write_bytes(b"left")
        left = self.commit("left")
        self.call("native", "branch", "left", left)
        self.call("native", "checkout", root)
        if conflict:
            (self.root / "asset").write_bytes(b"right")
        else:
            (self.root / "right").write_bytes(b"right")
        right = self.commit("right")
        self.call("native", "branch", "right", right)
        self.call("native", "checkout", left)
        arguments = []
        if conflict:
            context = self.plan("merge-plan", right, ready=False)["resolution_context"]
            context["files"] = {"asset": {"choice": "incoming"}}
            choices = self.store / "merge-choices.json"
            choices.write_text(json.dumps(context), encoding="utf-8")
            arguments = ["--resolutions", choices]
        merged = json.loads(self.call("native", "merge", right, *arguments).stdout)["target"]
        # A real edit made at the merge must survive recreation of its parents.
        (self.root / "merge-only").write_bytes(b"manual edit")
        if conflict:
            (self.root / "asset").write_bytes(b"manual resolution")
        merged = self.call("native", "commit", "--amend", "--expect", merged,
                           "-m", "manual merge").stdout.decode().strip()
        return root, onto, left, right, merged

    def test_preview_edit_drop_apply_and_gc(self):
        root, onto, left, right, head = self.fixture()
        before = self.snapshot()
        self.plan("rebase-plan", root, "--onto", onto, ready=False)
        preview = self.plan("rebase-plan", root, "--onto", onto, "--rebase-merges", "--verify")
        self.assertEqual(preview, self.plan("rebase-plan", root, "--onto", "upstream",
                                            "--rebase-merges", "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        template = json.loads(self.call("native", "rebase-todo", root, "--onto", onto,
                                        "--rebase-merges").stdout)
        self.assertEqual(template["steps"][-1], {"source": head, "action": "merge"})
        for row in template["steps"]:
            if row["source"] == left:
                row["action"] = "drop"
        document = self.store / "todo.json"
        document.write_text(json.dumps(template), encoding="utf-8")
        dropped = self.plan("rebase-plan", root, "--onto", onto, "--rebase-merges", "--todo", document)
        rows = {row["source"]: row for row in dropped["steps"]}
        self.assertEqual(rows[head]["result_parents"], [onto, rows[right]["target"]])
        applied = json.loads(self.call("native", "rebase", root, "--onto", onto, "--rebase-merges",
                                       "--todo", document, "--expect", head).stdout)
        self.assertEqual(applied["target"], dropped["target"])
        self.assertTrue(applied["preserve_merges"])
        self.assertFalse((self.root / "left").exists())
        self.assertEqual((self.root / "merge-only").read_bytes(), b"manual edit")
        self.assertEqual((self.root / "upstream").read_bytes(), b"upstream")
        revision = json.loads(self.cmd("inspect", "revision", applied["target"]))
        self.assertEqual(revision["parents"], rows[head]["result_parents"])
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/left"]["id"], left)
        self.assertEqual(refs["heads/right"]["id"], right)
        self.assertEqual(refs[applied["archive"]]["id"], head)
        self.call("gc", "--delete")
        self.call("fsck")
        self.call("native", "export", head, self.store / "original")
        self.assertEqual((self.store / "original/left").read_bytes(), b"left")

    def test_recorded_resolution_changed_inputs_and_stale_context(self):
        root, onto, left, right, head = self.fixture(conflict=True)
        preview = self.plan("rebase-plan", root, "--onto", onto, "--rebase-merges")
        self.assertEqual(preview["steps"][-1]["recorded_resolutions_reused"], ["asset"])
        template = preview["todo"]
        for row in template["steps"]:
            if row["source"] == left:
                row["action"] = "drop"
        document = self.store / "todo.json"
        document.write_text(json.dumps(template), encoding="utf-8")
        blocked = self.plan("rebase-plan", root, "--onto", onto, "--rebase-merges",
                            "--todo", document, ready=False)
        self.assertEqual(blocked["steps"][-1]["conflicts"][0]["reason"], "ambiguous_base")
        context = blocked["resolution_context"]
        step = blocked["steps"][-1]["resolution_context"]
        step["files"] = {"asset": {"choice": "incoming"}}
        context["steps"] = [step]
        choices = self.store / "rebase-choices.json"
        choices.write_text(json.dumps(context), encoding="utf-8")
        self.plan("rebase-plan", root, "--onto", onto, "--rebase-merges", "--todo", document,
                  "--resolutions", choices, "--verify")
        # A changed side candidate invalidates the later merge's bound context.
        for row in template["steps"]:
            if row["source"] == right:
                row.update(action="reword", message="changed side")
        document.write_text(json.dumps(template), encoding="utf-8")
        before = self.snapshot()
        failed = self.call("--lang", "fr", "native", "rebase-plan", root, "--onto", onto,
                           "--rebase-merges", "--todo", document, "--resolutions", choices, ok=False)
        self.assertIn("contexte", failed.stderr.decode())
        self.assertEqual(self.snapshot(), before)

    def test_interactive_and_guarded_graph_budgets(self):
        root, onto, left, right, head = self.fixture()
        before = self.snapshot()
        for flags in (("--rebase-cousins",), ("--max-revisions", 100),
                      ("--rebase-max-work", 500), ("--base-max-count", 2)):
            self.call("native", "rebase-plan", root, "--onto", onto, *flags, ok=False)
        for flags in (("--max-revisions", 1), ("--max-links", 0), ("--rebase-max-work", 1),
                      ("--rebase-max-delta-bytes", 1023), ("--rebase-max-delta-bytes", 268435457),
                      ("--rebase-max-work", 1073741825), ("--base-max-depth", 0)):
            self.call("--lang", "fr", "native", "rebase-plan", root, "--onto", onto,
                      "--rebase-merges", *flags, ok=False)
        self.assertEqual(self.snapshot(), before)
        result = subprocess.run([base.base.CLI, "--store", str(self.store), "native", "rebase-plan",
                                 root, "--onto", onto, "--rebase-merges", "--interactive", "--lang", "fr"],
                                cwd=self.root, env=self.env, input=b".\n", capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"merge " + head.encode(), result.stderr)
        self.assertIn("Sources du rebase", result.stderr.decode())
        self.assertEqual(json.loads(result.stdout)["result_merges"], 1)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
