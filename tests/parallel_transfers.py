"""Bounded simultaneous libcurl requests with a real HTTP peer."""
from collections import Counter
import hashlib
import json
import threading
import time
import unittest
import network_operations as base


class ParallelHandler(base.Handler):
    def begin_object(self):
        self.tracked = self.path.startswith("/v1/objects/")
        if self.tracked:
            with self.server.condition:
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
                self.server.object_calls[self.command, self.path] += 1
            time.sleep(0.04)  # Controlled latency, not a throughput benchmark.

    def end_object(self):
        if getattr(self, "tracked", False):
            with self.server.condition:
                self.server.active -= 1
                self.tracked = False
                self.server.condition.notify_all()

    def do_GET(self):
        self.begin_object()
        try:
            super().do_GET()
        finally:
            self.end_object()

    def do_PUT(self):
        self.begin_object()
        try:
            super().do_PUT()
        finally:
            self.end_object()

    def reply(self, value, status=200):
        # Stop counting before the final response can cause the client to
        # schedule a replacement; avoids counting completed handlers as active.
        self.end_object()
        if self.command == "PUT" and self.path.startswith("/v1/objects/") and self.server.oversized:
            value = b"x" * 4097
        super().reply(value, status)


class ParallelRepository(base.Repository):
    def __init__(self):
        super().__init__(ParallelHandler)
        self.condition = threading.Condition()
        self.active, self.peak = 0, 0
        self.object_calls = Counter()
        self.oversized = False

    def drain(self):
        with self.condition:
            if not self.condition.wait_for(lambda: self.active == 0, timeout=4):
                raise AssertionError("HTTP handlers did not finish")


class ParallelTransfers(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe

    def setUp(self):
        base.NetworkOperations.setUp(self)
        self.data = b"".join(hashlib.sha256(bytes([i])).digest() * 2048 for i in range(12))
        source = self.root / "parallel.bin"
        source.write_bytes(self.data)
        self.identifier = json.loads(self.cmd("put", source, "--fixed-size", 65536).stdout)["manifest"]
        self.root_key = {"kind": "manifest", "id": self.identifier}
        self.source_ref = "files/" + self.identifier

    def test_parallel_push_pull_and_deduplicated_inflight_objects(self):
        with base.server(ParallelRepository) as remote:
            pushed = self.probe(remote, "ok", requests=4)
            self.assertEqual(pushed["result"]["transferred_objects"], 13)
            self.assertEqual(pushed["result"]["peak_object_requests"], 4)
            self.assertGreater(remote.peak, 1)
            self.assertLessEqual(remote.peak, 4)
            self.assertLessEqual(pushed["result"]["peak_object_buffer_bytes"], 64 * 1024**2)
            self.assertTrue(all(n == 1 for (method, _), n in remote.object_calls.items() if method == "PUT"))
            remote.peak = 0
            pulled = self.probe(remote, "ok", "pull", "received", requests=4)
            self.assertEqual(pulled["result"]["peak_object_requests"], 4)
            self.assertEqual(pulled["result"]["peak_object_buffer_bytes"], 64 * 1024**2)
            self.assertGreater(remote.peak, 1)
            self.assertLessEqual(remote.peak, 4)
            self.assertTrue(all(n == 1 for (method, _), n in remote.object_calls.items() if method == "GET"))
            target = self.root / "restored.bin"
            self.cmd("get", self.identifier, target, store=self.target, encrypted=True)
            self.assertEqual(target.read_bytes(), self.data)
            again = self.probe(remote, "ok", "pull", "received", requests=4)
            self.assertEqual(again["result"]["transferred_objects"], 0)
            self.assertEqual(again["result"]["peak_object_requests"], 0)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_buffer_budget_limits_concurrency_and_invalid_options_never_connect(self):
        with base.server(ParallelRepository) as remote:
            for args in (("--requests", 0), ("--requests", 33), ("--transfer-buffer-mib", 16),
                         ("--transfer-buffer-mib", 1025)):
                self.cmd("agent", "push", remote.url, self.source_ref, "invalid", *args, expected=1)
            self.assertEqual(remote.calls, [])
            self.probe(remote, "ok", requests=4)
            remote.peak = 0
            pulled = self.probe(remote, "ok", "pull", "received", requests=4, buffer_mib=17)
            self.assertEqual(pulled["result"]["peak_object_requests"], 1)
            self.assertEqual(remote.peak, 1)
            self.assertLessEqual(pulled["result"]["peak_object_buffer_bytes"], 17 * 1024**2)

    def test_cancel_and_observer_exception_abort_the_pool_without_publishing(self):
        with base.server(ParallelRepository) as remote:
            for mode in ("transfer", "throw", "publish"):
                remote.drain()
                remote.objects.clear()
                result = self.probe(remote, mode, requests=4)
                self.assertTrue(result.get("cancelled") or result.get("observer_error"))
                remote.drain()
                self.assertEqual(remote.refs, {})
                self.assertNotIn(("PUT", "/v1/refs/asset"), remote.calls)
            resumed = self.probe(remote, "ok", requests=4)
            self.assertEqual(resumed["result"]["transferred_objects"], 0)
            self.assertEqual(remote.refs["asset"], self.root_key)
            previous = self.root / "previous.bin"
            previous.write_bytes(b"retained previous version")
            old = json.loads(self.cmd("put", previous, store=self.target, encrypted=True).stdout)["manifest"]
            self.cmd("pin", "received", "manifest", old, store=self.target, encrypted=True)
            before = self.cmd("refs", store=self.target, encrypted=True).stdout
            for mode in ("transfer", "throw", "publish"):
                result = self.probe(remote, mode, "pull", "received", requests=4)
                self.assertTrue(result.get("cancelled") or result.get("observer_error"))
                remote.drain()
                self.assertEqual(self.cmd("refs", store=self.target, encrypted=True).stdout, before)
                restored = self.root / f"previous-{mode}.bin"
                self.cmd("get", old, restored, store=self.target, encrypted=True)
                self.assertEqual(restored.read_bytes(), b"retained previous version")
            self.probe(remote, "ok", "pull", "received", requests=4)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_corruption_and_oversized_upload_replies_do_not_publish(self):
        with base.server(ParallelRepository) as remote:
            remote.oversized = True
            self.assertTrue(self.probe(remote, "overlong", requests=4)["oversized_response"])
            remote.drain()
            self.assertEqual(remote.refs, {})
            remote.oversized = False
            self.probe(remote, "ok", requests=4)
            remote.corrupt_download = True
            result = self.probe(remote, "corrupt", "pull", "received", requests=4)
            self.assertTrue(result["corrupt_response"])
            remote.drain()
            self.assertEqual(json.loads(self.cmd("refs", store=self.target, encrypted=True).stdout), {})
            remote.corrupt_download = False
            self.probe(remote, "ok", "pull", "received", requests=4)
            self.cmd("fsck", store=self.target, encrypted=True)


if __name__ == "__main__":
    unittest.main()
