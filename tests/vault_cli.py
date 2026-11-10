"""CLI vault contracts; real account access requires the disposable OS fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from fixture_timing import remove_owned_case

CLI = str(Path(sys.argv.pop(1)).resolve())
PASSWORD = "  Vault backup 🔐 密钥  "
PURPOSES = ("storage", "signing", "sealed")


def environment():
    return {key: value for key, value in os.environ.items() if not key.upper().startswith("SYRNCK_")}


def call(root, *args, env=None, ok=True):
    result = subprocess.run([CLI, *map(str, args)], cwd=root, env=env or environment(),
                            capture_output=True, timeout=30)
    if (result.returncode == 0) != ok:
        raise AssertionError(result.stderr.decode("utf-8", "replace"))
    if PASSWORD.encode() in result.stdout + result.stderr:
        raise AssertionError("backup password appeared in command output")
    return result


class VaultCLI(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="syrnck-vault-cli-negative-"))
        self.addCleanup(remove_owned_case, self.root.parent, self.root.name)
        self.provider = json.loads(call(self.root, "vault", "provider").stdout)["provider"]

    def receipt(self):
        return {"version": 1, "type": "system-vault-reference", "provider": self.provider,
                "reference": "syrnck-vault:1:storage:" + "0" * 64}

    def test_help_and_arguments(self):
        for language in ("en", "fr"):
            result = call(self.root, "--lang", language, "--help")
            self.assertIn(b"vault create", result.stdout)
            self.assertIn(b"SYRNCK_KEY_VAULT", result.stdout)
        for args in (("provider", "extra"), ("create",), ("create", "wrong", "output"),
                     ("import", "storage"), ("erase",), ("unknown",),
                     ("inspect", "missing", "--keychain", "relative")):
            call(self.root, "vault", *args, ok=False)
        result = call(self.root, "--lang", "fr", "vault", "create", "wrong", "output", ok=False)
        self.assertIn("l’usage".encode(), result.stderr)
        self.assertFalse((self.root / "output").exists())

    def test_reference_output_refuses_replacement_before_account_access(self):
        output = self.root / "owned.json"
        output.write_bytes(b"preserved")
        call(self.root, "vault", "create", "storage", output, ok=False)
        self.assertEqual(output.read_bytes(), b"preserved")
        call(self.root, "vault", "create", "storage", "missing/output", ok=False)
        self.assertFalse((self.root / "missing").exists())

    def test_receipt_schema_duplicate_and_bounds(self):
        path = self.root / "reference.json"
        original = self.receipt()
        invalid = [[], {}, dict(original, version=1.5), dict(original, version=2**64),
                   dict(original, type=1), dict(original, provider="other-provider"),
                   dict(original, reference=original["reference"] + "\n"),
                   dict(original, keychain=1), dict(original, unexpected=True)]
        for value in invalid:
            path.write_text(json.dumps(value), encoding="utf-8")
            call(self.root, "vault", "inspect", path, ok=False)
        for data in ("{" + json.dumps(original)[1:-1] + ',"version":1}', "x" * 16385):
            path.write_text(data, encoding="utf-8")
            call(self.root, "vault", "erase", path, ok=False)
        self.assertTrue(path.exists())

    def test_export_validates_before_account_access(self):
        path = self.root / "reference.json"
        path.write_text(json.dumps(self.receipt()), encoding="utf-8")
        for options in ((), ("--password-env", "lowercase"), ("--memory-mib", "63"), ("--passes", "11")):
            call(self.root, "vault", "export", path, "new.envelope", *options, ok=False)
        self.assertFalse((self.root / "new.envelope").exists())

    def test_storage_environment_exclusivity_and_role_before_store_creation(self):
        for other in ("SYRNCK_KEY", "SYRNCK_KEY_FILE"):
            selected = environment()
            selected["SYRNCK_KEY_VAULT"] = self.receipt()["reference"]
            selected[other] = "conflicting"
            result = call(self.root, "init", "--encrypted", env=selected, ok=False)
            self.assertIn(b"choose only one", result.stderr)
        for reference in ("", "syrnck-vault:1:signing:" + "0" * 64, self.receipt()["reference"] + "\n"):
            selected = environment()
            selected["SYRNCK_KEY_VAULT"] = reference
            call(self.root, "init", "--encrypted", env=selected, ok=False)
        self.assertFalse((self.root / ".syrnck").exists())


def real_phase(mode, root, keychain):
    """Only called inside key_vault_system.py's owned OS-account fixture."""
    env = environment()
    env["VAULT_TEST_BACKUP_PASSWORD"] = PASSWORD
    if keychain:
        env["SYRNCK_VAULT_KEYCHAIN"] = keychain
    paths = {purpose: root / (purpose + ".vault.json") for purpose in PURPOSES}
    def cli(*args, **kwargs):
        return call(root, *args, env=kwargs.pop("env", env), **kwargs)
    def inspect(path):
        return json.loads(cli("vault", "inspect", path).stdout)
    if mode == "create":
        # Journal ownership before the first OS write so finally-cleanup works
        # even if a later creation/assertion fails. All selectors are public.
        (root / "state.json").write_text("{}", encoding="utf-8")
        for purpose, path in paths.items():
            cli("vault", "create", purpose, path, *(["--keychain", keychain] if keychain else []))
            receipt = json.loads(path.read_text(encoding="utf-8"))
            assert receipt["reference"].startswith("syrnck-vault:1:" + purpose + ":")
        return
    if mode == "cleanup":
        for path in root.glob("*.vault.json"):
            cli("vault", "erase", path)
        return
    if mode == "inaccessible":
        for path in paths.values():
            cli("vault", "inspect", path, ok=False)
        return
    if mode == "erase":
        for path in paths.values():
            assert json.loads(cli("vault", "erase", path).stdout)["removed"]
            assert not json.loads(cli("vault", "erase", path).stdout)["removed"]
            assert path.exists()  # Erasing an entry never silently deletes its receipt.
            cli("vault", "inspect", path, ok=False)
        return
    if mode != "verify":
        raise ValueError("unexpected CLI vault fixture phase")
    identities = {}
    for purpose, path in paths.items():
        identities[purpose] = inspect(path)
        assert identities[purpose]["authenticated"]
        original = path.read_bytes()
        cli("vault", "create", purpose, path, ok=False)
        assert path.read_bytes() == original
        backup = root / (purpose + ".envelope")
        cli("vault", "export", path, backup, "--password-env", "VAULT_TEST_BACKUP_PASSWORD")
        assert backup.stat().st_size == 104
        verb = {"storage": "key", "signing": "signing-key", "sealed": "sealed-key"}[purpose]
        cli(verb, "verify", backup, "--password-env", "VAULT_TEST_BACKUP_PASSWORD")
        imported = root / (purpose + "-imported.vault.json")
        cli("vault", "import", purpose, backup, imported, "--password-env", "VAULT_TEST_BACKUP_PASSWORD",
            *(["--keychain", keychain] if keychain else []))
        restored = inspect(imported)
        assert restored["reference"] != identities[purpose]["reference"]
        for field in ("public_key", "key_id", "realm"):
            assert restored.get(field) == identities[purpose].get(field)
        wrong = dict(env, VAULT_TEST_BACKUP_PASSWORD="wrong backup password")
        cli("vault", "import", purpose, backup, root / "failed.vault.json", "--password-env",
            "VAULT_TEST_BACKUP_PASSWORD", env=wrong, ok=False)
        other = PURPOSES[(PURPOSES.index(purpose) + 1) % 3]
        cli("vault", "import", other, backup, root / "failed.vault.json", "--password-env",
            "VAULT_TEST_BACKUP_PASSWORD", ok=False)
        assert not (root / "failed.vault.json").exists()
    env["SYRNCK_KEY_VAULT"] = identities["storage"]["reference"]
    work = root / "work"
    work.mkdir()
    store = work / ".syrnck"
    cli("--store", store, "init", "--encrypted")
    contents = b"native CLI vault-backed encrypted asset\n" * 4096
    asset = work / "asset.bin"
    asset.write_bytes(contents)
    manifest = json.loads(cli("--store", store, "put", asset).stdout)["manifest"]
    cli("--store", store, "get", manifest, root / "restored.bin")
    assert (root / "restored.bin").read_bytes() == contents
    env.update(SYRNCK_USER_NAME="Vault test author", SYRNCK_USER_EMAIL="vault@example.invalid",
               SYRNCK_SIGNING_KEY_VAULT=identities["signing"]["reference"])
    revision = call(work, "--store", store, "native", "commit", "-m", "Vault-backed signed commit", env=env).stdout.decode().strip()
    metadata = json.loads(cli("--store", store, "inspect", "revision", revision).stdout)
    assert metadata["signature"]["key_id"] == identities["signing"]["key_id"]
    env["SYRNCK_SEALED_KEY_VAULT"] = identities["sealed"]["reference"]
    wire = root / "wire"
    prepared = json.loads(cli("--store", store, "sealed", "snapshot", "heads/main", "--wire", wire).stdout)
    destination = root / "restored-store"
    cli("--store", destination, "init", "--encrypted")
    cli("--store", destination, "sealed", "open", prepared["snapshot"], "--wire", wire,
        "--checkpoint", "sealed/checkpoints/vault-cli")
    cli("--store", destination, "native", "export", revision, root / "sealed-restored")
    assert (root / "sealed-restored" / "asset.bin").read_bytes() == contents
    print("CLI vault references, protected recovery, encrypted Store, signed revision and sealed restoration passed")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--system":
        real_phase(sys.argv[2], Path(sys.argv[3]).resolve(strict=True), sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        unittest.main()
