"""Negotiated object frames against a real server and a fault-injecting peer."""
import http.client
import json
import random
import socket
import unittest
import urllib.error
import urllib.request
import network_operations as base
import parallel_transfers as parallel
import server_upload_leases as production


class CompressedHandler(base.Handler):
    def object_reply(self, data, encoding):
        if self.server.fault == "truncated":
            data = data[:-1]
        elif self.server.fault == "trailing":
            data += data
        elif self.server.fault == "corrupt":
            data = data[:-1] + bytes([data[-1] ^ 1])
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        if encoding:
            self.send_header("Syrnck-Object-Encoding", "br" if self.server.fault == "encoding" else encoding)
            if self.server.fault == "duplicate":
                self.send_header("Syrnck-Object-Encoding", encoding)
        if self.server.fault == "http":
            self.send_header("Content-Encoding", "gzip")
        self.end_headers()
        if self.server.cut_download and encoding:
            self.server.cut_download = False
            data = data[:len(data) // 2]
            self.server.downloaded += len(data)
            self.wfile.write(data)
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
            return
        self.server.downloaded += len(data)
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_GET(self):
        if self.path == "/v1/capabilities":
            if self.authorized():
                self.reply(self.server.caps, self.server.caps_status)
        elif self.path.startswith("/v1/objects/"):
            if self.authorized():
                self.server.gets.append(self.path)
                encoding = self.server.encodings.get(self.path)
                if encoding:
                    assert self.headers.get("Syrnck-Accept-Object-Encoding") == "zstd"
                self.object_reply(self.server.objects[self.path], encoding)
        else:
            super().do_GET()

    def do_PUT(self):
        if not self.path.startswith("/v1/objects/"):
            return super().do_PUT()
        if not self.authorized():
            return
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        encoding = self.headers.get("Syrnck-Object-Encoding")
        self.server.uploads.append((self.path, data, encoding))
        self.server.objects[self.path] = data
        self.server.encodings[self.path] = encoding
        retry = self.server.retry_upload
        self.server.retry_upload = False
        self.reply({}, 503 if retry else 201)


class CompressedRepository(base.Repository):
    def __init__(self):
        super().__init__(CompressedHandler)
        self.caps = {"upload_leases": 0, "read_leases": 0, "object_zstd": 1}
        self.caps_status = 200
        self.encodings, self.uploads, self.gets = {}, [], []
        self.fault = ""
        self.retry_upload, self.cut_download = False, False
        self.downloaded = 0


class CompressedTransfers(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe
    setUp = parallel.ParallelTransfers.setUp
    tearDown = base.NetworkOperations.tearDown
    setup_server = production.ServerUploadLeases.setup_server
    serving = production.ServerUploadLeases.serving
    http = production.ServerUploadLeases.http

    def push(self, remote, name="asset", *options):
        return json.loads(self.cmd("agent", "push", remote.url, self.source_ref, name,
                                  "--requests", 4, *options).stdout)

    def pull(self, remote, *options, expected=0):
        return self.cmd("agent", "pull", remote.url, "asset", "received", "--requests", 4,
                        *options, store=self.target, encrypted=True, expected=expected)

    def restore(self):
        target = self.root / ("restored-" + self.identifier + ".bin")
        self.cmd("get", self.identifier, target, store=self.target, encrypted=True)
        self.assertEqual(target.read_bytes(), self.data)

    def test_parallel_logical_and_wire_counts_retry_and_partial_frame_restart(self):
        with base.server(CompressedRepository) as remote:
            remote.retry_upload = True
            pushed = self.push(remote, "asset", "--compression", "zstd")
            self.assertEqual(pushed["object_retries"], 1)
            self.assertEqual(pushed["transferred_objects"], 13)
            self.assertLess(pushed["object_upload_bytes"], pushed["transferred_bytes"] // 10)
            self.assertEqual(pushed["object_upload_bytes"], sum(len(data) for _, data, _ in remote.uploads))
            self.assertEqual(pushed["object_download_bytes"], len(remote.uploads) * 2)
            remote.cut_download = True
            pulled = json.loads(self.pull(remote, "--compression", "zstd", "--transfer-buffer-mib", 17).stdout)
            self.assertEqual(pulled["object_retries"], 1)
            self.assertEqual(pulled["transferred_bytes"], pushed["transferred_bytes"])
            self.assertEqual(pulled["object_download_bytes"], remote.downloaded)
            self.assertLess(pulled["object_download_bytes"], pulled["transferred_bytes"] // 10)
            self.assertLessEqual(pulled["peak_object_buffer_bytes"], 17 * 1024**2)
            self.restore()

    def test_maximum_objects_fit_minimum_budget_with_compression_or_raw_fallback(self):
        with base.server(CompressedRepository) as remote:
            for compressible in (True, False):
                size = 16 * 1024**2
                self.data = b"x" * size if compressible else random.Random(42).randbytes(size)
                source = self.root / "maximum.bin"
                source.write_bytes(self.data)
                self.identifier = json.loads(self.cmd("put", source, "--fixed-size", size).stdout)["manifest"]
                self.source_ref = "files/" + self.identifier
                pushed = self.push(remote, "asset", "--compression", "zstd", "--transfer-buffer-mib", 17)
                self.assertEqual(pushed["transferred_objects"], 2)
                self.assertLessEqual(pushed["peak_object_buffer_bytes"], 17 * 1024**2)
                if compressible:
                    self.assertLess(pushed["object_upload_bytes"], size // 10)
                else:
                    self.assertEqual(pushed["object_upload_bytes"], pushed["transferred_bytes"])
                pulled = json.loads(self.pull(remote, "--transfer-buffer-mib", 17).stdout)
                self.assertEqual(pulled["transferred_bytes"], pushed["transferred_bytes"])
                self.assertLessEqual(pulled["peak_object_buffer_bytes"], 17 * 1024**2)
                self.restore()

    def test_bad_frames_and_headers_never_publish_and_resume_verified_data(self):
        with base.server(CompressedRepository) as remote:
            self.push(remote)
            for fault in ("truncated", "trailing", "corrupt", "encoding", "duplicate", "http"):
                remote.fault = fault
                before = len(remote.gets)
                self.pull(remote, "--compression", "zstd", expected=1)
                self.assertEqual(json.loads(self.cmd("refs", store=self.target, encrypted=True).stdout), {})
                self.assertEqual(len(remote.gets), before + 1)  # Format errors are never retried.
            remote.fault = ""
            self.pull(remote)
            self.restore()
            before = len(remote.calls)
            result = json.loads(self.pull(remote).stdout)
            self.assertEqual(result["transferred_objects"], 0)
            self.assertEqual(result["object_download_bytes"], 0)
            self.assertFalse(any(path == "/v1/capabilities" for _, path in remote.calls[before:]))

    def test_legacy_disabled_required_and_malformed_negotiation(self):
        with base.server(CompressedRepository) as remote:
            for index, caps in enumerate(({"upload_leases": 0}, {"upload_leases": 0, "object_zstd": 0})):
                remote.caps = caps
                remote.objects.clear()
                self.push(remote, "legacy" + str(index))
                self.assertTrue(all(value is None for value in remote.encodings.values()))
                remote.objects.clear()
                self.cmd("agent", "push", remote.url, self.source_ref, "required" + str(index),
                         "--compression", "zstd", expected=1)
                self.assertEqual(remote.objects, {})
            remote.caps_status = 404
            self.push(remote, "old")
            remote.objects.clear()
            remote.caps_status = 200
            for index, value in enumerate((True, 2, -1, "1", None)):
                remote.caps = {"upload_leases": 0, "object_zstd": value}
                self.cmd("agent", "push", remote.url, self.source_ref, "bad" + str(index), expected=1)
                self.assertEqual(remote.objects, {})
            self.push(remote, "off", "--compression", "off")
            self.assertTrue(all(value is None for value in remote.encodings.values()))
            self.cmd("agent", "push", remote.url, self.source_ref, "invalid", "--compression", "gzip", expected=1)

    def test_real_encrypted_server_identity_and_compressed_representations(self):
        self.setup_server()
        with self.serving() as remote:
            pushed = self.push(remote, "asset", "--compression", "zstd", "--require-lease")
            self.assertLess(pushed["object_upload_bytes"], pushed["transferred_bytes"] // 10)
            pulled = json.loads(self.pull(remote, "--compression", "zstd", "--require-read-lease").stdout)
            self.assertLess(pulled["object_download_bytes"], pulled["transferred_bytes"] // 10)
            self.restore()
            path = "/v1/objects/manifest/" + self.identifier
            def request(method="GET", data=None, headers=None, expected=200, target=path):
                req = urllib.request.Request(remote.url + target, data=data, method=method,
                    headers={"Authorization": "Bearer " + base.TOKEN, **(headers or {})})
                try:
                    response = urllib.request.urlopen(req, timeout=5)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    body = response.read()
                    self.assertEqual(response.status, expected, body)
                    return body, response.headers
            raw, headers = request()
            self.assertNotIn("Syrnck-Object-Encoding", headers)
            encoded, headers = request(headers={"Syrnck-Accept-Object-Encoding": "zstd"})
            self.assertEqual(headers["Syrnck-Object-Encoding"], "zstd")
            self.assertLess(len(encoded), len(raw))
            request("PUT", encoded, {"Syrnck-Object-Encoding": "zstd"}, 204)
            for bad in (encoded[:-1], encoded + encoded, b"invalid frame"):
                request("PUT", bad, {"Syrnck-Object-Encoding": "zstd"}, 400)
            request("PUT", encoded, {"Syrnck-Object-Encoding": "br"}, 415)
            request("PUT", encoded, {"Syrnck-Object-Encoding": "zstd"}, 415, "/v1/refs/invalid")
            # Unsupported HTTP content codings are rejected by httplib before
            # it reads a body; use an empty body to observe its status reliably.
            request("PUT", b"", {"Content-Encoding": "zstd"}, 415)
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
            try:
                for _ in range(3):
                    connection.putrequest("PUT", path)
                    connection.putheader("Authorization", "Bearer " + base.TOKEN)
                    connection.putheader("Content-Length", str(len(encoded)))
                    connection.putheader("Syrnck-Object-Encoding", "zstd")
                    connection.putheader("Syrnck-Object-Encoding", "zstd")
                    connection.endheaders(encoded)
                    with connection.getresponse() as response:
                        self.assertEqual(response.status, 415)
                        response.read()
                    connection.request("GET", path, headers={"Authorization": "Bearer " + base.TOKEN})
                    with connection.getresponse() as response:
                        self.assertEqual(response.status, 200)
                        self.assertEqual(response.read(), raw)
            finally:
                connection.close()
            self.assertEqual(request()[0], raw)
        self.cmd("fsck", store=self.remote_store, encrypted=True)

    def test_server_can_disable_compression_and_clients_keep_identity(self):
        self.setup_server()
        with self.serving(compression=False) as remote:
            self.cmd("agent", "push", remote.url, self.source_ref, "required", "--compression", "zstd", expected=1)
            pushed = self.push(remote)
            self.assertEqual(pushed["object_upload_bytes"], pushed["transferred_bytes"])
            pulled = json.loads(self.pull(remote).stdout)
            self.assertEqual(pulled["object_download_bytes"], pulled["transferred_bytes"])
            self.restore()


if __name__ == "__main__":
    unittest.main()
