"""CLI and C ABI range reads, reuse plans and failure isolation."""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())
LIBRARY = str(Path(sys.argv.pop(1)).resolve())


class FileOperations(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="syrnck-file-operations-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.upper().startswith("SYRNCK_")}

    def cli(self, *args, expected=0):
        result = subprocess.run([CLI, *map(str, args)], cwd=self.root, env=self.env,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result

    def test_cli_range_export_comparison_summary_and_language_boundaries(self):
        self.cli("init")
        original = b"a" * 1024 + b"b" * 1024 + b"a" * 1024
        changed = b"b" * 1024 + b"d" * 2048 + b"a" * 1024
        ids = []
        for index, data in enumerate((original, changed)):
            path = self.root / f"input-{index}"
            path.write_bytes(data)
            ids.append(json.loads(self.cli("put", path, "--fixed-size", 1024).stdout)["manifest"])
        refs = self.cli("refs").stdout
        en = self.cli("compare", *ids, "--progress-json")
        fr = self.cli("--lang", "fr", "compare", *ids)
        self.assertEqual(en.stdout, fr.stdout)
        plan = json.loads(en.stdout)
        self.assertEqual((plan["reused_bytes"], plan["new_bytes"], plan["unique_new_bytes"]), (2048, 2048, 1024))
        events = [json.loads(line) for line in en.stderr.splitlines()]
        self.assertTrue(events[-1]["complete"])
        self.assertTrue(all(event["phase"] == "planning" for event in events))
        summary = json.loads(self.cli("compare", *ids, "--summary").stdout)
        self.assertEqual(summary, {k: v for k, v in plan.items() if k not in ("blocks", "needed_chunks")})
        output = self.root / "extrait-échantillon.bin"
        result = self.cli("get", ids[1], output, "--offset", 1000, "--length", 100, "--progress-json")
        self.assertEqual(result.stdout, b"")
        self.assertEqual(output.read_bytes(), changed[1000:1100])
        self.assertEqual(json.loads(result.stderr.splitlines()[-1])["total_bytes"], 100)
        self.cli("get", ids[1], output, "--offset", 0, "--length", 10, expected=1)
        self.assertEqual(output.read_bytes(), changed[1000:1100])
        for options in (("--offset", 0), ("--length", 1), ("--offset", -1, "--length", 1),
                        ("--offset", 1, "--length", 2**64 - 1)):
            self.cli("get", ids[1], self.root / "invalid", *options, expected=1)
            self.assertFalse((self.root / "invalid").exists())
        error = self.cli("--lang", "fr", "get", ids[1], self.root / "invalid", "--offset", 99999,
                         "--length", 1, expected=1)
        self.assertIn("dépasse", error.stderr.decode())
        self.cli("pack", "--max-objects", 1)
        self.cli("get", ids[1], self.root / "packed", "--offset", 1020, "--length", 42)
        self.assertEqual((self.root / "packed").read_bytes(), changed[1020:1062])
        self.assertEqual(self.cli("refs").stdout, refs)

    def test_c_buffers_queries_integrity_and_comparison(self):
        lib = c.CDLL(LIBRARY)
        handle = c.c_void_p
        lib.syrnck_last_error.restype = c.c_char_p
        lib.syrnck_store_open.argtypes = [c.c_char_p, c.c_bool, c.c_void_p, c.c_size_t, c.POINTER(handle)]
        lib.syrnck_store_close.argtypes = [handle]
        lib.syrnck_ingest_file_ex.argtypes = [handle, c.c_char_p, c.c_void_p, c.c_void_p]
        lib.syrnck_file_read_range.argtypes = [handle, c.c_char_p, c.c_uint64, c.c_uint64, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
        lib.syrnck_file_compare_json.argtypes = [handle, c.c_char_p, c.c_char_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
        lib.syrnck_restore_file_range.argtypes = [handle, c.c_char_p, c.c_char_p, c.c_uint64, c.c_uint64]
        lib.syrnck_manifest_json.argtypes = [handle, c.c_char_p, c.POINTER(c.c_void_p)]
        lib.syrnck_string_free.argtypes = [c.c_void_p]

        def check(code):
            self.assertEqual(code, 0, lib.syrnck_last_error().decode())

        for encrypted in (False, True):
            with self.subTest(encrypted=encrypted):
                store = handle()
                root = self.root / f"store-{encrypted}"
                key = c.create_string_buffer(bytes([63]) * 32) if encrypted else None
                check(lib.syrnck_store_open(str(root).encode(), True, key, 32 if key else 0, c.byref(store)))
                try:
                    config = (c.c_uint32 * 4)(1, 1024, 1024, 1024)
                    ids = []
                    content = b"a" * 1024 + b"b" * 1024 + b"c" * 1024
                    for data in (content, content + b"d" * 2048, b""):
                        source = self.root / "source"
                        source.write_bytes(data)
                        identifier = c.create_string_buffer(65)
                        check(lib.syrnck_ingest_file_ex(store, str(source).encode(), config, identifier))
                        ids.append(identifier.value)
                    size = c.c_size_t(99)
                    sentinel = c.create_string_buffer(b"sentinel")
                    check(lib.syrnck_file_read_range(store, ids[0], 1000, 100, None, 0, c.byref(size)))
                    self.assertEqual(size.value, 100)
                    self.assertEqual(lib.syrnck_file_read_range(store, ids[0], 1000, 100, sentinel, 1, c.byref(size)), 1)
                    self.assertEqual(size.value, 100)
                    self.assertEqual(sentinel.value, b"sentinel")
                    data = c.create_string_buffer(100)
                    check(lib.syrnck_file_read_range(store, ids[0], 1000, 100, data, 100, c.byref(size)))
                    self.assertEqual(data.raw, content[1000:1100])
                    check(lib.syrnck_file_read_range(store, ids[2], 0, 0, sentinel, len(sentinel), c.byref(size)))
                    self.assertEqual(size.value, 0)
                    for h, offset, length in ((None, 0, 1), (store, 2**64 - 1, 2), (store, 0, 16 * 1024**2 + 1)):
                        self.assertEqual(lib.syrnck_file_read_range(h, ids[0], offset, length, sentinel, 1, c.byref(size)), 1)
                        self.assertEqual(size.value, 0)
                        self.assertEqual(sentinel.value, b"sentinel")
                    self.assertEqual(lib.syrnck_file_read_range(store, ids[0], 0, 1, data, 100, None), 1)
                    check(lib.syrnck_file_compare_json(store, ids[0], ids[1], None, 0, c.byref(size)))
                    required = size.value
                    self.assertEqual(lib.syrnck_file_compare_json(store, ids[0], ids[1], sentinel, 1, c.byref(size)), 1)
                    self.assertEqual((size.value, sentinel.value), (required, b"sentinel"))
                    plan = c.create_string_buffer(required)
                    check(lib.syrnck_file_compare_json(store, ids[0], ids[1], plan, required, c.byref(size)))
                    self.assertEqual(json.loads(plan.value)["unique_new_bytes"], 1024)
                    output = self.root / f"range-{encrypted}"
                    check(lib.syrnck_restore_file_range(store, ids[0], str(output).encode(), 1020, 20))
                    self.assertEqual(output.read_bytes(), content[1020:1040])
                    self.assertEqual(lib.syrnck_restore_file_range(store, ids[0], str(output).encode(), 0, 1), 1)
                    value = c.c_void_p()
                    check(lib.syrnck_manifest_json(store, ids[0], c.byref(value)))
                    manifest = json.loads(c.string_at(value))
                    check(lib.syrnck_string_free(value))
                    oid = manifest["blocks"][2]["id"]
                    path = root / "objects/chunk" / oid[:2] / oid
                    broken = bytearray(path.read_bytes()); broken[0] ^= 1; path.write_bytes(broken)
                    unchanged = c.create_string_buffer(b"X" * 2048)
                    self.assertEqual(lib.syrnck_file_read_range(store, ids[0], 1, 2048, unchanged, 2048, c.byref(size)), 1)
                    self.assertEqual(size.value, 0)
                    self.assertEqual(unchanged.value, b"X" * 2048)
                    check(lib.syrnck_file_read_range(store, ids[0], 1, 2048, None, 0, c.byref(size)))
                finally:
                    check(lib.syrnck_store_close(store))


if __name__ == "__main__":
    unittest.main()
