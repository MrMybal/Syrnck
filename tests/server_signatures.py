"""Real authenticated HTTP publication with registered native signing identities."""
import json
import unittest
import server_access as base


class ServerSignatures(unittest.TestCase):
    cmd = base.ServerAccess.cmd
    http = base.ServerAccess.http
    serving = base.ServerAccess.serving
    commit = base.ServerAccess.commit
    publish = base.ServerAccess.publish

    def setUp(self):
        base.ServerAccess.setUp(self)
        self.seeds = {"alice": "29", "bob": "2a", "admin": "2b"}
        self.config["version"] = 2
        for principal in self.config["principals"]:
            actor = principal["id"]
            principal["signing_keys"] = []
            if actor != "reader":
                profile = self.signer(actor)
                principal["signing_keys"] = [self.binding(profile)]
                if actor == "bob":
                    principal["write_paths"] = ["Source", "Content"]
        for branch in self.config["branches"]:
            branch["require_signatures"] = True
        self.save_policy()
        self.signer("alice")

    def signer(self, actor, seed=None, name=None, email=None):
        self.env["SYRNCK_SIGNING_SEED"] = (seed or self.seeds[actor]) * 32
        return json.loads(self.cmd("native", "identity", "--name", name or actor.title(),
                                   "--email", email or actor + "@example.invalid").stdout)

    @staticmethod
    def binding(profile):
        return {field: profile[field] for field in ("public_key", "name", "email")}

    def save_policy(self):
        self.access.write_text(json.dumps(self.config), encoding="utf-8")

    def metadata(self, identifier, store=None, key=False):
        return json.loads(self.cmd("inspect", "revision", identifier, store=store, key=key).stdout)

    def assert_remote(self, revision, branch="main"):
        self.assertEqual(self.http("GET", "/v1/refs")[branch]["id"], revision)

    def test_delegated_signed_publication_and_reader_fetch(self):
        first = self.commit("Source/code.cpp", b"Alice source\n")
        signed = self.metadata(first)
        with self.serving(strict=True):
            # Bob transports Alice's approved work; he does not impersonate her signer.
            self.publish(first, actor="bob")
            self.signer("bob")
            second = self.commit("Source/code.cpp", b"Bob source\n")
            self.publish(second, actor="alice")
            self.assert_remote(second)
            clone = self.root / "clone"
            self.cmd("init", store=clone)
            self.env.pop("SYRNCK_SIGNING_SEED")
            self.cmd("native", "fetch", self.url, "--require-read-lease", actor="reader", store=clone)
            self.assertEqual(self.metadata(first, clone), signed)
            self.cmd("gc", "--delete", store=clone)
            self.cmd("fsck", store=clone)
            self.assertIsNone(json.loads(self.cmd("native", "identity", store=clone).stdout))
        self.cmd("gc", "--delete", store=self.server, key=True)
        self.cmd("fsck", store=self.server, key=True)
        self.assertEqual(self.metadata(first, self.server, True), signed)
        self.assertFalse((self.server / "native-identity.json").exists())

    def test_correct_tip_cannot_hide_unsigned_unknown_or_mismatched_committers(self):
        first = self.commit("Source/code.cpp", b"approved\n")
        with self.serving(strict=True):
            self.publish(first)
            for mode in ("unsigned", "unknown-key", "wrong-name", "wrong-email"):
                with self.subTest(mode=mode):
                    self.cmd("native", "checkout", first)
                    self.signer("alice", seed="2c" if mode == "unknown-key" else None,
                                name="Mallory" if mode == "wrong-name" else None,
                                email="mallory@example.invalid" if mode == "wrong-email" else None)
                    if mode == "unsigned":
                        self.env.pop("SYRNCK_SIGNING_SEED")
                    denied = self.commit("Source/code.cpp", mode.encode())
                    # Local cryptographic validation succeeds; policy must reject it.
                    self.assertEqual(self.metadata(denied)["version"], 2)
                    self.publish(denied, actor="bob", ok=False)
                    self.assert_remote(first)
                    self.signer("bob")
                    tip = self.commit("Source/code.cpp", b"approved\n")
                    self.publish(tip, actor="bob", ok=False)
                    self.assert_remote(first)

    def test_restart_revocation_rotation_and_revoked_rewind_protection(self):
        first = self.commit("Source/code.cpp", b"first\n")
        with self.serving(strict=True):
            self.publish(first)
            self.publish(first, name="scratch")
        self.config["principals"][0]["signing_keys"] = []
        self.save_policy()
        revoked = self.commit("Source/code.cpp", b"old signing key\n")
        with self.serving(strict=True):
            self.publish(revoked, actor="bob", ok=False)
            self.cmd("native", "checkout", first)
            self.signer("bob")
            bob = self.commit("Source/code.cpp", b"Bob after revocation\n")
            # The already admitted Alice parent remains a readable baseline.
            self.publish(bob, actor="bob")
            self.publish(bob, actor="bob", name="scratch")
            self.publish(first, actor="bob", name="scratch", ok=False)
            self.assert_remote(bob, "scratch")
        rotated = self.signer("alice", seed="2d")
        self.config["principals"][0]["signing_keys"] = [self.binding(rotated)]
        self.save_policy()
        current = self.commit("Source/code.cpp", b"rotated key\n")
        with self.serving(strict=True):
            self.publish(current, actor="bob")
            self.assert_remote(current)
        # Exact retry can finish a lease even after the current tip's key is revoked.
        self.config["principals"][0]["signing_keys"] = []
        self.save_policy()
        self.env.pop("SYRNCK_SIGNING_SEED")
        with self.serving(strict=True):
            self.publish(current, actor="bob")
            self.assert_remote(current)
        self.cmd("gc", "--delete", store=self.server, key=True)
        self.cmd("fsck", store=self.server, key=True)
        self.assertEqual(self.metadata(first, self.server, True)["author"]["name"], "Alice")


if __name__ == "__main__":
    unittest.main()
