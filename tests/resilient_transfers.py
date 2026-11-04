"""Transient failures, bounded retries, cancellation and shared traffic targets."""
from collections import Counter
import email.utils
import json
import socket
import time
import unittest
import network_operations as base
import parallel_transfers as parallel


class ResilientHandler(parallel.ParallelHandler):
    def authorized(self):
        if not super().authorized():
            return False
        with self.server.condition:
            self.server.attempts[self.command, self.path] += 1
            for rule in self.server.rules:
                if self.command == rule[0] and self.path.startswith(rule[1]) and rule[2]:
                    rule[2] -= 1
                    fault, retry_after = rule[3:]
                    break
            else:
                return True
        if self.command != "GET":
            payload = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if fault == "drop":
            # Store a complete object before losing its acknowledgement.
            if self.command == "PUT" and self.path.startswith("/v1/objects/"):
                self.server.objects[self.path] = payload
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
        elif fault == "partial":
            data = self.server.objects[self.path]
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data[:len(data) // 2])
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
        else:
            self.send_response(fault)
            self.send_header("Content-Length", "2")
            if retry_after is not None:
                self.send_header("Retry-After", retry_after)
            self.end_headers()
            self.wfile.write(b"{}")
        return False


class ResilientRepository(parallel.ParallelRepository):
    def __init__(self):
        # ParallelRepository's fixture initializer accepts no handler argument.
        super().__init__()
        self.RequestHandlerClass = ResilientHandler
        self.rules = []
        self.attempts = Counter()

    def fail(self, method, path, count=1, fault=503, retry_after=None):
        self.rules.append([method, path, count, fault, retry_after])


class ResilientTransfers(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe
    setUp = parallel.ParallelTransfers.setUp

    def push(self, remote, *options, name="asset", expected=0, lang="en"):
        return self.cmd("agent", "push", remote.url, self.source_ref, name, "--requests", 4,
                        *options, expected=expected, lang=lang)

    def pull(self, remote, *options, expected=0):
        return self.cmd("agent", "pull", remote.url, "asset", "received", "--requests", 4,
                        *options, store=self.target, encrypted=True, expected=expected)

    def test_transient_control_requests_and_objects_resume_with_bounded_buffers(self):
        with base.server(ResilientRepository) as remote:
            remote.fail("GET", "/v1/refs")
            remote.fail("POST", "/v1/missing", fault=429)
            remote.fail("PUT", "/v1/objects/", count=2)
            remote.fail("PUT", "/v1/objects/", fault="drop")
            result = json.loads(self.push(remote).stdout)
            self.assertEqual(result["object_retries"], 3)
            self.assertEqual(result["transferred_objects"], 13)
            self.assertLessEqual(result["peak_object_buffer_bytes"], 64 * 1024**2)
            self.assertEqual(remote.attempts["GET", "/v1/refs"], 2)
            self.assertEqual(remote.attempts["POST", "/v1/missing"], 2)
            self.assertEqual(remote.attempts["PUT", "/v1/refs/asset"], 1)
            remote.fail("GET", "/v1/refs", fault="drop")
            remote.fail("GET", "/v1/objects/chunk/", fault="partial")
            result = json.loads(self.pull(remote).stdout)
            self.assertEqual(result["object_retries"], 1)
            self.assertEqual(result["transferred_objects"], 13)
            destination = self.root / "restored.bin"
            self.cmd("get", self.identifier, destination, store=self.target, encrypted=True)
            self.assertEqual(destination.read_bytes(), self.data)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_status_policy_exhaustion_and_explicit_retry_disable(self):
        with base.server(ResilientRepository) as remote:
            for status in (401, 403, 404, 409, 501):
                remote.fail("GET", "/v1/refs", fault=status)
                before = remote.attempts["GET", "/v1/refs"]
                self.push(remote, expected=1)
                self.assertEqual(remote.attempts["GET", "/v1/refs"] - before, 1)
            for retries in (0, 2):
                remote.fail("GET", "/v1/refs", count=retries + 1)
                before = remote.attempts["GET", "/v1/refs"]
                result = self.push(remote, "--retries", retries, expected=1, lang="fr")
                self.assertIn("statut 503", result.stderr.decode())
                self.assertEqual(remote.attempts["GET", "/v1/refs"] - before, retries + 1)
            self.assertEqual(remote.refs, {})
            remote.fail("PUT", "/v1/objects/", count=100)
            result = self.push(remote, "--retries", 1, expected=1)
            self.assertEqual(result.stdout, b"")
            self.assertEqual(remote.refs, {})
            self.assertNotIn(("PUT", "/v1/refs/asset"), remote.calls)
            remote.drain()
            remote.rules.clear()
            self.push(remote)
            self.assertEqual(remote.refs["asset"], self.root_key)

    def test_reference_publication_is_never_retried(self):
        with base.server(ResilientRepository) as remote:
            for name, fault in (("lost", "drop"), ("unavailable", 503)):
                remote.fail("PUT", "/v1/refs/" + name, fault=fault)
                self.push(remote, name=name, expected=3)
                self.assertEqual(remote.attempts["PUT", "/v1/refs/" + name], 1)
            # Existing fixture also commits then disconnects/sends 500.
            remote.disconnect = True
            self.push(remote, name="committed", expected=3)
            self.assertEqual(remote.attempts["PUT", "/v1/refs/committed"], 1)
            self.assertEqual(remote.refs["committed"], self.root_key)

    def test_retry_after_and_exponential_backoff(self):
        with base.server(ResilientRepository) as remote:
            remote.fail("GET", "/v1/refs", retry_after="1")
            start = time.monotonic()
            self.push(remote)
            self.assertGreaterEqual(time.monotonic() - start, 0.95)
            remote.fail("GET", "/v1/refs", count=3)
            start = time.monotonic()
            self.push(remote)
            self.assertGreaterEqual(time.monotonic() - start, 1.65)  # .25 + .5 + 1
            # A past HTTP-date is accepted without adding an unbounded delay.
            remote.fail("GET", "/v1/refs", retry_after=email.utils.formatdate(0, usegmt=True))
            self.push(remote)

    def test_cancellation_during_backoff_and_bandwidth_waits(self):
        with base.server(ResilientRepository) as remote:
            remote.fail("PUT", "/v1/objects/chunk/", count=100)
            start = time.monotonic()
            self.assertTrue(self.probe(remote, "backoff", requests=4)["cancelled"])
            self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(remote.refs, {})

            remote.drain()
            remote.rules.clear()
            start = time.monotonic()
            self.assertTrue(self.probe(remote, "pace", requests=4)["cancelled"])
            self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(remote.refs, {})
            remote.drain()
            self.push(remote)
            for mode in ("backoff", "pace"):
                if mode == "backoff":
                    remote.fail("GET", "/v1/objects/chunk/", count=100)
                start = time.monotonic()
                self.assertTrue(self.probe(remote, mode, "pull", "received", requests=4)["cancelled"])
                self.assertLess(time.monotonic() - start, 1.5)
                self.assertEqual(json.loads(self.cmd("refs", store=self.target, encrypted=True).stdout), {})
                remote.drain()
                remote.rules.clear()
            self.pull(remote)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_rate_is_shared_by_parallel_requests_and_covers_small_object_bursts(self):
        with base.server(ResilientRepository) as remote:
            rate_kib = 384
            start = time.monotonic()
            pushed = json.loads(self.push(remote, "--upload-kib-s", rate_kib).stdout)
            elapsed = time.monotonic() - start
            minimum = pushed["transferred_bytes"] / (rate_kib * 1024)
            self.assertGreaterEqual(elapsed, minimum * .98)
            self.assertLess(elapsed, minimum + 8)
            start = time.monotonic()
            pulled = json.loads(self.pull(remote, "--download-kib-s", rate_kib).stdout)
            elapsed = time.monotonic() - start
            minimum = pulled["transferred_bytes"] / (rate_kib * 1024)
            self.assertGreaterEqual(elapsed, minimum * .98)
            self.assertLess(elapsed, minimum + 8)
            self.assertGreater(pulled["peak_object_requests"], 1)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_invalid_limits_fail_before_connecting(self):
        with base.server(ResilientRepository) as remote:
            for args in (("--retries", 9), ("--upload-kib-s", 1073741825),
                         ("--download-kib-s", 1073741825)):
                self.push(remote, *args, expected=1)
            self.assertEqual(remote.calls, [])


if __name__ == "__main__":
    unittest.main()
