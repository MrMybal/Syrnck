"""Opt-in text composition of virtual rebase steps and frozen staging deltas."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows


class NativeRebaseText(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, operation, *args, ready=True):
        run = subprocess.run([workflows.CLI, "--store", str(self.store), "native", operation + "-plan", *map(str, args)],
                             cwd=self.root, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(run.returncode, 0 if ready else 1, run.stderr.decode("utf-8", "replace"))
        value = json.loads(run.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def versions(self, late_conflict=False):
        name = "Source/\u00c9dit source.cpp"
        path = self.root / name
        path.parent.mkdir()
        path.write_bytes(b"a\r\nb\r\nc\r\nd")
        first = self.commit("base")
        path.write_bytes(b"A\r\nb\r\nc\r\nd")
        onto = self.commit("upstream")
        self.call("native", "branch", "upstream", onto)
        self.call("native", "checkout", first)
        path.write_bytes(b"a\r\nb\r\nc\r\nD")
        middle = self.commit("first feature change")
        path.write_bytes(b"Z\r\nb\r\nc\r\nD" if late_conflict else b"a\r\nb\r\nC\r\nD")
        head = self.commit("second feature change")
        return first, onto, middle, head, name

    def test_virtual_steps_languages_expected_head_and_retention(self):
        first, onto, middle, head, name = self.versions()
        before = self.snapshot()
        self.plan("rebase", first, "--onto", onto, ready=False)
        plan = self.plan("rebase", first, "--onto", "upstream", "--text-merge", "--verify")
        self.assertEqual(plan, self.plan("rebase", first, "--onto", onto, "--text-merge", "--verify", "--lang", "fr"))
        self.assertEqual(plan["text_merge"]["attempted_files"], 2)
        self.assertEqual([len(step["text_merge"]["files"]) for step in plan["steps"]], [1, 1])
        self.assertEqual(self.snapshot(), before)
        self.call("native", "rebase", first, "--onto", onto, "--text-merge", "--expect", middle, ok=False)
        self.assertEqual(self.snapshot(), before)
        run = self.call("--lang", "fr", "native", "rebase", first, "--onto", "upstream", "--text-merge",
                        "--expect", head, "--progress-json")
        applied = json.loads(run.stdout)
        self.assertEqual(applied["target"], plan["target"])
        self.assertTrue(any(json.loads(row)["phase"] == "ingesting" for row in run.stderr.splitlines()))
        self.assertEqual((self.root / name).read_bytes(), b"A\r\nb\r\nC\r\nD")
        self.call("gc", "--delete")
        for revision, directory, expected in ((plan["steps"][0]["target"], "merged-first", b"A\r\nb\r\nc\r\nD"),
                                               (middle, "original-first", b"a\r\nb\r\nc\r\nD")):
            self.call("native", "export", revision, self.store / directory)
            self.assertEqual((self.store / directory / name).read_bytes(), expected)

    def test_late_conflict_blocks_all_and_explicit_step_wins(self):
        first, onto, _, head, name = self.versions(late_conflict=True)
        before = self.snapshot()
        plan = self.plan("rebase", first, "--onto", onto, "--text-merge", ready=False)
        self.assertEqual(plan["completed_commits"], 1)
        self.assertEqual(plan["steps"][1]["text_merge"]["files"][0]["status"], "conflict")
        self.call("native", "rebase", first, "--onto", onto, "--text-merge", ok=False)
        self.assertEqual(self.snapshot(), before)
        choices = plan["resolution_context"]
        step = plan["steps"][1]["resolution_context"]
        step["files"] = {name: {"choice": "current"}}
        choices["steps"] = [step]
        document = self.store / "choices.json"
        document.write_text(json.dumps(choices), encoding="utf-8")
        selected = self.plan("rebase", first, "--onto", onto, "--text-merge", "--resolutions", document)
        self.assertTrue(selected["steps"][1]["empty"])
        self.assertEqual(selected["text_merge"]["attempted_files"], 1)
        self.call("native", "rebase", first, "--onto", onto, "--text-merge", "--resolutions", document, "--expect", head)
        self.assertEqual((self.root / name).read_bytes(), b"A\r\nb\r\nc\r\nD")

    def test_rebase_limits_dirty_staged_and_invalid_options(self):
        first, onto, _, _, name = self.versions()
        before = self.snapshot()
        plan = self.plan("rebase", first, "--onto", onto, "--text-merge", "--max-work", 1, ready=False)
        self.assertEqual(plan["steps"][0]["text_merge"]["files"][0]["reason"], "work_limit")
        self.call("native", "rebase-plan", first, "--onto", onto, "--max-work", 1, ok=False)
        self.call("native", "rebase-plan", first, "--onto", onto, "--text-merge", "--max-file-bytes", 4194305, ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / name).write_bytes(b"dirty")
        self.call("native", "rebase", first, "--onto", onto, "--text-merge", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.call("native", "add", name)
        staged = self.snapshot()
        self.plan("rebase", first, "--onto", onto, "--text-merge", ready=False)
        self.call("native", "rebase", first, "--onto", onto, "--text-merge", ok=False)
        self.assertEqual(self.snapshot(), staged)

    def index_versions(self, binary=False):
        name = "\u00c9dit index.cpp"
        path = self.root / name
        path.write_bytes(b"a\nb\nc" + (b"\x00" if binary else b""))
        first = self.commit("base")
        path.write_bytes(b"A\nb\nc" + (b"\x00" if binary else b""))
        head = self.commit("upstream")
        self.call("native", "checkout", first)
        path.write_bytes(b"a\nb\nC" + (b"\x00" if binary else b""))
        index = self.call("native", "add", name).stdout.decode().strip()
        self.call("pin", "heads/main", "revision", head)
        path.write_bytes(b"later dirty bytes")
        return first, head, index, name

    def test_index_frozen_bytes_languages_atomic_archive_and_commit(self):
        first, head, index, name = self.index_versions()
        before = self.snapshot()
        self.plan("rebase-index", ready=False)
        plan = self.plan("rebase-index", "--text-merge", "--verify")
        self.assertEqual(plan, self.plan("rebase-index", "--text-merge", "--verify", "--lang", "fr"))
        self.assertEqual(self.snapshot(), before)
        self.call("native", "rebase-index", "--text-merge", "--expect", first, "--expect-index", index, ok=False)
        self.call("native", "rebase-index", "--text-merge", "--expect", head, "--expect-index", first, ok=False)
        self.assertEqual(self.snapshot(), before)
        choices = plan["resolution_context"]
        choices["files"] = {name: {"choice": "current"}}
        document = self.store / "choices.json"
        document.write_text(json.dumps(choices), encoding="utf-8")
        selected = self.plan("rebase-index", "--text-merge", "--resolutions", document)
        self.assertTrue(selected["empty"])
        self.assertEqual(selected["text_merge"]["attempted_files"], 0)
        run = self.call("--lang", "fr", "native", "rebase-index", "--text-merge", "--expect", head,
                        "--expect-index", index, "--progress-json")
        applied = json.loads(run.stdout)
        self.assertEqual(applied["target"], plan["target"])
        self.assertEqual((self.root / name).read_bytes(), b"later dirty bytes")
        refs = json.loads(self.call("refs").stdout)
        self.assertEqual(refs["heads/main"]["id"], head)
        self.assertEqual(refs["archive/rebase-index/" + index]["id"], index)
        fresh = self.snapshot()
        self.assertEqual(json.loads(self.call("native", "rebase-index", "--text-merge").stdout)["state"], "unchanged")
        self.assertEqual(self.snapshot(), fresh)
        commit = self.call("native", "commit", "--staged", "-m", "rebased index").stdout.decode().strip()
        self.call("gc", "--delete")
        self.call("native", "export", commit, self.store / "result")
        self.call("native", "export", index, self.store / "old-index")
        self.assertEqual((self.store / "result" / name).read_bytes(), b"A\nb\nC")
        self.assertEqual((self.store / "old-index" / name).read_bytes(), b"a\nb\nC")

    def test_index_binary_declined_and_limits_validated(self):
        _, _, _, _ = self.index_versions(binary=True)
        before = self.snapshot()
        plan = self.plan("rebase-index", "--text-merge", ready=False)
        self.assertEqual(plan["text_merge"]["files"][0]["status"], "binary")
        limited = self.plan("rebase-index", "--text-merge", "--max-file-bytes", 1, ready=False)
        self.assertEqual(limited["text_merge"]["files"][0]["reason"], "file_limit")
        self.call("native", "rebase-index-plan", "--max-file-bytes", 1, ok=False)
        self.call("native", "rebase-index-plan", "--text-merge", "--max-lines", 0, ok=False)
        self.call("native", "rebase-index", "--text-merge", ok=False)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
