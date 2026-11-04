"""Real HTTP transfers, cancellation, resume and uncertain CAS replies."""
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())
PROBE = str(Path(sys.argv.pop(1)).resolve())
RELAY = str(Path(sys.argv.pop(1)).resolve()) if os.name == "nt" else None
TOKEN = "syrnck-test-token-0123456789-0123456789"


class Repository(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, handler=None):
        self.objects, self.refs, self.calls = {}, {}, []
        self.idle, self.disconnect, self.server_error, self.conflict = False, False, False, False
        self.publish_delay = 0
        self.corrupt_download = False
        self.entered, self.release = threading.Event(), threading.Event()
        super().__init__(("127.0.0.1", 0), handler or Handler)

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value, status=200):
        data = value if isinstance(value, bytes) else json.dumps(value).encode()
        try:
            self.send_response(status)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def authorized(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.reply({}, 401)
            return False
        self.server.calls.append((self.command, self.path))
        return True

    def do_GET(self):
        if not self.authorized():
            return
        if self.path == "/v1/refs":
            if self.server.idle:
                self.server.entered.set()
                self.server.release.wait(8)
            self.reply(self.server.refs)
        elif self.path == "/v1/capabilities":
            self.reply({"upload_leases": 0})
        else:
            data = self.server.objects[self.path]
            if self.server.corrupt_download and self.path.startswith("/v1/objects/chunk/"):
                data = data[:-1] + bytes([data[-1] ^ 1])
            self.reply(data, 200)

    def do_POST(self):
        if not self.authorized():
            return
        batch = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        self.reply([key for key in batch if f"/v1/objects/{key['kind']}/{key['id']}" not in self.server.objects])

    def do_PUT(self):
        if not self.authorized():
            return
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if len(body) != length:
            return  # An interrupted request must not become a partial stored object.
        if self.path.startswith("/v1/objects/"):
            self.server.objects[self.path] = body
            self.reply({})
            return
        value = json.loads(body)
        name = self.path.rsplit("/", 1)[1]
        if self.server.conflict or self.server.refs.get(name) != value["expected"]:
            self.reply({}, 409)
            return
        if self.server.publish_delay:
            time.sleep(self.server.publish_delay)
        self.server.refs[name] = value["root"]
        if self.server.disconnect:
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
        else:
            self.reply({}, 500 if self.server.server_error else 200)


@contextlib.contextmanager
def server(factory=Repository):
    repository = factory()
    thread = threading.Thread(target=repository.serve_forever, daemon=True)
    thread.start()
    try:
        yield repository
    finally:
        repository.release.set()
        repository.shutdown()
        repository.server_close()
        thread.join(timeout=3)


class NetworkOperations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="syrnck-network-operations-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_LANG", "SYRNCK_TOKEN")}
        self.env["SYRNCK_TOKEN"] = TOKEN
        self.source, self.target = self.root / "source", self.root / "target"
        self.cmd("init", store=self.source)
        self.cmd("init", "--encrypted", store=self.target, encrypted=True)
        self.data = bytes(range(256)) * 8192 + b"tail"
        asset = self.root / "asset.bin"
        asset.write_bytes(self.data)
        self.identifier = json.loads(self.cmd("put", asset, "--fixed-size", 65536).stdout)["manifest"]
        self.root_key = {"kind": "manifest", "id": self.identifier}
        self.source_ref = "files/" + self.identifier

    def cmd(self, *args, store=None, encrypted=False, expected=0, lang="en"):
        env = dict(self.env)
        if encrypted:
            env["SYRNCK_KEY"] = "39" * 32
        result = subprocess.run([CLI, "--store", str(store or self.source), "--lang", lang, *map(str, args)],
                                cwd=self.root, env=env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result

    def probe(self, repository, mode, operation="push", name="asset", requests=1, buffer_mib=64):
        target = self.target if operation == "pull" else self.source
        result = subprocess.run([PROBE, mode, operation, str(target), repository.url, "manifest", self.identifier,
                                 name, "encrypted" if operation == "pull" else "plain", str(requests), str(buffer_mib)], cwd=self.root,
                                env=self.env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout)

    def test_push_cancel_resume_and_observer_errors(self):
        with server() as remote:
            self.assertTrue(self.probe(remote, "pre")["cancelled"])
            self.assertEqual(remote.calls, [])
            for mode in ("transfer", "throw", "publish"):
                remote.objects.clear()
                result = self.probe(remote, mode)
                self.assertTrue(result.get("cancelled") or result.get("observer_error"))
                self.assertEqual(remote.refs, {})
                self.assertNotIn(("PUT", "/v1/refs/asset"), remote.calls)
                remote.calls.clear()
            # Cancellation at publication leaves all verified payloads reusable.
            sent_before = len(remote.objects)
            resumed = self.probe(remote, "ok")
            self.assertEqual(resumed["result"]["transferred_objects"], 0)
            self.assertEqual(resumed["result"]["reused_objects"], sent_before)
            self.assertEqual(remote.refs["asset"], self.root_key)
            self.assertGreater(resumed["completed"], 0)

    def test_pull_preserves_previous_pin_and_reuses_verified_imports(self):
        with server() as remote:
            self.probe(remote, "ok")
            previous = self.root / "previous.bin"
            previous.write_bytes(b"retained previous version")
            old_id = json.loads(self.cmd("put", previous, store=self.target, encrypted=True).stdout)["manifest"]
            self.cmd("pin", "received", "manifest", old_id, store=self.target, encrypted=True)
            before = self.cmd("refs", store=self.target, encrypted=True).stdout
            for mode in ("transfer", "throw", "publish"):
                result = self.probe(remote, mode, "pull", "received")
                self.assertTrue(result.get("cancelled") or result.get("observer_error"))
                self.assertEqual(self.cmd("refs", store=self.target, encrypted=True).stdout, before)
                restored = self.root / f"previous-{mode}"
                self.cmd("get", old_id, restored, store=self.target, encrypted=True)
                self.assertEqual(restored.read_bytes(), b"retained previous version")
            resumed = self.probe(remote, "ok", "pull", "received")
            self.assertEqual(resumed["result"]["transferred_objects"], 0)
            destination = self.root / "restored.bin"
            self.cmd("get", self.identifier, destination, store=self.target, encrypted=True)
            self.assertEqual(destination.read_bytes(), self.data)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_publication_commit_point_uncertainty_and_cli_progress(self):
        with server() as remote:
            remote.publish_delay = 0.4
            result = self.probe(remote, "late")
            self.assertTrue(result["stopped"])
            self.assertEqual(remote.refs["asset"], self.root_key)
            remote.publish_delay = 0
            remote.disconnect = True
            self.assertTrue(self.probe(remote, "uncertain", name="unknown")["uncertain"])
            self.assertEqual(remote.refs["unknown"], self.root_key)
            result = self.cmd("agent", "push", remote.url, self.source_ref, "cli-unknown", expected=3,
                              lang="fr")
            self.assertEqual(result.stdout, b"")
            self.assertIn("résultat de publication", result.stderr.decode("utf-8"))
            remote.disconnect, remote.server_error = False, True
            self.assertTrue(self.probe(remote, "uncertain", name="server-error")["uncertain"])
            remote.server_error = False
            result = self.cmd("agent", "push", remote.url, self.source_ref, "cli", "--progress-json")
            self.assertEqual(set(json.loads(result.stdout)), {"transferred_objects", "transferred_bytes", "reused_objects",
                                                              "peak_object_requests", "peak_object_buffer_bytes", "object_retries",
                                                              "session", "reconciled", "upload_lease"})
            events = [json.loads(line) for line in result.stderr.splitlines()]
            self.assertTrue(any(e["phase"] == "transferring" for e in events))
            self.assertEqual(events[-1]["phase"], "publishing")
            self.assertNotIn(TOKEN, result.stderr.decode())
            self.assertNotIn(remote.url, result.stderr.decode())
            remote.conflict = True
            self.cmd("agent", "push", remote.url, self.source_ref, "conflict", expected=1)
            self.assertNotIn("conflict", remote.refs)

    def test_real_console_signal_interrupts_an_idle_http_request(self):
        with server() as remote:
            remote.idle = True
            command = [CLI, "--store", str(self.source), "--lang", "fr", "agent", "push", remote.url,
                       self.source_ref, "cancelled", "--progress-json"]
            startup = {}
            request = self.root / "interrupt"
            if RELAY:
                command = [RELAY, str(request), *command]
                info = subprocess.STARTUPINFO()
                info.dwFlags, info.wShowWindow = subprocess.STARTF_USESHOWWINDOW, subprocess.SW_HIDE
                startup = {"creationflags": subprocess.CREATE_NEW_CONSOLE, "startupinfo": info}
            child = subprocess.Popen(command, cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, **startup)
            try:
                self.assertTrue(remote.entered.wait(5), "CLI did not reach the blocking HTTP request")
                if RELAY:
                    request.touch()
                else:
                    child.send_signal(signal.SIGINT)
                output, error = child.communicate(timeout=6)
                self.assertEqual(child.returncode, 130, error.decode("utf-8", "replace"))
                self.assertEqual(output, b"")
                self.assertIn("opération annulée", error.decode("utf-8"))
                self.assertEqual(remote.refs, {})
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate(timeout=3)

    def test_invalid_source_file_hash_fails_before_sending_objects(self):
        with server() as remote:
            self.assertTrue(self.probe(remote, "invalid")["invalid_source"])
            self.assertEqual(remote.objects, {})
            self.assertEqual(remote.refs, {})
            self.assertFalse(any(method == "PUT" for method, _ in remote.calls))

    def test_corrupt_download_does_not_publish_and_retry_reuses_the_manifest(self):
        with server() as remote:
            self.probe(remote, "ok")
            before = self.cmd("refs", store=self.target, encrypted=True).stdout
            remote.corrupt_download = True
            self.assertTrue(self.probe(remote, "corrupt", "pull", "received")["corrupt_response"])
            self.assertEqual(self.cmd("refs", store=self.target, encrypted=True).stdout, before)
            remote.corrupt_download = False
            result = self.probe(remote, "ok", "pull", "received")
            self.assertGreater(result["result"]["reused_objects"], 0)
            self.cmd("fsck", store=self.target, encrypted=True)


if __name__ == "__main__":
    unittest.main()
