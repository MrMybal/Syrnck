"""Actual CLI repair of loose/packed corruption with independent donor keys."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from fixture_timing import remove_owned_case

CLI = str(Path(sys.argv.pop(1)).resolve())


class StorageRepair(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="syrnck-storage-repair-cli-"))
        self.addCleanup(remove_owned_case, self.root.parent, self.root.name)

    def call(self, work, store, env, *args, ok=True, lang="en"):
        result = subprocess.run([CLI, "--lang", lang, "--store", str(store), *map(str, args)], cwd=work,
                                env=env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        for name in ("SYRNCK_KEY", "SYRNCK_BACKUP_KEY"):
            if env.get(name): self.assertNotIn(env[name].encode(), result.stdout + result.stderr)
        return result

    def test_encrypted_and_plain_repair_retains_roots_and_bytes(self):
        for encrypted in (False, True):
            for layout in range(3):
                with self.subTest(encrypted=encrypted, layout=layout):
                    work = self.root / f"source-{encrypted}-{layout}"
                    work.mkdir()
                    store = work / ".syrnck"
                    backup = self.root / f"réplique-{encrypted}-{layout}"
                    env = {name: value for name, value in os.environ.items() if not name.upper().startswith("SYRNCK_")}
                    env["SYRNCK_BACKUP_KEY"] = "77" * 32
                    if encrypted: env["SYRNCK_KEY"] = "39" * 32
                    def cli(*args, **kwargs): return self.call(work, store, env, *args, **kwargs)
                    cli("init", *(["--encrypted"] if encrypted else []))
                    payload = bytes(range(251)) * 64
                    asset = work / "asset.bin"; asset.write_bytes(payload)
                    manifest = json.loads(cli("put", asset, "--fixed-size", "1024").stdout)["manifest"]
                    metadata = json.loads(cli("inspect", "manifest", manifest).stdout)
                    chunk = metadata["blocks"][0]["id"]
                    cli("pin", "fixture/asset", "manifest", manifest)
                    cli("native", "commit", "-m", "retain asset")
                    cli("backup", "create", backup)
                    if layout: cli("pack", *(["--max-objects", "3"] if layout == 2 else []))
                    refs = (store / "refs.json").read_bytes()
                    config = json.loads((store / "config.json").read_text(encoding="utf-8"))
                    if not layout:
                        path = store / "objects/chunk" / chunk[:2] / chunk
                        path.write_bytes(b"damaged envelope")
                    else:
                        found = False
                        for pack in config.get("packs", [config.get("pack")]):
                            index = (store / "packs" / ("pack-" + pack["name"] + ".syi")).read_bytes()
                            for ordinal in range(pack["objects"]):
                                offset = 16 + ordinal * 45
                                if index[offset + 1:offset + 33].hex() == chunk:
                                    position, _ = struct.unpack_from("<QI", index, offset + 33)
                                    path = store / "packs" / ("pack-" + pack["name"] + ".syp")
                                    damaged = bytearray(path.read_bytes()); damaged[position] ^= 127; path.write_bytes(damaged)
                                    found = True; break
                            if found: break
                        self.assertTrue(found)
                    cli("backup", "repair-storage", backup, ok=False)
                    result = cli("backup", "repair-storage", backup, "--expect-plan", "0" * 64, ok=False, lang="fr")
                    self.assertIn("plan de réparation", result.stderr.decode("utf-8"))
                    plan = json.loads(cli("backup", "repair-storage-plan", backup).stdout)
                    self.assertEqual(plan["corrupt_count"], 1)
                    self.assertEqual(plan["packs_to_rewrite"], bool(layout))
                    repaired = json.loads(cli("backup", "repair-storage", backup, "--expect-plan", plan["plan"]).stdout)
                    self.assertTrue(repaired["applied"])
                    self.assertEqual((store / "refs.json").read_bytes(), refs)
                    cli("fsck")
                    restored = self.root / f"restored-{encrypted}-{layout}"
                    cli("get", manifest, restored)
                    self.assertEqual(restored.read_bytes(), payload)


if __name__ == "__main__": unittest.main()
