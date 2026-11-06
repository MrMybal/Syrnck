"""Exercise the compiled C ABI with Python's standard library.

Usage: python examples/ffi_smoke.py build/Release/syrnck_ffi.dll
Linux: build/libsyrnck_ffi.so; macOS: build/libsyrnck_ffi.dylib.
"""
import ctypes as c
import json
from pathlib import Path
import sys
import tempfile


def main():
    lib = c.CDLL(str(Path(sys.argv[1]).resolve()))
    handle = c.c_void_p
    lib.syrnck_last_error.restype = c.c_char_p
    lib.syrnck_store_open.argtypes = [c.c_char_p, c.c_bool, c.c_void_p, c.c_size_t, c.POINTER(handle)]
    lib.syrnck_store_close.argtypes = [handle]
    lib.syrnck_ingest_file.argtypes = [handle, c.c_char_p, c.c_void_p]
    lib.syrnck_restore_file.argtypes = [handle, c.c_char_p, c.c_char_p]
    lib.syrnck_file_verify.argtypes = [handle, c.c_char_p]
    lib.syrnck_manifest_json.argtypes = [handle, c.c_char_p, c.POINTER(c.c_void_p)]
    lib.syrnck_string_free.argtypes = [c.c_void_p]
    lib.syrnck_abi_version.restype = c.c_uint32
    lib.syrnck_object_read.argtypes = [handle, c.c_char_p, c.c_char_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    lib.syrnck_closure_json.argtypes = [handle, c.c_char_p, c.c_char_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    lib.syrnck_store_verify.argtypes = [handle, c.POINTER(c.c_size_t)]
    assert lib.syrnck_abi_version() == 1

    def check(code):
        if code:
            raise RuntimeError(lib.syrnck_last_error().decode("utf-8"))

    def encoded(path):
        return str(path).encode("utf-8")

    with tempfile.TemporaryDirectory(prefix="syrnck-ffi-") as folder:
        root = Path(folder)
        content = b"native ABI round-trip\x00" * 65536
        source = root / "source.bin"
        source.write_bytes(content)
        store = handle()
        check(lib.syrnck_store_open(encoded(root / "store"), True, None, 0, c.byref(store)))
        try:
            identifier = c.create_string_buffer(65)
            check(lib.syrnck_ingest_file(store, encoded(source), identifier))
            check(lib.syrnck_file_verify(store, identifier.value))
            value = c.c_void_p()
            check(lib.syrnck_manifest_json(store, identifier.value, c.byref(value)))
            try:
                manifest = json.loads(c.string_at(value).decode("utf-8"))
                assert manifest["size"] == len(content)
            finally:
                check(lib.syrnck_string_free(value))
            length = c.c_size_t()
            check(lib.syrnck_object_read(store, b"manifest", identifier.value, None, 0, c.byref(length)))
            required = length.value
            short = c.create_string_buffer(b"sentinel")
            assert lib.syrnck_object_read(store, b"manifest", identifier.value, short, 1, c.byref(length)) == 1
            assert short.value == b"sentinel" and length.value == required
            data = c.create_string_buffer(required)
            check(lib.syrnck_object_read(store, b"manifest", identifier.value, data, required, c.byref(length)))
            assert json.loads(data.raw) == manifest
            check(lib.syrnck_closure_json(store, b"manifest", identifier.value, None, 0, c.byref(length)))
            closure = c.create_string_buffer(length.value)
            check(lib.syrnck_closure_json(store, b"manifest", identifier.value, closure, length.value, c.byref(length)))
            assert {"kind": "manifest", "id": identifier.value.decode()} in json.loads(closure.value)
            check(lib.syrnck_store_verify(store, c.byref(length)))
            assert length.value >= 2
            check(lib.syrnck_restore_file(store, identifier.value, encoded(root / "restored.bin")))
            assert (root / "restored.bin").read_bytes() == content
            print("C ABI verified: reconstruction, object/closure reads, capacity checks and ownership")
        finally:
            check(lib.syrnck_store_close(store))


if __name__ == "__main__":
    main()
