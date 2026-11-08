"""Restart the production CLI after a private probe is killed mid-checkout."""
import hashlib
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

CLI, PROBE, READER, MANUAL = [str(Path(sys.argv.pop(1)).resolve()) for _ in range(4)]


class RefHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status):
        self.server.calls.append((self.command, self.path))
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def do_GET(self):
        self.reply(200 if self.path == "/v1/refs" else 404)

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.reply(503)

    do_PUT = do_POST


@contextlib.contextmanager
def empty_remote():
    server = ThreadingHTTPServer(("127.0.0.1", 0), RefHandler)
    server.daemon_threads = True
    server.calls = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server, f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class OwnedAgentRecovery(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="syrnck-owned-agent-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = {k: v for k, v in os.environ.items() if not k.upper().startswith("SYRNCK_")}
        self.env["SYRNCK_TOKEN"] = "syrnck-owned-recovery-test-token-0000"

    def command(self, executable, *args, expected=0, env=None):
        result = subprocess.run([executable, *map(str, args)], env=env or self.env,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        return result

    def kill(self, executable, root, kind):
        child = subprocess.Popen([executable, "run", str(root), "checkout.prepared", kind, "1"],
            env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        lines = queue.Queue()
        reader = threading.Thread(target=lambda: lines.put(child.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = lines.get(timeout=20)
            if not line:
                _, errors = child.communicate(timeout=5)
                self.fail("checkout never reached its gate: " + errors.decode("utf-8", "replace"))
            self.assertEqual(json.loads(line)["phase"], "checkout.prepared")
            self.assertIsNone(child.poll())
            child.kill()
            child.communicate(timeout=5)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=5)
            reader.join(timeout=1)

    @staticmethod
    def snapshot(root):
        return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in root.rglob("*") if path.is_file() and path.name not in ("store.lock", "agent-push.lock")}

    def test_real_cli_resumes_exact_owned_targets_offline_without_signing_seed(self):
        cases = [(False, "primary", "checkout", "ordinary"), (True, "partial", "checkout", "transition"),
                 (False, "detached", "merge", "transition"), (True, "linked", "merge", "ordinary")]
        for encrypted, scope, action, transition in cases:
            with self.subTest(encrypted=encrypted, scope=scope, action=action):
                root = self.root / f"{encrypted}-{scope}"
                self.command(READER, "prepare", root, "encrypted" if encrypted else "plain", scope, action, transition)
                self.kill(PROBE, root, "filesystem")
                store = root / "primary/.syrnck"
                env = dict(self.env)
                if encrypted:
                    env["SYRNCK_KEY"] = "5c" * 32
                self.assertNotIn("SYRNCK_SIGNING_SEED", env)
                selection = ["--workspace", "edit"] if scope != "primary" else []
                before = json.loads(self.command(CLI, "--store", store, *selection, "native", "checkout-status", env=env).stdout)
                frozen = json.loads(self.command(CLI, "--store", store, "inspect", "revision", before["target"], env=env).stdout)
                directory = ["--directory", root / "primary"] if scope == "primary" else []
                result = self.command(CLI, "--store", store, *selection, "agent", "run", "http://127.0.0.1:9",
                    "--bidirectional", *directory, "--max-updates", "1", env=env)
                frame, = [json.loads(row) for row in result.stdout.splitlines()]
                self.assertEqual(frame["state"], "recovered")
                self.assertEqual(frame["local_revision"], before["target"])
                self.assertEqual(frame["checkout"]["owner"], before["owner"])
                current = json.loads(self.command(CLI, "--store", store, *selection, "native", "head", env=env).stdout)
                self.assertEqual(current["revision"], before["target"])
                after = json.loads(self.command(CLI, "--store", store, "inspect", "revision", before["target"], env=env).stdout)
                self.assertEqual(after, frozen)
                self.assertIsNotNone(after["signature"])
                self.command(CLI, "--store", store, "gc", "--delete", env=env)
                self.command(CLI, "--store", store, "fsck", env=env)

    def test_real_cli_leaves_manual_checkout_and_wrong_endpoint_untouched(self):
        owned = self.root / "owned"
        self.command(READER, "prepare", owned, "plain", "primary", "checkout", "ordinary")
        self.kill(PROBE, owned, "filesystem")
        manual = self.root / "manual"
        prepared = self.command(MANUAL, "prepare", manual / "store", "plain", "checkout")
        (manual / "fixture.json").write_text(prepared.stdout.decode("utf-8"), encoding="utf-8")
        self.kill(MANUAL, manual / "store", "checkout")
        for root, store, directory, endpoint in [(owned, owned / "primary/.syrnck", owned / "primary", "http://127.0.0.1:10"),
                                                (manual, manual / "store", manual, "http://127.0.0.1:9")]:
            with self.subTest(root=root.name):
                before = self.snapshot(root)
                result = self.command(CLI, "--store", store, "agent", "run", endpoint, "--directory", directory,
                    "--bidirectional", "--max-updates", "1", expected=2)
                frame, = [json.loads(row) for row in result.stdout.splitlines()]
                self.assertEqual((frame["state"], frame["reason"]), ("blocked", "checkout_pending"))
                self.assertEqual(self.snapshot(root), before)

    def test_deleted_remote_is_not_recreated_after_owned_reception_recovery(self):
        with empty_remote() as (remote, url):
            root = self.root / "deleted"
            self.command(READER, "prepare", root, "plain", "primary", "checkout", "ordinary")
            info_path = root / "fixture.json"
            info = json.loads(info_path.read_text(encoding="utf-8"))
            info["endpoint"] = url
            info_path.write_text(json.dumps(info), encoding="utf-8")
            self.kill(PROBE, root, "filesystem")
            result = self.command(CLI, "--store", root / "primary/.syrnck", "agent", "run", url,
                "--directory", root / "primary", "--bidirectional", "--max-updates", "2", "--retries", "0",
                "--duration-ms", "5000", expected=2)
            recovered, blocked = [json.loads(row) for row in result.stdout.splitlines()]
            self.assertEqual(recovered["state"], "recovered")
            self.assertEqual((blocked["state"], blocked["reason"]), ("blocked", "remote_deleted"))
            self.assertEqual(blocked["last_observed_remote"], info["source"])
            self.assertTrue(remote.calls)
            self.assertTrue(all(method == "GET" for method, path in remote.calls), remote.calls)


if __name__ == "__main__":
    unittest.main()
