"""Individual changed-row staging/reset, plan binding and exact newline failures."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows

OLD = b"before\nold A\nold B\nafter\n"
NEW = b"before\nnew A\nnew B\nafter\n"


class NativeLines(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, operation="add", path="code.cpp", *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", operation + "-lines-plan", path,
                                 *map(str, args)], cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        value = json.loads(result.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def fixture(self):
        code = self.root / "code.cpp"
        code.write_bytes(OLD)
        (self.root / "other.txt").write_bytes(b"original")
        head = self.commit("baseline")
        (self.root / "other.txt").write_bytes(b"prepared neighbor")
        index = self.call("native", "add", "other.txt").stdout.decode().strip()
        code.write_bytes(NEW)
        return code, head, index

    def test_add_and_remove_individual_rows_keep_other_preparations_and_archives(self):
        code, head, previous = self.fixture()
        before = self.snapshot()
        plan = self.plan()
        self.assertEqual(plan, self.plan("add", "code.cpp", "--lang", "fr"))
        self.assertEqual(before, self.snapshot())
        self.assertEqual(plan["change_count"], 4)
        rows = plan["diff"]["hunks"][0]["lines"]
        self.assertEqual([r["change_number"] for r in rows if "change_number" in r], [1, 2, 3, 4])
        applied = json.loads(self.call("--lang", "fr", "native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"],
                                      "--lines", "4,2", "--progress-json").stdout)
        self.call("native", "export", applied["index"], "first")
        self.assertEqual((self.root / "first/code.cpp").read_bytes(), b"before\nold A\nnew B\nafter\n")
        reset = self.plan("reset")
        self.assertEqual(reset["change_count"], 2)
        code.write_bytes(b"binary\0dirty")
        self.assertEqual(reset, self.plan("reset"))
        code.unlink()
        self.assertEqual(reset, self.plan("reset"))
        result = json.loads(self.call("native", "reset-lines", "code.cpp", "--expect-plan", reset["plan_id"], "--lines", "2").stdout)
        self.call("gc", "--delete")
        self.call("native", "export", result["index"], "second")
        self.assertEqual((self.root / "second/code.cpp").read_bytes(), b"before\nold A\nafter\n")
        self.assertEqual((self.root / "second/other.txt").read_bytes(), b"prepared neighbor")
        for index in (previous, applied["index"]):
            self.call("native", "archive-show", "archive/stage-index/" + index, "--verify")
        committed = self.call("native", "commit", "--staged", "-m", "chosen rows").stdout.decode().strip()
        self.call("gc", "--delete")
        self.call("native", "export", committed, "committed")
        self.assertEqual((self.root / "committed/code.cpp").read_bytes(), b"before\nold A\nafter\n")
        self.assertFalse(code.exists())
        self.assertNotEqual(head, committed)

    def test_bad_selections_wrong_plan_kind_and_context_do_not_write(self):
        code, head, previous = self.fixture()
        plan = self.plan("add", "code.cpp", "--context", "0")
        hunk = json.loads(self.call("native", "add-hunks-plan", "code.cpp").stdout)
        before = self.snapshot()
        for numbers in ("", "0", "1,1", "5", "1,", "-1", "100001"):
            self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", numbers, ok=False)
        self.call("native", "add-lines", "code.cpp", "--lines", "1", ok=False)
        self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", ok=False)
        self.call("native", "add-lines", "code.cpp", "--expect-plan", hunk["plan_id"], "--lines", "1", ok=False)
        self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", "1", ok=False)
        self.assertEqual(before, self.snapshot())
        applied = json.loads(self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", "3",
                                      "--context", "0").stdout)
        self.call("native", "export", applied["index"], "addition")
        self.assertEqual((self.root / "addition/code.cpp").read_bytes(), b"before\nold A\nold B\nnew A\nafter\n")
        reset = self.plan("reset")
        before = self.snapshot()
        self.call("native", "reset-lines", "code.cpp", "--expect-plan", reset["plan_id"], "--lines", "1", "--directory", ".", ok=False)
        self.assertEqual(before, self.snapshot())

    def test_unterminated_line_conflicts_are_explicit_and_localized(self):
        code = self.root / "code.cpp"
        code.write_bytes(b"old")
        self.commit("no final newline")
        code.write_bytes(b"new\n")
        plan = self.plan()
        before = self.snapshot()
        failed = self.call("--lang", "fr", "native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", "2", ok=False)
        self.assertIn("sans fin de ligne", failed.stderr.decode())
        self.assertEqual(before, self.snapshot())
        self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", "1,2")
        reset = self.plan("reset")
        before = self.snapshot()
        self.call("native", "reset-lines", "code.cpp", "--expect-plan", reset["plan_id"], "--lines", "1", ok=False)
        self.assertEqual(before, self.snapshot())
        restored = json.loads(self.call("native", "reset-lines", "code.cpp", "--expect-plan", reset["plan_id"], "--lines", "1,2").stdout)
        self.call("native", "export", restored["index"], "restored")
        self.assertEqual((self.root / "restored/code.cpp").read_bytes(), b"old")
        self.assertEqual(code.read_bytes(), b"new\n")

    def test_binary_presence_limits_and_unborn_index(self):
        code, head, previous = self.fixture()
        limited = self.plan("add", "code.cpp", "--max-lines", "1", ready=False)
        self.assertEqual(limited["diff"]["reason"], "line_limit")
        code.write_bytes(b"binary\0payload")
        self.assertEqual(self.plan(ready=False)["diff"]["status"], "binary")
        code.unlink()
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan(ready=False)["blockers"]])
        code.write_bytes(OLD)
        self.call("native", "add", "code.cpp", "--branch", "unborn")
        code.write_bytes(NEW)
        plan = self.plan("add", "code.cpp", "--branch", "unborn")
        applied = json.loads(self.call("native", "add-lines", "code.cpp", "--expect-plan", plan["plan_id"], "--lines", "1,3", "--branch", "unborn").stdout)
        self.call("native", "export", applied["index"], "initial")
        self.assertEqual((self.root / "initial/code.cpp").read_bytes(), b"before\nold B\nnew A\nafter\n")
        blocked = self.plan("reset", "code.cpp", "--branch", "unborn", ready=False)
        self.assertIn("existing_file_required", [b["reason"] for b in blocked["blockers"]])


if __name__ == "__main__":
    unittest.main()
