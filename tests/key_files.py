"""Real CLI protected-key files, Unicode credentials and non-destructive refusal."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())

class KeyFiles(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="syrnck-key-files-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {key:value for key,value in os.environ.items() if not key.upper().startswith("SYRNCK_")}
        self.password = "  Clé locale 🔐 密钥  "
        self.env["SYRNCK_KEY_PASSWORD"] = self.password

    def cli(self, *args, ok=True, lang="en"):
        result = subprocess.run([CLI,"--lang",lang,*map(str,args)], cwd=self.root,
                                env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8","replace"))
        self.assertNotIn(self.password.encode(), result.stdout + result.stderr)
        for name in ("SYRNCK_KEY_PASSWORD", "SYRNCK_NEW_KEY_PASSWORD", "OTHER_PASSWORD"):
            if self.env.get(name): self.assertNotIn(self.env[name].encode(), result.stdout + result.stderr)
        self.assertNotIn(b"ab" * 32, result.stdout + result.stderr)
        return result

    def test_empty_key_environment_is_rejected_with_the_specific_diagnostic(self):
        self.env["SYRNCK_KEY"] = ""
        result = self.cli("init", "--encrypted", ok=False)
        self.assertIn(b"SYRNCK_KEY must encode exactly 32 bytes", result.stderr)
        self.assertFalse((self.root / ".syrnck").exists())
        self.env.pop("SYRNCK_KEY")
        self.env["SYRNCK_KEY_FILE"] = ""
        result = self.cli("init", "--encrypted", ok=False)
        self.assertIn(b"SYRNCK_KEY_FILE must name a protected key file", result.stderr)
        self.assertFalse((self.root / ".syrnck").exists())

    def test_create_unlock_rewrap_preserves_store_and_old_envelope(self):
        key_file = self.root / "clé protégée.bin"
        result=json.loads(self.cli("key","create",key_file,"--memory-mib","65","--passes","4").stdout)
        self.assertEqual((result["bytes"],result["memory_kib"],result["passes"]),(104,65*1024,4))
        original=key_file.read_bytes()
        inspect=json.loads(self.cli("key","inspect",key_file).stdout)
        self.assertFalse(inspect["authenticated"])
        self.assertTrue(json.loads(self.cli("key","verify",key_file).stdout)["authenticated"])
        if os.name != "nt": self.assertEqual(key_file.stat().st_mode & 0o777, 0o600)
        self.env["SYRNCK_KEY_FILE"]=str(key_file)
        self.cli("init","--encrypted")
        payload=b"all retained bytes\x00"*4096
        (self.root/"asset.bin").write_bytes(payload)
        manifest=json.loads(self.cli("put","asset.bin").stdout)["manifest"]
        before={p.relative_to(self.root/".syrnck").as_posix():p.read_bytes() for p in (self.root/".syrnck").rglob("*") if p.is_file() and p.name!="store.lock"}
        next_file=self.root/"new-key.bin"
        self.env["SYRNCK_NEW_KEY_PASSWORD"]="nouveau secret"
        self.cli("key","rewrap",key_file,next_file,"--passes","5")
        after={p.relative_to(self.root/".syrnck").as_posix():p.read_bytes() for p in (self.root/".syrnck").rglob("*") if p.is_file() and p.name!="store.lock"}
        self.assertEqual(before,after)
        self.assertEqual(key_file.read_bytes(),original)
        changed=json.loads(self.cli("key","inspect",next_file).stdout)
        self.assertEqual((changed["memory_kib"],changed["passes"]),(65*1024,5))
        self.env["SYRNCK_KEY_FILE"]=str(next_file)
        self.cli("fsck",ok=False)

        self.env["SYRNCK_KEY_PASSWORD"]="nouveau secret"
        self.cli("get",manifest,"restored.bin")
        self.assertEqual((self.root/"restored.bin").read_bytes(),payload)
        self.env["SYRNCK_KEY_FILE"]=str(key_file);self.env["SYRNCK_KEY_PASSWORD"]=self.password
        self.cli("fsck")

    def test_import_existing_key_failures_and_output_nonreplacement(self):
        key_file=self.root/"existing.bin"
        self.env["SYRNCK_KEY"]="ab"*32
        self.cli("init","--encrypted")
        self.cli("key","protect",key_file)
        original=key_file.read_bytes()
        self.cli("key","create",key_file,ok=False)
        self.cli("key","rewrap",key_file,key_file,ok=False)
        self.assertEqual(key_file.read_bytes(),original)
        self.env["SYRNCK_KEY_FILE"]=str(key_file)
        self.cli("fsck",ok=False) # Ambiguous providers must not choose one.
        self.env.pop("SYRNCK_KEY")
        self.cli("fsck")
        self.env["SYRNCK_KEY_PASSWORD"]="incorrect credential for key test"
        self.env["SYRNCK_NEW_KEY_PASSWORD"]="changed credential for key test"
        translated=self.cli("key","verify",key_file,ok=False,lang="fr").stderr.decode("utf-8")
        self.assertIn("authentification",translated)
        self.assertNotIn("protected key authentication failed",translated)
        self.cli("key","rewrap",key_file,self.root/"not-created",ok=False)
        self.assertFalse((self.root/"not-created").exists())
        target=self.root/"no-store"
        self.cli("--store",target,"init","--encrypted",ok=False)
        self.assertFalse(target.exists())
        self.env["SYRNCK_KEY_PASSWORD"]=self.password
        damaged=bytearray(original);damaged[88]^=1;key_file.write_bytes(damaged)
        self.cli("key","verify",key_file,ok=False)
        self.cli("fsck",ok=False)
        key_file.write_bytes(original+b"extra")
        self.cli("key","inspect",key_file,ok=False)
        key_file.write_bytes(original)
        self.cli("key","create",self.root/"no-key","--memory-mib","1",ok=False)
        self.cli("key","create",self.root/"no-key","--password-env","bad-name",ok=False)
        self.assertFalse((self.root/"no-key").exists())
        self.env["OTHER_PASSWORD"]=self.env.pop("SYRNCK_KEY_PASSWORD")
        self.assertTrue(json.loads(self.cli("key","verify",key_file,"--password-env","OTHER_PASSWORD").stdout)["authenticated"])
        self.cli("fsck",ok=False)

    def test_linked_key_input_and_existing_link_destination_are_refused(self):
        target=self.root/"key.bin"
        self.cli("key","create",target)
        link=self.root/"linked.bin"
        try: link.symlink_to(target)
        except OSError as error: self.skipTest("local symlink creation unavailable: "+str(error))
        original=target.read_bytes()
        self.cli("key","verify",link,ok=False)
        self.cli("key","create",link,ok=False)
        self.assertEqual(target.read_bytes(),original)

if __name__ == "__main__": unittest.main()
