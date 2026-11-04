"""Grouped Git assets against the production encrypted block server."""
import contextlib
import json
import os
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request
import git_catalog as base

TOKEN = "syrnck-test-token-0123456789-0123456789"


class GitNetwork(unittest.TestCase):
    initialize = base.GitCatalog.initialize
    git = base.GitCatalog.git
    cli = base.GitCatalog.cli
    snapshot = base.GitCatalog.snapshot

    def setUp(self):
        base.GitCatalog.setUp(self)
        self.env["SYRNCK_TOKEN"] = TOKEN
        self.server_store = self.root / "block-server"
        self.server_command("init", "--encrypted")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.url = f"http://127.0.0.1:{self.port}"

    def server_command(self, *args):
        result = subprocess.run([base.CLI, "--store", str(self.server_store), *map(str, args)],
            env=dict(self.env, SYRNCK_KEY="39" * 32), capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return result.stdout

    def references(self):
        request = urllib.request.Request(self.url + "/v1/refs", headers={"Authorization": "Bearer " + TOKEN})
        with urllib.request.urlopen(request, timeout=2) as response:
            return json.loads(response.read())

    @contextlib.contextmanager
    def serving(self):
        process = subprocess.Popen([base.CLI, "--store", str(self.server_store), "serve", "--listen",
                                    f"127.0.0.1:{self.port}", "--require-lease"],
            env=dict(self.env, SYRNCK_KEY="39" * 32), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 8
            while True:
                if process.poll() is not None:
                    self.fail(process.stderr.read().decode("utf-8", "replace"))
                try:
                    self.references()
                    break
                except urllib.error.URLError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.02)
            yield
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)


class GitAssetTransfers(GitNetwork):
    def test_grouped_transfer_clone_checkout_deduplication_and_gc(self):
        self.snapshot()
        (self.repo / "Duplicate.uasset").write_bytes(self.data)
        self.git("add", "Duplicate.uasset")
        self.git("commit", "-m", "shared manifest")
        commit = self.git("rev-parse", "HEAD").decode().strip()
        with self.serving():
            pushed = json.loads(self.cli("git", "push-assets", self.url, commit, "--requests", 4,
                                         "--require-lease").stdout)
            self.assertEqual(pushed["assets"], 2)
            self.assertEqual(self.references(), {"git-" + commit: pushed["bundle"]})
            again = json.loads(self.cli("git", "push-assets", self.url, commit).stdout)
            self.assertEqual(again["transfer"]["transferred_objects"], 0)
            clone = self.root / "clone"
            self.git("clone", "--no-checkout", self.repo, clone)
            self.repo = clone
            self.store = clone / ".git/syrnck"
            self.cli("git", "install")
            keep = clone / "keep.txt"
            keep.write_bytes(b"untracked working file")
            pulled = json.loads(self.cli("git", "pull-assets", self.url, "HEAD", "--requests", 4,
                                         "--require-read-lease").stdout)
            self.assertEqual(pulled["bundle"], pushed["bundle"])
            self.assertEqual(keep.read_bytes(), b"untracked working file")
            self.assertFalse((clone / "Map.uasset").exists())
            refs = json.loads(self.cli("--store", self.store, "refs").stdout)
            self.assertEqual(refs, {"git-assets/" + commit: pushed["bundle"]})
            self.git("checkout", "--force", "HEAD")
            self.assertEqual((clone / "Map.uasset").read_bytes(), self.data)
            self.assertEqual((clone / "Duplicate.uasset").read_bytes(), self.data)
            self.cli("--store", self.store, "gc", "--delete")
            self.cli("--store", self.store, "fsck")
        self.server_command("gc", "--delete")
        self.server_command("fsck")

    def test_commit_references_are_immutable_and_wrong_roots_do_not_import(self):
        commit = self.snapshot()
        with self.serving():
            pushed = json.loads(self.cli("git", "push-assets", self.url, commit).stdout)
        revision = json.loads(self.server_command("inspect", "revision", pushed["bundle"]["id"]))
        manifest = json.loads(self.server_command("inspect", "manifest", revision["files"]["Map.uasset"]["manifest"]))
        chunk = manifest["blocks"][0]["id"]
        self.server_command("pin", "git-" + commit, "chunk", chunk)
        target = self.root / "empty"
        self.cli("--store", target, "init")
        with self.serving():
            self.cli("git", "push-assets", self.url, commit, expected=1)
            self.assertEqual(self.references()["git-" + commit], {"kind": "chunk", "id": chunk})
            self.cli("--store", target, "git", "pull-assets", self.url, commit, expected=1)
        self.assertEqual(json.loads(self.cli("--store", target, "refs").stdout), {})

    def test_network_failure_keeps_source_retained_and_retry_publishes_complete_bundle(self):
        commit = self.snapshot()
        self.cli("git", "push-assets", self.url, commit, "--retries", 0, expected=1)
        refs = json.loads(self.cli("--store", self.store, "refs").stdout)
        self.assertIn("git-assets/" + commit, refs)
        for name in refs:
            if name.startswith("git/"):
                self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        with self.serving():
            self.cli("git", "push-assets", self.url, commit)
            self.assertIn("git-" + commit, self.references())


if __name__ == "__main__":
    unittest.main()
