"""Check language boundaries, canonical output, and owned diagnostic coverage."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parent.parent
STRING = r'"(?:\\.|[^"\\])*"'


def diagnostic_keys(source):
    # Tokenize strings/comments before parentheses so punctuation inside a
    # diagnostic or comment cannot affect argument boundaries.
    tokens = re.findall(STRING + r'|//[^\n]*|/\*[\s\S]*?\*/|[A-Za-z_]\w*|[^\s]', source)
    tokens = [t for t in tokens if not t.startswith(("//", "/*"))]
    for i, token in enumerate(tokens):
        if token == "require" and tokens[i + 1:i + 2] == ["("]:
            start, wanted = i + 2, 1
        elif token == "throw" and tokens[i + 1:i + 2] in (["Error"], ["MissingObject"], ["Cancelled"]):
            start, wanted = i + 3, 0
        else:
            continue
        depth, argument, literals = 1, 0, []
        for t in tokens[start:]:
            if t in ("(", "[", "{"):
                depth += 1
            elif t in (")", "]", "}"):
                depth -= 1
                if not depth:
                    break
            elif t == "," and depth == 1:
                argument += 1
            elif argument == wanted and t.startswith('"'):
                literals.append(json.loads(t))
        if literals:
            yield "".join(literals)


class Localization(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="syrnck-i18n-")
        self.root = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *args, env=None, data=None, ok=True):
        result = subprocess.run([CLI, *map(str, args)], cwd=self.root, env=env or self.env,
                                input=data, capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", errors="replace"))
        return result

    def test_default_and_selection(self):
        env = dict(self.env, LANG="fr_FR.UTF-8", LC_ALL="fr_FR.UTF-8")
        self.assertIn(b"Create a new block store", self.run_cli("--help", env=env).stdout)
        self.assertIn("Créer un stockage", self.run_cli("--lang", "fr", "--help").stdout.decode())
        env["SYRNCK_LANG"] = "fr_FR"
        self.assertIn("Créer un stockage", self.run_cli("--help", env=env).stdout.decode())
        self.assertIn(b"Create a new block store", self.run_cli("--lang", "en", "--help", env=env).stdout)
        env["SYRNCK_LANG"] = "invalid"
        self.run_cli("--help", env=env, ok=False)
        self.run_cli("--lang", "fr", "--help", env=env)
        self.run_cli("--lang", "de", "--help", ok=False)
        self.run_cli("--lang", "fr", "--lang", "en", "--help", ok=False)
        self.run_cli("--lang", ok=False)

    def test_messages_and_data_boundaries(self):
        store = self.root / "données"
        initialized = self.run_cli("--lang", "fr", "--store", store, "init").stdout.decode()
        self.assertIn("Initialisé", initialized)
        emitted_path = initialized.split(" : ", 1)[1].rsplit(" (", 1)[0]
        self.assertTrue(Path(emitted_path).samefile(store))
        source = self.root / "source.bin"
        data = bytes(range(256)) * 100
        source.write_bytes(data)
        result = self.run_cli("--lang", "fr", "--store", store, "put", source)
        first = json.loads(result.stdout)
        self.assertIn("manifest", first)
        self.assertIn("file_bytes", first["stats"])
        second = json.loads(self.run_cli("--lang", "en", "--store", store, "put", source).stdout)
        self.assertEqual(first["manifest"], second["manifest"])
        self.run_cli("--lang", "fr", "--store", store, "get", first["manifest"], "restored.bin")
        self.assertEqual((self.root / "restored.bin").read_bytes(), data)
        self.assertIn("Vérification réussie", self.run_cli("--lang", "fr", "--store", store, "fsck").stdout.decode())
        missing_id = "a" * 64
        error = self.run_cli("--lang", "fr", "--store", store, "get", missing_id, "missing", ok=False).stderr.decode()
        self.assertIn("Erreur : objet introuvable", error)
        self.assertIn(missing_id, error)
        error = self.run_cli("--lang", "fr", "--store", store, "nonsense", ok=False).stderr.decode()
        self.assertIn("commande inconnue", error)
        self.assertIn("--help", error)

    def test_filters_remain_binary_and_language_independent(self):
        self.run_cli("init")
        data = bytes(range(256)) * 40
        english = self.run_cli("--lang", "en", "git", "clean", data=data).stdout
        french = self.run_cli("--lang", "fr", "git", "clean", data=data).stdout
        self.assertEqual(english, french)
        restored = self.run_cli("--lang", "fr", "git", "smudge", data=french).stdout
        self.assertEqual(restored, data)

    def test_owned_diagnostics_have_french_translations(self):
        catalog = (ROOT / "src/messages.inc").read_text(encoding="utf-8")
        keys = {json.loads(match) for match in re.findall(r'SYRNCK_(?:MESSAGE|PREFIX)\((' + STRING + ')', catalog)}
        missing = set()
        for path in (ROOT / "src").iterdir():
            if path.suffix in (".cpp", ".hpp"):
                missing.update(set(diagnostic_keys(path.read_text(encoding="utf-8"))) - keys)
        self.assertEqual(sorted(missing), [], "Add translations for these owned English diagnostics")


if __name__ == "__main__":
    unittest.main()
