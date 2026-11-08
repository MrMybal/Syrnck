"""Detached native HEAD: Git oracle, defaults, private index and rewrite operations."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import native_workflows as base
import workflows


class NativeDetached(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def head(self):
        return json.loads(self.cmd("native", "head"))

    def refs(self):
        return json.loads(self.cmd("refs"))

    def test_git_oracle_selection_parentage_and_private_index(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-detached-git-") as temporary:
            oracle = Path(temporary)

            def git(*arguments):
                result = subprocess.run(["git", "-c", "safe.directory=" + oracle.as_posix(),
                    "-c", "user.name=Detached fixture", "-c", "user.email=detached@example.invalid",
                    "-c", "core.autocrlf=false", "-c", "commit.gpgsign=false", *arguments],
                    cwd=oracle, env=self.env, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
                return result.stdout.decode().strip()

            def commit_pair(content, message):
                (self.root / "code").write_bytes(content)
                (oracle / "code").write_bytes(content)
                native = self.commit(message)
                git("add", "code")
                git("commit", "-m", message)
                return native, git("rev-parse", "HEAD")

            git("init", "-b", "main")
            first, git_first = commit_pair(b"first\n", "first")
            main, git_main = commit_pair(b"main\n", "main")
            before = self.snapshot()
            plan = json.loads(self.cmd("native", "switch-plan", "--detach", first, "--verify"))
            self.assertTrue(plan["ready"] and plan["objects_verified"])
            self.assertIsNone(plan["destination_branch"])
            self.assertEqual(before, self.snapshot())
            self.cmd("native", "switch", "--detach", first)
            git("switch", "--detach", git_first)
            self.assertIsNone(self.head()["branch"])
            self.assertEqual(git("branch", "--show-current"), "")
            self.assertEqual((self.root / "code").read_bytes(), (oracle / "code").read_bytes())
            detached, git_detached = commit_pair(b"detached\n", "detached")
            self.assertEqual(json.loads(self.cmd("inspect", "revision", detached))["parents"], [first])
            self.assertEqual(git("rev-parse", git_detached + "^"), git_first)
            self.assertEqual(self.refs()["heads/main"]["id"], main)
            self.assertEqual(git("rev-parse", "main"), git_main)
            (self.root / "code").write_bytes(b"prepared\n")
            self.cmd("native", "add", "code")
            self.assertEqual(json.loads(self.cmd("native", "staged"))["branch"], ":HEAD")
            self.assertIn("workspace/staging", self.refs())
            self.assertNotIn("staging/:HEAD", self.refs())
            (self.root / "code").write_bytes(b"later\n")
            staged = self.cmd("native", "commit", "--staged", "-m", "prepared").decode().strip()
            self.assertEqual(self.head()["revision"], staged)
            self.assertEqual(json.loads(self.cmd("inspect", "revision", staged))["parents"], [detached])
            combined = json.loads(self.cmd("native", "status", "--combined"))
            self.assertFalse(combined["unstaged"]["clean"])
            self.commit("later")
            self.cmd("native", "branch", "rescue", "HEAD")
            self.cmd("native", "tag", "saved", "HEAD")
            self.cmd("native", "switch", "main")
            git("switch", "main")
            self.assertEqual(self.head()["revision"], main)
            self.assertEqual((self.root / "code").read_bytes(), (oracle / "code").read_bytes())
            self.cmd("gc", "--delete")
            self.cmd("native", "export", staged, self.store / "retained")
            self.assertEqual((self.store / "retained/code").read_bytes(), b"prepared\n")

    def test_replay_merge_rebase_amend_and_partial_staging_follow_detached_head(self):
        code = self.root / "code"
        code.write_bytes(b"base\n")
        first = self.commit("base")
        (self.root / "upstream").write_bytes(b"upstream\n")
        main = self.commit("upstream")
        self.cmd("native", "switch", "--detach", first)
        code.write_bytes(b"own\n")
        own = self.commit("own")
        rebased = json.loads(self.cmd("native", "rebase", first, "--onto", main))
        self.assertEqual(self.head()["revision"], rebased["target"])
        self.assertEqual(self.refs()[rebased["archive"]]["id"], own)
        self.assertEqual((self.root / "upstream").read_bytes(), b"upstream\n")
        self.cmd("native", "commit", "--amend", "--message-only", "-m", "renamed message")
        amended = self.head()["revision"]
        self.assertEqual(json.loads(self.cmd("inspect", "revision", amended))["message"], "renamed message")
        reverted = json.loads(self.cmd("native", "revert", main, "-m", "undo upstream"))
        self.assertEqual(self.head()["revision"], reverted["target"])
        self.assertFalse((self.root / "upstream").exists())
        picked = json.loads(self.cmd("native", "cherry-pick", main, "-m", "restore upstream"))
        self.assertEqual(self.head()["revision"], picked["target"])
        self.cmd("native", "switch", "--detach", first)
        merged = json.loads(self.cmd("native", "merge", main))
        self.assertEqual(self.head()["revision"], main)
        self.assertEqual(merged["state"], "committed")
        code.write_bytes(b"new base\n")
        plan = json.loads(self.cmd("native", "add-hunks-plan", "code"))
        self.cmd("native", "add-hunks", "code", "--expect-plan", plan["plan_id"], "--hunks", "1")
        self.cmd("native", "commit", "--staged", "-m", "partial change")
        self.assertEqual(self.refs()["heads/main"]["id"], main)
        self.assertEqual(self.head()["mode"], "detached")

    def test_preflight_blockers_and_french_named_publication_error(self):
        asset = self.root / "asset"
        asset.write_bytes(b"first")
        first = self.commit("first")
        asset.write_bytes(b"main")
        main = self.commit("main")
        asset.write_bytes(b"dirty")
        before = self.snapshot()
        blocked = json.loads(self.cmd("native", "switch-plan", "--detach", first, ok=False))
        self.assertFalse(blocked["ready"])
        self.call("native", "switch", "--detach", first, ok=False)
        self.assertEqual(before, self.snapshot())
        asset.write_bytes(b"main")
        self.cmd("native", "switch", "--detach", first)
        asset.write_bytes(b"staged")
        self.cmd("native", "add", "asset")
        before = self.snapshot()
        self.call("native", "switch", "main", ok=False)
        self.assertEqual(before, self.snapshot())
        self.cmd("native", "reset")
        asset.write_bytes(b"first")
        error = self.call("--lang", "fr", "native", "push", "http://127.0.0.1:1", ok=False)
        self.assertIn("branche nommée", error.stderr.decode("utf-8"))
        self.assertEqual(self.refs()["heads/main"]["id"], main)
        self.call("native", "branch", ":HEAD", first, ok=False)

    def test_branch_lifecycle_does_not_attach_or_discard_the_private_index(self):
        asset = self.root / "asset"
        asset.write_bytes(b"base")
        main = self.commit("base")
        self.cmd("native", "switch", "--detach", main)
        asset.write_bytes(b"detached child")
        detached = self.commit("detached child")
        asset.write_bytes(b"private prepared")
        index = self.cmd("native", "add", "asset").decode().strip()
        asset.write_bytes(b"private working")
        before = asset.read_bytes()
        self.cmd("native", "branch", "rescue", "HEAD")
        listing = json.loads(self.cmd("native", "branches", "--verify"))
        self.assertTrue(all(not row["current"] for row in listing["branches"]))
        deleted = json.loads(self.cmd("native", "branch-delete", "main", "--expect", main))
        self.assertNotIn("heads/main", self.refs())
        self.cmd("native", "branch-restore", deleted["archive"])
        self.assertEqual(self.refs()["heads/main"]["id"], main)
        self.assertEqual(self.head()["revision"], detached)
        self.assertIsNone(self.head()["branch"])
        self.assertEqual(self.refs()["workspace/staging"]["id"], index)
        self.assertEqual(asset.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
