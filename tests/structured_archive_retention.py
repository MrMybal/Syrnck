"""Complete branch/tag archive retirement and tag archive discovery through the CLI."""
import json
import subprocess
import unittest
import workflows


class StructuredArchiveRetirement(unittest.TestCase):
    setUp = workflows.Workflows.setUp
    tearDown = workflows.Workflows.tearDown
    cmd = workflows.Workflows.cmd

    def test_group_retirement_preserves_backup_recipes_and_reused_names(self):
        self.env = {k: v for k, v in self.env.items() if k.upper() != "SYRNCK_BACKUP_KEY"}
        asset = self.root / "asset"
        asset.write_bytes(b"old branch bytes")
        old = self.cmd("native", "commit", "-m", "old").decode().strip()
        self.cmd("native", "branch", "feature", old)
        asset.write_bytes(b"prepared index bytes")
        index = self.cmd("native", "add", "asset", "--branch", "feature").decode().strip()
        branch = json.loads(self.cmd("native", "branch-delete", "feature", "--expect", old,
                                    "--archive-index", "--expect-index", index))["archive"]
        self.cmd("native", "tag", "release/v1", old, "-m", "Archived notes: é")
        tag = json.loads(self.cmd("native", "tag-delete", "release/v1", "--expect", old))["archive"]
        self.cmd("native", "tag", "light", old)
        light = json.loads(self.cmd("native", "tag-delete", "light", "--expect", old))["archive"]
        asset.write_bytes(b"current independent bytes")
        self.cmd("native", "commit", "-m", "current")
        current = self.cmd("native", "detach-history", "-m", "independent").decode().strip()
        simple = [r["reference"] for r in json.loads(self.cmd("native", "archives"))["archives"]]
        selectors = ["archive/branches/" + branch, "archive/tags/" + tag, "archive/tags/" + light, *simple]
        backup = self.root / "backup"
        env = dict(self.env, SYRNCK_BACKUP_KEY="59" * 32)
        self.cmd("backup", "create", backup, env=env)
        self.cmd("native", "branch", "feature", current)
        self.cmd("native", "tag", "release/v1", current, "-m", "Current notes")
        asset.write_bytes(b"dirty current bytes")
        refs = json.loads(self.cmd("refs"))
        first = json.loads(self.cmd("native", "tag-archives", "--limit", 1, "--verify", "--progress-json"))
        french = json.loads(self.cmd("--lang", "fr", "native", "tag-archives", "--limit", 1, "--verify"))
        self.assertEqual(first, french)
        self.assertEqual(first["archives"][0]["archive"], min(tag, light))
        self.assertTrue(first["has_more"])
        last = json.loads(self.cmd("native", "tag-archives", "--after", first["next_after"], "--limit", 1))
        self.assertEqual(last["archives"][0]["archive"], max(tag, light))
        self.assertFalse(last["has_more"])
        self.assertIsNone(last["next_after"])
        self.cmd("native", "tag-archives", "--limit", 0, ok=False)
        self.cmd("native", "tag-archives", "--after", "../invalid", ok=False)
        self.cmd("native", "tags", "--verify", ok=False)
        for member in (selectors[0] + "/index", selectors[1] + "/target", selectors[1] + "/record"):
            self.cmd("backup", "retire-plan", member, "--from", backup, env=env, ok=False)
        plan = json.loads(self.cmd("backup", "retire-plan", *selectors, "--from", backup, env=env))
        self.assertEqual(plan["removed_references"], 8 + len(simple))
        self.assertEqual(json.loads(self.cmd("refs")), refs)
        self.cmd("backup", "retire", *selectors, "--from", backup, env=env, ok=False)
        self.cmd("pin", "unrelated", "revision", current)
        self.cmd("backup", "retire", *selectors, "--from", backup, "--expect-plan", plan["plan"], env=env, ok=False)
        fresh = json.loads(self.cmd("--lang", "fr", "backup", "retire-plan", *selectors, "--from", backup, env=env))
        done = json.loads(self.cmd("backup", "retire", *selectors, "--from", backup, "--expect-plan", fresh["plan"],
                                   "--progress-json", env=env))
        self.assertTrue(done["applied"])
        remaining = json.loads(self.cmd("refs"))
        for selector in selectors:
            self.assertFalse(any(name == selector or name.startswith(selector + "/") for name in remaining))
        self.assertEqual(remaining["heads/feature"]["id"], current)
        self.assertEqual(remaining["tags/release/v1"]["id"], current)
        self.assertEqual(asset.read_bytes(), b"dirty current bytes")
        self.cmd("gc", "--delete")
        self.cmd("inspect", "revision", old, ok=False)
        self.cmd("inspect", "revision", index, ok=False)
        backup_env = dict(self.env, SYRNCK_KEY="59" * 32)
        restored = self.root / "restored"
        restore_env = dict(backup_env, SYRNCK_BACKUP_KEY="61" * 32)
        self.cmd("backup", "restore", restored, store=backup, env=restore_env)
        restored_env = dict(self.env, SYRNCK_KEY="61" * 32)
        restored_branch = json.loads(self.cmd("native", "branch-restore", branch, store=restored, env=restored_env))
        self.assertEqual(restored_branch["index"], index)
        restored_tag = json.loads(self.cmd("native", "tag-restore", tag, store=restored, env=restored_env))
        self.assertEqual(restored_tag["tag"]["message"], "Archived notes: é")
        for revision, expected in ((old, b"old branch bytes"), (index, b"prepared index bytes")):
            destination = self.root / ("export-" + revision)
            self.cmd("native", "export", revision, destination, store=restored, env=restored_env)
            self.assertEqual((destination / "asset").read_bytes(), expected)
        self.cmd("backup", "verify", store=backup, env=backup_env)
        self.assertEqual(json.loads(self.cmd("native", "tag-archives"))["archives"], [])

    def test_progress_stream_is_separate_from_listing_json(self):
        result = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "tag-archives", "--progress-json"],
                                cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        self.assertEqual(json.loads(result.stdout)["archives"], [])
        self.assertTrue(result.stderr)
        for line in result.stderr.splitlines():
            self.assertIsInstance(json.loads(line), dict)


if __name__ == "__main__":
    unittest.main()
