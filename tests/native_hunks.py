"""Numbered hunk selection, preview drift and retained prepared versions."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows


def body(mask):
    return b"".join((b"changed " if (line == 2 and mask & 1) or (line == 12 and mask & 2) or
                     (line == 22 and mask & 4) else b"") + f"line {line}\n".encode()
                    for line in range(25))


class NativeHunks(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, path="code.cpp", *args, ready=True):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "add-hunks-plan", path,
                                 *map(str, args)], cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ready else 1, result.stderr.decode("utf-8", "replace"))
        value = json.loads(result.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def test_selected_text_preserves_other_preparations_and_retains_old_index(self):
        code = self.root / "code.cpp"
        other = self.root / "other.txt"
        code.write_bytes(body(0))
        other.write_bytes(b"original other")
        head = self.commit("baseline")
        other.write_bytes(b"prepared other")
        previous = self.call("native", "add", other.name).stdout.decode().strip()
        code.write_bytes(body(7))
        before = self.snapshot()
        plan = self.plan()
        self.assertEqual(plan, self.plan("code.cpp", "--lang", "fr"))
        self.assertEqual([h["number"] for h in plan["diff"]["hunks"]], [1, 2, 3])
        self.assertEqual(before, self.snapshot())
        result = self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"],
                           "--hunks", "3,1", "--progress-json")
        applied = json.loads(result.stdout)
        self.assertEqual(applied["previous_index"], previous)
        self.assertEqual(applied["state"], "applied")
        self.assertTrue(any(json.loads(line)["phase"] == "publishing" for line in result.stderr.splitlines()))
        self.call("gc", "--delete")
        self.call("native", "export", applied["index"], "frozen")
        self.assertEqual((self.root / "frozen/code.cpp").read_bytes(), body(5))
        self.assertEqual((self.root / "frozen/other.txt").read_bytes(), b"prepared other")
        self.call("native", "export", previous, "previous")
        self.assertEqual((self.root / "previous/code.cpp").read_bytes(), body(0))
        archive = json.loads(self.call("native", "archive-show", "archive/stage-index/" + previous, "--verify").stdout)
        self.assertEqual(archive["archive"]["kind"], "stage-index")
        committed = self.call("native", "commit", "--staged", "-m", "chosen changes").stdout.decode().strip()
        self.call("gc", "--delete")
        self.call("native", "export", committed, "committed")
        self.assertEqual((self.root / "committed/code.cpp").read_bytes(), body(5))
        self.assertEqual(code.read_bytes(), body(7))
        self.assertNotEqual(head, committed)

    def test_stale_previews_invalid_numbers_and_changed_index_are_refused(self):
        code = self.root / "code.cpp"
        code.write_bytes(body(0))
        (self.root / "other.txt").write_bytes(b"old")
        first = self.commit("base")
        second = self.commit("same files, new commit")
        self.call("pin", "heads/main", "revision", first)
        code.write_bytes(body(7))
        plan = self.plan()
        before = self.snapshot()
        code.write_bytes(body(3))
        self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", ok=False)
        code.write_bytes(body(7))
        for numbers in ("", "0", "1,1", "4", "1,", "-1"):
            self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", numbers, ok=False)
        self.call("native", "add-hunks", "code.cpp", "--hunks", "1", ok=False)
        self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", "--context", "0", ok=False)
        self.assertEqual(before, self.snapshot())
        self.call("native", "add", "other.txt")
        before = self.snapshot()
        self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "1", ok=False)
        self.assertEqual(before, self.snapshot())
        self.call("pin", "heads/main", "revision", second)
        blocked = self.plan(ready=False)
        self.assertIn("stale_staging", [b["reason"] for b in blocked["blockers"]])
        before = self.snapshot()
        self.call("native", "add-hunks", "code.cpp", "--expect-plan", blocked["plan_id"], "--hunks", "1", ok=False)
        self.assertEqual(before, self.snapshot())

    def test_blocked_binary_presence_and_budget_results(self):
        code = self.root / "code.cpp"
        code.write_bytes(body(0))
        self.commit("base")
        equal = self.plan(ready=False)
        self.assertIn("no_text_changes", [b["reason"] for b in equal["blockers"]])
        code.write_bytes(b"binary\0payload")
        self.assertEqual(self.plan(ready=False)["diff"]["status"], "binary")
        code.unlink()
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan(ready=False)["blockers"]])
        code.write_bytes(body(7))
        limited = self.plan("code.cpp", "--max-lines", "1", ready=False)
        self.assertEqual(limited["diff"]["reason"], "line_limit")
        (self.root / "new.cpp").write_bytes(b"new")
        self.assertIn("existing_file_required", [b["reason"] for b in self.plan("new.cpp", ready=False)["blockers"]])

    def test_nondefault_context_is_bound_to_plan(self):
        code = self.root / "code.cpp"
        code.write_bytes(body(0))
        self.commit("base")
        code.write_bytes(body(7))
        plan = self.plan("code.cpp", "--context", "0")
        self.call("native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"], "--hunks", "2", ok=False)
        applied = json.loads(self.call("--lang", "fr", "native", "add-hunks", "code.cpp", "--expect-plan", plan["plan_id"],
                                      "--hunks", "2", "--context", "0").stdout)
        self.call("native", "export", applied["index"], "frozen")
        self.assertEqual((self.root / "frozen/code.cpp").read_bytes(), body(2))


if __name__ == "__main__":
    unittest.main()
