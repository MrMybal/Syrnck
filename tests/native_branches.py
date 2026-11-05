"""Native branch inventory and retained branch lifecycle through the CLI."""
import json
import unittest
import native_workflows as base


class NativeBranches(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_pages_verified_creation_and_language_parity(self):
        self.assertEqual(json.loads(self.call("native", "branches").stdout)["branches"], [])
        (self.root / "asset").write_bytes(b"base")
        first = self.commit("base")
        self.assertEqual(self.call("native", "branch", "feature/topic", first).stdout, b"")
        (self.root / "asset").write_bytes(b"frozen")
        index = self.call("native", "add", "asset", "--branch", "feature/topic").stdout.decode().strip()
        before = self.snapshot()
        page = json.loads(self.call("native", "branches", "--limit", 1, "--verify").stdout)
        self.assertEqual(page, json.loads(self.call("--lang", "fr", "native", "branches", "--limit", 1, "--verify").stdout))
        self.assertEqual(page["branches"][0]["index"], index)
        other = json.loads(self.call("native", "branches", "--after", page["next_after"]).stdout)
        self.assertEqual(other["branches"][0]["name"], "main")
        self.assertTrue(other["branches"][0]["current"])
        self.assertEqual(self.snapshot(), before)
        self.call("native", "branch", "feature/topic", first, ok=False)
        self.call("native", "branches", "--limit", 0, ok=False)
        self.call("native", "branches", "--after", "../bad", ok=False)


if __name__ == "__main__":
    unittest.main()
