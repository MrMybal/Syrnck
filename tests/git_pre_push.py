"""Real pushes to an isolated local Git remote, guarded by the installed hook."""
import json
import unittest
import git_asset_transfers as base


class GitPrePush(base.GitNetwork):
    def setup_remote(self, sha256=False):
        self.origin = self.root / "origin.git"
        self.git("init", "--bare", "--initial-branch=main",
                 *(["--object-format=sha256"] if sha256 else []), self.origin)
        self.git("remote", "add", "origin", self.origin)
        self.cli("git", "install-hook")

    def remote_refs(self):
        return self.git("--git-dir", self.origin, "for-each-ref", "--format=%(refname)").splitlines()

    def test_hook_uploads_all_ancestors_tags_and_multiple_refs_before_git_publication(self):
        first = self.snapshot()
        (self.repo / "Map.uasset").write_bytes(b"second version of asset")
        self.git("add", "Map.uasset")
        self.git("commit", "-m", "second version")
        second = self.git("rev-parse", "HEAD").decode().strip()
        self.git("rm", "Map.uasset")
        self.git("commit", "-m", "remove from tip")
        self.git("branch", "retained", first)
        self.git("tag", "-a", "release", first, "-m", "tagged version")
        self.setup_remote()
        self.cli("git", "configure-remote", "origin", self.url)
        self.cli("git", "install-hook")  # Idempotent, with a quoted/UTF-8 store path.
        hook = (self.repo / ".git/hooks/pre-push").read_text(encoding="utf-8")
        self.assertNotIn(base.TOKEN, hook)
        self.assertNotIn(base.TOKEN, (self.repo / ".git/config").read_text(encoding="utf-8"))
        with self.serving():
            self.git("push", "origin", "main", "retained", "refs/tags/release")
            self.assertEqual(set(self.references()), {"git-" + first, "git-" + second})
            self.assertEqual(set(self.remote_refs()), {b"refs/heads/main", b"refs/heads/retained", b"refs/tags/release"})
            clone = self.root / "clone"
            self.git("clone", "--no-checkout", self.origin, clone)
            self.repo = clone
            self.cli("git", "install")
            self.cli("git", "pull-assets", self.url, first)
            self.git("checkout", "--detach", first)
            self.assertEqual((clone / "Map.uasset").read_bytes(), self.data)
        self.server_command("gc", "--delete")
        self.server_command("fsck")

    def test_missing_endpoint_and_network_failure_block_git_and_can_retry(self):
        self.snapshot()
        self.setup_remote()
        self.git("push", "origin", "main", expected=1)
        self.assertEqual(self.remote_refs(), [])
        self.cli("git", "configure-remote", "origin", self.url)
        self.git("push", "origin", "main", expected=1)
        self.assertEqual(self.remote_refs(), [])
        with self.serving():
            self.git("push", "origin", "main")
            self.assertEqual(self.remote_refs(), [b"refs/heads/main"])

    def test_sha256_worktree_uses_the_shared_hook_and_store(self):
        self.repo = self.root / "sha256"
        self.repo.mkdir()
        self.initialize(sha256=True)
        commit = self.snapshot()
        self.setup_remote(sha256=True)
        self.cli("git", "configure-remote", "origin", self.url)
        linked = self.root / "linked-worktree"
        self.git("worktree", "add", "-b", "linked", linked)
        self.repo = linked
        (linked / "Map.uasset").write_bytes(b"asset edited in linked SHA-256 worktree")
        self.git("add", "Map.uasset")
        self.git("commit", "-m", "linked worktree edit")
        linked_commit = self.git("rev-parse", "HEAD").decode().strip()
        self.cli("git", "install-hook")
        with self.serving():
            self.git("push", "origin", "linked")
            self.assertEqual(set(self.references()), {"git-" + commit, "git-" + linked_commit})
        self.cli("--store", self.store, "fsck")

    def test_missing_old_assets_block_push_even_if_current_tree_has_no_assets(self):
        self.snapshot()
        self.git("rm", "Map.uasset")
        self.git("commit", "-m", "no assets at tip")
        for name in json.loads(self.cli("--store", self.store, "refs").stdout):
            self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        self.setup_remote()
        self.cli("git", "configure-remote", "origin", self.url)
        with self.serving():
            self.git("push", "origin", "main", expected=1)
            self.assertEqual(self.references(), {})
            self.assertEqual(self.remote_refs(), [])

    def test_deletions_and_code_only_push_need_no_block_remote(self):
        (self.repo / "Game.cpp").write_text("int main() {}\n", encoding="utf-8")
        self.git("add", "Game.cpp")
        self.git("commit", "-m", "code only")
        self.setup_remote()
        self.env.pop("SYRNCK_TOKEN")
        self.git("push", "origin", "main")
        oid = self.git("rev-parse", "HEAD").decode().strip()
        deleted = f"(delete) {'0' * len(oid)} refs/heads/main {oid}\n".encode()
        summary = json.loads(self.cli("git", "pre-push", "origin", "unused", data=deleted).stdout)
        self.assertEqual(summary["commits_scanned"], 0)

    def test_hooks_configuration_and_input_fail_closed(self):
        commit = self.snapshot()
        hook = self.repo / ".git/hooks/pre-push"
        hook.write_bytes(b"#!/bin/sh\nexit 42\n")
        self.cli("git", "install-hook", expected=1)
        self.assertEqual(hook.read_bytes(), b"#!/bin/sh\nexit 42\n")
        hook.unlink()
        self.git("config", "core.hooksPath", str(self.root / "custom-hooks"))
        self.cli("git", "install-hook", expected=1)
        self.git("config", "--unset", "core.hooksPath")
        self.cli("git", "install-hook")
        for data in (b"invalid\n", b"x" * 4097, b"\0", b"a b c d e\n"):
            self.cli("git", "pre-push", "origin", "unused", data=data, expected=1)
        for url in ("https://user:password@example.invalid", "http://192.0.2.1", "https://example.invalid?token=x"):
            self.cli("git", "configure-remote", "origin", url, expected=1)
        update = f"refs/heads/main {commit} refs/heads/main {'0' * len(commit)}\n".encode()
        shallow = self.repo / ".git/shallow"
        shallow.write_text(commit + "\n", encoding="utf-8")
        try:
            self.cli("git", "pre-push", "origin", "unused", data=update, expected=1)
        finally:
            shallow.unlink()
        grafts = self.repo / ".git/info/grafts"
        grafts.write_text(commit + "\n", encoding="utf-8")
        self.cli("git", "pre-push", "origin", "unused", data=update, expected=1)


if __name__ == "__main__":
    unittest.main()
