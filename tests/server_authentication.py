"""Production HTTP authentication, body framing and rejection ordering."""
import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import select
import socket
import tempfile
import unittest

import server_upload_leases as leases


class ServerAuthentication(unittest.TestCase):
    cmd = leases.ServerUploadLeases.cmd
    setup_server = leases.ServerUploadLeases.setup_server
    serving = leases.ServerUploadLeases.serving
    http = leases.ServerUploadLeases.http

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="syrnck-server-auth-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_LANG", "SYRNCK_TOKEN")}
        self.env["SYRNCK_TOKEN"] = leases.base.TOKEN
        self.setup_server()

    def request(self, method, path, body=b"", headers=()):
        with contextlib.closing(http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)) as connection:
            connection.putrequest(method, path)
            connection.putheader("Content-Length", str(len(body)))
            for name, value in headers:
                connection.putheader(name, value)
            connection.endheaders(body)
            response = connection.getresponse()
            data = response.read()
            return response.status, response.headers, data

    def test_delayed_bodies_are_consumed_before_rejection(self):
        # Sending only headers exposes early rejection without relying on the
        # intermittent Windows reset when a body races the connection close.
        with self.serving():
            for path in ("/v1/missing", "/unknown"):
                for chunked in (False, True):
                    with self.subTest(path=path, chunked=chunked):
                        with socket.create_connection(("127.0.0.1", self.port), timeout=8) as sock:
                            body = b"not valid JSON" * 1024
                            framing = "Transfer-Encoding: chunked" if chunked else f"Content-Length: {len(body)}"
                            headers = (f"POST {path} HTTP/1.1\r\nHost: localhost\r\n{framing}\r\n"
                                       "Authorization: Bearer invalid\r\nContent-Type: application/json\r\n\r\n")
                            sock.sendall(headers.encode())
                            self.assertEqual(select.select([sock], [], [], .2)[0], [],
                                             "server rejected headers with the body still unread")
                            sock.sendall((f"{len(body):x}\r\n".encode() + body + b"\r\n0\r\n\r\n")
                                         if chunked else body)
                            with http.client.HTTPResponse(sock) as response:
                                response.begin()
                                self.assertEqual(response.status, 401, response.read())
                                self.assertEqual(response.getheader("WWW-Authenticate"), 'Bearer realm="Syrnck"')

    def test_missing_invalid_and_duplicate_credentials_cannot_access_storage(self):
        valid = ("Authorization", "Bearer " + leases.base.TOKEN)
        invalid = ("Authorization", "Bearer invalid")
        credentials = ((), (invalid,), (("Authorization", "Basic invalid"),),
                       (valid, invalid), (invalid, valid), (valid, valid))
        routes = (("GET", "/v1/refs"), ("HEAD", "/v1/capabilities"),
                  ("PUT", "/v1/objects/chunk/" + "0" * 64),
                  ("POST", "/v1/uploads/" + "0" * 64 + "/confirm"),
                  ("PUT", "/v1/refs/forbidden"), ("PATCH", "/unknown"),
                  ("DELETE", "/unknown"), ("OPTIONS", "/unknown"))
        with self.serving() as remote:
            before = {p.relative_to(self.remote_store): p.read_bytes()
                      for p in self.remote_store.rglob("*") if p.is_file() and p.name != "store.lock"}
            # Exercise simultaneous rejections, large unread TCP buffers and
            # coding headers that must not bypass or precede authentication.
            def rejected(item):
                method, path, auth = item
                return self.request(method, path, b"invalid body" * 8192,
                                    (*auth, ("Syrnck-Object-Encoding", "unsupported")))

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(rejected, ((method, path, auth)
                                    for method, path in routes for auth in credentials)))
            for status, headers, data in results:
                self.assertEqual(status, 401, data)
                self.assertEqual(headers.get("WWW-Authenticate"), 'Bearer realm="Syrnck"')
                self.assertNotIn(leases.base.TOKEN.encode(), data)
            self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), {})
            after = {p.relative_to(self.remote_store): p.read_bytes()
                     for p in self.remote_store.rglob("*") if p.is_file() and p.name != "store.lock"}
            self.assertEqual(before, after)

    def test_expect_continue_and_payload_limit(self):
        with self.serving():
            with socket.create_connection(("127.0.0.1", self.port), timeout=8) as sock:
                sock.sendall(b"PUT /unknown HTTP/1.1\r\nHost: localhost\r\n"
                             b"Authorization: Bearer invalid\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\n")
                reply = b""
                while not reply.endswith(b"\r\n\r\n"):
                    part = sock.recv(1024)
                    self.assertTrue(part, "server closed before 100 Continue")
                    reply += part
                self.assertEqual(reply, b"HTTP/1.1 100 Continue\r\n\r\n")
                sock.sendall(b"{}")
                with http.client.HTTPResponse(sock) as response:
                    response.begin()
                    self.assertEqual(response.status, 401, response.read())
            # HTTP framing/size validation precedes application authentication.
            for size, expected in ((16 * 1024**2, 401), (16 * 1024**2 + 1, 413)):
                status, _, data = self.request("POST", "/v1/missing", b"x" * size)
                self.assertEqual(status, expected, data)

    def test_valid_credentials_keep_route_and_validation_errors(self):
        auth = (("Authorization", "Bearer " + leases.base.TOKEN),)
        with self.serving():
            for method, path, body, headers, expected in (
                    ("GET", "/v1/capabilities", b"", auth, 200),
                    ("POST", "/v1/missing", b"invalid JSON", auth, 400),
                    ("POST", "/v1/missing", b"[]", auth, 200),
                    ("PUT", "/unknown", b"{}", auth, 404),
                    ("POST", "/v1/missing", b"[]", (*auth, ("Content-Encoding", "gzip")), 415)):
                status, _, data = self.request(method, path, body, headers)
                self.assertEqual(status, expected, data)

    def test_large_authenticated_reference_table_and_native_pull(self):
        asset = self.root / "asset.bin"
        asset.write_bytes(b"complete retained HTTP file version\n" * 64)
        result = self.cmd("put", asset, store=self.remote_store, encrypted=True)
        identifier = json.loads(result.stdout)["manifest"]
        expected = {f"snapshot/{i:08d}": {"kind": "manifest", "id": identifier} for i in range(16384)}
        (self.remote_store / "refs.json").write_text(json.dumps(expected), encoding="utf-8")
        client = self.root / "client"
        self.cmd("init", store=client)
        with self.serving() as remote:
            self.assertEqual(self.http(remote.url, "GET", "/v1/refs"), expected)
            self.cmd("agent", "pull", remote.url, "snapshot/00000000", "received", store=client)
        restored = self.root / "restored.bin"
        self.cmd("get", identifier, restored, store=client)
        self.assertEqual(restored.read_bytes(), asset.read_bytes())
        self.assertEqual(json.loads(self.cmd("refs", store=client).stdout), {"received": expected["snapshot/00000000"]})


if __name__ == "__main__":
    unittest.main()
