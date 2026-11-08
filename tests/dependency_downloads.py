"""Exercise verified build downloads against a local transient-error server."""
import hashlib
import faulthandler
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
from socketserver import TCPServer
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from fetch_dependencies import Archive, fetch_archive, read_manifest


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        index = self.server.requests
        self.server.requests += 1
        status = self.server.statuses[min(index, len(self.server.statuses) - 1)]
        body = self.server.payload if status == 200 else b"temporary upstream response"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class LoopbackServer(ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer otherwise performs reverse DNS via socket.getfqdn even
        # though this fixture only needs an ephemeral IPv4 loopback socket.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


class DependencyDownloads(unittest.TestCase):
    def setUp(self):
        # Keep real loopback HTTP independent of user/system proxy discovery.
        environment = patch.dict(os.environ, {"no_proxy": "*", "NO_PROXY": "*"})
        environment.start()
        self.addCleanup(environment.stop)
        self.temp = tempfile.TemporaryDirectory(prefix="syrnck-dependency-download-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.payload = b"verified pinned archive bytes\x00" * 1024
        self.digest = hashlib.sha256(self.payload).hexdigest()
        self.server = LoopbackServer(("127.0.0.1", 0), Handler)
        self.server.requests = 0
        self.server.statuses = [200]
        self.server.payload = self.payload
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.02), daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.archive = Archive("fixture", f"http://127.0.0.1:{self.server.server_port}/archive.tar.gz", self.digest)
        self.cache = self.root / "cache"

    def close_server(self):
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.server.server_close()

    def fetch(self):
        return fetch_archive(self.archive, self.cache, retries=2, delay=0, timeout=2)

    def test_loopback_fixture_does_not_resolve_hostnames(self):
        with patch("socket.getfqdn", side_effect=AssertionError("unexpected reverse DNS")):
            server = LoopbackServer(("127.0.0.1", 0), Handler)
            try:
                self.assertEqual(server.server_name, "127.0.0.1")
                self.assertGreater(server.server_port, 0)
            finally:
                server.server_close()

    def test_transient_504_then_success_verifies_and_publishes_once(self):
        self.server.statuses = [504, 504, 200]
        path, downloaded = self.fetch()
        self.assertTrue(downloaded)
        self.assertEqual(path.read_bytes(), self.payload)
        self.assertEqual(self.server.requests, 3)
        self.assertEqual(list(self.cache.iterdir()), [path])
        self.assertEqual(self.fetch(), (path, False))
        self.assertEqual(self.server.requests, 3)

    def test_exhausted_transient_errors_leave_no_trusted_archive(self):
        self.server.statuses = [503]
        with self.assertRaises(HTTPError) as error:
            self.fetch()
        self.assertEqual(error.exception.code, 503)
        self.assertEqual(self.server.requests, 3)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_permanent_404_is_not_retried(self):
        self.server.statuses = [404]
        with self.assertRaises(HTTPError):
            self.fetch()
        self.assertEqual(self.server.requests, 1)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_wrong_download_hash_is_refused_without_publishing_or_retry(self):
        self.server.payload = b"wrong archive"
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.fetch()
        self.assertEqual(self.server.requests, 1)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_bad_existing_cache_is_preserved_and_refused(self):
        self.cache.mkdir()
        path = self.cache / self.archive.filename
        path.write_bytes(b"preserve invalid evidence")
        with self.assertRaisesRegex(ValueError, "cached dependency checksum mismatch"):
            self.fetch()
        self.assertEqual(path.read_bytes(), b"preserve invalid evidence")
        self.assertEqual(self.server.requests, 0)

    def test_verified_cache_needs_no_upstream_connection(self):
        self.cache.mkdir()
        path = self.cache / self.archive.filename
        path.write_bytes(self.payload)
        self.server.statuses = [504]
        self.assertEqual(self.fetch(), (path, False))
        self.assertEqual(self.server.requests, 0)

    def fallback_archive(self, payload):
        return Archive("source", self.archive.url.replace("archive.tar.gz", "source.tar.gz"),
                       hashlib.sha256(payload).hexdigest(), "source.tar.gz")

    def test_exhausted_primary_uses_independently_verified_source_archive(self):
        payload = b"alternate archive representation"
        fallback = self.fallback_archive(payload)
        primary = Archive(self.archive.name, self.archive.url, self.archive.sha256, fallback=fallback)
        self.server.statuses = [504, 504, 504, 200]
        self.server.payload = payload
        path, downloaded = fetch_archive(primary, self.cache, retries=2, delay=0, timeout=2)
        self.assertTrue(downloaded)
        self.assertEqual(path.name, "source.tar.gz")
        self.assertEqual(path.read_bytes(), payload)
        self.assertEqual(self.server.requests, 4)
        self.assertFalse((self.cache / self.archive.filename).exists())
        self.assertEqual(fetch_archive(primary, self.cache), (path, False))
        self.assertEqual(self.server.requests, 4)

    def test_wrong_fallback_checksum_never_publishes(self):
        fallback = self.fallback_archive(b"expected alternative")
        primary = Archive(self.archive.name, self.archive.url, self.archive.sha256, fallback=fallback)
        self.server.statuses = [504, 504, 504, 200]
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            fetch_archive(primary, self.cache, retries=2, delay=0, timeout=2)
        self.assertEqual(self.server.requests, 4)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_permanent_or_integrity_errors_do_not_switch_to_fallback(self):
        fallback = self.fallback_archive(self.payload)
        primary = Archive(self.archive.name, self.archive.url, self.archive.sha256, fallback=fallback)
        for status, payload, error in [(404, self.payload, HTTPError), (200, b"bad content", ValueError)]:
            with self.subTest(status=status):
                self.server.requests = 0
                self.server.statuses = [status]
                self.server.payload = payload
                with self.assertRaises(error):
                    fetch_archive(primary, self.cache, retries=2, delay=0, timeout=2)
                self.assertEqual(self.server.requests, 1)
                self.assertEqual(list(self.cache.iterdir()), [])

    def test_verified_fallback_cache_does_not_contact_primary(self):
        fallback = self.fallback_archive(self.payload)
        primary = Archive(self.archive.name, self.archive.url, self.archive.sha256, fallback=fallback)
        self.cache.mkdir()
        path = self.cache / fallback.filename
        path.write_bytes(self.payload)
        self.server.statuses = [504]
        self.assertEqual(fetch_archive(primary, self.cache), (path, False))
        self.assertEqual(self.server.requests, 0)

    def test_invalid_fallback_cache_is_preserved_without_network(self):
        fallback = self.fallback_archive(self.payload)
        primary = Archive(self.archive.name, self.archive.url, self.archive.sha256, fallback=fallback)
        self.cache.mkdir()
        path = self.cache / fallback.filename
        path.write_bytes(b"preserve invalid alternative")
        with self.assertRaisesRegex(ValueError, "cached dependency checksum mismatch"):
            fetch_archive(primary, self.cache)
        self.assertEqual(path.read_bytes(), b"preserve invalid alternative")
        self.assertEqual(self.server.requests, 0)

    def test_manifest_requires_versioned_https_pins_without_credentials(self):
        current = read_manifest()
        self.assertEqual({item.name for item in current}, {"zstd", "curl"})
        source = {"version": 1, **{a.name: {"url": a.url, "sha256": a.sha256} for a in current}}
        manifest = self.root / "manifest.json"
        for bad in ["http://example.test/archive.tar.gz", "https://u:p@example.test/archive.tar.gz",
                    "https://@example.test/archive.tar.gz",
                    "https://example.test/archive.tar.gz?value=1", "https://example.test/archive.tar.gz#fragment"]:
            with self.subTest(url=bad):
                value = json.loads(json.dumps(source))
                value["zstd"]["url"] = bad
                manifest.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_manifest(manifest)
        source["version"] = True
        manifest.write_text(json.dumps(source), encoding="utf-8")
        with self.assertRaises(ValueError):
            read_manifest(manifest)


if __name__ == "__main__":
    faulthandler.dump_traceback_later(10, repeat=True)
    unittest.main()
