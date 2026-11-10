"""Keep crash checkpoint coordinates independent of fixture path spelling."""
from collections import Counter
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from fixture_timing import case_directory

PROBE, READER = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(2)]
AUTHORSHIP = "--authorship" in sys.argv
if AUTHORSHIP:
    sys.argv.remove("--authorship")
ENV = {key: value for key, value in os.environ.items() if key.upper() not in
       ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_USER_NAME", "SYRNCK_USER_EMAIL",
        "SYRNCK_USER_TIMEZONE", "SYRNCK_SIGNING_SEED")}


def alternate_path(root):
    if os.name == "nt":
        function = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
        function.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
        function.restype = ctypes.c_uint32
        size = function(str(root), None, 0)
        if size:
            buffer = ctypes.create_unicode_buffer(size)
            length = function(str(root), buffer, size)
            if 0 < length < size and buffer.value != str(root.resolve()):
                return buffer.value, True
    # Also exercise a lexical alias on filesystems without Windows short names.
    return str(root) + "/../" + root.name, False


class CheckpointPaths(unittest.TestCase):
    def command(self, executable, mode, root, *arguments):
        result = subprocess.run([executable, mode, str(root), *arguments], env=ENV,
                                capture_output=True, text=True, encoding="utf-8", timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout) if result.stdout else None

    def test_coordinates_survive_physical_and_lexical_aliases(self):
        short_names = 0
        with tempfile.TemporaryDirectory(prefix="syrnck-checkpoint-paths-") as temporary:
            owner = Path(temporary)
            for encrypted in (False, True):
                for scope in (("primary", "linked", "partial", "detached") if AUTHORSHIP else ("primary", "partial")):
                    with self.subTest(encrypted=encrypted, scope=scope):
                        seed = owner / f"seed-{encrypted}-{scope}"
                        self.command(READER, "prepare", seed, "encrypted" if encrypted else "plain", scope, "commit" if AUTHORSHIP else "ordinary")
                        observed = []
                        for spelling in ("canonical", "alias"):
                            with case_directory(owner, f"case-{encrypted}-{scope}") as root:
                                shutil.copytree(seed, root)
                                argument = str(root.resolve())
                                if spelling == "alias":
                                    argument, short = alternate_path(root)
                                    short_names += short
                                if AUTHORSHIP:
                                    self.command(READER, "bind", argument)
                                events = self.command(PROBE, "trace", argument)
                                kinds = Counter(row["kind"] for row in events)
                                if AUTHORSHIP:
                                    self.assertTrue({"chunk", "manifest", "revision", "refs"} <= set(kinds), kinds)
                                    self.assertTrue(set(kinds) <= {"chunk", "manifest", "revision", "refs", "config", "filesystem"}, kinds)
                                else:
                                    self.assertEqual(set(kinds), {"chunk", "manifest", "revision", "refs", "filesystem"})
                                self.assertEqual(len({(row["phase"], row["kind"], row["occurrence"]) for row in events}), len(events))
                                observed.append(events)
                        self.assertEqual(observed[0], observed[1], "path spelling changed termination coordinates")
        print(json.dumps({"scenarios": 8 if AUTHORSHIP else 4, "windows_short_aliases": short_names,
                          "checkpoint_coordinates_identical": True}), flush=True)


if __name__ == "__main__":
    unittest.main()
