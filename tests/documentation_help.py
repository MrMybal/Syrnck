"""Exercise installed contextual-help routing without a repository or browser."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import unquote, urlsplit

EXE = Path(sys.argv.pop(1)).resolve()
DOCS = EXE.parent / "Documentation"


class DocumentationHelp(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="syrnck-help-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_cli(self, *args, exe=EXE, ok=True):
        env = os.environ.copy()
        env.update(SYRNCK_LANG="en", SYRNCK_KEY="invalid-unused-key")
        result = subprocess.run([str(exe), *args], cwd=self.root, env=env,
                                capture_output=True, encoding="utf-8", timeout=20)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
        self.assertFalse((self.root / ".syrnck").exists())
        return result

    def test_default_context_languages_and_legacy_links(self):
        self.assertTrue(self.run_cli("help", "--print-url").stdout.strip().endswith("#/en/overview"))
        self.assertTrue(self.run_cli("--lang", "fr", "help", "native", "tags", "--print-url").stdout.strip().endswith("#/fr/native-tags"))
        self.assertTrue(self.run_cli("help", "native", "switch", "--detach", "--print-url").stdout.strip().endswith("#/en/native-detached"))
        self.assertTrue(self.run_cli("help", "agent", "run", "--print-url").stdout.strip().endswith("#/en/resident-agent"))
        self.assertTrue(self.run_cli("--lang", "fr", "help", "agent", "run", "--section", "recovery", "--print-url").stdout.strip().endswith("#/fr/resident-agent/recovery"))
        self.assertTrue(self.run_cli("--lang", "fr", "help", "native", "switch-plan", "--detach", "--print-url").stdout.strip().endswith("#/fr/native-detached/recovery"))
        for command in ("workspaces", "workspace-add"):
            self.assertTrue(self.run_cli("help", "native", command, "--print-url").stdout.strip().endswith("#/en/native-workspaces"))
        self.assertTrue(self.run_cli("--lang", "fr", "help", "native", "workspace-restore", "--print-url").stdout.strip().endswith("#/fr/native-workspaces/restore"))
        for language in ("en", "fr"):
            self.assertTrue(self.run_cli("--lang", language, "help", "server-signatures", "--print-url")
                            .stdout.strip().endswith(f"#/{language}/server-signatures"))
            self.assertTrue(self.run_cli("--lang", language, "help", "agent", "push", "--section", "expected-publication", "--print-url")
                            .stdout.strip().endswith(f"#/{language}/transfers/expected-publication"))
            for command, section in (("identity", "identity"), ("log", "rewrites")):
                self.assertTrue(self.run_cli("--lang", language, "help", "native", command, "--print-url")
                                .stdout.strip().endswith(f"#/{language}/native-authorship/{section}"))
            self.assertTrue(self.run_cli("--lang", language, "help", "signing", "--print-url")
                            .stdout.strip().endswith(f"#/{language}/signing-api"))
            self.assertTrue(self.run_cli("--lang", language, "help", "native", "materialization", "--print-url")
                            .stdout.strip().endswith(f"#/{language}/native-workspaces/partial"))
            for command, section in (("workspace-unregister-plan", "unregister"), ("workspace-unregister", "unregister"),
                                     ("workspace-archives", "contents"), ("workspace-archive", "contents"),
                                     ("workspace-archive-restore-plan", "restore"), ("workspace-archive-restore", "restore")):
                self.assertTrue(self.run_cli("--lang", language, "help", "native", command, "--print-url")
                                .stdout.strip().endswith(f"#/{language}/native-workspace-archives/{section}"))
            for command, section in (("workspace-relocate-plan", "usage"), ("workspace-relocate", "usage"),
                                     ("workspace-relocation-status", "recovery"), ("workspace-relocation-recover", "recovery")):
                self.assertTrue(self.run_cli("--lang", language, "help", "native", command, "--print-url")
                                .stdout.strip().endswith(f"#/{language}/native-workspace-relocation/{section}"))
        for command in ("required-features", "require-feature"):
            self.assertTrue(self.run_cli("help", "config", command, "--print-url").stdout.strip().endswith("#/en/settings/reader-features"))
        for topic in ["docs/PACKS.md#mémoire-du-gc", "docs\\PACKS.md#m%C3%A9moire-du-gc"]:
            self.assertTrue(self.run_cli("help", topic, "--print-url").stdout.strip().endswith("#/en/packs/memoire-du-gc"))
        self.assertTrue(self.run_cli("help", "native-tags", "--section", "native-revision-selectors", "--print-url").stdout.strip().endswith("/native-revision-selectors"))

    def test_relocated_installation_and_file_url_escaping(self):
        install = self.root / "Équipe 漢字😀 #100%" / "bin"
        install.mkdir(parents=True)
        exe = install / EXE.name
        shutil.copy2(EXE, exe)
        shutil.copytree(DOCS, install / "Documentation")
        result = self.run_cli("help", "--print-url", exe=exe).stdout.strip()
        url = urlsplit(result)
        self.assertEqual(url.scheme, "file")
        self.assertIn("%23", result)
        self.assertIn("%25", result)
        path = unquote(url.path)
        if os.name == "nt" and path.startswith("/"):
            path = path[1:]
        self.assertEqual(Path(path).resolve(), (install / "Documentation/Documentation.html").resolve())
        override = self.run_cli("help", "--directory", str(install / "Documentation"), "--print-url").stdout.strip()
        self.assertEqual(result, override)

    def test_bad_topic_section_missing_install_and_catalog(self):
        for args in [("unknown-topic",), ("native-tags", "--section", "missing"),
                     ("native-tags#native-revision-selectors", "--section", "native-revision-selectors"),
                     ("bad%escape",)]:
            self.run_cli("help", *args, "--print-url", ok=False)
        self.run_cli("help", "--directory", str(self.root / "missing"), "--print-url", ok=False)
        broken = self.root / "broken"
        (broken / "DocData").mkdir(parents=True)
        (broken / "Documentation.html").write_text("<!doctype html>", encoding="utf-8")
        (broken / "DocData/manifest.json").write_text(json.dumps({"schemaVersion": 99, "pages": []}), encoding="utf-8")
        (broken / "DocData/help-topics.json").write_text("{}", encoding="utf-8")
        self.run_cli("help", "--directory", str(broken), "--print-url", ok=False)


if __name__ == "__main__":
    unittest.main()
