"""Repair copies combine intact newer source data and verified older backup bytes."""
import hashlib
import json
from pathlib import Path
import unittest
import backups as base


class RepairCopy(unittest.TestCase):
    tearDown = base.Backups.tearDown
    cmd = base.Backups.cmd

    def setUp(self):
        base.Backups.setUp(self)
        self.env.pop("SYRNCK_BACKUP_KEY", None)
        self.env.pop("SYRNCK_RECOVERY_KEY", None)

    def snapshot(self, path):
        return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in path.rglob("*") if p.is_file() and p.name != "store.lock"}

    def test_newer_roots_survive_missing_and_corrupt_old_objects(self):
        (self.root / "asset.bin").write_bytes(b"old asset\x00" * 2000)
        first = self.cmd("native", "commit", "-m", "old").decode().strip()
        backup = self.root / "donor"
        self.cmd("backup", "create", backup)
        (self.root / "new.cpp").write_bytes(b"newer source unavailable in old backup")
        self.cmd("native", "add", "new.cpp")
        head = self.cmd("native", "commit", "--staged", "-m", "newer").decode().strip()
        revision = json.loads(self.cmd("inspect", "revision", first))
        manifest = revision["files"]["asset.bin"]["manifest"]
        metadata = json.loads(self.cmd("inspect", "manifest", manifest))
        chunk = metadata["blocks"][0]["id"]
        (self.store / "objects/chunk" / chunk[:2] / chunk).write_bytes(b"corrupt original envelope")
        (self.store / "objects/manifest" / manifest[:2] / manifest).unlink()
        before, donor = self.snapshot(self.store), self.snapshot(backup)
        plan = json.loads(self.cmd("backup", "repair-copy-plan", backup))
        self.assertTrue(plan["ready"])
        self.assertEqual(len(plan["corrupt"]), 1)
        self.assertEqual(len(plan["missing"]), 1)
        self.assertEqual(self.snapshot(self.store), before)
        output = self.root / "recovered"
        result = json.loads(self.cmd("--lang", "fr", "backup", "repair-copy", backup, output))
        self.assertEqual(result["state"], "recovered")
        self.assertEqual(result["recovered_objects"], 2)
        self.assertEqual(self.snapshot(self.store), before)
        self.assertEqual(self.snapshot(backup), donor)
        self.assertFalse((output / "backup.json").exists())
        self.cmd("gc", "--delete", store=output)
        self.cmd("pack", store=output)
        self.cmd("fsck", store=output)
        refs = json.loads(self.cmd("refs", store=output))
        self.assertEqual(refs["heads/main"]["id"], head)
        for name, commit in (("old", first), ("newer", head)):
            self.cmd("native", "export", commit, self.root / name, store=output)
        self.assertEqual((self.root / "old/asset.bin").read_bytes(), b"old asset\x00" * 2000)
        self.assertEqual((self.root / "newer/new.cpp").read_bytes(), b"newer source unavailable in old backup")
        self.cmd("backup", "repair-copy", backup, output, ok=False)
        self.cmd("backup", "repair-copy", backup, backup / "nested", ok=False)
        self.assertEqual(self.snapshot(self.store), before)
        self.assertEqual(self.snapshot(backup), donor)

    def test_three_independent_keys_and_invalid_donor_never_publish(self):
        project = self.root / "project"
        project.mkdir()
        source = project / ".syrnck"
        primary = dict(self.env, SYRNCK_KEY="61" * 32)
        self.cmd("init", "--encrypted", store=source, env=primary)
        (project / "asset.bin").write_bytes(b"encrypted retained asset" * 1000)
        revision = self.cmd("native", "commit", "--directory", project, "-m", "encrypted",
                            store=source, env=primary).decode().strip()
        donor = self.root / "encrypted-donor"
        donor_env = dict(primary, SYRNCK_BACKUP_KEY="63" * 32)
        self.cmd("backup", "create", donor, store=source, env=donor_env)
        metadata = json.loads(self.cmd("inspect", "revision", revision, store=source, env=primary))
        manifest = metadata["files"]["asset.bin"]["manifest"]
        (source / "objects/manifest" / manifest[:2] / manifest).write_bytes(b"corrupted encrypted metadata")
        before = self.snapshot(source)
        output = self.root / "recovered-encrypted"
        self.cmd("backup", "repair-copy", donor, output, store=source, env=primary, ok=False)
        self.assertFalse(output.exists())
        recovery = dict(donor_env, SYRNCK_RECOVERY_KEY="65" * 32)
        result = json.loads(self.cmd("backup", "repair-copy", donor, output, store=source, env=recovery))
        self.assertTrue(result["encrypted"])
        self.assertEqual(self.snapshot(source), before)
        self.cmd("fsck", store=output, env=dict(self.env, SYRNCK_KEY="65" * 32))
        self.cmd("fsck", store=output, env=primary, ok=False)
        marker = donor / "backup.json"
        original = marker.read_bytes()
        value = json.loads(original)
        value["refs_hash"] = "0" * 64
        marker.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
        blocked = self.root / "blocked-recovery"
        self.cmd("backup", "repair-copy", donor, blocked, store=source, env=recovery, ok=False)
        self.assertFalse(blocked.exists())
        marker.write_bytes(original)
        invalid = dict(donor_env, SYRNCK_RECOVERY_KEY="short")
        self.cmd("backup", "repair-copy", donor, blocked, store=source, env=invalid, ok=False)
        self.assertFalse(blocked.exists())


if __name__ == "__main__":
    unittest.main()
