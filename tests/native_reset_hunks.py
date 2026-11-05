"""Remove prepared hunks while preserving workspace and retained index versions."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows
from native_hunks import body


class NativeResetHunks(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, path="code.cpp", *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "reset-hunks-plan", path,
                                 *map(str, args)], cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        value = json.loads(result.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def setup_index(self):
        code = self.root / "code.cpp"
        code.write_bytes(body(0))
        (self.root / "other.txt").write_bytes(b"original")
        head = self.commit("baseline")
        code.write_bytes(body(7))
        (self.root / "other.txt").write_bytes(b"prepared")
        index = self.call("native", "add", "code.cpp", "other.txt").stdout.decode().strip()
        return code, head, index

    def test_removal_ignores_dirty_absent_working_files_and_retains_previous_index(self):
        code, head, previous = self.setup_index()
        before = self.snapshot()
        plan = self.plan()
        self.assertEqual(plan, self.plan("code.cpp", "--lang", "fr"))
        code.write_bytes(b"binary\0dirty")
        self.assertEqual(plan, self.plan())
        code.unlink()
        self.assertEqual(plan, self.plan())
        self.assertEqual(before, self.snapshot())
        applied = json.loads(self.call("--lang", "fr", "native", "reset-hunks", "code.cpp", "--expect-plan",
                                      plan["plan_id"], "--hunks", "3,1", "--progress-json").stdout)
        self.assertEqual(applied["previous_index"], previous)
        self.assertEqual(applied["operation"], "reset-hunks")
        self.call("gc", "--delete")
        self.call("native", "export", applied["index"], "frozen")
        self.assertEqual((self.root / "frozen/code.cpp").read_bytes(), body(2))
        self.assertEqual((self.root / "frozen/other.txt").read_bytes(), b"prepared")
        self.call("native", "export", previous, "previous")
        self.assertEqual((self.root / "previous/code.cpp").read_bytes(), body(7))
        self.call("native", "archive-show", "archive/stage-index/" + previous, "--verify")
        committed = self.call("native", "commit", "--staged", "-m", "keep middle").stdout.decode().strip()
        self.call("gc", "--delete")
        self.call("native", "export", committed, "committed")
        self.assertEqual((self.root / "committed/code.cpp").read_bytes(), body(2))
        self.assertFalse(code.exists())
        self.assertNotEqual(head, committed)

    def test_invalid_numbers_options_drift_and_stale_index_do_not_write(self):
        code, head, previous = self.setup_index()
        plan = self.plan()
        before = self.snapshot()
        for numbers in ("", "0", "1,1", "4", "1,", "-1"):
            self.call("native", "reset-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", numbers, ok=False)
        self.call("native", "reset-hunks", "code.cpp", "--hunks", "1", ok=False)
        self.call("native", "reset-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", "--context", "0", ok=False)
        self.assertEqual(before, self.snapshot())
        self.call("native", "reset", "other.txt")
        before = self.snapshot()
        self.call("native", "reset-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", ok=False)
        self.assertEqual(before, self.snapshot())
        self.call("pin", "heads/main", "revision", previous)
        blocked = self.plan(ready=False)
        self.assertIn("stale_staging", [b["reason"] for b in blocked["blockers"]])
        before = self.snapshot()
        self.call("native", "reset-hunks", "code.cpp", "--expect-plan", blocked["plan_id"], "--hunks", "1", ok=False)
        self.assertEqual(before, self.snapshot())

    def test_file_presence_binary_equal_and_budget_are_blocked(self):
        code, head, previous = self.setup_index()
        limited = self.plan("code.cpp", "--max-lines", "1", ready=False)
        self.assertEqual(limited["diff"]["reason"], "line_limit")
        self.call("native", "reset")
        self.assertIn("staging_required", [b["reason"] for b in self.plan(ready=False)["blockers"]])
        code.write_bytes(body(0))
        self.call("native", "add", "code.cpp")
        self.assertIn("no_text_changes", [b["reason"] for b in self.plan(ready=False)["blockers"]])
        code.write_bytes(b"binary\0payload")
        self.call("native", "add", "code.cpp")
        self.assertEqual(self.plan(ready=False)["diff"]["status"], "binary")
        code.unlink()
        self.call("native", "add", "code.cpp")
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan(ready=False)["blockers"]])
        (self.root / "new.cpp").write_bytes(b"new")
        self.call("native", "add", "new.cpp")
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan("new.cpp", ready=False)["blockers"]])
        self.call("native", "add", "new.cpp", "--branch", "unborn")
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan("new.cpp", "--branch", "unborn", ready=False)["blockers"]])

    def test_context_and_explicit_branch_bound_to_plan(self):
        code, head, previous = self.setup_index()
        self.call("native", "branch", "feature", head)
        self.call("native", "add", "code.cpp", "--branch", "feature")
        plan = self.plan("code.cpp", "--branch", "feature", "--context", "0")
        self.call("native", "reset-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "2", "--branch", "feature", ok=False)
        applied = json.loads(self.call("native", "reset-hunks", "code.cpp", "--expect-plan", plan["plan_id"],
                                      "--hunks", "2", "--branch", "feature", "--context", "0").stdout)
        self.call("native", "export", applied["index"], "selected")
        self.assertEqual((self.root / "selected/code.cpp").read_bytes(), body(5))
        self.assertEqual(json.loads(self.call("native", "staged").stdout)["snapshot"], previous)


if __name__ == "__main__":
    unittest.main()
