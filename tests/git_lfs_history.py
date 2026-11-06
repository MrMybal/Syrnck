"""Complete LFS DAG conversion, byte-exact identities and retained old assets."""
import hashlib
import json
import shutil
import stat
import subprocess
import unittest
import git_lfs_migration as base

PROBE = base.PROBE


class LfsHistory(unittest.TestCase):
    setUp = base.LfsMigration.setUp
    initialize = base.LfsMigration.initialize
    git = base.LfsMigration.git
    cli = base.LfsMigration.cli
    entry = base.LfsMigration.entry
    source = base.LfsMigration.source

    def cache_object(self, path, data):
        oid = hashlib.sha256(data).hexdigest()
        cached = self.repo / ".git/lfs/objects" / oid[:2] / oid[2:4] / oid
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(data)
        self.entry(path, f"version https://git-lfs.github.com/spec/v1\noid sha256:{oid}\nsize {len(data)}\n".encode())
        return cached

    def commit(self, parents=(), message=b"revision\n", extra=b""):
        tree = self.git("write-tree").strip()
        raw = b"tree " + tree + b"\n" + b"".join(b"parent " + p.encode() + b"\n" for p in parents)
        raw += (b"author A\xc3\xa9 <author@example.invalid> 1700000000 +0230\n"
                b"committer Builder <build@example.invalid> 1700000010 -0430\n"
                b"encoding ISO-8859-1\n") + extra + b"\n" + message
        return self.git("hash-object", "-w", "-t", "commit", "--stdin", data=raw).decode().strip()

    def preview(self, *options, expected=0):
        return json.loads(self.cli("git", "lfs-history-preview", "HEAD", *options, expected=expected).stdout)

    def migrate(self, branch="history", *options, expected=0):
        return self.cli("git", "migrate-lfs-history", "HEAD", "--branch", branch, *options, expected=expected)

    def store_state(self):
        return {str(p.relative_to(self.store)): p.read_bytes() for p in self.store.rglob("*") if p.is_file()}

    def test_merge_topology_messages_modes_and_all_old_versions_survive_gc(self):
        self.entry("Game.cpp", b"base\n")
        root = self.commit(message=b"code-only root\n")
        self.entry(".gitattributes", b"*.uasset filter=lfs -text\n")
        data1, data2 = b"old asset" * 20000, b"new asset" * 25000
        old_cache = self.cache_object("Content/Carte [a].uasset", data1)
        first = self.commit([root], b"first asset\n")
        self.cache_object("Content/Carte [a].uasset", data2)
        self.entry("run.bin", b"executable\n", "100755")
        left = self.commit([first], b"left \xe9\n", b"x-fixture alpha\n continued\n")
        self.git("read-tree", first)
        self.cache_object("Content/Duplicate.uasset", data1)
        self.entry("side.cpp", b"side branch\n")
        right = self.commit([first], b"right\n")
        self.git("read-tree", left)
        self.cache_object("Content/Duplicate.uasset", data1)
        self.entry("side.cpp", b"side branch\n")
        merge = self.commit([left, right], b"merge\n")
        self.git("update-index", "--force-remove", "Content/Carte [a].uasset")
        self.cache_object("Renamed.uasset", data2)
        renamed = self.commit([merge], b"rename\n")
        self.git("update-index", "--force-remove", "Renamed.uasset", "Content/Duplicate.uasset")
        deleted = self.commit([renamed], b"delete both assets\n")
        tip = self.commit([deleted], b"empty\n")
        self.git("update-ref", "refs/heads/main", tip)
        # Preserve a dirty working tree and a staged addition, independent of the source tree.
        (self.repo / "Game.cpp").write_bytes(b"dirty working code")
        self.entry("staged.cpp", b"staged addition\n")
        index = (self.repo / ".git/index").read_bytes()
        before = self.store_state()
        preview = self.preview()
        self.assertEqual(self.store_state(), before)
        self.assertEqual(preview["commits_scanned"], 8)
        self.assertEqual(preview["unique_objects"], 2)
        self.assertEqual(preview["rewritten_commits"], 7)
        self.assertEqual(preview["assets"][0]["status"], "verified")
        result = json.loads(self.migrate().stdout)
        mapped = {row["source"]: row["commit"] for row in result["commits"]}
        self.assertEqual(mapped[root], root)
        self.assertEqual(result["commit"], mapped[tip])
        self.assertEqual(self.git("rev-parse", "HEAD").decode().strip(), tip)
        self.assertEqual((self.repo / ".git/index").read_bytes(), index)
        self.assertEqual((self.repo / "Game.cpp").read_bytes(), b"dirty working code")
        self.assertEqual(old_cache.read_bytes(), data1)
        for source, target in mapped.items():
            old = self.git("cat-file", "commit", source)
            new = self.git("cat-file", "commit", target)
            old_head, old_body = old.split(b"\n\n", 1)
            new_head, new_body = new.split(b"\n\n", 1)
            stable = lambda raw: [line for line in raw.splitlines() if not line.startswith((b"tree ", b"parent "))]
            self.assertEqual(stable(old_head), stable(new_head))
            self.assertEqual(old_body, new_body)
            parents = [line[7:].decode() for line in old_head.splitlines() if line.startswith(b"parent ")]
            actual = [line[7:].decode() for line in new_head.splitlines() if line.startswith(b"parent ")]
            self.assertEqual(actual, [mapped[p] for p in parents])
        self.assertEqual(self.git("ls-tree", left, "run.bin"), self.git("ls-tree", mapped[left], "run.bin"))
        refs = json.loads(self.cli("--store", self.store, "refs").stdout)
        for source in (first, left, right, merge, renamed):
            self.assertIn("git-assets/" + mapped[source], refs)
        shutil.rmtree(self.repo / ".git/lfs")
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "pack")
        self.cli("--store", self.store, "fsck")
        for source in (first, left, right, merge, renamed):
            catalog = json.loads(self.cli("git", "catalog", mapped[source]).stdout)
            for asset in catalog["assets"]:
                expected = data1 if asset["path"] == "Content/Duplicate.uasset" or source in (first, right) else data2
                output = self.root / "reconstructed"
                self.cli("--store", self.store, "get", asset["manifest"], output)
                self.assertEqual(output.read_bytes(), expected)
                output.unlink()
        linked = self.root / "historical checkout"
        self.git("worktree", "add", "--detach", linked, mapped[first])
        self.assertEqual((linked / "Content/Carte [a].uasset").read_bytes(), data1)
        self.git("fsck", "--strict")

    def test_missing_old_only_object_blocks_before_any_store_or_git_object_write(self):
        first = self.source({"Old.uasset": b"old" * 2000})
        cached = self.repo / ".git/lfs/objects" / self.oids["Old.uasset"][:2] / self.oids["Old.uasset"][2:4] / self.oids["Old.uasset"]
        data = cached.read_bytes()
        self.git("update-index", "--force-remove", "Old.uasset")
        tip = self.commit([first], b"asset removed\n")
        self.git("update-ref", "refs/heads/main", tip)
        before = self.store_state()
        git_objects = {p.relative_to(self.repo / ".git/objects") for p in (self.repo / ".git/objects").rglob("*") if p.is_file()}
        for replacement, status in ((None, "missing"), (b"wrong", "size_mismatch"), (b"x" * len(data), "hash_mismatch")):
            if replacement is None:
                cached.unlink()
            else:
                cached.write_bytes(replacement)
            self.assertEqual(self.preview(expected=1)["assets"][0]["status"], status)
            self.migrate(expected=1)
            self.assertEqual(self.store_state(), before)
            self.assertEqual({p.relative_to(self.repo / ".git/objects") for p in (self.repo / ".git/objects").rglob("*") if p.is_file()}, git_objects)
            self.git("show-ref", "--verify", "--quiet", "refs/heads/history", expected=1)
        cached.write_bytes(data)
        self.migrate()

    def test_explicit_signature_removal_preserves_unsigned_and_unaffected_headers(self):
        self.entry("Game.cpp", b"root")
        signature = b"gpgsig -----BEGIN PGP SIGNATURE-----\n fixture\n -----END PGP SIGNATURE-----\n"
        root = self.commit(extra=signature)
        self.entry(".gitattributes", b"*.uasset filter=lfs -text\n")
        self.cache_object("Map.uasset", b"signed content")
        signed = self.commit([root], extra=signature + b"gpgsig-sha256 fixture\n continuation\nmergetag object fixture\n continuation\n")
        self.git("update-ref", "refs/heads/main", signed)
        before = self.store_state()
        preview = self.preview(expected=1)
        self.assertEqual(preview["signature_headers_to_strip"], 3)
        self.migrate(expected=1)
        self.assertEqual(self.store_state(), before)
        self.assertTrue(self.preview("--strip-signatures")["ready"])
        result = json.loads(self.migrate("history", "--strip-signatures").stdout)
        mapped = {row["source"]: row["commit"] for row in result["commits"]}
        self.assertEqual(mapped[root], root)
        self.assertIn(signature, self.git("cat-file", "commit", root))
        raw = self.git("cat-file", "commit", mapped[signed])
        for name in (b"gpgsig ", b"gpgsig-sha256 ", b"mergetag "):
            self.assertNotIn(name, raw)
        self.assertEqual(result["stripped_signature_headers"], 3)

    def test_sha256_encrypted_history_and_limits(self):
        self.repo = self.root / "sha256"
        self.repo.mkdir()
        self.git("init", "--initial-branch=main", "--object-format=sha256")
        self.git("config", "user.name", "History tests")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.env["SYRNCK_KEY"] = "45" * 32
        self.cli("git", "install", "--encrypted")
        self.store = self.repo / ".git/syrnck"
        first = self.source({"Map.uasset": b"first" * 4000})
        self.cache_object("Map.uasset", b"second" * 6000)
        second = self.commit([first])
        self.git("update-ref", "refs/heads/main", second)
        before = self.store_state()
        self.cli("git", "lfs-history-preview", "HEAD", "--max-commits", "1", expected=1)
        self.migrate("limited", "--max-commits", "1", expected=1)
        self.assertEqual(self.store_state(), before)
        result = json.loads(self.migrate("history", "--max-commits", "2").stdout)
        self.assertEqual(len(result["commit"]), 64)
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "fsck")
        for value in ("0", "100001", "18446744073709551615"):
            self.cli("git", "lfs-history-preview", "HEAD", "--max-commits", value, expected=1)

    def test_boundaries_hooks_races_cancellation_and_lost_acknowledgements(self):
        first = self.source({"Map.uasset": b"old" * 2000})
        self.cache_object("Map.uasset", b"new" * 2000)
        second = self.commit([first])
        self.git("update-ref", "refs/heads/main", second)
        for mode in ("cancel_ingest", "cancel_publish", "race_ref", "race_symbolic", "lost_ack", "advanced_ack", "late_stop"):
            if mode == "late_stop":
                hook = self.repo / ".git/hooks/reference-transaction"
                hook.write_bytes(b'#!/bin/sh\nif test "$1" = committed; then sleep 1; fi\nexit 0\n')
                hook.chmod(0o755)
            result = subprocess.run([PROBE, "history_" + mode], cwd=self.repo, env=self.env,
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        hook = self.repo / ".git/hooks/reference-transaction"
        hook.write_bytes(b'#!/bin/sh\nif test "$1" = prepared; then exit 1; fi\n')
        hook.chmod(0o755)
        self.migrate("rejected", expected=1)
        self.git("show-ref", "--verify", "--quiet", "refs/heads/rejected", expected=1)

    def test_shallow_boundary_does_not_hide_missing_raw_ancestors(self):
        first = self.source({"Map.uasset": b"old" * 1000})
        self.cache_object("Map.uasset", b"new" * 1000)
        second = self.commit([first])
        self.git("update-ref", "refs/heads/main", second)
        # Model a depth-one clone: Git traversal stops at the marker, while the
        # raw commit still identifies an absent parent. Avoid Windows file://
        # transport quoting/Unicode behavior in this history-closure test.
        (self.repo / ".git/shallow").write_text(second + "\n", encoding="ascii")
        missing = self.repo / ".git/objects" / first[:2] / first[2:]
        missing.chmod(stat.S_IREAD | stat.S_IWRITE)
        missing.unlink()
        self.assertEqual(self.git("rev-list", "--count", "HEAD"), b"1\n")
        before = self.store_state()
        self.cli("git", "lfs-history-preview", "HEAD", expected=1)
        self.assertEqual(self.store_state(), before)

    def test_existing_native_asset_missing_only_in_old_history_blocks_preflight(self):
        first = self.source({"Map.uasset": b"LFS bytes"})
        native = b"existing native bytes" * 1000
        pointer = self.cli("--store", self.store, "git", "clean", data=native).stdout
        self.entry("Existing.syr", pointer)
        mixed = self.commit([first])
        self.git("update-index", "--force-remove", "Existing.syr")
        tip = self.commit([mixed])
        self.git("update-ref", "refs/heads/main", tip)
        for name in json.loads(self.cli("--store", self.store, "refs").stdout):
            self.cli("--store", self.store, "unpin", name)
        self.cli("--store", self.store, "gc", "--delete")
        # The storeless preview checks the LFS cache; migration also checks old native roots.
        self.assertTrue(self.preview()["ready"])
        before = self.store_state()
        self.migrate(expected=1)
        self.assertEqual(self.store_state(), before)
        self.assertEqual(self.cli("--store", self.store, "git", "clean", data=native).stdout, pointer)
        result = json.loads(self.migrate().stdout)
        converted = next(row["commit"] for row in result["commits"] if row["source"] == mixed)
        self.cli("--store", self.store, "gc", "--delete")
        catalog = json.loads(self.cli("git", "catalog", converted).stdout)
        asset = next(row for row in catalog["assets"] if row["path"] == "Existing.syr")
        output = self.root / "native-restored"
        self.cli("--store", self.store, "get", asset["manifest"], output)
        self.assertEqual(output.read_bytes(), native)

    def test_total_attribute_budget_is_enforced_before_payload_import(self):
        self.source({f"dir{i}/Map.uasset": b"same bytes" for i in range(25)})
        attrs = b"#" + b"x" * 699998 + b"\n"
        for i in range(25):
            self.entry(f"dir{i}/.gitattributes", attrs)
        self.git("commit", "-m", "large attributes")
        before = self.store_state()
        failed = self.migrate(expected=1)
        self.assertIn(b"attributes exceed 16 MiB", failed.stderr)
        self.assertEqual(self.store_state(), before)


if __name__ == "__main__":
    unittest.main()
