"""Authored native revisions and private-seed ownership through the CLI."""
import json
import unittest
import native_workflows as base


class NativeAuthorship(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    commit = base.NativeWorkflows.commit
    snapshot = base.NativeWorkflows.snapshot

    def metadata(self, identifier):
        return json.loads(self.cmd("inspect", "revision", identifier))

    def test_profile_signed_commits_rewrites_and_public_history(self):
        (self.root / "code").write_bytes(b"base\n")
        legacy = self.commit("legacy")
        self.assertEqual(self.metadata(legacy)["version"], 1)
        self.env["SYRNCK_SIGNING_SEED"] = "15" * 32
        profile = json.loads(self.cmd("native", "identity", "--name", "Alice Équipe", "--email", "alice@example.invalid", "--timezone-offset", "+60"))
        self.assertTrue(profile["signing_key"].startswith("ed25519:"))
        private_profile = (self.store / "native-identity.json").read_text(encoding="utf-8")
        self.assertNotIn(self.env["SYRNCK_SIGNING_SEED"], private_profile)
        self.assertEqual(set(json.loads(private_profile)), {"version", "type", "name", "email", "timezone_offset"})
        (self.root / "code").write_bytes(b"alice\n")
        authored = self.commit("signed content")
        value = self.metadata(authored)
        self.assertEqual(value["version"], 2)
        self.assertEqual(value["author"]["name"], "Alice Équipe")
        self.assertEqual(value["committer"], value["author"])
        self.assertEqual(value["signature"]["key_id"], profile["signing_key"])
        history = json.loads(self.cmd("native", "log", "--json"))
        self.assertEqual(history["revisions"][0]["author"], value["author"])
        self.assertEqual(history["revisions"][0]["signing_key"], profile["signing_key"])
        self.assertEqual(json.loads(self.cmd("--lang", "fr", "native", "log", "--json")), history)
        displayed = self.cmd("--lang", "fr", "native", "log", "--show-signature").decode("utf-8")
        self.assertIn("Auteur : Alice Équipe", displayed)
        self.assertIn("Clé de signature : " + profile["signing_key"], displayed)
        self.env["SYRNCK_SIGNING_SEED"] = "16" * 32
        bob = json.loads(self.cmd("native", "identity", "--name", "Bob", "--email", "bob@example.invalid"))
        amended = self.cmd("native", "commit", "--amend", "--message-only", "-m", "rewritten").decode().strip()
        changed = self.metadata(amended)
        self.assertEqual(changed["author"], value["author"])
        self.assertEqual(changed["committer"]["name"], "Bob")
        self.assertEqual(changed["signature"]["key_id"], bob["signing_key"])
        file_history = json.loads(self.cmd("native", "log-file", "HEAD", "code"))
        self.assertEqual(file_history["entries"][0]["author"], value["author"])
        blame = json.loads(self.cmd("native", "blame", "HEAD", "code"))
        self.assertEqual(blame["commits"][amended]["committer"]["name"], "Bob")
        self.cmd("native", "identity", "--clear")
        self.env.pop("SYRNCK_SIGNING_SEED")
        before = self.snapshot()
        error = self.call("--lang", "fr", "native", "commit", "--amend", "--message-only", "-m", "blocked", ok=False)
        self.assertIn("identité de committer", error.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)
        (self.root / "code").write_bytes(b"unattributed\n")
        unconfigured = self.commit("explicitly unconfigured")
        self.assertEqual(self.metadata(unconfigured)["version"], 1)
        self.cmd("gc", "--delete"); self.cmd("fsck")

    def test_environment_utf8_identity_and_unsigned_profile(self):
        self.cmd("native", "identity", "--name", "Persisted", "--email", "local@example.invalid", "--timezone-offset", "-120")
        (self.root / "code").write_bytes(b"source\n")
        first = self.commit("unsigned authored")
        self.assertIsNone(self.metadata(first)["signature"])
        original = (self.store / "native-identity.json").read_bytes()
        self.env.update(SYRNCK_USER_NAME="Renée 漢字", SYRNCK_USER_EMAIL="renee@example.invalid", SYRNCK_USER_TIMEZONE="90", SYRNCK_SIGNING_SEED="17" * 32)
        (self.root / "code").write_bytes(b"updated\n")
        second = self.commit("environment identity")
        value = self.metadata(second)
        self.assertEqual(value["author"]["name"], "Renée 漢字")
        self.assertEqual(value["author"]["timezone_offset"], 90)
        self.assertEqual((self.store / "native-identity.json").read_bytes(), original)
        self.assertIsNotNone(value["signature"])

    def test_invalid_profile_seed_and_partial_environment_write_nothing(self):
        before = self.snapshot()
        for arguments in (("--name", "Alice"), ("--name", "Alice", "--email", "a@example.invalid", "--timezone-offset", "9999"),
                          ("--clear", "--name", "Alice", "--email", "a@example.invalid")):
            self.call("native", "identity", *arguments, ok=False)
            self.assertEqual(self.snapshot(), before)
        self.env["SYRNCK_SIGNING_SEED"] = "00"
        self.call("native", "identity", "--name", "Alice", "--email", "a@example.invalid", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.env.pop("SYRNCK_SIGNING_SEED")
        self.env["SYRNCK_USER_NAME"] = "Unpaired"
        error = self.call("--lang", "fr", "native", "identity", ok=False)
        self.assertIn("à la fois", error.stderr.decode("utf-8"))
        self.assertEqual(self.snapshot(), before)

    def test_signed_rebase_fold_reuses_frozen_clock_and_first_author(self):
        (self.root / "code").write_bytes(b"base\n")
        original = self.commit("base")
        (self.root / "onto").write_bytes(b"upstream\n")
        onto = self.commit("upstream")
        self.cmd("native", "branch", "upstream", onto)
        self.cmd("native", "checkout", original)
        for name, seed in (("Alice", "1a"), ("Bob", "1b")):
            self.env["SYRNCK_SIGNING_SEED"] = seed * 32
            self.cmd("native", "identity", "--name", name, "--email", name.lower() + "@example.invalid")
            (self.root / name).write_bytes(name.encode())
            tip = self.commit(name + " work")
            if name == "Alice":
                author = self.metadata(tip)["author"]
        self.env["SYRNCK_SIGNING_SEED"] = "1c" * 32
        committer = json.loads(self.cmd("native", "identity", "--name", "Carol", "--email", "carol@example.invalid"))
        todo = json.loads(self.cmd("native", "rebase-todo", original, "--onto", onto, "--committer-time", "1234"))
        todo["steps"][1]["action"] = "squash"
        document = self.store / "authored-todo.json"
        document.write_text(json.dumps(todo), encoding="utf-8")
        args = (original, "--onto", onto, "--todo", document, "--committer-time", "1234")
        before = self.snapshot()
        plan = json.loads(self.cmd("native", "rebase-plan", *args))
        self.assertEqual(plan["committer_timestamp"], 1234)
        self.assertEqual(json.loads(self.cmd("native", "rebase-plan", *args)), plan)
        self.assertEqual(self.snapshot(), before)
        applied = json.loads(self.cmd("native", "rebase", *args, "--expect", tip))
        self.assertEqual(applied["target"], plan["target"])
        value = self.metadata(applied["target"])
        self.assertEqual(value["author"], author)
        self.assertEqual(value["committer"]["name"], "Carol")
        self.assertEqual(value["committer"]["timestamp"], 1234)
        self.assertEqual(value["signature"]["key_id"], committer["signing_key"])
        self.assertEqual(value["parents"], [onto])
        self.assertEqual(value["message"], "Alice work\n\nBob work")
        self.cmd("native", "identity", "--clear")
        self.env.pop("SYRNCK_SIGNING_SEED")
        self.cmd("gc", "--delete"); self.cmd("fsck")


if __name__ == "__main__":
    unittest.main()
