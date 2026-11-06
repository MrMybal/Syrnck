"""CLI backup/restore, independent keys, integrity checks and missing-block repair."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import workflows


class Backups(unittest.TestCase):
    setUp = workflows.Workflows.setUp
    tearDown = workflows.Workflows.tearDown

    def cmd(self, *args, store=None, env=None, ok=True):
        result = subprocess.run([workflows.CLI, "--store", str(store or self.store), *map(str, args)],
                                cwd=self.root, env=env or self.env, capture_output=True, timeout=25)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        if not ok:
            self.assertEqual(result.stdout, b"")
        return result.stdout

    def test_restore_all_retained_versions_with_independent_encryption_keys(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(bytes(range(256)) * 100)
        first = self.cmd("native", "commit", "-m", "first").decode().strip()
        asset.write_bytes(b"replacement asset")
        second = self.cmd("native", "commit", "-m", "second").decode().strip()
        self.cmd("native", "branch", "working", second)
        self.cmd("native", "switch", "working")
        backup = self.root / "backup-store"
        backup_env = dict(self.env, SYRNCK_BACKUP_KEY="41" * 32)
        created = json.loads(self.cmd("backup", "create", backup, "--progress-json", env=backup_env))
        self.assertTrue(created["encrypted"])
        self.assertIn("workspace/HEAD", created["excluded_refs"])
        encrypted = dict(self.env, SYRNCK_KEY="41" * 32)
        checked = json.loads(self.cmd("--lang", "fr", "backup", "verify", store=backup, env=encrypted))
        self.assertEqual(created["snapshot"], checked["snapshot"])
        restored = self.root / "restored-store"
        restore_env = dict(encrypted, SYRNCK_BACKUP_KEY="43" * 32)
        self.cmd("backup", "restore", restored, store=backup, env=restore_env)
        changed = dict(self.env, SYRNCK_KEY="43" * 32)
        self.cmd("fsck", store=restored, env=changed)
        refs = json.loads(self.cmd("refs", store=restored, env=changed))
        self.assertEqual(refs["heads/main"]["id"], second)
        self.assertNotIn("workspace/HEAD", refs)
        for revision, expected in ((first, bytes(range(256)) * 100), (second, b"replacement asset")):
            destination = self.root / ("export-" + revision)
            self.cmd("native", "export", revision, destination, store=restored, env=changed)
            self.assertEqual((destination / "asset.bin").read_bytes(), expected)
        self.cmd("backup", "restore", restored, store=backup, env=restore_env, ok=False)
        self.cmd("backup", "verify", store=backup, ok=False)

    def test_dry_run_missing_repair_and_tampered_backup_rejection(self):
        (self.root / "asset.bin").write_bytes(b"retained bytes\x00" * 1024)
        revision = self.cmd("native", "commit", "-m", "retained").decode().strip()
        backup = self.root / "backup-store"
        self.cmd("backup", "create", backup)
        metadata = json.loads(self.cmd("inspect", "revision", revision))
        manifest = metadata["files"]["asset.bin"]["manifest"]
        value = json.loads(self.cmd("inspect", "manifest", manifest))
        chunk = value["blocks"][0]["id"]
        removed = self.store / "objects/chunk" / chunk[:2] / chunk
        removed.unlink()
        refs = self.cmd("refs")
        plan = json.loads(self.cmd("backup", "repair", backup))
        self.assertEqual(len(plan["missing"]), 1)
        self.assertFalse(removed.exists())
        fixed = json.loads(self.cmd("backup", "repair", backup, "--apply"))
        self.assertEqual(fixed["repaired"], 1)
        self.assertEqual(refs, self.cmd("refs"))
        self.cmd("fsck")
        descriptor = backup / "backup.json"
        marker = json.loads(descriptor.read_text(encoding="utf-8"))
        marker["logical_bytes"] += 1
        descriptor.write_text(json.dumps(marker, separators=(",", ":")), encoding="utf-8")
        self.cmd("backup", "verify", store=backup, ok=False)
        destination = self.root / "refused-restore"
        self.cmd("backup", "restore", destination, store=backup, ok=False)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
