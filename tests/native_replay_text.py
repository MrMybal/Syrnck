"""Opt-in text cherry-pick/revert CLI, inverse deltas and complete publication."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows


class NativeReplayText(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, operation, source, *args, ready=True):
        run = subprocess.run([workflows.CLI, "--store", str(self.store), "native", operation + "-plan", source, *map(str, args)],
                             cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0 if ready else 1, run.stderr.decode("utf-8", "replace"))
        value = json.loads(run.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def versions(self, inverse, conflict=False):
        name = "Source/\u00c9dit source.cpp"
        path = self.root / name
        path.parent.mkdir()
        path.write_bytes(b"a\r\nb\r\nc")
        if conflict:
            (self.root / "conflict.cpp").write_bytes(b"parent\n")
        first = self.commit("base")
        path.write_bytes(b"a\r\nb\r\nC")
        if conflict:
            (self.root / "conflict.cpp").write_bytes(b"source\n")
        source = self.commit("feature")
        self.call("native", "branch", "feature", source)
        if not inverse:
            self.call("native", "checkout", first)
        path.write_bytes(b"A\r\nb\r\nC" if inverse else b"A\r\nb\r\nc")
        if conflict:
            (self.root / "conflict.cpp").write_bytes(b"ours\n")
        head = self.commit("current")
        return first, head, source, name

    def apply_scenario(self, inverse):
        operation = "revert" if inverse else "cherry-pick"
        first, head, source, name = self.versions(inverse)
        expected = b"A\r\nb\r\nc" if inverse else b"A\r\nb\r\nC"
        before = self.snapshot()
        self.plan(operation, source, ready=False)
        plan = self.plan(operation, "feature", "--text-merge", "--verify")
        self.assertEqual(plan, self.plan(operation, source, "--text-merge", "--verify", "--lang", "fr"))
        self.assertEqual(plan["text_merge"]["files"][0]["status"], "clean")
        self.assertEqual(plan["resolution_context"]["base"], source if inverse else first)
        self.assertEqual(plan["resolution_context"]["parent"], first)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual((self.root / name).read_bytes(), b"A\r\nb\r\nC" if inverse else b"A\r\nb\r\nc")
        self.call("native", operation, source, "--text-merge", "-m", "wrong head", "--expect", first, ok=False)
        limited = self.plan(operation, source, "--text-merge", "--max-work", "1", ready=False)
        self.assertEqual(limited["text_merge"]["files"][0]["reason"], "work_limit")
        self.call("native", operation + "-plan", source, "--max-work", "1", ok=False)
        choices = plan["resolution_context"]
        choices["files"] = {name: {"choice": "current"}}
        document = self.store / "choices.json"
        document.write_text(json.dumps(choices), encoding="utf-8")
        selected = self.plan(operation, source, "--text-merge", "--resolutions", document, ready=False)
        self.assertTrue(selected["empty"])
        self.assertEqual(selected["text_merge"]["attempted_files"], 0)
        before = self.snapshot()
        self.call("native", operation, source, "--text-merge", "--resolutions", document, "-m", "empty", ok=False)
        self.assertEqual(self.snapshot(), before)
        run = self.call("--lang", "fr", "native", operation, "feature", "--text-merge", "--expect", head,
                        "-m", "Replayed text", "--progress-json")
        applied = json.loads(run.stdout)
        self.assertEqual(applied["previous_revision"], head)
        self.assertEqual(applied["replayed_revision"], source)
        self.assertTrue(any(json.loads(row)["phase"] == "ingesting" for row in run.stderr.splitlines()))
        self.assertEqual((self.root / name).read_bytes(), expected)
        revision = json.loads(self.call("inspect", "revision", applied["target"]).stdout)
        self.assertEqual(revision["parents"], [head])
        self.assertEqual(json.loads(self.call("refs").stdout)["heads/feature"]["id"], source)
        self.call("unpin", "heads/feature")
        self.call("gc", "--delete")
        self.call("native", "export", applied["target"], "retained")
        self.assertEqual((self.root / "retained" / name).read_bytes(), expected)
        self.call("native", "export", head, "old-head")
        self.assertEqual((self.root / "old-head" / name).read_bytes(), b"A\r\nb\r\nC" if inverse else b"A\r\nb\r\nc")

    def test_cherry_pick_text_composes_and_retains_single_parent_result(self):
        self.apply_scenario(False)

    def test_revert_text_removes_only_the_selected_delta(self):
        self.apply_scenario(True)

    def blocked_scenario(self, inverse):
        operation = "revert" if inverse else "cherry-pick"
        _, _, source, name = self.versions(inverse, conflict=True)
        before = self.snapshot()
        plan = self.plan(operation, source, "--text-merge", ready=False)
        self.assertEqual([row["status"] for row in plan["text_merge"]["files"]], ["clean", "conflict"])
        self.assertIsNone(plan["workspace"])
        self.call("native", operation, source, "--text-merge", "-m", "blocked", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual((self.root / name).read_bytes(), b"A\r\nb\r\nC" if inverse else b"A\r\nb\r\nc")

    def test_cherry_pick_one_conflict_blocks_all_generated_files(self):
        self.blocked_scenario(False)

    def test_revert_one_conflict_blocks_all_generated_files(self):
        self.blocked_scenario(True)

    def test_dirty_and_staged_workspaces_stay_blocked_before_materialization(self):
        _, _, source, name = self.versions(False)
        path = self.root / name
        path.write_bytes(b"dirty")
        before = self.snapshot()
        self.assertFalse(self.plan("cherry-pick", source, "--text-merge", ready=False)["workspace"]["ready"])
        self.call("native", "cherry-pick", source, "--text-merge", "-m", "dirty", ok=False)
        self.assertEqual(self.snapshot(), before)
        path.write_bytes(b"A\r\nb\r\nc")
        self.call("native", "add", name)
        before = self.snapshot()
        self.plan("cherry-pick", source, "--text-merge", ready=False)
        self.call("native", "cherry-pick", source, "--text-merge", "-m", "staged", ok=False)
        self.assertEqual(self.snapshot(), before)

    def test_root_presence_and_binary_conflicts_keep_existing_rules(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"root\0")
        first = self.commit("root")
        self.assertEqual(self.plan("revert", first, "--text-merge")["text_merge"]["attempted_files"], 0)
        self.call("native", "revert", first, "--text-merge", "-m", "remove root")
        self.assertFalse(asset.exists())
        self.call("native", "cherry-pick", first, "--text-merge", "-m", "restore root")
        self.assertEqual(asset.read_bytes(), b"root\0")
        asset.write_bytes(b"source\0")
        source = self.commit("source")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        asset.write_bytes(b"ours\0")
        self.commit("ours")
        self.assertEqual(self.plan("cherry-pick", source, "--text-merge", ready=False)["text_merge"]["files"][0]["status"], "binary")
        self.call("native", "cherry-pick", source, "--text-merge", "-m", "binary", ok=False)


if __name__ == "__main__":
    unittest.main()
