"""CLI retirement requires a bound preview and complete independent backup coverage."""
import json
import unittest
import workflows


class ArchiveRetirement(unittest.TestCase):
    setUp = workflows.Workflows.setUp
    tearDown = workflows.Workflows.tearDown
    cmd = workflows.Workflows.cmd

    def test_backed_up_archive_retirement_stale_plans_and_gc(self):
        self.env = {k: v for k, v in self.env.items() if k.upper() != "SYRNCK_BACKUP_KEY"}
        source = self.root / "asset.bin"
        source.write_bytes(b"old retained bytes")
        old = self.cmd("native", "commit", "-m", "old").decode().strip()
        source.write_bytes(b"new independent bytes")
        self.cmd("native", "commit", "-m", "second")
        current = self.cmd("native", "detach-history", "-m", "independent").decode().strip()
        archives = json.loads(self.cmd("native", "archives"))["archives"]
        name = next(row["reference"] for row in archives if row["kind"] == "detached")
        backup = self.root / "backup"
        self.cmd("backup", "create", backup)
        refs = self.cmd("refs")
        plan = json.loads(self.cmd("backup", "retire-plan", name, "--from", backup))
        self.assertFalse(plan["applied"])
        self.assertEqual(self.cmd("refs"), refs)
        self.cmd("backup", "retire", name, "--from", backup, ok=False)
        self.cmd("pin", "unrelated", "revision", current)
        self.cmd("backup", "retire", name, "--from", backup, "--expect-plan", plan["plan"], ok=False)
        fresh = json.loads(self.cmd("--lang", "fr", "backup", "retire-plan", name, "--from", backup))
        done = json.loads(self.cmd("backup", "retire", name, "--from", backup, "--expect-plan", fresh["plan"]))
        self.assertTrue(done["applied"])
        self.assertNotIn(name, json.loads(self.cmd("refs")))
        self.cmd("gc", "--delete")
        destination = self.root / "old-export"
        self.cmd("native", "export", old, destination, store=backup)
        self.assertEqual((destination / "asset.bin").read_bytes(), b"old retained bytes")
        self.cmd("backup", "verify", store=backup)
        self.cmd("backup", "retire-plan", "heads/main", "--from", backup, ok=False)


if __name__ == "__main__":
    unittest.main()
