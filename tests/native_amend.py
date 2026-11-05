"""Native amendment CLI modes, branch defaults, expected tips and retained versions."""
import json
import unittest

import native_workflows as base


class NativeAmendment(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_directory_staged_message_and_archived_exports_after_gc(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"original")
        original = self.commit("first")
        asset.write_bytes(b"replacement")
        result = self.call("native", "commit", "--amend", "--expect", original,
                           "-m", "replacement", "--progress-json")
        replacement = result.stdout.decode().strip()
        events = [json.loads(line) for line in result.stderr.splitlines()]
        self.assertEqual((events[-1]["phase"], events[-1]["path"]), ("publishing", "refs"))
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["archive/amend/" + original]["id"], original)
        asset.write_bytes(b"prepared")
        self.call("native", "add", "asset.bin")
        asset.write_bytes(b"working edit")
        before = self.snapshot()
        self.call("native", "commit", "--amend", "-m", "implicit staged forbidden", ok=False)
        self.call("native", "commit", "--amend", "--message-only", "-m", "pending", ok=False)
        self.assertEqual(self.snapshot(), before)
        staged = self.call("native", "commit", "--amend", "--staged", "-m", "prepared replacement").stdout.decode().strip()
        self.assertEqual(asset.read_bytes(), b"working edit")
        asset.unlink()
        result = self.call("--lang", "fr", "native", "commit", "--amend", "--message-only",
                           "--expect", staged, "-m", "Message corrigé", "--progress-json")
        final = result.stdout.decode().strip()
        events = [json.loads(line) for line in result.stderr.splitlines()]
        self.assertNotIn("scanning", [event["phase"] for event in events])
        self.call("gc", "--delete")
        for name, revision, expected in (("old", original, b"original"),
                                         ("replacement", replacement, b"replacement"),
                                         ("prepared", staged, b"prepared"),
                                         ("final", final, b"prepared")):
            self.call("native", "export", revision, self.root / name)
            self.assertEqual((self.root / name / "asset.bin").read_bytes(), expected)
        log = self.call("native", "log").stdout.decode("utf-8").splitlines()
        self.assertEqual(len(log), 1)
        self.assertTrue(log[0].startswith(final + " "))
        self.assertTrue(log[0].endswith("Message corrigé"))

    def test_invalid_options_and_expected_tip_leave_store_unchanged(self):
        before = self.snapshot()
        self.call("native", "commit", "--amend", "-m", "unborn", ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / "asset.bin").write_bytes(b"first")
        first = self.commit("first")
        before = self.snapshot()
        for args in (("--message-only",), ("--expect", first),
                     ("--amend", "--message-only", "--staged"),
                     ("--amend", "--expect", "HEAD"),
                     ("--amend", "--expect", "0" * 64),
                     ("--amend", "--staged"),
                     ("--amend", "--message-only", "--directory", ".")):
            self.call("native", "commit", *args, "-m", "invalid", ok=False)
        self.assertEqual(self.snapshot(), before)
        error = self.call("--lang", "fr", "native", "commit", "--amend", "--expect", "0" * 64,
                          "-m", "refus", ok=False).stderr.decode("utf-8")
        self.assertIn("le dernier commit natif a changé", error)
        self.assertEqual(self.snapshot(), before)

    def test_selected_branch_and_explicit_override_preserve_head(self):
        (self.root / "asset.bin").write_bytes(b"first")
        first = self.commit("first")
        self.call("native", "branch", "feature", first)
        self.call("native", "switch", "feature")
        head = json.loads(self.call("native", "head").stdout)
        amended = self.call("native", "commit", "--amend", "--message-only", "-m", "feature corrected").stdout.decode().strip()
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/main"]["id"], first)
        self.assertEqual(refs["heads/feature"]["id"], amended)
        head["revision"] = amended
        self.assertEqual(json.loads(self.call("native", "head").stdout), head)
        self.call("native", "commit", "--amend", "--branch", "main", "--message-only", "-m", "main corrected")
        self.assertEqual(json.loads(self.call("native", "head").stdout), head)


if __name__ == "__main__":
    unittest.main()
