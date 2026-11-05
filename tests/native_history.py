"""Native ancestry and history operations through the executable."""
import json
import unittest

import native_workflows as base


class NativeHistory(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def test_ancestry_selectors_budgets_progress_and_read_only_queries(self):
        (self.root / "asset.bin").write_bytes(b"first")
        first = self.commit("first")
        self.call("native", "tag", "v1")
        (self.root / "asset.bin").write_bytes(b"second")
        second = self.commit("second")
        self.call("native", "branch", "feature", first)
        before = self.snapshot()
        result = self.call("native", "ancestry", "tags/v1", "HEAD", "--progress-json")
        report = json.loads(result.stdout)
        self.assertEqual(report["source"], first)
        self.assertEqual(report["target"], second)
        self.assertEqual(report["merge_bases"], [first])
        self.assertTrue(report["source_is_ancestor"])
        self.assertFalse(report["target_is_ancestor"])
        self.assertEqual(report["visited_revisions"], 2)
        self.assertEqual(report["parent_links"], 1)
        self.assertTrue([json.loads(line) for line in result.stderr.splitlines()][-1]["complete"])
        self.assertEqual(json.loads(self.call("--lang", "fr", "native", "ancestry", "feature", second).stdout), report)
        reverse = json.loads(self.call("native", "ancestry", "HEAD", "v1").stdout)
        self.assertTrue(reverse["target_is_ancestor"])
        self.assertFalse(reverse["source_is_ancestor"])
        for args in (("--max-revisions", "1"), ("--max-links", "0"),
                     ("--max-revisions", "0"), ("--max-links", "10000001")):
            self.call("native", "ancestry", first, second, *args, ok=False)
        self.call("native", "ancestry", first, ok=False)
        error = self.call("--lang", "fr", "native", "ancestry", first, second,
                          "--max-revisions", "1", ok=False).stderr.decode("utf-8")
        self.assertIn("limite de révisions du graphe natif dépassée", error)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
