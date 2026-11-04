"""Opt-in Git checkout hydration and pointer-only clones against the real server."""
import json
import shutil
import unittest
import git_asset_transfers as base


class GitCheckout(base.GitNetwork):
    def clone(self, mode="download", encrypted=False):
        source = self.repo
        target = self.root / "checkout clone 'é"
        self.git("clone", "--no-checkout", source, target)
        self.repo = target
        self.store = target / ".git/syrnck"
        if encrypted:
            self.env["SYRNCK_KEY"] = "17" * 32
        self.cli("git", "install", *(["--encrypted"] if encrypted else []))
        self.cli("git", "configure-remote", "origin", self.url)
        self.cli("git", "configure-checkout", mode,
                 *(["--remote", "origin"] if mode == "download" else []))
        return source

    def refs(self):
        return json.loads(self.cli("--store", self.store, "refs").stdout)

    def raw_smudge(self, pointer, expected=0, *options):
        return self.cli("--store", self.store, "git", "smudge", "--checkout-policy",
                        "--retries", "0", *options, data=pointer, expected=expected)

    def test_selected_paths_old_versions_and_offline_reuse_in_encrypted_clone(self):
        first = self.snapshot()
        original = self.data
        with self.serving():
            self.cli("git", "push-assets", self.url, first)
            self.data = b"new asset version" * 65536
            (self.repo / "Map.uasset").write_bytes(self.data)
            (self.repo / "Other.uasset").write_bytes(b"independent asset" * 10000)
            self.git("add", ".")
            self.git("commit", "-m", "second assets")
            second = self.git("rev-parse", "HEAD").decode().strip()
            assets = json.loads(self.cli("git", "catalog", second).stdout)["assets"]
            self.cli("git", "push-assets", self.url, second)
            self.clone(encrypted=True)
            self.git("checkout", "HEAD", "--", ".gitattributes")
            # Git HEAD still names the second commit while smudge sees the first.
            self.git("checkout", first, "--", "Map.uasset")
            self.assertEqual(self.git("rev-parse", "HEAD").decode().strip(), second)
            self.assertEqual((self.repo / "Map.uasset").read_bytes(), original)
            self.assertEqual(len(self.refs()), 1)
            self.assertFalse((self.repo / "Other.uasset").exists())
            self.git("checkout", "HEAD", "--", "Map.uasset")
            self.assertEqual((self.repo / "Map.uasset").read_bytes(), self.data)
            other = next(a for a in assets if a["path"] == "Other.uasset")
            self.assertNotIn("git/" + other["manifest"], self.refs())
            self.git("checkout", "--force", "HEAD")
            self.assertEqual((self.repo / "Other.uasset").read_bytes(), b"independent asset" * 10000)
            self.assertEqual(len(self.refs()), 3)
            sessions = json.loads(self.cli("--store", self.store, "agent", "sessions").stdout)
            self.assertEqual(len(sessions), 3)
            self.assertTrue(all(s["state"] == "complete" and s["root"]["kind"] == "manifest"
                                for s in sessions))
        # No token, usable endpoint or server is needed when everything is local.
        self.env.pop("SYRNCK_TOKEN")
        self.git("config", "--local", "remote.origin.syrnckUrl", "invalid offline endpoint")
        self.git("checkout", first, "--", "Map.uasset")
        self.assertEqual((self.repo / "Map.uasset").read_bytes(), original)
        self.git("checkout", "--force", "HEAD")
        self.assertEqual(self.git("status", "--porcelain"), b"")
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("--store", self.store, "fsck")

    def test_pointer_only_checkout_status_add_and_later_hydration(self):
        commit = self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        with self.serving():
            self.cli("git", "push-assets", self.url, commit)
        self.clone(mode="skip")
        self.env.pop("SYRNCK_TOKEN")
        self.git("checkout", "--force", "HEAD")
        self.assertEqual((self.repo / "Map.uasset").read_bytes(), pointer)
        self.assertEqual(self.git("status", "--porcelain"), b"")
        self.git("add", "Map.uasset")
        self.assertEqual(self.git("show", ":Map.uasset"), pointer)
        self.assertEqual(self.refs(), {})
        self.cli("--store", self.store, "gc", "--delete")
        self.cli("git", "configure-checkout", "download", "--remote", "origin")
        missing = self.raw_smudge(pointer, 1)
        self.assertEqual(missing.stdout, b"")
        self.assertIn(b"set SYRNCK_TOKEN", missing.stderr)
        self.env["SYRNCK_TOKEN"] = base.TOKEN
        with self.serving():
            # Policy changes do not rewrite an already checked-out pointer.
            self.assertEqual((self.repo / "Map.uasset").read_bytes(), pointer)
            (self.repo / "Map.uasset").unlink()
            self.git("checkout", "HEAD", "--", "Map.uasset")
        self.assertEqual((self.repo / "Map.uasset").read_bytes(), self.data)
        self.assertEqual(self.git("status", "--porcelain"), b"")

    def test_skip_override_needs_no_store_key_repository_or_credentials(self):
        self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        absent = self.root / "no store"
        self.env.update(SYRNCK_SKIP_SMUDGE="1", SYRNCK_KEY="not a key")
        self.env.pop("SYRNCK_TOKEN")
        for data in (pointer, bytes(range(256)) * 8192):
            output = self.cli("--store", absent, "git", "smudge", "--checkout-policy",
                              directory=self.root, data=data).stdout
            self.assertEqual(output, data)
        self.assertFalse(absent.exists())
        malformed = self.cli("--store", absent, "git", "smudge", expected=1,
                             directory=self.root, data=b"version syrnck-pointer-v1\ninvalid")
        self.assertEqual(malformed.stdout, b"")
        self.env["SYRNCK_SKIP_SMUDGE"] = "yes"
        self.assertIn(b"SYRNCK_SKIP_SMUDGE must be 0 or 1",
                      self.cli("git", "smudge", data=pointer, expected=1).stderr)

    def test_failure_preserves_empty_output_session_and_partial_local_objects(self):
        commit = self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        asset = json.loads(self.cli("git", "catalog", commit).stdout)["assets"][0]
        manifest = asset["manifest"]
        source_store = self.store
        with self.serving():
            self.cli("git", "push-assets", self.url, commit)
        self.clone()
        # A pre-existing verified manifest is reused; its chunks are missing.
        source = source_store / "objects/manifest" / manifest[:2] / manifest
        target = self.store / "objects/manifest" / manifest[:2] / manifest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        failed = self.raw_smudge(pointer, 1)
        self.assertEqual(failed.stdout, b"")
        self.assertEqual(self.refs(), {})
        sessions = json.loads(self.cli("--store", self.store, "agent", "sessions").stdout)
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0]["state"], "transferring")
        self.cli("--store", self.store, "gc", "--delete")
        self.assertTrue(target.exists())
        with self.serving():
            restored = self.raw_smudge(pointer)
            self.assertEqual(restored.stdout, self.data)
        self.assertEqual(set(self.refs()), {"git/" + manifest})
        sessions = json.loads(self.cli("--store", self.store, "agent", "sessions").stdout)
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0]["state"], "complete")

    def test_corrupt_local_blocks_and_size_mismatch_fail_before_output(self):
        self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        # Local default must never silently enable networking.
        manifest = json.loads(self.cli("git", "catalog", "HEAD").stdout)["assets"][0]["manifest"]
        metadata = json.loads(self.cli("--store", self.store, "inspect", "manifest", manifest).stdout)
        block = metadata["blocks"][-1]["id"]
        target = self.store / "objects/chunk" / block[:2] / block
        good = target.read_bytes()
        corrupt = bytearray(good)
        corrupt[-1] ^= 1
        target.write_bytes(corrupt)
        self.assertEqual(self.raw_smudge(pointer, 1).stdout, b"")
        self.cli("git", "configure-remote", "origin", self.url)
        self.cli("git", "configure-checkout", "download", "--remote", "origin")
        self.env.pop("SYRNCK_TOKEN")
        failed = self.raw_smudge(pointer, 1)
        self.assertEqual(failed.stdout, b"")
        self.assertNotIn(b"set SYRNCK_TOKEN", failed.stderr)
        self.assertEqual(target.read_bytes(), corrupt)
        target.write_bytes(good)
        wrong_size = pointer.rsplit(b"size ", 1)[0] + b"size 1\n"
        failure = self.raw_smudge(wrong_size, 1)
        self.assertEqual(failure.stdout, b"")
        self.assertIn(b"pointer size mismatch", failure.stderr)

    def test_corrected_size_finishes_staging_offline(self):
        commit = self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        manifest = pointer.split(b"manifest blake3:")[1].split(b"\n")[0].decode()
        with self.serving():
            self.cli("git", "push-assets", self.url, commit)
            self.clone()
            wrong = pointer.rsplit(b"size ", 1)[0] + b"size 1\n"
            result = self.raw_smudge(wrong, 1)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"pointer size mismatch", result.stderr)
            self.assertEqual(set(self.refs()), {"git-fetch/" + manifest})
        self.env.pop("SYRNCK_TOKEN")
        self.assertEqual(self.raw_smudge(pointer).stdout, self.data)
        self.assertEqual(set(self.refs()), {"git/" + manifest})

    def test_policy_is_local_explicit_validated_and_legacy_filter_upgrade_is_owned(self):
        self.snapshot()
        pointer = self.git("show", "HEAD:Map.uasset")
        self.clone(mode="local")
        self.env.pop("SYRNCK_TOKEN")
        failure = self.raw_smudge(pointer, 1)
        self.assertEqual(failure.stdout, b"")
        self.assertIn(b"object not found", failure.stderr)
        for args in (("auto",), ("download",), ("skip", "--remote", "origin"),
                     ("download", "--remote", "unknown")):
            self.cli("git", "configure-checkout", *args, expected=1)
        self.assertEqual(self.git("config", "--local", "syrnck.checkout"), b"local\n")
        self.git("config", "--local", "syrnck.checkout", "download:")
        self.assertEqual(self.raw_smudge(pointer, 1).stdout, b"")
        self.git("config", "--local", "syrnck.checkout", "local")
        current = self.git("config", "filter.syrnck.smudge").decode().strip()
        self.assertTrue(current.endswith(" --checkout-policy"))
        legacy = current.removesuffix(" --checkout-policy")
        self.git("config", "filter.syrnck.smudge", legacy)
        self.cli("git", "install")
        self.assertEqual(self.git("config", "filter.syrnck.smudge").decode().strip(), current)
        self.git("config", "filter.syrnck.smudge", "custom checkout command")
        self.cli("git", "configure-checkout", "skip", expected=1)
        self.assertEqual(self.git("config", "filter.syrnck.smudge"), b"custom checkout command\n")


if __name__ == "__main__":
    unittest.main()
