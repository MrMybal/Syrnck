"""Conservative Git retention inventory and atomic verified pin additions."""
import json
from pathlib import Path
import subprocess
import sys
import git_catalog as base
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())


class GitRetention(unittest.TestCase):
    setUp = base.GitCatalog.setUp
    initialize = base.GitCatalog.initialize
    git = base.GitCatalog.git
    cli = base.GitCatalog.cli
    snapshot = base.GitCatalog.snapshot

    def pointer(self, data):
        raw = self.cli("--store", self.store, "git", "clean", data=data).stdout
        manifest = raw.split(b"manifest blake3:")[1].splitlines()[0].decode()
        blob = self.git("hash-object", "-w", "--stdin", data=raw).decode().strip()
        return manifest, blob, raw

    def orphan(self, data):
        manifest, blob, raw = self.pointer(data)
        tree = self.git("mktree", data=f"100644 blob {blob}\tAsset.uasset\n".encode()).decode().strip()
        commit = self.git("commit-tree", tree, data=b"independent history\n").decode().strip()
        return manifest, commit

    def scan(self, *args, expected=0):
        return json.loads(self.cli("git", "retention-scan", *args, expected=expected).stdout)

    def retention(self, *args, expected=0):
        return json.loads(self.cli("git", "retention", *args, expected=expected).stdout)

    def ids(self, inventory):
        return {item["manifest"] for item in inventory["manifests"]}

    def refs(self):
        return json.loads(self.cli("--store", self.store, "refs").stdout)

    def test_preview_apply_preserve_git_state_and_retain_index_and_history_after_gc(self):
        source = self.snapshot()
        original = json.loads(self.cli("git", "catalog", "HEAD").stdout)["assets"][0]["manifest"]
        staged, blob, raw = self.pointer(b"staged independent version" * 8192)
        self.git("update-index", "--add", "--cacheinfo", f"100644,{blob},Staged.uasset")
        for name in self.refs(): self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "pin", "user/keep", "manifest", original)
        before = self.refs()
        index = (self.repo / ".git/index").read_bytes()
        (self.repo / "Map.uasset").write_bytes(b"dirty working file")
        inventory = self.scan()
        self.assertEqual(self.ids(inventory), {original, staged})
        preview = self.retention()
        self.assertEqual(preview, self.retention("--lang", "fr"))
        self.assertTrue(preview["ready"])
        self.assertFalse(preview["applied"])
        self.assertEqual(len(preview["add"]), 2)
        self.assertEqual(self.refs(), before)
        result = self.retention("--apply", "--progress-json")
        self.assertTrue(result["applied"])
        self.assertEqual(result["added"], 2)
        self.assertEqual(self.refs()["user/keep"], before["user/keep"])
        self.assertEqual(self.retention("--apply")["added"], 0)
        self.assertEqual(self.git("rev-parse", "HEAD").decode().strip(), source)
        self.assertEqual((self.repo / ".git/index").read_bytes(), index)
        self.assertEqual((self.repo / "Map.uasset").read_bytes(), b"dirty working file")
        self.cli("--store", self.store, "unpin", "user/keep")
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "pack")
        for identifier, expected in ((original, self.data), (staged, b"staged independent version" * 8192)):
            path = self.root / identifier
            self.cli("--store", self.store, "get", identifier, path)
            self.assertEqual(path.read_bytes(), expected)
        self.cli("--store", self.store, "fsck")

    def test_branches_tags_remotes_stash_reflogs_and_unobserved_pins_are_conservative(self):
        self.snapshot()
        main = self.git("rev-parse", "HEAD").decode().strip()
        records = [self.orphan(bytes([i]) * 65536) for i in range(1, 7)]
        self.git("update-ref", "refs/heads/kept", records[0][1])
        self.git("tag", "-a", "release", "-m", "release", records[1][1])
        self.git("update-ref", "refs/remotes/origin/kept", records[2][1])
        self.git("update-ref", "--create-reflog", "refs/stash", records[3][1])
        self.git("update-ref", "--create-reflog", "refs/heads/reflog-only", records[4][1])
        self.git("update-ref", "refs/heads/reflog-only", main)
        self.git("update-ref", "--no-deref", "HEAD", records[5][1])
        inventory = self.scan()
        self.assertTrue({item[0] for item in records} <= self.ids(inventory),
                        [i for i, item in enumerate(records) if item[0] not in self.ids(inventory)])
        self.cli("git", "prepare-assets", records[0][1])
        self.retention("--apply")
        for ref in ("refs/heads/kept", "refs/remotes/origin/kept", "refs/stash", "refs/heads/reflog-only"):
            self.git("update-ref", "-d", ref)
        self.git("tag", "-d", "release")
        self.git("reflog", "expire", "--expire=now", "--expire-unreachable=now", "--all")
        preview = self.retention()
        self.assertFalse({item[0] for item in records[:5]} & self.ids(preview))
        self.assertIn("git-assets/" + records[0][1], preview["unobserved_pins"])
        self.assertGreaterEqual(len(preview["unobserved_pins"]), 6)
        before = self.refs()
        self.retention("--apply")
        self.assertEqual(self.refs(), before)  # No automatic deletion of unobserved pins.

    def test_linked_detached_heads_private_refs_reflogs_and_all_index_stages(self):
        self.snapshot()
        records = [self.orphan(bytes([i]) * 16384) for i in range(21, 28)]
        linked = self.root / "linked worktree é"
        self.git("worktree", "add", "--detach", linked, records[0][1])
        self.git("-C", linked, "update-ref", "--create-reflog", "refs/worktree/saved", records[6][1])
        self.git("-C", linked, "update-ref", "refs/worktree/saved", records[1][1])
        self.git("-C", linked, "update-ref", "--no-deref", "HEAD", records[2][1])
        self.git("-C", linked, "update-ref", "--no-deref", "HEAD", records[0][1])
        for stage, index in ((1, 3), (2, 4), (3, 5)):
            raw = self.git("show", records[index][1] + ":Asset.uasset")
            blob = self.git("hash-object", "-w", "--stdin", data=raw).decode().strip()
            self.git("-C", linked, "update-index", "--index-info",
                     data=f"100644 {blob} {stage}\tConflict.uasset\n".encode())
        index_before = self.git("-C", linked, "ls-files", "--stage", "-z")
        inventory = self.scan()
        self.assertTrue({item[0] for item in records} <= self.ids(inventory),
                        [i for i, item in enumerate(records) if item[0] not in self.ids(inventory)])
        self.assertEqual(inventory, self.scan("--directory", linked))
        self.retention("--apply", "--directory", linked)
        self.assertEqual(index_before, self.git("-C", linked, "ls-files", "--stage", "-z"))

    def test_unborn_index_is_protected_without_creating_a_git_commit(self):
        manifest, blob, raw = self.pointer(b"staged before the first commit")
        self.git("update-index", "--add", "--cacheinfo", f"100644,{blob},New.uasset")
        self.assertEqual(self.ids(self.scan()), {manifest})
        self.assertEqual(self.retention("--apply")["added"], 1)
        self.git("rev-parse", "--verify", "HEAD", expected=128)

    def test_corrupt_assets_and_conflicting_managed_pins_leave_refs_unchanged(self):
        self.snapshot()
        inventory = self.scan()
        manifest = inventory["manifests"][0]["manifest"]
        metadata = json.loads(self.cli("--store", self.store, "inspect", "manifest", manifest).stdout)
        oid = metadata["blocks"][0]["id"]
        path = self.store / "objects/chunk" / oid[:2] / oid
        before = self.refs()
        good = path.read_bytes()
        path.write_bytes(bytes([good[0] ^ 1]) + good[1:])
        self.assertFalse(self.retention("--apply", expected=1)["ready"])
        self.assertEqual(self.refs(), before)
        path.write_bytes(good)
        other, blob, raw = self.pointer(b"other retained asset")
        name = "git-live/" + inventory["repository"] + '/' + manifest
        self.cli("--store", self.store, "pin", name, "manifest", other)
        before = self.refs()
        self.cli("git", "retention", "--apply", expected=1)
        self.assertEqual(self.refs(), before)

    def test_missing_assets_never_publish_any_pins_then_retry_succeeds(self):
        self.snapshot()
        for name in self.refs(): self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        preview = self.retention(expected=1)
        self.assertFalse(preview["ready"])
        self.assertTrue(preview["verification_error"])
        result = self.retention("--apply", expected=1)
        self.assertFalse(result["applied"])
        self.assertEqual(self.refs(), {})
        self.pointer(self.data)
        self.assertTrue(self.retention("--apply")["applied"])

    def test_storeless_scan_blob_tags_and_bounded_small_blob_batches(self):
        self.snapshot()
        pointer, blob, raw = self.pointer(b"tagged blob asset")
        self.git("tag", "raw-blob", blob)
        for index in range(700): (self.repo / f"small-{index}.txt").write_text(str(index))
        self.git("add", "--", *[f"small-{index}.txt" for index in range(700)])
        self.git("commit", "-m", "many small ordinary blobs")
        expected = self.scan()
        self.assertIn(pointer, self.ids(expected))
        clone = self.root / "bare clone"
        self.git("clone", "--bare", self.repo, clone)
        self.env["SYRNCK_KEY"] = "invalid key is irrelevant to inventory"
        actual = json.loads(self.cli("git", "retention-scan", directory=clone).stdout)
        self.assertEqual(self.ids(expected), self.ids(actual))
        self.assertNotEqual(expected["repository"], actual["repository"])
        self.assertFalse((clone / "syrnck").exists())

    def test_encrypted_store_and_sha256_git_ids(self):
        self.repo = self.root / "sha256"
        self.repo.mkdir()
        self.git("init", "--initial-branch=main", "--object-format=sha256")
        self.git("config", "user.name", "Tests")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.env["SYRNCK_KEY"] = "39" * 32
        self.cli("git", "install", "--encrypted")
        self.store = self.repo / ".git/syrnck"
        self.snapshot()
        self.assertEqual(len(self.git("rev-parse", "HEAD").strip()), 64)
        self.assertTrue(self.retention("--apply")["applied"])
        self.cli("--store", self.store, "fsck")

    def test_pruned_reflog_commit_is_refused_even_when_git_returns_success(self):
        self.snapshot()
        manifest, commit = self.orphan(b"version only in a damaged reflog")
        current = self.git("rev-parse", "HEAD").decode().strip()
        self.git("update-ref", "--create-reflog", "refs/heads/damaged-log", commit)
        self.git("update-ref", "refs/heads/damaged-log", current)
        path = self.repo / ".git/objects" / commit[:2] / commit[2:]
        path.chmod(0o600)
        path.unlink()
        before = self.refs()
        self.cli("git", "retention-scan", expected=1)
        self.cli("git", "retention", "--apply", expected=1)
        self.assertEqual(self.refs(), before)

    def test_shallow_overrides_grafts_and_malformed_pointers_fail_closed(self):
        self.snapshot()
        refs = self.refs()
        shallow = self.root / "shallow"
        self.git("clone", "--no-local", "--depth", 1, self.repo, shallow)
        self.cli("git", "retention-scan", directory=shallow, expected=1)
        for name in ("GIT_NAMESPACE", "GIT_INDEX_FILE", "GIT_SHALLOW_FILE"):
            self.env[name] = "unsupported"
            self.cli("git", "retention-scan", expected=1)
            self.env.pop(name)
        grafts = self.repo / ".git/info/grafts"
        grafts.write_bytes(self.git("rev-parse", "HEAD"))
        self.cli("git", "retention", "--apply", expected=1)
        grafts.unlink()
        malformed = self.git("hash-object", "-w", "--stdin", data=b"version syrnck-pointer-v1\nbad\n").decode().strip()
        self.git("update-index", "--add", "--cacheinfo", f"100644,{malformed},Malformed.bin")
        self.cli("git", "retention", "--apply", expected=1)
        self.assertEqual(refs, self.refs())

    def test_cancellation_and_changed_git_inventory_leave_refs_unchanged(self):
        self.snapshot()
        for mode in ("cancel_verify", "cancel_publish", "change_inventory"):
            result = subprocess.run([PROBE, mode], cwd=self.repo, env=self.env, capture_output=True, timeout=25)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))


if __name__ == "__main__":
    unittest.main()
