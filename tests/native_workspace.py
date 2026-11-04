"""Combined native status and read-only checkout preflight through the CLI."""
import json
import os
import subprocess
import unittest

import native_workflows as base
from workflows import CLI


class NativeWorkspace(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def plan(self, target, *args, ready=True):
        run = subprocess.run([CLI, "--store", str(self.store), "native", "checkout-plan",
                              target, *map(str, args)], cwd=self.root, env=self.env,
                             capture_output=True, timeout=20)
        self.assertEqual(run.returncode, 0 if ready else 1, run.stderr.decode("utf-8", "replace"))
        result = json.loads(run.stdout)
        self.assertEqual(result["ready"], ready)
        return result, run

    def test_combined_status_keeps_both_sides_of_the_same_path(self):
        empty = json.loads(self.call("native", "status", "--combined").stdout)
        self.assertTrue(empty["clean"])
        self.assertIsNone(empty["staged"]["head"])
        (self.root / "file.txt").write_bytes(b"baseline")
        first = self.commit("baseline")
        (self.root / "file.txt").write_bytes(b"prepared")
        prepared = self.call("native", "add", ".").stdout.decode().strip()
        (self.root / "file.txt").write_bytes(b"baseline")
        self.assertTrue(json.loads(self.call("native", "status").stdout)["clean"])
        before = self.snapshot()
        run = self.call("native", "status", "--combined", "--progress-json")
        result = json.loads(run.stdout)
        self.assertFalse(result["clean"])
        self.assertEqual(result["staged"]["base"], first)
        self.assertEqual(result["staged"]["snapshot"], prepared)
        self.assertEqual(result["unstaged"]["base"], prepared)
        for side in ("staged", "unstaged"):
            self.assertEqual(result[side]["changes"][0]["path"], "file.txt")
            self.assertEqual(result[side]["changes"][0]["status"], "modified")
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "status", "--combined").stdout), result)
        self.assertTrue(all(json.loads(line)["phase"] != "publishing" for line in run.stderr.splitlines()))
        self.assertEqual(before, self.snapshot())
        self.call("native", "status", "--combined", "--revision", first, ok=False)
        self.call("native", "status", "--combined", "--combined", ok=False)
        self.call("native", "reset")
        self.assertTrue(json.loads(self.call("native", "status", "--combined").stdout)["clean"])

    def test_clean_plan_counts_progress_verification_and_custom_directory(self):
        (self.root / "edited").write_bytes(b"old")
        (self.root / "removed").write_bytes(b"remove")
        first = self.commit("first")
        (self.root / "edited").write_bytes(b"new")
        (self.root / "removed").unlink()
        (self.root / "Éditeur.cpp").write_bytes(b"source\n")
        second = self.call("native", "commit", "--branch", "target", "-m", "target").stdout.decode().strip()
        (self.root / "edited").write_bytes(b"old")
        (self.root / "removed").write_bytes(b"remove")
        (self.root / "Éditeur.cpp").unlink()
        before = self.snapshot()
        plan, run = self.plan(second, "--directory", self.root, "--verify", "--progress-json")
        self.assertEqual(plan["source"], first)
        self.assertTrue(plan["objects_verified"])
        self.assertEqual(plan["policy"], "clean-tracked-v1")
        self.assertEqual(plan["counts"], {"create": 1, "remove": 1, "replace": 1,
                                         "reuse": 0, "set_executable": 0, "unchanged": 0})
        self.assertEqual(plan["restore_bytes"], 10)
        self.assertEqual(plan["hashed_bytes"], 9)
        events = [json.loads(line) for line in run.stderr.splitlines()]
        self.assertTrue(any(e["phase"] == "scanning" for e in events))
        self.assertTrue(any(e["phase"] == "verifying" for e in events))
        self.assertEqual(events[-1]["phase"], "planning")
        self.assertTrue(events[-1]["complete"])
        self.assertFalse(any(e["phase"] == "publishing" for e in events))
        french = self.call("--lang", "fr", "native", "checkout-plan", second, "--verify")
        self.assertEqual(json.loads(french.stdout), plan)
        self.assertEqual(before, self.snapshot())
        self.assertEqual((self.root / "edited").read_bytes(), b"old")
        self.assertFalse((self.root / "Éditeur.cpp").exists())
        self.assertEqual(json.loads(self.cmd("refs"))["heads/main"]["id"], first)

    def test_blockers_include_ignored_files_staging_and_portable_spelling(self):
        (self.root / ".syrnckignore").write_text("Ignored/\n", encoding="utf-8")
        (self.root / "file").write_bytes(b"baseline")
        first = self.commit("first")
        (self.root / "Ignored").mkdir()
        (self.root / "Ignored/collision").write_bytes(b"local ignored content")
        # Temporarily include the directory to build a target snapshot.
        (self.root / ".syrnckignore").write_bytes(b"")
        second = self.call("native", "commit", "--branch", "target", "-m", "target").stdout.decode().strip()
        (self.root / ".syrnckignore").write_text("Ignored/\n", encoding="utf-8")
        plan, _ = self.plan(second, ready=False)
        self.assertIn({"path": "Ignored/collision", "reason": "untracked_collision",
                       "obstruction": "Ignored/collision"}, plan["conflicts"])
        self.call("native", "add", "file")
        plan, _ = self.plan(first, ready=False)
        self.assertIn("staging_present", [c["reason"] for c in plan["conflicts"]])
        self.call("native", "detach-history", "-m", "move branch")
        plan, _ = self.plan(first, ready=False)
        self.assertIn("stale_staging", [c["reason"] for c in plan["conflicts"]])
        combined = json.loads(self.call("native", "status", "--combined").stdout)
        self.assertFalse(combined["clean"])
        self.assertTrue(combined["staged"]["stale"])
        self.call("native", "reset")

    def test_link_obstructions_are_never_followed(self):
        (self.root / "base").write_bytes(b"base")
        self.commit("baseline")
        (self.root / "linked").mkdir()
        (self.root / "linked/child").write_bytes(b"target")
        target = self.call("native", "commit", "--branch", "target", "-m", "target").stdout.decode().strip()
        (self.root / "linked/child").unlink()
        (self.root / "linked").rmdir()
        # The destination is owned by this fixture and remains untracked.
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "child").write_bytes(b"preserve outside bytes")
        link = self.root / "linked"
        if os.name == "nt":
            made = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(outside)],
                                  capture_output=True, timeout=10)
            self.assertEqual(made.returncode, 0, made.stderr.decode(errors="replace"))
        else:
            link.symlink_to(outside, target_is_directory=True)
        try:
            plan, _ = self.plan(target, ready=False)
            self.assertIn({"path": "linked", "reason": "symlink_obstruction", "obstruction": "linked"}, plan["conflicts"])
            self.assertEqual(plan["hashed_bytes"], 4)
            self.assertEqual((outside / "child").read_bytes(), b"preserve outside bytes")
        finally:
            if os.name == "nt":
                os.rmdir(link)  # Remove only the junction, never recurse into its target.
            else:
                link.unlink()

    def test_errors_do_not_return_partial_reports(self):
        (self.root / "file").write_bytes(b"baseline")
        first = self.commit("baseline")
        before = self.snapshot()
        for args in ((), ("invalid",), ("0" * 64,), (first, "--from", first),
                     (first, "--verify", "--verify"), (first, "--branch", "../bad"),
                     (first, "--directory", self.store)):
            self.call("native", "checkout-plan", *args, ok=False)
        self.assertEqual(before, self.snapshot())


if __name__ == "__main__":
    unittest.main()
