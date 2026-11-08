"""Native line attribution compared with Git through merges and exact renames."""
import json
import subprocess
import unittest
import native_workflows as base


class NativeBlame(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    call = base.NativeWorkflows.call
    cmd = base.NativeWorkflows.cmd
    snapshot = base.NativeWorkflows.snapshot

    def git(self, *args):
        result = subprocess.run(["git", "-c", "safe.directory=" + self.root.as_posix(), *map(str, args)],
                                cwd=self.root, env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return result.stdout

    def git_commit(self, message, path, parents=()):
        self.git("add", "-A", "--", *path)
        tree = self.git("write-tree").decode().strip()
        args = ["commit-tree", tree, "-m", message]
        for parent in parents:
            args += ["-p", parent]
        return self.git(*args).decode().strip()

    def test_matches_git_merge_parents_and_renamed_unicode_file(self):
        self.git("init")
        self.git("config", "user.name", "Blame fixture")
        self.git("config", "user.email", "blame@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "core.quotePath", "false")
        self.git("config", "commit.gpgsign", "false")
        old, new = "code é.cpp", "renamed é.cpp"
        (self.root / old).write_bytes(b"root\ncommon\n")
        first = self.cmd("native", "commit", "-m", "root").decode().strip()
        git_first = self.git_commit("root", [old])
        self.cmd("native", "branch", "side", first)
        (self.root / old).write_bytes(b"left\ncommon\n")
        self.cmd("native", "commit", "-m", "left")
        git_left = self.git_commit("left", [old], [git_first])
        (self.root / old).write_bytes(b"root\ncommon\nright\n")
        right = self.cmd("native", "commit", "--branch", "side", "-m", "right").decode().strip()
        git_right = self.git_commit("right", [old], [git_first])
        (self.root / old).write_bytes(b"left\ncommon\n")
        merged = json.loads(self.call("native", "merge", right, "--text-merge", "-m", "merged").stdout)
        self.assertEqual(merged["state"], "committed")
        git_merged = self.git_commit("merged", [old], [git_left, git_right])
        (self.root / old).rename(self.root / new)
        self.cmd("native", "commit", "-m", "renamed")
        git_renamed = self.git_commit("renamed", [old, new], [git_merged])
        before = self.snapshot()
        report = json.loads(self.call("--lang", "fr", "native", "blame", "HEAD", new, "--progress-json").stdout)
        oracle, summary = [], None
        for line in self.git("blame", "--line-porcelain", git_renamed, "--", new).decode("utf-8").splitlines():
            if line.startswith("summary "):
                summary = line[8:]
            elif line.startswith("\t"):
                oracle.append((summary, line[1:]))
        actual = [(report["commits"][row["revision"]]["message"], row["content"].removesuffix("\n"))
                  for row in report["lines"]]
        self.assertEqual(actual, oracle)
        self.assertEqual([value[0] for value in actual], ["left", "root", "right"])
        self.assertEqual([row["path"] for row in report["lines"]], [old] * 3)
        self.assertEqual(before, self.snapshot())
        first_parent = json.loads(self.call("native", "blame", "HEAD", new, "--first-parent").stdout)
        self.assertEqual(first_parent["commits"][first_parent["lines"][-1]["revision"]]["message"], "merged")
        no_follow = json.loads(self.call("native", "blame", "HEAD", new, "--no-follow").stdout)
        self.assertTrue(all(no_follow["commits"][row["revision"]]["message"] == "renamed" for row in no_follow["lines"]))
        self.cmd("pack")
        self.cmd("gc", "--delete")
        self.assertEqual(json.loads(self.call("native", "blame", "HEAD", new).stdout)["lines"], report["lines"])

    def test_budgets_binary_empty_and_french_failures(self):
        path = self.root / "code"
        path.write_bytes(b"first\r\nsecond")
        self.cmd("native", "commit", "-m", "first")
        path.write_bytes(b"changed\r\nsecond")
        self.cmd("native", "commit", "-m", "changed")
        report = json.loads(self.call("native", "blame", "HEAD", "code").stdout)
        self.assertEqual([row["content"] for row in report["lines"]], ["changed\r\n", "second"])
        for flag, value, reason in (("--max-revisions", 1, "revision_limit"),
                                    ("--max-input-bytes", 1, "input_limit"),
                                    ("--max-file-bytes", 1, "file_limit"),
                                    ("--max-lines", 1, "line_limit"),
                                    ("--max-work", 1, "work_limit")):
            with self.subTest(flag=flag):
                limited = json.loads(self.cmd("native", "blame", "HEAD", "code", flag, value, ok=False))
                self.assertEqual((limited["status"], limited["reason"], limited["lines"], limited["commits"]),
                                 ("limited", reason, [], {}))
        error = self.call("--lang", "fr", "native", "blame", "HEAD", "absent", ok=False)
        self.assertIn("absent de la révision", error.stderr.decode("utf-8"))
        for flag in ("--max-revisions", "--max-input-bytes"):
            self.call("native", "blame", "HEAD", "code", flag, 0, ok=False)
        path.write_bytes(b"a\0b")
        self.cmd("native", "commit", "-m", "binary")
        binary = json.loads(self.cmd("native", "blame", "HEAD", "code", ok=False))
        self.assertEqual((binary["status"], binary["lines"]), ("binary", []))
        path.write_bytes(b"")
        self.cmd("native", "commit", "-m", "empty")
        empty = json.loads(self.call("native", "blame", "HEAD", "code").stdout)
        self.assertEqual((empty["status"], empty["lines"]), ("complete", []))


if __name__ == "__main__":
    unittest.main()
