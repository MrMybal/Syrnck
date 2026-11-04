"""Production CLI checkout, receipt inspection and rejection of unsafe starts."""
import json
import unittest

import native_workflows as base


class NativeCheckout(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_checkout_moves_branch_preserves_archive_and_reports_in_both_languages(self):
        (self.root / "asset.bin").write_bytes(b"before")
        (self.root / "removed").write_bytes(b"removed")
        first = self.commit("baseline")
        (self.root / "asset.bin").write_bytes(b"after")
        (self.root / "removed").unlink()
        (self.root / "Éditeur.cpp").write_bytes(b"source\n")
        target = self.call("native", "commit", "--branch", "target", "-m", "target").stdout.decode().strip()
        (self.root / "asset.bin").write_bytes(b"before")
        (self.root / "removed").write_bytes(b"removed")
        (self.root / "Éditeur.cpp").unlink()
        (self.root / "untracked").write_bytes(b"keep local")
        run = self.call("native", "checkout", target, "--progress-json")
        result = json.loads(run.stdout)
        self.assertEqual(result["state"], "committed")
        self.assertFalse(result["pending"])
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"after")
        self.assertFalse((self.root / "removed").exists())
        self.assertEqual((self.root / "Éditeur.cpp").read_bytes(), b"source\n")
        self.assertEqual((self.root / "untracked").read_bytes(), b"keep local")
        refs = json.loads(self.cmd("refs"))
        self.assertEqual(refs["heads/main"]["id"], target)
        self.assertEqual(refs[result["archive"]]["id"], first)
        self.assertEqual(refs["checkouts/completed/" + result["id"]]["id"], target)
        self.assertFalse(any(name.startswith("checkouts/pending/") for name in refs))
        receipt = json.loads(self.call("native", "checkout-status", "--id", result["id"]).stdout)
        self.assertEqual(receipt["state"], "committed")
        self.assertFalse(receipt["pending"])
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "checkout-status", "--id", result["id"]).stdout), receipt)
        self.assertFalse(json.loads(self.call("native", "checkout-status").stdout)["pending"])
        events = [json.loads(line) for line in run.stderr.splitlines()]
        self.assertTrue(any(e["phase"] == "reconstructing" for e in events))
        self.assertTrue(any(e["phase"] == "exporting" for e in events))
        self.assertEqual(events[-1]["phase"], "publishing")
        self.assertEqual(events[-1]["path"], "checkout")
        self.call("gc", "--delete")
        self.call("native", "checkout", first)
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"before")
        self.assertEqual((self.root / "removed").read_bytes(), b"removed")
        self.assertFalse((self.root / "Éditeur.cpp").exists())
        self.assertFalse(json.loads(self.call("native", "checkout-recover").stdout)["pending"])

    def test_unborn_checkout_empty_files_and_same_revision_noop(self):
        (self.root / "empty").write_bytes(b"")
        target = self.call("native", "commit", "--branch", "saved", "-m", "empty").stdout.decode().strip()
        (self.root / "empty").unlink()
        result = json.loads(self.call("--lang", "fr", "native", "checkout", target).stdout)
        self.assertIsNone(result["source"])
        self.assertIsNone(result["archive"])
        self.assertEqual((self.root / "empty").read_bytes(), b"")
        before = self.snapshot()
        self.assertEqual(json.loads(self.call("native", "checkout", target).stdout)["state"], "unchanged")
        self.assertEqual(before, self.snapshot())

    def test_blocked_checkout_never_starts_a_transaction(self):
        (self.root / "file").write_bytes(b"baseline")
        first = self.commit("first")
        (self.root / "file").write_bytes(b"local edit")
        before = self.snapshot()
        self.call("native", "checkout", first, ok=False)
        self.assertEqual(before, self.snapshot())
        self.assertFalse(json.loads(self.call("native", "checkout-status").stdout)["pending"])
        (self.root / "file").write_bytes(b"baseline")
        self.call("native", "add", "file")
        before = self.snapshot()
        self.call("native", "checkout", first, ok=False)
        self.assertEqual(before, self.snapshot())
        self.call("native", "reset")
        for args in (("checkout",), ("checkout", "invalid"), ("checkout", first, "--force"),
                     ("checkout-recover", "--abort", "--abort"), ("checkout-recover", "--directory", "."),
                     ("checkout-status", "--id", "0" * 64)):
            self.call("native", *args, ok=False)


if __name__ == "__main__":
    unittest.main()
