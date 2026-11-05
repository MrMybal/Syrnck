"""Three-way text merge CLI, Git comparison and explicit resolved publication."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import native_workflows as base
import workflows


class NativeTextMerge(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def result(self, *args, ready=True):
        run = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "merge-text", *map(str, args)],
                             cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0 if ready else 1, run.stderr.decode("utf-8", "replace"))
        report = json.loads(run.stdout)
        self.assertEqual(report["ready"], ready)
        return run, report

    def versions(self, original, current, incoming, name="Source/\u00c9dit source.cpp"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(original)
        first = self.commit("base")
        path.write_bytes(incoming)
        source = self.commit("incoming")
        self.call("native", "branch", "feature", source)
        self.call("native", "checkout", first)
        path.write_bytes(current)
        head = self.commit("current")
        return first, head, source, name

    def test_json_selectors_languages_progress_raw_bytes_and_no_writes(self):
        old, ours, theirs = b"one\r\ntwo\r\nlast", b"ONE\r\ntwo\r\nlast", b"one\r\ntwo\r\nLAST"
        first, head, source, name = self.versions(old, ours, theirs)
        (self.root / name).write_bytes(b"binary\0uncommitted")
        before = self.snapshot()
        run, report = self.result(first, "HEAD", "feature", name, "--progress-json")
        self.assertEqual(report["current_revision"], head)
        self.assertEqual(report["incoming_revision"], source)
        self.assertTrue(report["objects_verified"])
        self.assertEqual(report["merge"]["content"].encode(), b"ONE\r\ntwo\r\nLAST")
        self.assertTrue(json.loads(run.stderr.splitlines()[-1])["complete"])
        self.assertEqual(self.result(first, head, source, name, "--lang", "fr")[1], report)
        raw = self.call("native", "merge-text", first, head, source, name, "--text").stdout
        self.assertEqual(raw, b"ONE\r\ntwo\r\nLAST")
        self.assertEqual(raw, self.call("--lang", "fr", "native", "merge-text", first, head, source, name, "--text").stdout)
        self.assertEqual(before, self.snapshot())
        self.assertEqual((self.root / name).read_bytes(), b"binary\0uncommitted")
        self.call("native", "merge-text", first, head, source, "../unsafe", ok=False)
        self.call("native", "merge-text", first, head, source, name, "--max-lines", "0", ok=False)

    def test_conflicts_binary_presence_and_limits_never_emit_partial_text(self):
        first, head, source, name = self.versions(b"base\n", b"ours\n", b"theirs\n", "code.cpp")
        for extra, reason in [((), "overlapping_changes"), (("--max-work", "1"), "work_limit"),
                              (("--max-file-bytes", "1"), "file_limit")]:
            _, report = self.result(first, head, source, name, *extra, ready=False)
            self.assertEqual(report["blockers"], [reason])
            if report["merge"] is not None:
                self.assertIsNone(report["merge"]["content"])
            failed = self.call("--lang", "fr", "native", "merge-text", first, head, source, name, "--text", *extra, ok=False)
            self.assertIn("fusion textuelle", failed.stderr.decode("utf-8"))
        path = self.root / name
        path.write_bytes(b"binary\0file")
        binary = self.commit("binary")
        self.assertEqual(self.result(first, binary, source, name, ready=False)[1]["merge"]["status"], "binary")
        self.call("native", "merge-text", first, binary, source, name, "--text", ok=False)
        path.unlink()
        removed = self.commit("removed")
        missing = self.result(first, removed, source, name, ready=False)[1]
        self.assertEqual(missing["blockers"], ["file_presence"])
        self.assertIsNone(missing["merge"])
        self.call("native", "merge-text", first, removed, source, name, "--text", ok=False)

    def test_clean_output_matches_git_merge_file(self):
        cases = [(b"a\nb\nc\nd\ne\n", b"A\nb\nc\nd\ne\n", b"a\nb\nc\nd\nE\n"),
                 (b"a\r\nb\r\nc", b"A\r\nb\r\nc", b"a\r\nb\r\nC"),
                 (b"", b"first", b"first"),
                 (b"one\nkeep\nlast", b"\nkeep\nlast", b"one\nkeep\nLAST")]
        # Separate paths allow all cases to share three native snapshots.
        for i, (old, _, _) in enumerate(cases):
            (self.root / f"case{i}").write_bytes(old)
        first = self.commit("all base files")
        for i, (_, ours, _) in enumerate(cases):
            (self.root / f"case{i}").write_bytes(ours)
        head = self.commit("all current files")
        for i, (_, _, theirs) in enumerate(cases):
            (self.root / f"case{i}").write_bytes(theirs)
        source = self.commit("all incoming files")
        with tempfile.TemporaryDirectory(prefix="syrnck-git-merge-") as tmp:
            root = Path(tmp)
            for i, (old, ours, theirs) in enumerate(cases):
                for label, data in [("base", old), ("current", ours), ("incoming", theirs)]:
                    (root / label).write_bytes(data)
                git = subprocess.run(["git", "merge-file", "-p", "current", "base", "incoming"], cwd=root,
                                     capture_output=True, timeout=15)
                self.assertEqual(git.returncode, 0, git.stderr)
                merged = self.call("native", "merge-text", first, head, source, f"case{i}", "--text").stdout
                self.assertEqual(merged, git.stdout)

    def test_explicit_resolution_is_recoverably_published_and_survives_gc(self):
        first, head, source, name = self.versions(b"a\nb\nc\n", b"A\nb\nc\n", b"a\nb\nC\n", "code.cpp")
        blocked = subprocess.run([workflows.CLI, "--store", str(self.store), "native", "merge-plan", source],
                                 cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(blocked.returncode, 1, blocked.stderr)
        context = json.loads(blocked.stdout)["resolution_context"]
        merged = self.call("native", "merge-text", first, head, source, name, "--text").stdout
        # Store-local review files are outside the tracked workspace scan.
        candidate = self.store / "combined.txt"
        candidate.write_bytes(merged)
        manifest = json.loads(self.call("put", candidate).stdout)["manifest"]
        context["files"] = {name: {"choice": "file", "manifest": manifest, "executable": False}}
        choices = self.store / "choices.json"
        choices.write_text(json.dumps(context), encoding="utf-8")
        verified = json.loads(self.call("native", "merge-plan", source, "--resolutions", choices, "--verify").stdout)
        self.assertTrue(verified["ready"])
        applied = json.loads(self.call("native", "merge", source, "--expect", head, "--resolutions", choices).stdout)
        self.assertEqual((self.root / name).read_bytes(), b"A\nb\nC\n")
        self.call("gc", "--delete")
        self.call("native", "export", applied["target"], "retained")
        self.assertEqual((self.root / "retained" / name).read_bytes(), b"A\nb\nC\n")
        revision = json.loads(self.call("inspect", "revision", applied["target"]).stdout)
        self.assertEqual(revision["parents"], [head, source])


if __name__ == "__main__":
    unittest.main()
