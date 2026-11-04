"""Executable-level regression tests; no Rust compiler or Python packages needed."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

CLI = str(Path(sys.argv.pop(1)).resolve())
TOKEN = "syrnck-test-token-0123456789-0123456789"
FIXTURES = Path(__file__).parent / "fixtures/v1"


def sample(size=5 * 1024 * 1024):
    return b"".join(hashlib.sha256(i.to_bytes(8, "little")).digest() for i in range((size + 31) // 32))[:size]


class Workflows(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="syrnck-cpp-")
        self.root = Path(self.tmp.name)
        self.store = self.root / ".syrnck"
        self.env = {k: v for k, v in os.environ.items() if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}
        self.cmd("init")

    def tearDown(self):
        self.tmp.cleanup()

    def cmd(self, *args, store=None, env=None, ok=True, data=None):
        result = subprocess.run([CLI, "--store", str(store or self.store), *map(str, args)], cwd=self.root, env=env or self.env, input=data, capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", errors="replace"))
        return result.stdout

    def ingest(self, data, fixed=None, env=None, store=None):
        source = self.root / "input.bin"
        source.write_bytes(data)
        args = ["put", source]
        if fixed:
            args += ["--fixed-size", fixed]
        return json.loads(self.cmd(*args, env=env, store=store))

    def metadata(self, kind, identifier):
        return json.loads(self.cmd("inspect", kind, identifier))

    def test_empty_file(self):
        value = self.ingest(b"")
        self.assertEqual(value["stats"]["blocks"], 0)
        output = self.root / "empty"
        self.cmd("get", value["manifest"], output)
        self.assertEqual(output.read_bytes(), b"")

    def test_repeated_blocks_and_reingestion(self):
        data = b"abcdefgh" * 128 * 3 + b"tail"
        value = self.ingest(data, fixed=1024)
        self.assertEqual(value["stats"]["new_bytes"], 1028)
        self.assertEqual(value["stats"]["reused_bytes"], 2048)
        again = self.ingest(data, fixed=1024)
        self.assertEqual(again["manifest"], value["manifest"])
        self.assertEqual(again["stats"]["new_bytes"], 0)
        self.cmd("get", value["manifest"], self.root / "restored")
        self.assertEqual((self.root / "restored").read_bytes(), data)

    def test_cdc_insertion_and_rust_boundaries(self):
        data = sample()
        first = self.ingest(data)
        if (FIXTURES / "cdc.json").exists():
            expected = json.loads((FIXTURES / "cdc.json").read_text())
            self.assertEqual(first["manifest"], expected["manifest_id"])
            self.assertEqual(self.metadata("manifest", first["manifest"]), expected["manifest"])
        edited = data[:1024 * 1024] + b"a new variable-length asset header" + data[1024 * 1024:]
        second = self.ingest(edited)
        self.assertGreater(second["stats"]["reused_bytes"], len(data) // 2)
        self.cmd("get", second["manifest"], self.root / "restored")
        self.assertEqual((self.root / "restored").read_bytes(), edited)

    def test_invalid_sizes(self):
        source = self.root / "file"
        source.write_bytes(b"data")
        for size in (0, 16 * 1024 * 1024 + 1, 2**32):
            self.cmd("put", source, "--fixed-size", size, ok=False)

    def test_parallel_ingest_preserves_v1_and_encryption(self):
        data = sample()
        source = self.root / "parallel.bin"
        source.write_bytes(data)
        for encrypted in (False, True):
            serial_store = self.root / ("encrypted-serial" if encrypted else "plain-serial")
            parallel_store = self.root / ("encrypted-parallel" if encrypted else "plain-parallel")
            env = dict(self.env, SYRNCK_KEY="31" * 32) if encrypted else self.env
            flags = ["--encrypted"] if encrypted else []
            for store in (serial_store, parallel_store):
                self.cmd("init", *flags, store=store, env=env)
            serial = json.loads(self.cmd("put", source, store=serial_store, env=env))
            parallel = json.loads(self.cmd("put", source, "--workers", 4, "--buffer-mib", 4, store=parallel_store, env=env))
            self.assertEqual(serial["manifest"], parallel["manifest"])
            self.assertEqual(parallel["manifest"], json.loads((FIXTURES / "cdc.json").read_text())["manifest_id"])
            self.assertLessEqual(parallel["stats"]["peak_in_flight_bytes"], 4 * 1024 * 1024)
            restored = self.root / ("encrypted-restored" if encrypted else "plain-restored")
            self.cmd("get", parallel["manifest"], restored, store=parallel_store, env=env)
            self.assertEqual(restored.read_bytes(), data)
            self.cmd("fsck", store=parallel_store, env=env)

    def test_invalid_pipeline_limits(self):
        source = self.root / "pipeline.bin"
        source.write_bytes(b"data")
        for workers in (0, 65, 2**64 - 1):
            self.cmd("put", source, "--workers", workers, ok=False)
        for budget in (0, 1, 1025):
            self.cmd("put", source, "--workers", 4, "--buffer-mib", budget, ok=False)
        self.assertEqual(json.loads(self.cmd("refs")), {})

    def test_cache_budget_option_preserves_objects(self):
        source = self.root / "cache.bin"
        data = b"cache option round-trip\x00" * 32768
        source.write_bytes(data)
        uncached = json.loads(self.cmd("--cache-mib", 0, "put", source))
        cached = json.loads(self.cmd("--cache-mib", 1, "put", source))
        self.assertEqual(uncached["manifest"], cached["manifest"])
        self.assertEqual(cached["stats"]["new_bytes"], 0)
        self.cmd("--cache-mib", 1, "get", cached["manifest"], self.root / "cached-copy")
        self.assertEqual((self.root / "cached-copy").read_bytes(), data)
        self.cmd("--cache-mib", 0, "fsck")
        for budget in (-1, 1025, 2**64 - 1):
            self.cmd("--cache-mib", budget, "refs", ok=False)

    def test_restore_never_overwrites(self):
        value = self.ingest(b"new")
        target = self.root / "protected"
        target.write_bytes(b"keep")
        self.cmd("get", value["manifest"], target, ok=False)
        self.assertEqual(target.read_bytes(), b"keep")

    def test_initialization_never_replaces_refs(self):
        other = self.root / "incomplete"
        other.mkdir()
        (other / "refs.json").write_bytes(b"preserve")
        self.cmd("init", store=other, ok=False)
        self.assertEqual((other / "refs.json").read_bytes(), b"preserve")
        self.cmd("init", ok=False)

    def test_encryption_wrong_key_and_authentication(self):
        encrypted = self.root / "encrypted"
        env = dict(self.env, SYRNCK_KEY="07" * 32)
        self.cmd("init", "--encrypted", store=encrypted, env=env)
        value = self.ingest(b"private asset", store=encrypted, env=env)
        self.cmd("fsck", store=encrypted, env=env)
        self.cmd("fsck", store=encrypted, ok=False)
        self.cmd("fsck", store=encrypted, env=dict(env, SYRNCK_KEY="08" * 32), ok=False)
        object_file = next((encrypted / "objects/manifest").glob("*/*"))
        data = bytearray(object_file.read_bytes())
        self.assertEqual(data[4], 1)
        data[-1] ^= 1
        object_file.write_bytes(data)
        self.cmd("get", value["manifest"], self.root / "bad", store=encrypted, env=env, ok=False)
        self.assertFalse((self.root / "bad").exists())

    def test_rust_encrypted_fixture(self):
        if not (FIXTURES / "encrypted/config.json").exists():
            self.skipTest("legacy fixture not generated yet")
        import shutil
        target = self.root / "legacy"
        shutil.copytree(FIXTURES / "encrypted", target)
        info = json.loads((FIXTURES / "encrypted.json").read_text())
        env = dict(self.env, SYRNCK_KEY=info["public_test_key"])
        self.cmd("get", info["manifest"], self.root / "legacy-file", store=target, env=env)
        self.assertEqual((self.root / "legacy-file").read_bytes(), bytes.fromhex(info["content_hex"]))
        self.cmd("fsck", store=target, env=env)

    def test_gc_fails_before_deleting_when_live_data_is_missing(self):
        live = self.ingest(b"live")
        orphan = self.ingest(b"orphan")
        self.cmd("unpin", "files/" + orphan["manifest"])
        block = self.metadata("manifest", live["manifest"])["blocks"][0]["id"]
        (self.store / "objects/chunk" / block[:2] / block).unlink()
        self.cmd("gc", "--delete", ok=False)
        self.cmd("inspect", "manifest", orphan["manifest"])
        self.cmd("fsck", ok=False)

    def test_bad_ids_and_missing_roots(self):
        for value in ("../config.json", "A" * 64, "0" * 64):
            self.cmd("pin", "bad", "manifest", value, ok=False)
        for name in ("../bad", "heads//x", "/x"):
            self.cmd("pin", name, "chunk", "0" * 64, ok=False)
        self.assertEqual(json.loads(self.cmd("refs")), {})

    def commit(self, message):
        return self.cmd("native", "commit", "--directory", self.root, "-m", message).decode().strip()

    def test_native_detach_archive_gc_and_export(self):
        asset = self.root / "Map.uasset"
        asset.write_bytes(b"original asset")
        first = self.commit("first")
        asset.write_bytes(b"edited asset")
        second = self.commit("edit")
        self.assertEqual(self.metadata("revision", second)["parents"], [first])
        current = self.cmd("native", "detach-history", "-m", "new root").decode().strip()
        self.cmd("gc", "--delete")
        self.cmd("inspect", "revision", first)
        self.cmd("unpin", "archive/" + second)
        self.assertGreater(json.loads(self.cmd("gc"))["unreachable_objects"], 0)
        self.cmd("gc", "--delete")
        self.cmd("inspect", "revision", first, ok=False)
        self.cmd("native", "export", current, self.root / "export")
        self.assertEqual((self.root / "export/Map.uasset").read_bytes(), b"edited asset")
        self.cmd("native", "export", current, self.root / "export", ok=False)

    def test_another_branch_keeps_history(self):
        (self.root / "Game.cpp").write_text("source", encoding="utf-8")
        first = self.commit("first")
        self.cmd("native", "branch", "release", first)
        second = self.cmd("native", "detach-history", "-m", "new root").decode().strip()
        self.cmd("unpin", "archive/" + first)
        self.cmd("gc", "--delete")
        self.cmd("inspect", "revision", first)
        self.assertNotEqual(first, second)

    def test_native_ignore_and_utf8_paths(self):
        (self.root / ".syrnckignore").write_text("Saved/\n**/Intermediate/\n*.tmp\n!important.tmp\n", encoding="utf-8")
        (self.root / "Saved").mkdir()
        (self.root / "Saved/cache").write_text("ignored")
        (self.root / "cache.tmp").write_text("ignored")
        (self.root / "important.tmp").write_text("retained")
        (self.root / "Éditeur.cpp").write_text("retained", encoding="utf-8")
        revision = self.commit("UTF-8")
        files = self.metadata("revision", revision)["files"]
        self.assertEqual(set(files), {".syrnckignore", "important.tmp", "Éditeur.cpp"})

    def test_case_collisions_and_reserved_paths(self):
        upper, lower = self.root / "Foo", self.root / "foo"
        upper.write_text("a")
        lower.write_text("b")
        if not upper.samefile(lower):
            self.cmd("native", "commit", "--directory", self.root, "-m", "bad", ok=False)
        else:
            # Case-insensitive APFS/NTFS aliases these names to one file. This is
            # a valid snapshot, not an engine collision between two entries.
            revision = self.commit("one physical file")
            files = self.metadata("revision", revision)["files"]
            self.assertEqual([name.casefold() for name in files], ["foo"])
        self.cmd("get", "..\\config.json", self.root / "bad", ok=False)

    def test_git_filters_preserve_code_and_binary_history(self):
        repo = self.root / "repo with spaces and 'quote é"
        repo.mkdir()
        def git(*args):
            result = subprocess.run(["git", "-C", str(repo), *args], env=self.env, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            return result.stdout
        git("init", "--initial-branch=main")
        for name, value in (("user.name", "Syrnck test"), ("user.email", "test@example.invalid"), ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            git("config", name, value)
        self.cmd("git", "install", repo)
        (repo / ".gitattributes").write_text("*.uasset filter=syrnck -diff -merge -text\n")
        original = bytes(range(256)) * 8192
        (repo / "Map.uasset").write_bytes(original)
        (repo / "Game.cpp").write_bytes(b"int main() { return 0; }\n")
        git("add", ".")
        git("commit", "-m", "first")
        pointer = git("show", "HEAD:Map.uasset")
        self.assertTrue(pointer.startswith(b"version syrnck-pointer-v1\n"))
        self.assertLess(len(pointer), 1024)
        self.assertEqual(git("show", "HEAD:Game.cpp"), b"int main() { return 0; }\n")
        git("switch", "-c", "edit")
        (repo / "Map.uasset").write_bytes(b"second")
        git("add", "Map.uasset")
        git("commit", "-m", "second")
        git("switch", "main")
        self.assertEqual((repo / "Map.uasset").read_bytes(), original)
        git("switch", "edit")
        self.assertEqual((repo / "Map.uasset").read_bytes(), b"second")
        self.assertFalse(git("status", "--porcelain"))

    def test_filter_passthrough_and_strict_pointer(self):
        for data in (b"", b"plain text\n", bytes(range(256)) * 20):
            self.assertEqual(self.cmd("git", "smudge", data=data), data)
        self.cmd("git", "clean", data=b"version syrnck-pointer-v1\ninvalid", ok=False)

    @contextlib.contextmanager
    def server(self, encrypted=False, prepare=None):
        folder = self.root / "server"
        env = dict(self.env, SYRNCK_TOKEN=TOKEN)
        if encrypted:
            env["SYRNCK_KEY"] = "02" * 32
        self.cmd("init", *(["--encrypted"] if encrypted else []), store=folder, env=env)
        if prepare:
            prepare(folder, env)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        url = f"http://127.0.0.1:{port}"
        process = subprocess.Popen([CLI, "--store", str(folder), "serve", "--listen", f"127.0.0.1:{port}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    self.fail(process.stderr.read().decode(errors="replace"))
                try:
                    self.http(url, "GET", "/v1/refs")
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            yield url, folder
        finally:
            process.terminate()
            process.wait(timeout=5)
            process.stderr.close()

    def http(self, url, method, path, data=None, token=TOKEN):
        request = urllib.request.Request(url + path, data=json.dumps(data).encode() if data is not None else None, method=method, headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status, response.read()

    def test_network_only_missing_blocks_resume_and_encryption(self):
        client_env = dict(self.env, SYRNCK_TOKEN=TOKEN)
        first = self.ingest(b"a" * 4096 + b"b" * 4096 + b"c" * 4096, fixed=4096)
        with self.server(encrypted=True) as (url, folder):
            pushed = json.loads(self.cmd("agent", "push", url, "files/" + first["manifest"], "asset", env=client_env))
            self.assertEqual(pushed["transferred_objects"], 4)
            again = json.loads(self.cmd("agent", "push", url, "files/" + first["manifest"], "asset", env=client_env))
            self.assertEqual(again["transferred_bytes"], 0)
            second = self.ingest(b"a" * 4096 + b"d" * 4096 + b"c" * 4096, fixed=4096)
            pushed = json.loads(self.cmd("agent", "push", url, "files/" + second["manifest"], "asset", env=client_env))
            self.assertEqual(pushed["transferred_objects"], 2)
            self.assertEqual(pushed["reused_objects"], 2)
            target = self.root / "target"
            target_env = dict(client_env, SYRNCK_KEY="03" * 32)
            self.cmd("init", "--encrypted", store=target, env=target_env)
            # Seed one block to simulate a previous, interrupted download.
            self.ingest(b"a" * 4096, fixed=4096, store=target, env=target_env)
            pulled = json.loads(self.cmd("agent", "pull", url, "asset", "received", store=target, env=target_env))
            self.assertEqual(pulled["reused_objects"], 1)
            self.cmd("get", second["manifest"], self.root / "download", store=target, env=target_env)
            self.assertEqual((self.root / "download").read_bytes(), b"a" * 4096 + b"d" * 4096 + b"c" * 4096)
            self.cmd("fsck", store=target, env=target_env)
            self.cmd("fsck", store=folder, env=dict(self.env, SYRNCK_KEY="02" * 32), ok=False)

    def test_packed_stores_network_missing_read_and_mixed_import(self):
        client_env = dict(self.env, SYRNCK_TOKEN=TOKEN)
        original = b"a" * 4096 + b"b" * 4096 + b"c" * 4096
        first = self.ingest(original, fixed=4096)
        self.cmd("pack", "--max-objects", 1)

        def prepare(folder, env):
            seeded = self.ingest(original, fixed=4096, store=folder, env=env)
            self.assertEqual(seeded["manifest"], first["manifest"])
            self.cmd("pack", "--max-objects", 1, store=folder, env=env)

        with self.server(encrypted=True, prepare=prepare) as (url, folder):
            pushed = json.loads(self.cmd("agent", "push", url, "files/" + first["manifest"], "asset", env=client_env))
            self.assertEqual(pushed["transferred_objects"], 0)
            target = self.root / "target-packed"
            target_env = dict(client_env, SYRNCK_KEY="03" * 32)
            self.cmd("init", "--encrypted", store=target, env=target_env)
            self.ingest(b"a" * 4096, fixed=4096, store=target, env=target_env)
            self.cmd("pack", "--max-objects", 1, store=target, env=target_env)
            pulled = json.loads(self.cmd("agent", "pull", url, "asset", "received", store=target, env=target_env))
            self.assertEqual(pulled["transferred_objects"], 3)
            self.assertEqual(pulled["reused_objects"], 1)
            changed = b"a" * 4096 + b"d" * 4096 + b"c" * 4096
            second = self.ingest(changed, fixed=4096)
            pushed = json.loads(self.cmd("agent", "push", url, "files/" + second["manifest"], "asset", env=client_env))
            self.assertEqual(pushed["transferred_objects"], 2)
            self.assertEqual(pushed["reused_objects"], 2)
            self.cmd("agent", "pull", url, "asset", "received", store=target, env=target_env)
            self.cmd("pack", store=target, env=target_env)
            self.cmd("get", second["manifest"], self.root / "network-packed-output", store=target, env=target_env)
            self.assertEqual((self.root / "network-packed-output").read_bytes(), changed)
            self.cmd("fsck", store=target, env=target_env)

    def test_pack_upgrade_cli_gc_and_reconstruction(self):
        first = self.ingest(b"a" * 4096 + b"b" * 4096, fixed=4096)
        second = self.ingest(b"a" * 4096 + b"c" * 4096, fixed=4096)
        before = self.metadata("manifest", second["manifest"])
        packed = json.loads(self.cmd("pack"))
        self.assertEqual(packed["objects"], 5)
        self.assertEqual(packed["loose_objects_removed"], 5)
        self.assertFalse(any(path.is_file() for path in (self.store / "objects").rglob("*")))
        self.assertEqual(self.metadata("manifest", second["manifest"]), before)
        self.cmd("unpin", "files/" + first["manifest"])
        dry = json.loads(self.cmd("gc"))
        self.assertEqual(dry["unreachable_objects"], 2)
        self.assertTrue(dry["repack_required"])
        self.cmd("gc", "--delete")
        self.cmd("get", second["manifest"], self.root / "packed-output")
        self.assertEqual((self.root / "packed-output").read_bytes(), b"a" * 4096 + b"c" * 4096)
        self.cmd("get", first["manifest"], self.root / "removed-output", ok=False)
        self.cmd("fsck")

    def test_git_smudge_and_clean_work_after_packing(self):
        content = sample(16000)
        pointer = self.cmd("git", "clean", data=content)
        self.cmd("pack")
        self.assertEqual(self.cmd("git", "smudge", data=pointer), content)
        self.assertEqual(self.cmd("git", "clean", data=content), pointer)
        self.cmd("fsck")

    def test_incremental_multi_pack_and_explicit_compaction(self):
        original = b"a" * 4096 + b"b" * 4096 + b"c" * 4096
        first = self.ingest(original, fixed=4096)
        self.assertEqual(json.loads(self.cmd("pack", "--max-objects", 2))["pack_count"], 2)
        old_files = {p: p.read_bytes() for p in (self.store / "packs").iterdir()}
        changed = self.ingest(original + b"d" * 4096, fixed=4096)
        result = json.loads(self.cmd("pack"))
        self.assertEqual(result["pack_count"], 3)
        self.assertEqual(result["objects_written"], 2)
        self.assertEqual(result["obsolete_pack_files_removed"], 0)
        for path, content in old_files.items():
            self.assertEqual(path.read_bytes(), content)
        config_before = (self.store / "config.json").read_bytes()
        self.assertEqual(json.loads(self.cmd("pack"))["pack_bytes_written"], 0)
        self.assertEqual((self.store / "config.json").read_bytes(), config_before)
        compacted = json.loads(self.cmd("pack", "--compact", "--max-mib", 18))
        self.assertEqual(compacted["pack_count"], 1)
        self.assertEqual(compacted["objects_written"], 6)
        self.assertEqual(compacted["obsolete_pack_files_removed"], 6)
        self.assertEqual(json.loads((self.store / "config.json").read_text())["version"], 3)
        self.cmd("unpin", "files/" + first["manifest"])
        self.assertEqual(json.loads(self.cmd("gc", "--delete"))["packs_to_rewrite"], 1)
        self.cmd("get", changed["manifest"], self.root / "multi-pack-output")
        self.assertEqual((self.root / "multi-pack-output").read_bytes(), original + b"d" * 4096)
        self.cmd("fsck")

    def test_pack_limits_reject_invalid_values_before_migration(self):
        self.ingest(b"pack limits")
        before = {p: p.read_bytes() for p in self.store.rglob("*") if p.is_file()}
        for option, values in (("--max-mib", (0, 17, 4097, -1, 2**64)),
                               ("--max-objects", (0, -1, 1048577, 2**64))):
            for value in values:
                self.cmd("pack", option, value, ok=False)
        self.assertEqual({p: p.read_bytes() for p in self.store.rglob("*") if p.is_file()}, before)

    def test_paged_index_cli_budget_preserves_formats_and_reconstruction(self):
        data = sample(32768)
        first = self.ingest(data, fixed=64)
        self.cmd("pack", "--max-objects", 256)
        before = {p: p.read_bytes() for p in self.store.rglob("*") if p.is_file()}
        for budget in (0, 1, 4):
            output = self.root / f"index-budget-{budget}"
            self.cmd("--cache-mib", 0, "--index-cache-mib", budget, "get", first["manifest"], output)
            self.assertEqual(output.read_bytes(), data)
            self.cmd("--index-cache-mib", budget, "fsck")
        self.assertEqual({p: p.read_bytes() for p in self.store.rglob("*") if p.is_file()}, before)

    def test_invalid_index_cache_budget_precedes_store_creation(self):
        for budget in (-1, 1025, 2**64, "invalid"):
            target = self.root / ("index-budget-invalid-" + str(budget))
            self.cmd("--index-cache-mib", budget, "init", store=target, ok=False)
            self.assertFalse(target.exists())

    def test_server_authentication_and_compare_and_swap(self):
        value = self.ingest(b"asset")
        env = dict(self.env, SYRNCK_TOKEN=TOKEN)
        with self.server() as (url, _):
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.http(url, "GET", "/v1/refs", token="invalid")
            self.assertEqual(error.exception.code, 401)
            self.cmd("agent", "push", url, "files/" + value["manifest"], "asset", env=env)
            status, body = self.http(url, "GET", "/v1/refs")
            self.assertEqual(status, 200)
            root = json.loads(body)["asset"]
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.http(url, "PUT", "/v1/refs/asset", {"root": root, "expected": None})
            self.assertEqual(error.exception.code, 409)
            self.assertEqual(self.http(url, "PUT", "/v1/refs/asset", {"root": root, "expected": root})[0], 204)

    def test_plain_remote_http_and_embedded_credentials_are_rejected(self):
        env = dict(self.env, SYRNCK_TOKEN=TOKEN)
        for url in ("http://example.invalid", "https://user:password@example.invalid", "https://example.invalid?token=x"):
            self.cmd("agent", "pull", url, "main", "main", env=env, ok=False)
        self.cmd("serve", "--listen", "0.0.0.0:7437", env=env, ok=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
