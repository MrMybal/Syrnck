"""Offline Git LFS migration: independent hashes, preserved state and real checkouts."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest
import git_catalog as base

PROBE = str(Path(sys.argv.pop(1)).resolve())


class LfsMigration(unittest.TestCase):
    setUp = base.GitCatalog.setUp
    initialize = base.GitCatalog.initialize
    git = base.GitCatalog.git
    cli = base.GitCatalog.cli

    def entry(self, path, data, mode="100644"):
        oid = self.git("hash-object", "-w", "--stdin", data=data).decode().strip()
        self.git("update-index", "--add", "--cacheinfo", f"{mode},{oid},{path}")
        return oid

    def source(self, assets=None):
        self.files = assets or {"Content/Map.uasset": bytes(range(256)) * 4096}
        self.cache = self.repo / ".git/lfs/objects"
        self.oids = {}
        attrs = b"*.uasset filter=lfs diff=lfs merge=lfs -text\n"
        (self.repo / ".gitattributes").write_bytes(attrs)
        self.entry(".gitattributes", attrs)
        for name, data in self.files.items():
            oid = hashlib.sha256(data).hexdigest()
            self.oids[name] = oid
            pointer = f"version https://git-lfs.github.com/spec/v1\noid sha256:{oid}\nsize {len(data)}\n".encode()
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(pointer)
            self.entry(name, pointer, "100755" if name == "run.bin" else "100644")
            cached = self.cache / oid[:2] / oid[2:4] / oid
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(data)
        (self.repo / "Game.cpp").write_bytes(b"int main() { return 0; }\n")
        self.git("add", "Game.cpp")
        self.git("commit", "-m", "LFS source")
        return self.git("rev-parse", "HEAD").decode().strip()

    def preview(self, *options, expected=0):
        return json.loads(self.cli("git", "lfs-preview", "HEAD", *options, expected=expected).stdout)

    def migrate(self, branch="converted", *options, expected=0):
        return self.cli("git", "migrate-lfs", "HEAD", "--branch", branch, *options, expected=expected)

    def test_conversion_preserves_source_state_nested_attributes_modes_and_shared_data(self):
        data = bytes(range(256)) * 4096
        source = self.source({"Content/Space [one].uasset": data, "Duplicate.uasset": data,
                              "run.bin": b"executable asset", "Content/Deep/Nested.uasset": b"nested"})
        # Preserve unconverted trees, Gitlinks and symbolic-link blobs verbatim.
        self.entry("link", b"somewhere", "120000")
        self.git("update-index", "--add", "--cacheinfo", f"160000,{'a' * len(source)},module")
        (self.repo / "Untouched").mkdir()
        (self.repo / "Untouched/code.cpp").write_bytes(b"untouched subtree")
        self.git("add", "Untouched/code.cpp")
        self.git("commit", "-m", "extra entries")
        source = self.git("rev-parse", "HEAD").decode().strip()
        untouched = self.git("rev-parse", "HEAD:Untouched")
        (self.repo / "Game.cpp").write_bytes(b"staged edits")
        self.git("add", "Game.cpp")
        (self.repo / "Game.cpp").write_bytes(b"unstaged edits")
        (self.repo / "untracked").write_bytes(b"keep me")
        index = (self.repo / ".git/index").read_bytes()
        cache_before = {p: p.read_bytes() for p in self.cache.rglob("*") if p.is_file()}
        plan = self.preview()
        self.assertTrue(plan["ready"])
        self.assertEqual(plan["unique_objects"], 3)
        self.assertEqual({a["status"] for a in plan["assets"]}, {"verified"})
        result = json.loads(self.migrate("converted", "--workers", "4", "--progress-json").stdout)
        self.assertEqual(result["source"], source)
        self.assertEqual(result["converted_assets"], 4)
        self.assertEqual(result["unique_objects"], 3)
        commit = result["commit"]
        self.assertEqual(self.git("rev-parse", "HEAD").decode().strip(), source)
        self.assertEqual(self.git("rev-parse", "converted^").decode().strip(), source)
        self.assertEqual((self.repo / ".git/index").read_bytes(), index)
        self.assertEqual((self.repo / "Game.cpp").read_bytes(), b"unstaged edits")
        self.assertEqual((self.repo / "untracked").read_bytes(), b"keep me")
        self.assertEqual({p: p.read_bytes() for p in cache_before}, cache_before)
        for path in ("link", "module", "Untouched"):
            self.assertEqual(self.git("ls-tree", source, "--", path), self.git("ls-tree", commit, "--", path))
        self.assertEqual(self.git("rev-parse", "converted:Untouched"), untouched)
        self.assertEqual(self.git("show", "converted:Game.cpp"), b"int main() { return 0; }\n")
        catalog = json.loads(self.cli("git", "catalog", "converted").stdout)
        self.assertEqual(len(catalog["assets"]), 4)
        self.assertTrue(next(a for a in catalog["assets"] if a["path"] == "run.bin")["executable"])
        refs = json.loads(self.cli("--store", self.store, "refs").stdout)
        self.assertEqual(refs, {"git-assets/" + commit: result["bundle"]})
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "pack")
        self.cli("--store", self.store, "fsck")
        linked = self.root / "linked checkout"
        self.git("worktree", "add", linked, "converted")
        for name, data in self.files.items():
            self.assertEqual((linked / name).read_bytes(), data)
        attributes = self.git("-C", linked, "check-attr", "filter", "text", "working-tree-encoding", "--",
                              "Content/Space [one].uasset").decode()
        self.assertIn("filter: syrnck", attributes)
        self.assertIn("working-tree-encoding: unset", attributes)
        self.migrate("converted", expected=1)

    def test_preview_works_without_store_key_or_working_files_and_custom_cache_is_explicit(self):
        self.source()
        cache = self.cache
        clone = self.root / "storeless clone"
        self.git("clone", "--no-checkout", self.repo, clone)
        self.repo = clone
        self.env["SYRNCK_KEY"] = "invalid key is irrelevant to preview"
        missing = self.preview(expected=1)
        self.assertFalse(missing["ready"])
        self.assertEqual(missing["assets"][0]["status"], "missing")
        self.git("config", "lfs.storage", "custom storage")
        self.cli("git", "lfs-preview", "HEAD", expected=1)
        plan = self.preview("--lfs-objects", cache)
        self.assertTrue(plan["ready"])
        self.assertFalse((clone / ".git/syrnck").exists())
        self.assertFalse((clone / "Content/Map.uasset").exists())

    def test_corrupt_missing_and_missized_objects_never_publish_and_retry_succeeds(self):
        source = self.source()
        oid = self.oids["Content/Map.uasset"]
        cached = self.cache / oid[:2] / oid[2:4] / oid
        good = cached.read_bytes()
        refs = self.git("show-ref")
        cached.unlink()
        self.assertEqual(self.preview(expected=1)["assets"][0]["status"], "missing")
        self.migrate(expected=1)
        cached.write_bytes(b"wrong size")
        self.assertEqual(self.preview(expected=1)["assets"][0]["status"], "size_mismatch")
        self.migrate(expected=1)
        cached.write_bytes(bytes([good[0] ^ 1]) + good[1:])
        self.assertEqual(self.preview(expected=1)["assets"][0]["status"], "hash_mismatch")
        failure = self.migrate("converted", "--lang", "fr", expected=1)
        self.assertIn("SHA-256", failure.stderr.decode())
        self.assertEqual(self.git("show-ref"), refs)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})
        self.cli("--store", self.store, "gc", "--delete")
        self.assertEqual(self.git("rev-parse", "HEAD").decode().strip(), source)
        cached.write_bytes(good)
        self.assertTrue(self.preview()["ready"])
        self.migrate()

    def test_sha256_vectors_encrypted_store_and_sha256_git_repository(self):
        self.repo = self.root / "sha256"
        self.repo.mkdir()
        self.git("init", "--initial-branch=main", "--object-format=sha256")
        for key, value in (("user.name", "Syrnck tests"), ("user.email", "test@example.invalid"),
                           ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            self.git("config", key, value)
        self.env["SYRNCK_KEY"] = "43" * 32
        self.cli("git", "install", "--encrypted")
        self.store = self.repo / ".git/syrnck"
        files = {"empty.bin": b"", "abc.bin": b"abc", "million.bin": b"a" * 1000000}
        for size in (1, 55, 56, 63, 64, 65, 127, 128, 65535, 65536, 65537):
            files[f"vectors/{size}.bin"] = (bytes(range(256)) * (size // 256 + 1))[:size]
        source = self.source(files)
        self.assertEqual(len(source), 64)
        self.assertTrue(self.preview()["ready"])
        result = json.loads(self.migrate().stdout)
        self.assertEqual(len(result["commit"]), 64)
        linked = self.root / "sha256 linked"
        self.git("worktree", "add", linked, "converted")
        for name, data in files.items(): self.assertEqual((linked / name).read_bytes(), data)
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "fsck")

    def test_malformed_extended_and_legacy_pointers_and_existing_branches_are_refused(self):
        self.source()
        good = self.git("show", "HEAD:Content/Map.uasset")
        for branch in ("main", "HEAD", "bad name", "../other", "-option"):
            self.migrate(branch, expected=1)
        self.git("symbolic-ref", "refs/heads/dangling", "refs/heads/missing")
        self.migrate("dangling", expected=1)
        self.assertEqual(self.git("symbolic-ref", "refs/heads/dangling"), b"refs/heads/missing\n")
        self.git("symbolic-ref", "--delete", "refs/heads/dangling")
        for pointer in (good.replace(b"size ", b"size 0"), good.replace(b"sha256:", b"sha512:"),
                        good.replace(b"/spec/v1", b"/spec/v2"),
                        good + b"unknown value\n", good.replace(b"oid ", b"ext-0-transform x\noid "),
                        good.replace(b"git-lfs", b"hawser")):
            self.entry("Content/Map.uasset", pointer)
            self.git("commit", "-m", "unsupported pointer")
            self.cli("git", "lfs-preview", "HEAD", expected=1)
            self.migrate(expected=1)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})

    def test_symlinked_attributes_are_rejected_before_ingestion(self):
        self.source()
        self.entry("Content/.gitattributes", b"target", "120000")
        self.git("commit", "-m", "symlinked attributes")
        self.migrate(expected=1)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})

    def test_cancellation_reference_races_and_late_stop_preserve_publication_boundaries(self):
        self.source()
        for mode in ("cancel_ingest", "cancel_publish", "race_ref", "race_symbolic", "late_stop"):
            if mode == "late_stop":
                hook = self.repo / ".git/hooks/reference-transaction"
                hook.write_bytes(b'#!/bin/sh\nif test "$1" = committed; then sleep 1; fi\nexit 0\n')
                hook.chmod(0o755)
            result = subprocess.run([PROBE, mode], cwd=self.repo, env=self.env,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))

    def test_unborn_head_and_other_worktree_branches_are_protected(self):
        source = self.source()
        self.git("symbolic-ref", "HEAD", "refs/heads/selected")
        self.cli("git", "migrate-lfs", source, "--branch", "selected", expected=1)
        self.git("symbolic-ref", "HEAD", "refs/heads/main")
        linked = self.root / "unborn worktree"
        self.git("worktree", "add", "--detach", linked, source)
        self.git("-C", linked, "symbolic-ref", "HEAD", "refs/heads/selected")
        self.cli("git", "migrate-lfs", source, "--branch", "selected", expected=1)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})

    def test_reference_transaction_hooks_can_reject_or_log_without_being_bypassed(self):
        self.source()
        hook = self.repo / ".git/hooks/reference-transaction"
        hook.write_bytes(b'#!/bin/sh\necho "custom reference hook"\nif test "$1" = prepared; then exit 1; fi\n')
        hook.chmod(0o755)
        self.migrate(expected=1)
        self.git("show-ref", "--verify", "--quiet", "refs/heads/converted", expected=1)
        hook.write_bytes(b'#!/bin/sh\necho "custom reference hook"\nexit 0\n')
        self.migrate()

    def test_existing_syrnck_assets_must_also_be_available_before_new_branch(self):
        self.source()
        original = b"existing Syrnck asset" * 4096
        pointer = self.cli("--store", self.store, "git", "clean", data=original).stdout
        self.entry("Existing.syr", pointer)
        self.entry(".gitattributes", self.git("show", "HEAD:.gitattributes") + b"*.syr filter=syrnck -text\n")
        self.git("commit", "-m", "mixed asset formats")
        refs = json.loads(self.cli("--store", self.store, "refs").stdout)
        for name in refs: self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        self.assertTrue(self.preview()["ready"])  # The preview audits LFS cache objects only.
        self.migrate(expected=1)
        self.git("show-ref", "--verify", "--quiet", "refs/heads/converted", expected=1)
        self.assertEqual(self.cli("--store", self.store, "git", "clean", data=original).stdout, pointer)
        result = json.loads(self.migrate().stdout)
        self.assertEqual(len(json.loads(self.cli("git", "catalog", result["commit"]).stdout)["assets"]), 2)

    def test_lost_publication_acknowledgement_reconciles_only_the_exact_commit(self):
        self.source()
        for mode in ("lost_ack", "advanced_ack"):
            result = subprocess.run([PROBE, mode], cwd=self.repo, env=self.env,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))


if __name__ == "__main__":
    unittest.main()
