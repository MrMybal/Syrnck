"""Raw Git snapshot discovery and deterministic, retained asset bundles."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())


class GitCatalog(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="syrnck-git-catalog-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo with 'quote é"
        self.repo.mkdir()
        self.env = {k: v for k, v in os.environ.items() if k.upper() not in
                    ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_SKIP_SMUDGE")}
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.initialize()

    def initialize(self, sha256=False):
        self.git("init", "--initial-branch=main", *( ["--object-format=sha256"] if sha256 else []))
        for key, value in (("user.name", "Syrnck tests"), ("user.email", "test@example.invalid"),
                           ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            self.git("config", key, value)
        self.cli("git", "install")
        self.store = self.repo / ".git/syrnck"

    def git(self, *args, data=None, expected=0):
        result = subprocess.run(["git", "-C", str(self.repo), *map(str, args)], input=data,
                                env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result.stdout

    def cli(self, *args, expected=0, data=None, directory=None):
        result = subprocess.run([CLI, *map(str, args)], cwd=directory or self.repo, env=self.env,
                                input=data, capture_output=True, timeout=25)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result

    def snapshot(self):
        (self.repo / ".gitattributes").write_text("*.uasset filter=syrnck -text\n", encoding="utf-8")
        self.data = bytes(range(256)) * 4096
        (self.repo / "Map.uasset").write_bytes(self.data)
        (self.repo / "Game.cpp").write_text("int main() {}\n", encoding="utf-8")
        (self.repo / "large.raw").write_bytes(b"ordinary Git blob" * 131072)
        self.git("add", ".")
        self.git("commit", "-m", "snapshot")
        return self.git("rev-parse", "HEAD").decode().strip()

    def test_catalog_uses_commit_not_index_or_worktree_and_prepare_retains_closure(self):
        commit = self.snapshot()
        initial = json.loads(self.cli("git", "catalog", "HEAD").stdout)
        self.assertEqual(initial["commit"], commit)
        self.assertEqual(len(initial["assets"]), 1)
        asset = initial["assets"][0]
        self.assertEqual(asset["path"], "Map.uasset")
        self.assertEqual(asset["size"], len(self.data))
        (self.repo / "Map.uasset").write_bytes(b"dirty index")
        self.git("add", "Map.uasset")
        self.assertEqual(json.loads(self.cli("git", "catalog", commit).stdout), initial)
        prepared = json.loads(self.cli("git", "prepare-assets", commit).stdout)
        again = json.loads(self.cli("git", "prepare-assets", commit, "--progress-json").stdout)
        self.assertEqual(prepared, again)
        metadata = json.loads(self.cli("--store", self.store, "inspect", "revision", prepared["bundle"]["id"]).stdout)
        self.assertEqual(metadata["parents"], [])
        self.assertEqual(set(metadata["files"]), {"Map.uasset"})
        self.assertEqual(metadata["timestamp"], 0)
        refs = json.loads(self.cli("--store", self.store, "refs").stdout)
        for name in refs:
            if not name.startswith("git-assets/"):
                self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        output = self.root / "restored"
        self.cli("--store", self.store, "get", asset["manifest"], output)
        self.assertEqual(output.read_bytes(), self.data)

    def test_catalog_runs_in_clone_without_store_and_deduplicates_small_blob_reads(self):
        commit = self.snapshot()
        for index in range(160):
            (self.repo / f"file-{index}.txt").write_text(str(index), encoding="utf-8")
        pointer = self.git("show", "HEAD:Map.uasset")
        oid = self.git("hash-object", "-w", "--stdin", data=pointer).decode().strip()
        self.git("update-index", "--add", "--cacheinfo", f"120000,{oid},link")
        self.git("update-index", "--add", "--cacheinfo", f"160000,{commit},Module")
        self.git("add", "--", *[f"file-{i}.txt" for i in range(160)])
        self.git("commit", "-m", "more entries")
        clone = self.root / "clone"
        self.git("clone", "--no-checkout", self.repo, clone)
        value = json.loads(self.cli("git", "catalog", "HEAD", directory=clone).stdout)
        self.assertEqual([a["path"] for a in value["assets"]], ["Map.uasset"])
        self.assertFalse((clone / ".git/syrnck").exists())

    def test_missing_or_missized_assets_and_malformed_pointers_leave_pins_unchanged(self):
        self.snapshot()
        self.cli("git", "prepare-assets", "HEAD")
        refs = self.cli("--store", self.store, "refs").stdout
        for pointer in (b"version syrnck-pointer-v1\nbad\n",
                        b"version syrnck-pointer-v1\nmanifest blake3:" + b"f" * 64 + b"\nsize 12\n",
                        self.git("show", "HEAD:Map.uasset").replace(str(len(self.data)).encode(), b"42")):
            oid = self.git("hash-object", "-w", "--stdin", data=pointer).decode().strip()
            self.git("update-index", "--cacheinfo", f"100644,{oid},Map.uasset")
            self.git("commit", "-m", "invalid asset")
            self.cli("git", "prepare-assets", "HEAD", expected=1)
            self.assertEqual(self.cli("--store", self.store, "refs").stdout, refs)
        self.cli("git", "catalog", "--help")
        self.cli("git", "catalog", "--version^{commit}", expected=1)

    def test_sha256_git_ids(self):
        self.repo = self.root / "sha256"
        self.repo.mkdir()
        self.initialize(sha256=True)
        commit = self.snapshot()
        self.assertEqual(len(commit), 64)
        self.assertEqual(json.loads(self.cli("git", "catalog", "HEAD").stdout)["commit"], commit)
        self.cli("git", "prepare-assets", "HEAD")


if __name__ == "__main__":
    unittest.main()
