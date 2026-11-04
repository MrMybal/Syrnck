"""One-time migration audit: Rust <-> C++ stores, metadata, encryption and CDC.

Usage: python tests/migration_compat.py NEW_CPP_BINARY OLD_RUST_BINARY
The generated fixtures are subsequently tested without requiring Rust.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import socket
import time
import urllib.request

cpp, rust = (str(Path(p).resolve()) for p in sys.argv[1:3])
fixtures = Path(__file__).parent / "fixtures/v1"
fixtures.mkdir(parents=True, exist_ok=True)
env = {k: v for k, v in os.environ.items() if k.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN")}


def run(binary, store, *args, key=None):
    current = dict(env)
    if key:
        current["SYRNCK_KEY"] = key
    return subprocess.check_output([binary, "--store", str(store), *map(str, args)], env=current)


with tempfile.TemporaryDirectory(prefix="syrnck-migration-") as folder:
    root = Path(folder)
    data = b"".join(hashlib.sha256(i.to_bytes(8, "little")).digest() for i in range(5 * 1024 * 1024 // 32))
    source = root / "source.bin"
    source.write_bytes(data)
    for encrypted in (False, True):
        key = "07" * 32 if encrypted else None
        stores = []
        for name, binary in (("rust", rust), ("cpp", cpp)):
            store = root / (name + ("-encrypted" if encrypted else "-plain"))
            stores.append(store)
            run(binary, store, "init", *(["--encrypted"] if encrypted else []), key=key)
            result = json.loads(run(binary, store, "put", source, key=key))
            if name == "rust":
                expected = result["manifest"]
                manifest = json.loads(run(binary, store, "inspect", "manifest", expected, key=key))
            assert result["manifest"] == expected, ("CDC or serialization changed", name, encrypted)
            # The other executable reads and validates this implementation's store.
            other = cpp if name == "rust" else rust
            output = root / f"{name}-{encrypted}-restored"
            run(other, store, "get", expected, output, key=key)
            assert output.read_bytes() == data
            run(other, store, "fsck", key=key)
            assert json.loads(run(other, store, "inspect", "manifest", expected, key=key)) == manifest
        # Writes into the other implementation's store also preserve its configuration.
        run(cpp, stores[0], "put", source, key=key)
        run(rust, stores[1], "put", source, key=key)
        if not encrypted:
            (fixtures / "cdc.json").write_text(json.dumps({"producer": "Rust prototype, fastcdc 5.0.0, format v1", "input": "SHA256(LE64(counter)) concatenated, 5 MiB", "manifest_id": expected, "manifest": manifest}, indent=2) + "\n", encoding="utf-8")
        # Revision JSON, Unicode paths and parent links are also interoperable.
        for name, binary, other in (("rust", rust, cpp), ("cpp", cpp, rust)):
            project = root / f"native-{name}-{encrypted}"
            project.mkdir()
            store = project / ".syrnck"
            run(binary, store, "init", *(["--encrypted"] if encrypted else []), key=key)
            (project / "Éditeur.cpp").write_text("// source\n", encoding="utf-8")
            first = run(binary, store, "native", "commit", "--directory", project, "-m", "initial", key=key).decode().strip()
            second = run(other, store, "native", "commit", "--directory", project, "-m", "second", key=key).decode().strip()
            revision = json.loads(run(binary, store, "inspect", "revision", second, key=key))
            assert revision["parents"] == [first]
            run(other, store, "native", "export", first, project / "export", key=key)
            assert (project / "export/Éditeur.cpp").read_text(encoding="utf-8") == "// source\n"
    # Both network directions use the same /v1 protocol and object representation.
    token = "migration-test-token-0123456789-0123456789"
    for name, server_binary, client_binary in (("cpp", cpp, rust), ("rust", rust, cpp)):
        store = root / ("server-" + name)
        run(server_binary, store, "init")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        url = f"http://127.0.0.1:{port}"
        process = subprocess.Popen([server_binary, "--store", str(store), "serve", "--listen", f"127.0.0.1:{port}"], env=dict(env, SYRNCK_TOKEN=token), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    request = urllib.request.Request(url + "/v1/refs", headers={"Authorization": "Bearer " + token})
                    with urllib.request.urlopen(request, timeout=1):
                        break
                except OSError:
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(0.02)
            client = root / ("client-" + name)
            run(client_binary, client, "init")
            value = json.loads(run(client_binary, client, "put", source))
            transferred = subprocess.check_output([client_binary, "--store", str(client), "agent", "push", url, "files/" + value["manifest"], "asset"], env=dict(env, SYRNCK_TOKEN=token))
            assert json.loads(transferred)["transferred_objects"] > 0
        finally:
            process.terminate()
            process.wait(timeout=5)
            process.stderr.close()
    # Small encrypted store produced by the old Rust implementation, with a PUBLIC test key.
    small = root / "small-rust"
    key = "07" * 32
    source.write_bytes(b"Rust to C++ encrypted fixture\x00" + bytes(range(256)))
    run(rust, small, "init", "--encrypted", key=key)
    value = json.loads(run(rust, small, "put", source, key=key))
    destination = fixtures / "encrypted"
    if destination.exists():
        # Restrict cleanup to the exact generated fixture tree.
        assert destination.resolve().parent == fixtures.resolve()
        shutil.rmtree(destination)
    shutil.copytree(small, destination)
    (destination / "store.lock").unlink()
    (fixtures / "encrypted.json").write_text(json.dumps({"producer": "Rust prototype, chacha20poly1305 0.10.1", "public_test_key": key, "manifest": value["manifest"], "content_hex": source.read_bytes().hex()}, indent=2) + "\n", encoding="utf-8")
print("Migration verified: identical CDC/manifests; bidirectional plaintext and encrypted stores")
