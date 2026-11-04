"""Persistent native staging through the public CLI, with real Store reopenings."""
import json
import unittest

import native_workflows as base


class NativeStaging(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def staged(self, branch="main"):
        return json.loads(self.call("native", "staged", "--branch", branch).stdout)

    def metadata(self, revision):
        return json.loads(self.cmd("inspect", "revision", revision))

    def test_selected_commit_uses_frozen_bytes_and_consumes_index(self):
        (self.root / "edited.bin").write_bytes(b"before")
        (self.root / "other.txt").write_bytes(b"unselected before")
        (self.root / "removed.txt").write_bytes(b"remove")
        first = self.commit("baseline")
        (self.root / "edited.bin").write_bytes(b"prepared\x00bytes")
        (self.root / "other.txt").write_bytes(b"unselected after")
        (self.root / "removed.txt").unlink()
        (self.root / "Éditeur.cpp").write_bytes(b"prepared source\n")
        added = self.call("native", "add", "edited.bin", "removed.txt", "Éditeur.cpp", "--workers", "2",
                          "--progress-json")
        frozen = added.stdout.decode().strip()
        events = [json.loads(line) for line in added.stderr.splitlines()]
        self.assertEqual(events[-1]["phase"], "publishing")
        self.assertEqual(events[-1]["path"], "refs")
        report = self.staged()
        self.assertEqual(report["base"], first)
        self.assertFalse(report["stale"])
        self.assertEqual(report["snapshot"], frozen)
        self.assertEqual(report["counts"], {"added": 1, "removed": 1, "modified": 1, "unchanged": 1})
        before = self.snapshot()
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "staged").stdout), report)
        self.assertEqual(before, self.snapshot(), "staged inspection wrote Store data")
        blocked = self.call("native", "commit", "-m", "would overwrite staging", ok=False)
        self.assertIn(b"--staged", blocked.stderr)
        self.assertEqual(before, self.snapshot())
        (self.root / "edited.bin").write_bytes(b"new working version")
        (self.root / "Éditeur.cpp").unlink()
        self.cmd("pack", "--max-objects", "2")
        self.cmd("gc", "--delete")
        committed = self.call("native", "commit", "--staged", "-m", "selected changes", "--progress-json")
        second = committed.stdout.decode().strip()
        self.assertEqual(json.loads(committed.stderr.splitlines()[-1])["phase"], "publishing")
        metadata = self.metadata(second)
        self.assertEqual(metadata["parents"], [first])
        self.assertNotIn("removed.txt", metadata["files"])
        self.assertNotIn("staging/main", json.loads(self.cmd("refs")))
        # Export into this fixture so its ordinary cleanup owns every output.
        self.cmd("native", "export", second, self.root / "exported")
        self.assertEqual((self.root / "exported/edited.bin").read_bytes(), b"prepared\x00bytes")
        self.assertEqual((self.root / "exported/Éditeur.cpp").read_bytes(), b"prepared source\n")
        self.assertEqual((self.root / "exported/other.txt").read_bytes(), b"unselected before")
        self.assertEqual((self.root / "edited.bin").read_bytes(), b"new working version")
        self.assertIsNone(self.staged()["snapshot"])
        self.assertTrue(self.staged()["clean"])

    def test_unborn_branch_selective_and_full_reset_never_write_workspace(self):
        source = self.root / "-file.txt"
        source.write_bytes(b"source")
        added = self.call("native", "add", "--branch", "feature/topic", "--", source.name)
        frozen = added.stdout.decode().strip()
        self.assertIsNone(self.staged("feature/topic")["base"])
        self.assertIsNone(self.staged()["snapshot"])
        source.write_bytes(b"later working edit")
        self.call("native", "reset", "--branch", "feature/topic", "--", source.name)
        self.assertTrue(self.staged("feature/topic")["clean"])
        self.call("native", "commit", "--staged", "--branch", "feature/topic", "-m", "empty", ok=False)
        source.write_bytes(b"source")
        restaged = self.call("native", "add", "--branch", "feature/topic", ".", ".").stdout.decode().strip()
        self.assertEqual(restaged, frozen)
        source.unlink()
        second = self.call("native", "commit", "--staged", "--branch", "feature/topic", "-m", "unborn").stdout.decode().strip()
        self.assertEqual(self.metadata(second)["parents"], [])
        self.assertFalse(source.exists())
        self.call("native", "add", "--branch", "feature/topic", ".")
        self.assertEqual(self.staged("feature/topic")["counts"]["removed"], 1)
        self.call("native", "reset", "--branch", "feature/topic")
        self.assertIsNone(self.staged("feature/topic")["snapshot"])
        self.assertFalse(source.exists())
        self.call("native", "reset")  # Idempotent full reset with no index.

    def test_nested_ignores_directory_prefixes_and_path_type_changes(self):
        (self.root / "dir").mkdir()
        (self.root / "dir/old").write_bytes(b"old")
        (self.root / "dir2").mkdir()
        (self.root / "dir2/other").write_bytes(b"keep old")
        (self.root / "tracked.tmp").write_bytes(b"tracked")
        self.commit("baseline")
        (self.root / "dir/old").unlink()
        (self.root / "dir/new").write_bytes(b"new")
        (self.root / "dir/ignored.tmp").write_bytes(b"excluded")
        (self.root / "dir/.syrnckignore").write_text("*.tmp\n", encoding="utf-8")
        (self.root / "dir2/other").write_bytes(b"unselected edit")
        (self.root / ".syrnckignore").write_text("tracked.tmp\n", encoding="utf-8")
        self.call("native", "add", "./dir/", "tracked.tmp")
        files = self.metadata(self.staged()["snapshot"])["files"]
        self.assertEqual(sorted(files), ["dir/.syrnckignore", "dir/new", "dir2/other"])
        self.call("native", "reset", "dir", "tracked.tmp")
        self.assertTrue(self.staged()["clean"])
        self.call("native", "reset")
        (self.root / "dir/new").unlink()
        (self.root / "dir/ignored.tmp").unlink()
        (self.root / "dir/.syrnckignore").unlink()
        (self.root / "dir").rmdir()
        (self.root / "dir").write_bytes(b"directory became a file")
        self.call("native", "add", "dir")
        files = self.metadata(self.staged()["snapshot"])["files"]
        self.assertIn("dir", files)
        self.assertNotIn("dir/old", files)
        self.call("native", "reset", "dir")
        self.assertTrue(self.staged()["clean"])

    def test_stale_index_remains_inspectable_and_requires_explicit_full_reset(self):
        source = self.root / "file"
        source.write_bytes(b"baseline")
        first = self.commit("baseline")
        source.write_bytes(b"prepared")
        frozen = self.call("native", "add", ".").stdout.decode().strip()
        source.write_bytes(b"other head")
        self.call("native", "detach-history", "-m", "advance independently")
        before = json.loads(self.cmd("refs"))
        report = self.staged()
        self.assertTrue(report["stale"])
        self.assertEqual(report["base"], first)
        self.assertEqual(report["snapshot"], frozen)
        self.assertEqual(report["head"], before["heads/main"]["id"])
        for args in (("add", "."), ("reset", "file"), ("commit", "--staged", "-m", "stale")):
            error = self.call("--lang", "fr", "native", *args, ok=False)
            self.assertIn("la base du staging natif a changé", error.stderr.decode("utf-8"))
            self.assertEqual(json.loads(self.cmd("refs")), before)
        self.call("native", "reset")
        self.assertFalse(self.staged()["stale"])
        self.assertEqual(source.read_bytes(), b"other head")
        self.call("native", "add", ".")
        self.assertEqual(self.staged()["base"], before["heads/main"]["id"])
        self.assertEqual(self.staged()["counts"]["modified"], 1)

    def test_invalid_selections_and_option_combinations_never_publish(self):
        (self.root / "file").write_bytes(b"working")
        for args in (("add",), ("add", "missing"), ("add", "../outside"), ("add", "*.txt"),
                     ("add", "--unknown"), ("add", str(self.root / "file")),
                     ("reset", "file"), ("staged", "--directory", "."),
                     ("commit", "--staged", "-m", "missing index"),
                     ("commit", "--staged", "-m", "bad", "--workers", "2"),
                     ("commit", "--staged", "-m", "bad", "--directory", ".")):
            self.call("native", *args, ok=False)
            self.assertEqual(json.loads(self.cmd("refs")), {})
        frozen = self.call("native", "add", ".").stdout.decode().strip()
        before = json.loads(self.cmd("refs"))
        self.call("native", "add", "file", "missing", ok=False)
        self.call("native", "reset", "missing", ok=False)
        self.assertEqual(json.loads(self.cmd("refs")), before)
        self.assertEqual(self.staged()["snapshot"], frozen)


if __name__ == "__main__":
    unittest.main()
