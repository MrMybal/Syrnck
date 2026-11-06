"""Local asset lease CLI, grouped ownership, fencing and bilingual errors."""
import json
import subprocess
import unittest
import native_workflows as base
import workflows


class AssetLocks(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    snapshot = base.NativeWorkflows.snapshot

    def plan(self, owner, *paths, ready=True, args=()):
        run = subprocess.run([workflows.CLI, "--store", str(self.store), "locks", "acquire-plan", "--owner", owner,
                              *map(str, args), *paths], cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0 if ready else 1, run.stderr.decode("utf-8", "replace"))
        value = json.loads(run.stdout)
        self.assertEqual(value["ready"], ready)
        return value

    def test_grouped_lifecycle_portable_paths_fencing_pagination_and_gc(self):
        paths = ["Content/\u00c9dit asset.uasset", "Content/\u00c9dit asset.ubulk"]
        owner = "\u00c9lise"
        before = self.snapshot()
        plan = self.plan(owner, *paths)
        self.assertEqual(len(plan["locks"]), 2)
        self.assertEqual(self.snapshot(), before)
        run = self.call("--lang", "fr", "locks", "acquire", "--owner", owner, "--seconds", 60,
                        "--progress-json", *paths)
        acquired = json.loads(run.stdout)
        fence = acquired["locks"][0]["fence"]
        self.assertTrue(all(row["fence"] == fence for row in acquired["locks"]))
        self.assertTrue(any(json.loads(row)["phase"] == "publishing" for row in run.stderr.splitlines()))
        held = self.snapshot()
        blocked = self.plan("bob", "content/\u00e9DIT ASSET.UASSET", "Content/New.uasset", ready=False)
        self.assertEqual(len(blocked["conflicts"]), 1)
        self.call("locks", "acquire", "--owner", "bob", *paths, ok=False)
        self.call("locks", "release", "--owner", "bob", "--fence", fence, *paths, ok=False)
        self.assertEqual(self.snapshot(), held)
        first = json.loads(self.call("locks", "list", "--limit", 1).stdout)
        second = json.loads(self.call("--lang", "fr", "locks", "list", "--after", first["next"], "--limit", 1).stdout)
        self.assertEqual(len(first["locks"]), 1)
        self.assertEqual(len(second["locks"]), 1)
        self.assertNotEqual(first["locks"][0]["path"], second["locks"][0]["path"])
        renewed = json.loads(self.call("locks", "renew", "--owner", owner, "--fence", fence, "--seconds", 120, *paths).stdout)
        self.assertTrue(all(row["fence"] == fence for row in renewed["locks"]))
        self.assertTrue(json.loads(self.call("locks", "prune").stdout)["locks"] == [])
        self.call("pack")
        self.call("gc", "--delete")
        self.assertEqual(len(json.loads(self.call("locks", "list").stdout)["locks"]), 2)
        self.call("locks", "release", "--owner", owner, "--fence", fence, *paths)
        self.call("gc", "--delete")
        replacement = json.loads(self.call("locks", "acquire", "--owner", "bob", *paths).stdout)
        self.assertGreater(replacement["locks"][0]["fence"], fence)
        self.call("locks", "release", "--owner", owner, "--fence", fence, *paths, ok=False)
        self.assertEqual(len(json.loads(self.call("locks", "list").stdout)["locks"]), 2)

    def test_invalid_options_and_french_diagnostics_do_not_publish(self):
        before = self.snapshot()
        for arguments in (("acquire", "asset"), ("acquire", "--owner", "alice"),
                          ("acquire", "--owner", "alice", "--seconds", 0, "asset"),
                          ("acquire", "--owner", "alice", "--seconds", 86401, "asset"),
                          ("acquire", "--owner", "alice", "asset", "ASSET"),
                          ("renew", "--owner", "alice", "asset"),
                          ("release", "--owner", "alice", "--fence", 0, "asset"),
                          ("list", "--limit", 0), ("list", "--limit", 1001), ("unknown",)):
            self.call("locks", *arguments, ok=False)
        french = self.call("--lang", "fr", "locks", "acquire", "asset", ok=False)
        self.assertIn("exige --owner", french.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        help_en = self.call("--lang", "en", "--help").stdout.decode("utf-8")
        help_fr = self.call("--lang", "fr", "--help").stdout.decode("utf-8")
        self.assertIn("List asset locks", help_en)
        self.assertIn("verrous d’assets", help_fr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
