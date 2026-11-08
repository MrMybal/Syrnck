"""Required-reader CLI, malformed requirements, and optional real legacy refusal."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CLI = Path(sys.argv.pop(1)).resolve()
LEGACY = None
if "--legacy-cli" in sys.argv:
    position = sys.argv.index("--legacy-cli")
    sys.argv.pop(position)
    LEGACY = Path(sys.argv.pop(position)).resolve()
FEATURE = "linked-workspaces-v1"


class StoreFeatures(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="syrnck-reader-features-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.store = self.root / "store"
        self.env = {k: v for k, v in os.environ.items() if k.upper() not in
                    ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG", "SYRNCK_SKIP_SMUDGE")}
        self.call("init")

    def call(self, *arguments, expected=0, executable=CLI, store=None):
        result = subprocess.run([str(executable), "--store", str(store or self.store), *map(str, arguments)],
                                cwd=self.root, env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result

    def snapshot(self, store=None):
        return {p.relative_to(store or self.store).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (store or self.store).rglob("*") if p.is_file() and p.name != "store.lock"}

    def test_requirements_are_explicit_idempotent_and_preserved_in_backup(self):
        self.assertEqual(json.loads(self.call("config", "required-features").stdout), {"required_features": []})
        before = self.snapshot()
        error = self.call("--lang", "fr", "config", "require-feature", "future-feature", expected=1)
        self.assertIn("non prise en charge", error.stderr.decode("utf-8"))
        self.assertEqual(before, self.snapshot())
        self.call("config", "require-feature", FEATURE)
        enabled = self.snapshot()
        result = self.call("config", "require-feature", FEATURE, "--progress-json")
        self.assertEqual(json.loads(result.stdout), {"required_features": [FEATURE]})
        self.assertEqual(enabled, self.snapshot())
        backup, restored = self.root / "backup", self.root / "restored"
        self.call("backup", "create", backup)
        self.call("backup", "verify", store=backup)
        self.call("backup", "restore", restored, store=backup)
        self.assertEqual(json.loads(self.call("config", "required-features", store=restored).stdout),
                         {"required_features": [FEATURE]})
        marker = json.loads((backup / "backup.json").read_text(encoding="utf-8"))
        self.assertEqual((marker["version"], marker["required_features"]), (3, [FEATURE]))
        marker["version"] = 2
        del marker["required_features"]
        (backup / "backup.json").write_text(json.dumps(marker, separators=(",", ":")), encoding="utf-8")
        self.call("backup", "verify", store=backup, expected=1)

    def test_unknown_or_malformed_requirements_refuse_open_without_mutation(self):
        configuration = self.store / "config.json"
        original = json.loads(configuration.read_text(encoding="utf-8"))
        for invalid in ([], [FEATURE, FEATURE], ["unknown-future-feature"], [False], FEATURE, None):
            with self.subTest(requirements=invalid):
                configuration.write_text(json.dumps(dict(original, required_features=invalid)), encoding="utf-8")
                before = self.snapshot()
                self.call("refs", expected=1)
                self.assertEqual(self.snapshot(), before)

    @unittest.skipUnless(LEGACY, "Pass --legacy-cli for an independent historical executable")
    def test_real_legacy_reader_accepts_baseline_and_refuses_each_gated_physical_format(self):
        source = self.root / "asset"
        source.write_bytes(b"complete retained bytes")
        for layout in (1, 2, 3):
            with self.subTest(layout=layout):
                store = self.root / f"legacy-{layout}"
                self.call("init", store=store)
                self.call("put", source, store=store)
                if layout > 1:
                    self.call("pack", *(["--max-objects", 1] if layout == 3 else []), store=store)
                self.assertEqual(json.loads((store / "config.json").read_text(encoding="utf-8"))["version"], layout)
                self.call("refs", executable=LEGACY, store=store)
                self.call("config", "require-feature", FEATURE, store=store)
                before = self.snapshot(store)
                result = self.call("refs", executable=LEGACY, store=store, expected=1)
                self.assertIn(b"unsupported store configuration", result.stderr)
                self.assertEqual(before, self.snapshot(store))
                self.call("fsck", store=store)


if __name__ == "__main__":
    unittest.main()
