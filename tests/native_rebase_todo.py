"""Editable and interactive native rebase sequences, with recoverable history."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import unittest
import native_workflows as base


class RebaseTodo(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def fixture(self):
        (self.root / "base").write_bytes(b"base")
        root = self.commit("base")
        (self.root / "upstream").write_bytes(b"upstream")
        onto = self.commit("onto")
        self.call("native", "branch", "upstream", onto)
        self.call("native", "checkout", root)
        sources = []
        for name in ("a", "b", "c", "d"):
            (self.root / name).write_bytes(name.encode())
            sources.append(self.commit(name))
        return root, onto, sources

    def interactive(self, root, onto, data, *options, ok=True):
        result = subprocess.run([base.base.CLI, "--store", str(self.store), "native", "rebase",
                                 root, "--onto", onto, "--interactive", *options], cwd=self.root,
                                env=self.env, input=data, capture_output=True, timeout=25)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        if not ok:
            self.assertEqual(result.stdout, b"")
        return result

    def test_export_edit_plan_apply_and_stale_context(self):
        root, onto, (a, b, c, d) = self.fixture()
        before = self.snapshot()
        template = json.loads(self.call("native", "rebase-todo", root, "--onto", "upstream").stdout)
        self.assertEqual([s["source"] for s in template["steps"]], [a, b, c, d])
        self.assertEqual(self.snapshot(), before)
        template["steps"] = [{"source": b, "action": "reword", "message": "Deuxième réécrit\n\nDétails"},
                             {"source": a, "action": "squash"}, {"source": c, "action": "fixup"},
                             {"source": d, "action": "drop"}]
        path = self.store / "todo.json"
        path.write_text(json.dumps(template, ensure_ascii=False), encoding="utf-8")
        before = self.snapshot()
        plan = json.loads(self.call("native", "rebase-plan", root, "--onto", onto,
                                    "--todo", path, "--verify").stdout)
        self.assertEqual(plan["result_commits"], 1)
        self.assertEqual(plan["dropped_commits"], 1)
        self.assertEqual(self.snapshot(), before)
        result = json.loads(self.call("native", "rebase", root, "--onto", onto,
                                      "--todo", path, "--expect", d).stdout)
        self.assertEqual(result["target"], plan["target"])
        metadata = json.loads(self.cmd("inspect", "revision", result["target"]))
        self.assertEqual(metadata["message"], "Deuxième réécrit\n\nDétails\n\na")
        self.assertEqual(metadata["parents"], [onto])
        self.assertFalse((self.root / "d").exists())
        self.call("gc", "--delete")
        self.call("native", "export", d, self.store / "old")
        self.assertEqual((self.store / "old/d").read_bytes(), b"d")
        stale = self.call("native", "rebase", root, "--onto", onto, "--todo", path, "--lang", "fr", ok=False)
        self.assertIn("contexte", stale.stderr.decode("utf-8"))

    def test_interactive_reorder_fold_drop_and_french_prompt(self):
        root, onto, (a, b, c, d) = self.fixture()
        script = f"reword {b} Deuxième réécrit\r\nsquash {a}\r\nfixup {c}\r\ndrop {d}\r\n.\r\n"
        result = self.interactive(root, onto, script.encode(), "--lang", "fr")
        self.assertIn("Sources du rebase", result.stderr.decode("utf-8"))
        self.assertIn("Terminez", result.stderr.decode("utf-8"))
        applied = json.loads(result.stdout)
        metadata = json.loads(self.cmd("inspect", "revision", applied["target"]))
        self.assertEqual(metadata["message"], "Deuxième réécrit\n\na")
        self.assertEqual(metadata["parents"], [onto])
        self.assertEqual(applied["result_commits"], 1)
        for name in ("a", "b", "c", "upstream"):
            self.assertTrue((self.root / name).exists())
        self.assertFalse((self.root / "d").exists())

    def test_invalid_or_unconfirmed_input_never_writes(self):
        root, onto, (a, b, c, d) = self.fixture()
        before = self.snapshot()
        for script in (b"", f"drop {a}\n".encode(), f"drop {a}\n.\n".encode(),
                       f"fixup {a}\npick {b}\npick {c}\npick {d}\n.\n".encode(),
                       f"pick {a} unexpected\n.\n".encode(),
                       f"pick {a}\npick {a}\npick {c}\npick {d}\n.\n".encode()):
            self.interactive(root, onto, script, ok=False)
            self.assertEqual(self.snapshot(), before)
        result = self.interactive(root, onto, b".\n")
        self.assertEqual(json.loads(result.stdout)["replayed_commits"], 4)
        self.assertIn(b"Rebase sources", result.stderr)

    def test_duplicate_json_keys_unknown_fields_and_flag_conflicts(self):
        root, onto, sources = self.fixture()
        todo = json.loads(self.call("native", "rebase-todo", root, "--onto", onto).stdout)
        path = self.store / "todo.json"
        raw = json.dumps(todo)
        for invalid in (raw[:-1] + ',"head":"' + sources[-1] + '"}',
                        raw.replace('"action": "pick"', '"action": "drop", "action": "pick"', 1),
                        raw[:-1] + ',"unknown":true}'):
            path.write_text(invalid, encoding="utf-8")
            before = self.snapshot()
            self.call("native", "rebase-plan", root, "--onto", onto, "--todo", path, ok=False)
            self.assertEqual(self.snapshot(), before)
        path.write_text(raw, encoding="utf-8")
        self.call("native", "rebase-plan", root, "--onto", onto, "--todo", path, "--interactive", ok=False)
        self.call("native", "rebase-todo", root, "--onto", onto, "--todo", path, ok=False)
        self.interactive(root, onto, b".\n", "--progress-json", ok=False)

    def test_cli_messages_preserve_backslashes_and_unicode(self):
        (self.root / "code.cpp").write_bytes(b"source")
        message = "Preserve C:\\Éditeur\\Map and \\n literally"
        revision = self.commit(message)
        metadata = json.loads(self.cmd("inspect", "revision", revision))
        self.assertEqual(metadata["message"], message)

    def test_reordered_fixup_matches_real_git_interactive_rebase(self):
        root, onto, (a, b, c, d) = self.fixture()
        oracle = self.store / "git-oracle"
        oracle.mkdir()
        env = dict(self.env, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0")

        def git(*args):
            result = subprocess.run(["git", "-C", str(oracle), *map(str, args)], env=env,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
            return result.stdout.decode().strip()

        git("init", "--initial-branch=main")
        git("config", "user.name", "Rebase oracle")
        git("config", "user.email", "test@example.invalid")
        git("config", "commit.gpgsign", "false")
        git("config", "core.autocrlf", "false")
        (oracle / "base").write_bytes(b"base")
        git("add", "base")
        git("commit", "-m", "base")
        git_root = git("rev-parse", "HEAD")
        git("switch", "-c", "upstream")
        (oracle / "upstream").write_bytes(b"upstream")
        git("add", "upstream")
        git("commit", "-m", "onto")
        git_onto = git("rev-parse", "HEAD")
        git("switch", "-c", "feature", git_root)
        sources = []
        for name in ("a", "b", "c", "d"):
            (oracle / name).write_bytes(name.encode())
            git("add", name)
            git("commit", "-m", name)
            sources.append(git("rev-parse", "HEAD"))
        sequence = f"pick {sources[1]}\nfixup {sources[0]}\npick {sources[2]}\ndrop {sources[3]}\n"
        editor = self.store / "sequence_editor.py"
        editor.write_text("import sys\nfrom pathlib import Path\nPath(sys.argv[1]).write_text(" +
                          repr(sequence) + ", encoding='utf-8')\n", encoding="utf-8")
        env["GIT_SEQUENCE_EDITOR"] = shlex.quote(Path(sys.executable).as_posix()) + " " + shlex.quote(editor.as_posix())
        env["GIT_EDITOR"] = "true"
        git("rebase", "--interactive", "--onto", git_onto, git_root)
        result = json.loads(self.interactive(root, onto,
            f"pick {b}\nfixup {a}\npick {c}\ndrop {d}\n.\n".encode()).stdout)
        self.assertEqual(result["result_commits"], 2)
        self.assertEqual(git("log", "--format=%s", "--reverse", git_onto + "..HEAD").splitlines(), ["b", "c"])
        metadata = json.loads(self.cmd("inspect", "revision", result["target"]))
        self.assertEqual(metadata["message"], "c")
        previous = json.loads(self.cmd("inspect", "revision", metadata["parents"][0]))
        self.assertEqual(previous["message"], "b")
        self.assertEqual(previous["parents"], [onto])
        for name in ("base", "a", "b", "c", "upstream"):
            self.assertEqual((self.root / name).read_bytes(), (oracle / name).read_bytes())
        self.assertFalse((self.root / "d").exists())
        self.assertFalse((oracle / "d").exists())


if __name__ == "__main__":
    unittest.main()
