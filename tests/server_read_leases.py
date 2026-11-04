"""Persistent source protection, interrupted pulls, renewal and release over real HTTP."""
import json
import os
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
import network_operations as base
import parallel_transfers as parallel
import server_upload_leases as uploads


class ReadProxyHandler(base.Handler):
    def forward(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))) if self.command != "GET" else None
        with self.server.condition:
            self.server.active += 1
        try:
            if self.command == "GET" and self.path.startswith("/v1/objects/chunk/") and self.server.gate:
                self.server.entered.set()
                self.server.release.wait(8)
            request = urllib.request.Request(self.server.target + self.path, data=body, method=self.command,
                                            headers={"Authorization": "Bearer " + base.TOKEN,
                                                     "Content-Type": "application/json"})
            try:
                response = urllib.request.urlopen(request, timeout=8)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                status, data = response.status, response.read()
            self.server.calls.append((self.command, self.path))
            if self.command == "PUT" and self.path.startswith("/v1/reads/"):
                if self.server.calls.count((self.command, self.path)) == 2 and self.server.hold_renewal:
                    self.server.renewal_entered.set()
                    self.server.renewal_release.wait(5)
            elif self.command == "GET" and self.path.startswith("/v1/objects/"):
                if self.server.renewal_entered.is_set():
                    self.server.object_during_renewal.set()
                    if self.server.release_on_object:
                        self.server.renewal_release.set()
            if self.command == "POST" and self.path.endswith("/release") and self.server.drop_release:
                self.server.drop_release = False
                self.close_connection = True  # Applied by the production server, reply lost.
                return
            self.reply(data, status)
        finally:
            with self.server.condition:
                self.server.active -= 1
                self.server.condition.notify_all()

    do_GET = do_POST = do_PUT = forward


class ReadProxy(base.Repository):
    def __init__(self, target):
        super().__init__(ReadProxyHandler)
        self.target, self.gate, self.drop_release, self.hold_renewal = target, False, False, False
        self.release_on_object = True
        self.condition, self.active = threading.Condition(), 0
        self.renewal_entered, self.renewal_release, self.object_during_renewal = threading.Event(), threading.Event(), threading.Event()

    def drain(self):
        self.gate = False
        self.release.set()
        self.renewal_release.set()
        with self.condition:
            if not self.condition.wait_for(lambda: self.active == 0, timeout=10):
                raise AssertionError("read proxy handlers did not finish")


class CapabilitiesHandler(base.Handler):
    def do_GET(self):
        if self.path == "/v1/capabilities":
            if self.authorized():
                self.reply(self.server.capabilities)
        else:
            super().do_GET()


class ServerReadLeases(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe
    setUp = parallel.ParallelTransfers.setUp
    setup_server = uploads.ServerUploadLeases.setup_server
    serving = uploads.ServerUploadLeases.serving
    http = uploads.ServerUploadLeases.http

    def setup_source(self):
        self.setup_server()
        with self.serving() as remote:
            self.probe(remote, "ok", requests=4)

    def lease(self, url, identifier):
        return self.http(url, "GET", "/v1/reads/" + identifier)["lease"]

    def session(self):
        record, = json.loads(self.cmd("agent", "sessions", store=self.target, encrypted=True).stdout)
        return record

    def resume(self, url, expected=0):
        return self.cmd("agent", "resume", url, self.session()["id"], "--require-read-lease", "--requests", 4,
                        store=self.target, encrypted=True, expected=expected)

    def kill_pull(self, remote):
        remote.gate = True
        env = dict(self.env, SYRNCK_KEY="39" * 32)
        process = subprocess.Popen([base.CLI, "--store", str(self.target), "agent", "pull", remote.url,
                                    "asset", "received", "--requests", "4", "--require-read-lease"],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            self.assertTrue(remote.entered.wait(8), "agent did not reach the chunk request gate")
            self.assertIsNone(process.poll())
            process.kill()
            output, _ = process.communicate(timeout=5)
            self.assertEqual(output, b"")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            remote.drain()

    def verify_output(self):
        output = self.root / "read-output.bin"
        self.cmd("get", self.identifier, output, store=self.target, encrypted=True)
        self.assertEqual(output.read_bytes(), self.data)
        self.cmd("fsck", store=self.target, encrypted=True)

    def test_killed_agent_and_server_keep_unpinned_root_available(self):
        self.setup_source()
        with base.server(lambda: ReadProxy(f"http://127.0.0.1:{self.port}")) as proxy:
            with self.serving() as remote:
                self.kill_pull(proxy)
                record = self.session()
                self.assertEqual(record["state"], "transferring")
                self.assertEqual(self.lease(remote.url, record["read_lease"])["state"], "active")
                self.assertNotIn(base.TOKEN, json.dumps(record))
                self.assertNotIn(proxy.url, json.dumps(record))
            self.cmd("unpin", "asset", store=self.remote_store, encrypted=True)
            for flags in ((), ("--delete",)):
                error = self.cmd("gc", *flags, store=self.remote_store, encrypted=True, expected=1, lang="fr")
                self.assertIn("baux de lecture actifs", error.stderr.decode())
            with self.serving() as remote:
                result = json.loads(self.resume(proxy.url).stdout)
                self.assertGreater(result["reused_objects"], 0)
                self.assertGreater(result["transferred_objects"], 0)
                self.assertEqual(self.lease(remote.url, record["read_lease"])["state"], "released")
                self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), {})
            self.assertGreater(json.loads(self.cmd("gc", "--delete", store=self.remote_store, encrypted=True).stdout)["unreachable_objects"], 0)
            self.verify_output()

    def test_expired_hold_allows_gc_and_missing_source_fails_without_publication(self):
        self.setup_source()
        with base.server(lambda: ReadProxy(f"http://127.0.0.1:{self.port}")) as proxy:
            with self.serving() as remote:
                self.kill_pull(proxy)
            record = self.session()
            path = self.remote_store / "reads" / (record["read_lease"] + ".json")
            lease = json.loads(path.read_text(encoding="utf-8"))
            lease["expires_at"] = 1
            path.write_text(json.dumps(lease), encoding="utf-8")
            self.cmd("unpin", "asset", store=self.remote_store, encrypted=True)
            self.cmd("gc", "--delete", store=self.remote_store, encrypted=True)
            self.assertEqual(json.loads(path.read_text())["state"], "expired")
            with self.serving():
                self.resume(proxy.url, expected=1)
            self.assertEqual(json.loads(self.cmd("refs", store=self.target, encrypted=True).stdout), {})
            self.assertEqual(self.session()["state"], "transferring")

    def test_expired_hold_can_reacquire_a_still_retained_source(self):
        self.setup_source()
        with base.server(lambda: ReadProxy(f"http://127.0.0.1:{self.port}")) as proxy:
            with self.serving():
                self.kill_pull(proxy)
            path = self.remote_store / "reads" / (self.session()["read_lease"] + ".json")
            old = json.loads(path.read_text())
            old["expires_at"] = 1
            path.write_text(json.dumps(old), encoding="utf-8")
            self.cmd("gc", "--delete", store=self.remote_store, encrypted=True)
            with self.serving() as remote:
                self.resume(proxy.url)
                self.assertNotEqual(self.lease(remote.url, old["id"])["fence"], old["fence"])
            self.verify_output()

    def test_renewal_does_not_block_downloads_and_lost_release_reply_is_retried(self):
        self.setup_source()
        with self.serving(read_seconds=3) as remote, base.server(lambda: ReadProxy(remote.url)) as proxy:
            proxy.hold_renewal, proxy.drop_release = True, True
            try:
                result = self.probe(proxy, "renew", "pull", "received", requests=4)["result"]
                self.assertTrue(proxy.renewal_entered.is_set())
                self.assertTrue(proxy.object_during_renewal.is_set())
                self.assertEqual(self.lease(remote.url, result["read_lease"])["state"], "released")
                self.assertEqual(sum(m == "POST" and p.endswith("/release") for m, p in proxy.calls), 2)
            finally:
                proxy.drain()
        self.verify_output()

    def test_cancellation_joins_a_pending_read_renewal(self):
        self.setup_source()
        with self.serving(read_seconds=1) as remote, base.server(lambda: ReadProxy(remote.url)) as proxy:
            proxy.hold_renewal = True
            proxy.release_on_object = False
            try:
                start = time.monotonic()
                self.assertTrue(self.probe(proxy, "renew_stop", "pull", requests=4)["cancelled"])
                self.assertTrue(proxy.renewal_entered.is_set())
                self.assertLess(time.monotonic() - start, 3)
                self.assertEqual(json.loads(self.cmd("refs", store=self.target, encrypted=True).stdout), {})
            finally:
                proxy.drain()

    def test_release_precedes_local_publication_and_completed_resume_is_offline(self):
        self.setup_source()
        with self.serving() as remote:
            self.assertTrue(self.probe(remote, "publish", "pull", "received", requests=4)["cancelled"])
            identifier = self.session()["read_lease"]
            self.assertEqual(self.lease(remote.url, identifier)["state"], "released")
            result = json.loads(self.resume(remote.url).stdout)
            self.assertEqual(result["transferred_objects"], 0)
        result = json.loads(self.resume(remote.url).stdout)  # No server now.
        self.assertTrue(result["reconciled"])
        self.verify_output()

    def test_protocol_auth_fencing_admin_release_and_prune(self):
        self.setup_source()
        with self.serving() as remote:
            identifier = "a" * 64
            path = "/v1/reads/" + identifier
            self.http(remote.url, "GET", path, expected=401, token="incorrect")
            self.http(remote.url, "PUT", path, {"root": self.root_key, "fence": None, "extra": 1}, expected=400)
            lease = self.http(remote.url, "PUT", path, {"root": self.root_key, "fence": None})["lease"]
            self.http(remote.url, "POST", path + "/release", {"fence": "0" * 64}, expected=412)
            self.http(remote.url, "PUT", path, {"root": self.root_key, "fence": "0" * 64}, expected=412)
        leases = json.loads(self.cmd("leases", "list", "--read", store=self.remote_store, encrypted=True).stdout)
        self.assertEqual(leases, [lease])
        self.cmd("leases", "release", identifier, "--read", store=self.remote_store, encrypted=True)
        result = json.loads(self.cmd("leases", "prune", "--read", store=self.remote_store, encrypted=True).stdout)
        self.assertEqual(result["removed"], 1)

    def test_legacy_fallback_and_strict_mode_fail_before_downloads(self):
        for index, factory in enumerate((base.Repository, lambda: base.Repository(uploads.LegacyHandler))):
            self.target = self.root / ("legacy-target-" + str(index))
            self.cmd("init", "--encrypted", store=self.target, encrypted=True)
            with base.server(factory) as remote:
                self.probe(remote, "ok")
                result = self.cmd("agent", "pull", remote.url, "asset", "strict", "--require-read-lease",
                                  store=self.target, encrypted=True, expected=1)
                self.assertIn(b"does not support read leases", result.stderr)
                self.assertFalse(any(m == "GET" and p.startswith("/v1/objects/") for m, p in remote.calls))
                record = self.session_for_reference("strict")
                record["read_lease"] = "a" * 64
                path = self.target / "transfers" / (record["id"] + ".json")
                path.write_text(json.dumps(record), encoding="utf-8")
                result = self.cmd("agent", "resume", remote.url, record["id"], store=self.target,
                                  encrypted=True, expected=1)
                self.assertIn(b"no longer supports the saved read lease", result.stderr)
                self.cmd("agent", "pull", remote.url, "asset", "received", store=self.target, encrypted=True)
                self.assertIsNone(self.session_for_reference("received")["read_lease"])
                # A local copy with no missing objects needs no source lease or network object reads.
                self.cmd("agent", "pull", remote.url, "asset", "local", "--require-read-lease",
                         store=self.target, encrypted=True)

    def test_explicit_disable_and_malformed_capabilities(self):
        with base.server(lambda: base.Repository(CapabilitiesHandler)) as remote:
            remote.capabilities = {"upload_leases": 0, "read_leases": 0}
            self.probe(remote, "ok")
            for bad in ({}, {"read_leases": True}, {"read_leases": 2}, {"read_leases": "1"}):
                remote.capabilities = bad
                self.cmd("agent", "pull", remote.url, "asset", "received", store=self.target,
                         encrypted=True, expected=1)
            self.assertFalse(any(m == "GET" and p.startswith("/v1/objects/") for m, p in remote.calls))
            remote.capabilities = {"read_leases": 0}
            self.cmd("agent", "pull", remote.url, "asset", "received", store=self.target, encrypted=True)

    def session_for_reference(self, name):
        records = json.loads(self.cmd("agent", "sessions", store=self.target, encrypted=True).stdout)
        return next(r for r in records if r["reference"] == name)


if __name__ == "__main__":
    unittest.main()
