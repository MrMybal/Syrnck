"""Native release labels, revision selectors and archived deletion in the CLI."""
import json
import unittest

import native_workflows as base


class NativeTags(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_labels_annotations_read_only_reports_and_immutable_names(self):
        self.assertEqual(json.loads(self.call("native", "tags").stdout),
                         {"version": 1, "tags": [], "has_more": False, "next_after": None})
        before = self.snapshot()
        self.call("native", "tag", "unborn", ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / "asset.bin").write_bytes(b"original")
        first = self.commit("baseline")
        (self.root / "asset.bin").write_bytes(b"uncommitted edit")
        created = self.call("native", "tag", "release/v1", "-m", "Version française\nRelease notes", "--progress-json")
        tag = json.loads(created.stdout)["tag"]
        self.assertEqual(tag["revision"], first)
        self.assertEqual(tag["message"], "Version française\nRelease notes")
        self.assertIsInstance(tag["timestamp"], int)
        self.assertIsInstance(tag["annotation"], str)
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"uncommitted edit")
        events = [json.loads(line) for line in created.stderr.splitlines()]
        self.assertEqual((events[-1]["phase"], events[-1]["path"]), ("publishing", "refs"))
        self.call("native", "tag", "light", first)
        self.call("native", "tag", "empty", first, "-m", "")
        before = self.snapshot()
        report = json.loads(self.call("native", "tags").stdout)
        self.assertEqual([item["name"] for item in report["tags"]], ["empty", "light", "release/v1"])
        self.assertIsNone(report["tags"][1]["annotation"])
        self.assertIsNone(report["tags"][1]["message"])
        self.assertIsNone(report["tags"][1]["timestamp"])
        page = json.loads(self.call("native", "tags", "--limit", "2").stdout)
        self.assertTrue(page["has_more"])
        self.assertEqual(page["next_after"], "light")
        last = json.loads(self.call("native", "tags", "--after", page["next_after"], "--limit", "2").stdout)
        self.assertFalse(last["has_more"])
        self.assertEqual([tag["name"] for tag in last["tags"]], ["release/v1"])
        shown = json.loads(self.call("native", "tag-show", "release/v1").stdout)
        self.assertFalse(shown["objects_verified"])
        self.assertEqual(shown["tag"], tag)
        self.assertEqual(shown, json.loads(self.call("--lang", "fr", "native", "tag-show", "release/v1").stdout))
        self.assertTrue(json.loads(self.call("native", "tag-show", "release/v1", "--verify").stdout)["objects_verified"])
        self.assertEqual(self.snapshot(), before)
        for args in (("tag", "release/v1", first), ("tag", "bad:name"), ("tag", "-force"),
                     ("tag",), ("tag", "x", "HEAD", "extra"),
                     ("tags", "extra"), ("tags", "--limit", "0"), ("tags", "--limit", "1001"),
                     ("tag-show", "missing"), ("tag-delete", "release/v1"),
                     ("tag-delete", "release/v1", "--expect", "0" * 64),
                     ("tag-delete", "release/v1", "--expect", "HEAD"), ("tag-restore", "invalid"),
                     ("tag-archive", "0" * 64)):
            self.call("native", *args, ok=False)
        self.assertEqual(self.snapshot(), before)

    def test_revision_selectors_work_across_native_commands(self):
        (self.root / "asset.bin").write_bytes(b"first")
        first = self.commit("first")
        self.call("native", "tag", "v1")
        (self.root / "asset.bin").write_bytes(b"second")
        second = self.commit("second")
        self.call("native", "branch", "next", "HEAD")
        self.assertFalse(json.loads(self.call("native", "status", "--revision", "v1").stdout)["clean"])
        difference = json.loads(self.call("native", "diff", "tags/v1", "HEAD").stdout)
        self.assertEqual((difference["source"], difference["target"]), (first, second))
        self.assertTrue(json.loads(self.call("native", "checkout-plan", "v1").stdout)["ready"])
        self.call("native", "checkout", "tags/v1")
        self.assertEqual((self.root / "asset.bin").read_bytes(), b"first")
        self.call("native", "branch", "retry", "v1")
        self.call("native", "switch", "next")
        self.call("native", "tag", "current")
        current = json.loads(self.call("native", "tag-show", "current").stdout)
        self.assertEqual(current["tag"]["revision"], second)
        self.assertTrue(json.loads(self.call("native", "status", "--revision", "HEAD").stdout)["clean"])
        self.call("native", "branch", "v1", "heads/main")
        before = self.snapshot()
        self.call("native", "checkout-plan", "v1", ok=False)
        self.call("native", "checkout", "v1", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(json.loads(self.call("native", "diff", "heads/v1", "tags/v1").stdout)["equal"])
        output = self.root / "exported"
        self.call("native", "export", "tags/v1", output)
        self.assertEqual((output / "asset.bin").read_bytes(), b"first")

    def test_deletion_archive_is_the_only_gc_root_then_restores_the_annotation(self):
        (self.root / "asset.bin").write_bytes(b"kept version")
        first = self.commit("kept")
        self.call("native", "tag", "release/v1", "-m", "Durable release")
        original = json.loads(self.call("native", "tag-show", "release/v1").stdout)["tag"]
        (self.root / "asset.bin").write_bytes(b"new version")
        self.commit("new")
        deleted = json.loads(self.call("native", "tag-delete", "release/v1", "--expect", first).stdout)
        archive = deleted["archive"]
        self.assertEqual(deleted["tag"], original)
        self.assertEqual(json.loads(self.call("native", "tags").stdout)["tags"], [])
        refs = json.loads(self.cmd("refs"))
        for name in refs:
            if not name.startswith("archive/tags/" + archive + "/"):
                self.call("unpin", name)
        self.call("gc", "--delete")
        before = self.snapshot()
        saved = json.loads(self.call("native", "tag-archive", archive, "--verify").stdout)
        self.assertEqual(saved["tag"], original)
        self.assertTrue(saved["objects_verified"])
        self.assertEqual(self.snapshot(), before)
        restored = json.loads(self.call("native", "tag-restore", archive).stdout)
        self.assertEqual(restored["tag"], original)
        before = self.snapshot()
        self.call("native", "tag-restore", archive, ok=False)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(json.loads(self.cmd("refs"))["tags/release/v1"]["id"], first)
        self.assertIn("archive/tags/" + archive + "/target", json.loads(self.cmd("refs")))
        output = self.root / "restored"
        self.call("native", "export", "tags/release/v1", output)
        self.assertEqual((output / "asset.bin").read_bytes(), b"kept version")
        self.call("fsck")


if __name__ == "__main__":
    unittest.main()
