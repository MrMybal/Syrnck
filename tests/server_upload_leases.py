"""Production server leases, offline GC, renewal, fencing and lost acknowledgements."""
import contextlib
import json
import os
import socket
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
from types import SimpleNamespace
import network_operations as base
import parallel_transfers as parallel


class ProxyHandler(base.Handler):
    def forward(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))) if self.command != "GET" else None
        headers = {"Authorization": "Bearer " + base.TOKEN, "Content-Type": "application/json"}
        for name in ("Syrnck-Object-Encoding", "Syrnck-Accept-Object-Encoding"):
            if name in self.headers:
                headers[name] = self.headers[name]
        request = urllib.request.Request(self.server.target + self.path, data=body, method=self.command,
                                        headers=headers)
        try:
            response = urllib.request.urlopen(request, timeout=8)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status, data = response.status, response.read()
            encoding = response.headers.get("Syrnck-Object-Encoding")
        self.server.calls.append((self.command, self.path))
        if self.command == "GET" and self.path == "/v1/capabilities" and self.server.hide_inventory:
            capabilities = json.loads(data)
            capabilities.pop("upload_inventories", None)
            data = json.dumps(capabilities).encode()
        if self.command == "PUT" and "/inventory/" in self.path and self.server.hold_inventory and status == 204:
            self.server.inventory_entered.set()
            self.server.inventory_release.wait(8)
        if self.server.hold_renewal:
            if self.command == "PUT" and self.path.startswith("/v1/uploads/") and self.server.calls.count((self.command, self.path)) == 2:
                self.server.renewal_entered.set()
                self.server.renewal_release.wait(5)
            elif self.command == "PUT" and self.path.startswith("/v1/objects/") and self.server.renewal_entered.is_set():
                self.server.object_during_renewal.set()
                self.server.renewal_release.set()
        if self.server.gate and self.command == "PUT" and self.path == "/v1/refs/asset" and status == 204:
            self.server.entered.set()
            self.server.release.wait(8)
        if encoding:
            self.send_response(status)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Syrnck-Object-Encoding", encoding)
            self.end_headers()
            self.wfile.write(data)
        else:
            self.reply(data, status)

    do_GET = do_POST = do_PUT = forward


class Proxy(base.Repository):
    def __init__(self, target):
        super().__init__(ProxyHandler)
        self.target, self.gate = target, True
        self.hold_renewal = False
        self.hide_inventory, self.hold_inventory = False, False
        self.inventory_entered, self.inventory_release = threading.Event(), threading.Event()
        self.renewal_entered, self.renewal_release, self.object_during_renewal = threading.Event(), threading.Event(), threading.Event()


class LegacyHandler(base.Handler):
    def do_GET(self):
        if self.path == "/v1/capabilities":
            if self.authorized():
                self.reply({}, 404)
        else:
            super().do_GET()


class ServerUploadLeases(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe
    setUp = parallel.ParallelTransfers.setUp

    def setup_server(self):
        self.remote_store = self.root / "server"
        self.cmd("init", "--encrypted", store=self.remote_store, encrypted=True)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]

    @contextlib.contextmanager
    def serving(self, seconds=3600, strict=False, read_seconds=3600, compression=True):
        env = dict(self.env, SYRNCK_KEY="39" * 32)
        command = [base.CLI, "--store", str(self.remote_store), "serve", "--listen", f"127.0.0.1:{self.port}",
                   "--lease-seconds", str(seconds), "--read-lease-seconds", str(read_seconds)]
        if strict:
            command.append("--require-lease")
        if not compression:
            command.append("--no-compression")
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        url = f"http://127.0.0.1:{self.port}"
        try:
            deadline = time.monotonic() + 8
            while True:
                if process.poll() is not None:
                    self.fail(process.stderr.read().decode("utf-8", "replace"))
                try:
                    self.http(url, "GET", "/v1/refs")
                    break
                except urllib.error.URLError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.02)
            yield SimpleNamespace(url=url)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def http(self, url, method, path, value=None, expected=200, token=base.TOKEN):
        request = urllib.request.Request(url + path, method=method,
                                        data=None if value is None else json.dumps(value).encode(),
                                        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=8)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            self.assertEqual(response.status, expected, data.decode("utf-8", "replace"))
        return json.loads(data) if data and expected < 400 else data or None

    def active_session(self):
        records = json.loads(self.cmd("agent", "sessions").stdout)
        return next(r for r in records if r["root"] == self.root_key and r["reference"] == "asset")

    def lease(self, url, identifier):
        return self.http(url, "GET", "/v1/uploads/" + identifier)["lease"]

    def resume(self, url, identifier):
        return json.loads(self.cmd("agent", "resume", url, identifier, "--requests", 4, "--require-lease").stdout)

    def test_restart_keeps_uploads_protected_until_publication(self):
        self.setup_server()
        with self.serving() as remote:
            self.assertTrue(self.probe(remote, "publish", requests=4)["cancelled"])
            record = self.active_session()
            identifier = record["upload_lease"]
            self.assertIsInstance(identifier, str)
            self.assertEqual(self.lease(remote.url, identifier)["state"], "active")
            self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), {})
        orphan_file = self.root / "unrelated.bin"
        orphan_file.write_bytes(b"unrelated upload-lease garbage")
        orphan_id = json.loads(self.cmd("put", orphan_file, store=self.remote_store, encrypted=True).stdout)["manifest"]
        self.cmd("unpin", "files/" + orphan_id, store=self.remote_store, encrypted=True)
        for option in ((), ("--delete",)):
            result = json.loads(self.cmd("gc", *option, store=self.remote_store, encrypted=True).stdout)
            self.assertGreater(result["unreachable_objects"], 0)
            self.assertEqual(result["live_objects"], 13)
        self.assertEqual(len(json.loads(self.cmd("leases", "list", store=self.remote_store, encrypted=True).stdout)), 1)
        with self.serving() as remote:
            result = self.resume(remote.url, record["id"])
            self.assertEqual(result["transferred_objects"], 0)
            self.assertEqual(result["upload_lease"], identifier)
            self.assertEqual(self.lease(remote.url, identifier)["state"], "published")
        self.cmd("gc", "--delete", store=self.remote_store, encrypted=True)
        restored = self.root / "server-output.bin"
        self.cmd("get", self.identifier, restored, store=self.remote_store, encrypted=True)
        self.assertEqual(restored.read_bytes(), self.data)
        self.cmd("fsck", store=self.remote_store, encrypted=True)

    def test_expiry_allows_gc_and_resume_renegotiates_with_a_new_fence(self):
        self.setup_server()
        with self.serving(seconds=1) as remote:
            self.probe(remote, "publish", requests=4)
            record = self.active_session()
            old = self.lease(remote.url, record["upload_lease"])
        time.sleep(2.1)  # Whole-second expiry is rounded up by at most one second.
        self.assertGreater(json.loads(self.cmd("gc", "--delete", store=self.remote_store, encrypted=True).stdout)["unreachable_objects"], 0)
        with self.serving(seconds=1) as remote:
            result = self.resume(remote.url, record["id"])
            self.assertGreater(result["transferred_objects"], 0)
            lease = self.lease(remote.url, record["upload_lease"])
            self.assertNotEqual(lease["fence"], old["fence"])
            value = {"root": self.root_key, "expected": None,
                     "upload": {"id": lease["id"], "fence": old["fence"]}}
            self.http(remote.url, "PUT", "/v1/refs/asset", value, expected=412)
        removed = json.loads(self.cmd("leases", "prune", store=self.remote_store, encrypted=True).stdout)
        self.assertEqual(removed["removed"], 1)

    def test_short_leases_renew_while_bandwidth_limited(self):
        self.setup_server()
        with self.serving(seconds=1, strict=True) as remote:
            start = time.monotonic()
            result = self.probe(remote, "renew", requests=4)["result"]
            self.assertGreater(time.monotonic() - start, 4)
            self.assertEqual(self.lease(remote.url, result["upload_lease"])["state"], "published")

    def test_receipt_survives_lost_reply_and_a_later_reference_change(self):
        self.setup_server()
        with self.serving() as remote, base.server(lambda: Proxy(remote.url)) as proxy:
            command = [base.CLI, "--store", str(self.source), "agent", "push", proxy.url, self.source_ref, "asset", "--requests", "4"]
            process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                self.assertTrue(proxy.entered.wait(8))
                self.assertIsNone(process.poll())
                process.kill()
                process.communicate(timeout=5)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
                proxy.gate = False
                proxy.release.set()
            record = self.active_session()
            self.assertEqual(record["state"], "publishing")
            self.assertEqual(self.lease(remote.url, record["upload_lease"])["state"], "published")
            other = self.root / "newer.bin"
            other.write_bytes(b"a later remote revision")
            newer = json.loads(self.cmd("put", other).stdout)["manifest"]
            self.cmd("agent", "push", remote.url, "files/" + newer, "newer")
            self.http(remote.url, "PUT", "/v1/refs/asset", {"root": {"kind": "manifest", "id": newer},
                      "expected": self.root_key}, expected=204)
            refs_path = self.remote_store / "refs.json"
            timestamp = refs_path.stat().st_mtime_ns
            recovered = self.resume(proxy.url, record["id"])
            self.assertTrue(recovered["reconciled"])
            self.assertEqual(proxy.calls.count(("PUT", "/v1/refs/asset")), 1)
            self.assertEqual(refs_path.stat().st_mtime_ns, timestamp)
            self.assertEqual(self.http(remote.url, "GET", "/v1/refs")["asset"]["id"], newer)

    def test_strict_publication_auth_and_offline_release(self):
        self.setup_server()
        with self.serving(strict=True) as remote:
            self.http(remote.url, "GET", "/v1/capabilities", expected=401, token="invalid")
            self.http(remote.url, "GET", "/v1/uploads/" + "0" * 64, expected=401, token="invalid")
            self.http(remote.url, "PUT", "/v1/refs/asset", {"root": self.root_key, "expected": None}, expected=428)
            self.probe(remote, "publish")
            record = self.active_session()
        self.cmd("leases", "release", record["upload_lease"], store=self.remote_store, encrypted=True)
        self.cmd("gc", "--delete", store=self.remote_store, encrypted=True)
        result = json.loads(self.cmd("leases", "prune", store=self.remote_store, encrypted=True).stdout)
        self.assertEqual(result["removed"], 1)

    def test_delayed_renewal_does_not_block_object_io(self):
        self.setup_server()
        with self.serving(seconds=3) as remote, base.server(lambda: Proxy(remote.url)) as proxy:
            proxy.gate, proxy.hold_renewal = False, True
            try:
                result = self.probe(proxy, "renew", requests=4)
                self.assertTrue(proxy.renewal_entered.is_set())
                self.assertTrue(proxy.object_during_renewal.is_set())
                self.assertEqual(self.lease(remote.url, result["result"]["upload_lease"])["state"], "published")
            finally:
                proxy.renewal_release.set()

    def test_cancel_joins_an_inflight_renewal(self):
        self.setup_server()
        with self.serving(seconds=1) as remote, base.server(lambda: Proxy(remote.url)) as proxy:
            proxy.gate, proxy.hold_renewal = False, True
            try:
                start = time.monotonic()
                self.assertTrue(self.probe(proxy, "renew_stop", requests=4)["cancelled"])
                self.assertTrue(proxy.renewal_entered.is_set())
                self.assertLess(time.monotonic() - start, 3)
                self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), {})
            finally:
                proxy.renewal_release.set()

    def test_legacy_fallback_strict_client_and_no_silent_downgrade(self):
        with base.server() as remote:
            self.probe(remote, "publish")
            self.assertIsNone(self.active_session()["upload_lease"])
            before = list(remote.calls)
            self.cmd("agent", "push", remote.url, self.source_ref, "strict", "--require-lease", expected=1)
            self.assertEqual(sum(m == "PUT" and p.startswith("/v1/objects/") for m, p in before),
                             sum(m == "PUT" and p.startswith("/v1/objects/") for m, p in remote.calls))
            record = self.active_session()
            record["upload_lease"] = "a" * 64
            (self.source / "transfers" / (record["id"] + ".json")).write_text(json.dumps(record), encoding="utf-8")
            result = self.cmd("agent", "resume", remote.url, record["id"], expected=1)
            self.assertIn(b"no longer supports", result.stderr)
        with base.server(lambda: base.Repository(LegacyHandler)) as remote:
            result = self.probe(remote, "ok")["result"]
            self.assertIsNone(result["upload_lease"])
            self.assertEqual(remote.refs["asset"], self.root_key)

    def test_legacy_lease_keeps_its_global_hold_after_capability_upgrade(self):
        self.setup_server()
        with base.server(lambda: Proxy(f"http://127.0.0.1:{self.port}")) as proxy:
            proxy.gate, proxy.hide_inventory = False, True
            with self.serving():
                self.probe(proxy, "publish", requests=4)
                record = self.active_session()
            for option in ((), ("--delete",)):
                result = self.cmd("gc", *option, store=self.remote_store, encrypted=True, expected=1)
                self.assertIn(b"active upload leases", result.stderr)
            proxy.hide_inventory = False
            with self.serving() as remote:
                result = self.resume(proxy.url, record["id"])
                self.assertEqual(result["transferred_objects"], 0)
                self.assertEqual(self.lease(remote.url, record["upload_lease"])["version"], 1)
                self.assertFalse(any("/inventory/" in path for _, path in proxy.calls))

    def test_lost_inventory_ack_survives_agent_and_server_restart(self):
        self.setup_server()
        with self.serving() as remote:
            self.probe(remote, "ok", requests=4)  # Seed a complete version whose reference will be removed.
        self.cmd("unpin", "asset", store=self.remote_store, encrypted=True)
        with base.server(lambda: Proxy(f"http://127.0.0.1:{self.port}")) as proxy:
            proxy.gate, proxy.hold_inventory = False, True
            with self.serving() as remote:
                process = subprocess.Popen([base.CLI, "--store", str(self.source), "agent", "push", proxy.url,
                                            self.source_ref, "pending", "--require-lease"], env=self.env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                try:
                    self.assertTrue(proxy.inventory_entered.wait(8), "agent did not reach inventory acknowledgement")
                    self.assertIsNone(process.poll())
                    process.kill()
                    self.assertEqual(process.communicate(timeout=5)[0], b"")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout=5)
                    proxy.hold_inventory = False
                    proxy.inventory_release.set()
                record = next(r for r in json.loads(self.cmd("agent", "sessions").stdout) if r["reference"] == "pending")
                self.assertEqual(record["state"], "transferring")
                lease = self.lease(remote.url, record["upload_lease"])
                self.assertEqual(lease["inventory_pages"], 1)
                self.assertFalse(any(path == "/v1/missing" for _, path in proxy.calls))
            orphan = self.root / "inventory-unrelated.bin"
            orphan.write_bytes(b"unrelated to the registered inventory")
            identifier = json.loads(self.cmd("put", orphan, store=self.remote_store, encrypted=True).stdout)["manifest"]
            self.cmd("unpin", "files/" + identifier, store=self.remote_store, encrypted=True)
            result = json.loads(self.cmd("gc", "--delete", store=self.remote_store, encrypted=True).stdout)
            self.assertEqual(result["live_objects"], 13)
            self.assertGreater(result["unreachable_objects"], 0)
            with self.serving() as remote:
                result = self.resume(proxy.url, record["id"])
                self.assertEqual(result["transferred_objects"], 0)
                self.assertEqual(self.lease(remote.url, record["upload_lease"])["state"], "published")
            self.cmd("fsck", store=self.remote_store, encrypted=True)

    def test_inventory_auth_fencing_schema_and_no_capability_downgrade(self):
        self.setup_server()
        with self.serving() as remote, base.server(lambda: Proxy(remote.url)) as proxy:
            proxy.gate = False
            self.probe(proxy, "publish")
            record = self.active_session()
            lease = self.lease(remote.url, record["upload_lease"])
            path = "/v1/uploads/" + lease["id"] + "/inventory/0"
            self.http(remote.url, "PUT", path, {}, expected=401, token="invalid")
            self.http(remote.url, "PUT", path, {"fence": lease["fence"], "objects": []}, expected=400)
            self.http(remote.url, "PUT", path, {"fence": "0" * 64, "objects": [self.root_key]}, expected=412)
            self.http(remote.url, "PUT", path, {"fence": lease["fence"], "objects": [self.root_key]}, expected=400)
            proxy.hide_inventory = True
            before = list(proxy.calls)
            result = self.cmd("agent", "resume", proxy.url, record["id"], expected=1)
            self.assertIn(b"no longer supports the saved upload inventory", result.stderr)
            self.assertEqual(sum(path.startswith("/v1/objects/") for _, path in before),
                             sum(path.startswith("/v1/objects/") for _, path in proxy.calls))
            self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), {})


if __name__ == "__main__":
    unittest.main()
