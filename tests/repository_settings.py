"""Persistent CLI preferences and explicit per-command overrides."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())


class RepositorySettings(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="syrnck-settings-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_LANG", "SYRNCK_TOKEN", "SYRNCK_SKIP_SMUDGE")}
        self.run_cli("init")

    def run_cli(self, *args, ok=True, lang="en", data=None):
        result = subprocess.run([CLI, "--lang", lang, *map(str, args)], cwd=self.root,
                                env=self.env, input=data, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        return result

    def test_defaults_persist_overrides_and_pack_limits(self):
        defaults = json.loads(self.run_cli("config", "show").stdout)
        settings = self.root / ".syrnck/settings.json"
        config = self.root / ".syrnck/config.json"
        self.assertFalse(settings.exists())
        physical = config.read_bytes()
        saved = json.loads(self.run_cli("config", "set", "--workers", 4, "--buffer-mib", 4,
                                        "--pack-max-objects", 2, "--pack-max-mib", 18).stdout)
        self.assertEqual(saved["ingest"], {"workers": 4, "buffer_bytes": 4194304})
        self.assertEqual(json.loads(self.run_cli("config", "show", lang="fr").stdout), saved)
        self.assertEqual(config.read_bytes(), physical)
        source = self.root / "asset.bin"
        source.write_bytes(b"".join(bytes([i]) * 1024 for i in range(10)))
        parallel = json.loads(self.run_cli("put", source, "--fixed-size", 1024).stdout)
        self.assertGreater(parallel["stats"]["peak_in_flight_blocks"], 0)
        serial = json.loads(self.run_cli("put", source, "--fixed-size", 1024, "--workers", 1).stdout)
        self.assertEqual(serial["manifest"], parallel["manifest"])
        self.assertEqual(serial["stats"]["peak_in_flight_blocks"], 0)
        self.assertEqual(json.loads(settings.read_text()), saved)
        packed = json.loads(self.run_cli("pack").stdout)
        self.assertEqual(packed["pack_count"], 6)
        self.assertEqual(json.loads(self.run_cli("pack", "--compact", "--max-objects", 64).stdout)["pack_count"], 1)
        self.assertEqual(json.loads(settings.read_text()), saved)
        self.assertEqual(json.loads(self.run_cli("config", "reset").stdout), defaults)

    def test_invalid_updates_and_recovery_from_malformed_settings(self):
        self.run_cli("config", "set", "--workers", 4)
        settings = self.root / ".syrnck/settings.json"
        original = settings.read_bytes()
        for args in (("--workers", 0), ("--workers", 65), ("--buffer-mib", 0),
                     ("--pack-max-mib", 17), ("--pack-max-objects", 0), (),
                     ("--workers", 3, "--buffer-mib", 1025), ("--token", "unaccepted")):
            self.run_cli("config", "set", *args, ok=False)
            self.assertEqual(settings.read_bytes(), original)
        settings.write_text('{"version":1,"secret":"rejected"}', encoding="utf-8")
        error = self.run_cli("config", "show", lang="fr", ok=False).stderr.decode()
        self.assertIn("réglages du dépôt invalides", error)
        self.run_cli("fsck")
        self.run_cli("config", "reset")
        self.assertEqual(json.loads(self.run_cli("config", "show").stdout)["ingest"]["workers"], 1)

    def test_native_and_installed_git_filters_use_repo_defaults_and_overrides(self):
        data = b"configured pipeline\x00" * 4096
        source = self.root / "asset.uasset"
        source.write_bytes(data)
        self.run_cli("config", "set", "--workers", 4, "--buffer-mib", 1)
        self.run_cli("native", "commit", "-m", "must fail before publication", ok=False)
        self.assertEqual(json.loads(self.run_cli("refs").stdout), {})
        self.run_cli("native", "commit", "-m", "explicit override", "--buffer-mib", 4, "--workers", 4)
        self.assertEqual(json.loads(self.run_cli("config", "show").stdout)["ingest"]["buffer_bytes"], 1048576)

        def git(*args, ok=True):
            result = subprocess.run(["git", *args], cwd=self.root, env=self.env, capture_output=True, timeout=30)
            self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
            return result.stdout

        git("init", "-q")
        self.run_cli("git", "install")
        common = self.root / ".git/syrnck"
        (self.root / ".gitattributes").write_text("*.uasset filter=syrnck -text\n", encoding="utf-8")
        self.run_cli("--store", common, "config", "set", "--workers", 4, "--buffer-mib", 1)
        git("add", "asset.uasset", ok=False)
        pointer = self.run_cli("--store", common, "git", "clean", "--workers", 1,
                               "--buffer-mib", 4, data=data).stdout
        self.assertTrue(pointer.startswith(b"version syrnck-pointer-v1\n"))
        self.run_cli("--store", common, "config", "set", "--buffer-mib", 4)
        git("add", "asset.uasset")
        self.assertEqual(git("show", ":asset.uasset"), pointer)
        source.unlink()
        git("checkout", "--", "asset.uasset")
        self.assertEqual(source.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
