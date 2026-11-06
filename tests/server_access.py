"""Production server identities, write ACLs, atomic remote locks and branch policies."""
import concurrent.futures
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import workflows


class ServerAccess(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="syrnck-access-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k.upper() not in ("SYRNCK_KEY", "SYRNCK_LANG", "SYRNCK_TOKEN")}
        self.project = self.root / "project"
        self.project.mkdir()
        self.local = self.project / ".syrnck"
        self.server = self.root / "server"
        self.cmd("init", store=self.local)
        self.cmd("init", "--encrypted", store=self.server, key=True)
        self.tokens = {name: self.cmd("keygen").stdout.decode().strip()
                       for name in ("alice", "bob", "reader", "admin", "invalid")}
        self.config = {"version": 1, "principals": [], "branches": [
            {"name": "main", "fast_forward": True, "lock_paths": ["Content"]},
            {"name": "scratch", "fast_forward": False, "lock_paths": []}]}
        for name in ("alice", "bob", "reader", "admin"):
            paths = {"alice": ["Source", "Content"], "bob": ["Content"], "reader": [], "admin": [""]}[name]
            digest = self.cmd("token-hash", actor=name).stdout.decode().strip()
            self.config["principals"].append({"id": name, "token_hash": digest,
                "role": name if name in ("reader", "admin") else "writer",
                "write_paths": paths, "branches": [] if name == "reader" else ["main", "scratch"]})
        self.access = self.root / "access.json"
        self.access.write_text(json.dumps(self.config), encoding="utf-8")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.url = f"http://127.0.0.1:{self.port}"

    def cmd(self, *arguments, actor=None, store=None, key=False, ok=True):
        env = dict(self.env)
        if actor:
            env["SYRNCK_TOKEN"] = self.tokens[actor]
        if key:
            env["SYRNCK_KEY"] = "39" * 32
        result = subprocess.run([workflows.CLI, "--store", str(store or self.local), *map(str, arguments)],
                                cwd=self.project, env=env, capture_output=True, timeout=25)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        if not ok:
            self.assertEqual(result.stdout, b"")
        return result

    def http(self, method, path, value=None, actor="alice", expected=200):
        request = urllib.request.Request(self.url + path, method=method,
            data=None if value is None else json.dumps(value).encode(),
            headers={"Authorization": "Bearer " + self.tokens[actor], "Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            body = response.read()
            self.assertEqual(response.status, expected, body.decode("utf-8", "replace"))
            return json.loads(body) if response.status == 200 and body else body

    @contextlib.contextmanager
    def serving(self, strict=False):
        arguments = [workflows.CLI, "--store", str(self.server), "serve", "--access", str(self.access),
                     "--listen", f"127.0.0.1:{self.port}"]
        if strict:
            arguments.append("--require-lease")
        env = dict(self.env, SYRNCK_KEY="39" * 32, SYRNCK_TOKEN=self.tokens["invalid"])
        process = subprocess.Popen(arguments, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = time.monotonic() + 8
            while True:
                if process.poll() is not None:
                    self.fail(process.stderr.read().decode("utf-8", "replace"))
                try:
                    self.http("GET", "/v1/identity")
                    break
                except urllib.error.URLError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.02)
            yield
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def lock(self, *paths, actor="alice", seconds=60):
        return self.http("POST", "/v1/locks/acquire", {"paths": paths, "seconds": seconds}, actor=actor)

    def commit(self, path, data, message="commit"):
        target = self.project / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return self.cmd("native", "commit", "-m", message).stdout.decode().strip()

    def publish(self, revision, claims=(), actor="alice", ok=True, name="main"):
        path = self.root / "claims.json"
        path.write_text(json.dumps(list(claims)), encoding="utf-8")
        self.cmd("pin", "send", "revision", revision)
        return self.cmd("agent", "push", self.url, "send", name, "--lock-claims", path, actor=actor, ok=ok)

    @staticmethod
    def claims(result):
        return [{"path": row["path"], "fence": row["fence"]} for row in result["locks"]]

    def test_identity_acl_grouped_locks_expiration_and_no_token_fallback(self):
        paths = ["Content/Édit.uasset", "Content/Édit.ubulk"]
        with self.serving():
            self.assertEqual(self.http("GET", "/v1/identity")["id"], "alice")
            self.http("GET", "/v1/identity", actor="invalid", expected=401)
            self.http("POST", "/v1/locks/acquire", {"owner": "admin", "paths": paths, "seconds": 60}, expected=400)
            self.http("POST", "/v1/locks/acquire", {"paths": ["Source/code.cpp"], "seconds": 60}, actor="bob", expected=403)
            self.http("POST", "/v1/locks/acquire", {"paths": paths, "seconds": 60}, actor="reader", expected=403)
            acquired = self.lock(*paths, seconds=1)
            self.assertTrue(all(row["owner"] == "alice" for row in acquired["locks"]))
            self.http("POST", "/v1/locks/acquire", {"paths": [paths[0].lower(), "Content/Other"], "seconds": 60}, actor="bob", expected=400)
            page = self.http("POST", "/v1/locks/list", {"after": None, "limit": 100}, actor="reader")
            self.assertEqual(len(page["locks"]), 2)
            claims = self.claims(acquired)
            self.http("POST", "/v1/locks/release", {"claims": claims}, actor="bob", expected=400)
            self.http("POST", "/v1/locks/prune", {"delete": True}, expected=403)
            time.sleep(1.05)
            replacement = self.lock(*paths, actor="bob")
            self.assertGreater(replacement["locks"][0]["fence"], acquired["locks"][0]["fence"])
            self.http("POST", "/v1/locks/release", {"claims": claims}, expected=400)
            command = self.cmd("locks", "identity", "--remote", self.url, actor="bob")
            self.assertEqual(json.loads(command.stdout)["id"], "bob")
            self.assertEqual(self.http("GET", "/v1/refs"), {})
        with self.serving():
            page = json.loads(self.cmd("locks", "list", "--remote", self.url, actor="reader").stdout)
            self.assertTrue(all(row["owner"] == "bob" for row in page["locks"]))
            self.cmd("locks", "renew", "--remote", self.url, "--fence", replacement["locks"][0]["fence"],
                     "--seconds", 120, *paths, actor="bob")

    def test_concurrent_atomic_groups_have_one_winner(self):
        with self.serving():
            def attempt(actor):
                return json.loads(self.cmd("locks", "acquire-plan", "--remote", self.url,
                                           "Content/shared", f"Content/{actor}", actor=actor).stdout)
            self.assertTrue(attempt("alice")["ready"])
            def acquire(actor):
                request = urllib.request.Request(self.url + "/v1/locks/acquire", method="POST",
                    data=json.dumps({"paths": ["Content/shared", f"Content/{actor}"], "seconds": 60}).encode(),
                    headers={"Authorization": "Bearer " + self.tokens[actor]})
                try:
                    with urllib.request.urlopen(request, timeout=8) as response:
                        return response.status
                except urllib.error.HTTPError as error:
                    return error.code
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                self.assertEqual(sorted(pool.map(acquire, ("alice", "bob"))), [200, 400])
            page = self.http("POST", "/v1/locks/list", {"after": None, "limit": 100})
            self.assertEqual(len(page["locks"]), 2)
            self.assertEqual(len({row["owner"] for row in page["locks"]}), 1)

    def test_server_checks_claims_history_acl_branch_fast_forward_and_full_closure(self):
        first = self.commit("Content/map.uasset", b"first asset")
        second = self.commit("Content/map.uasset", b"second asset")
        with self.serving(strict=True):
            self.publish(first, ok=False)
            held = self.lock("Content/map.uasset")
            claims = self.claims(held)
            self.publish(first, claims)
            self.publish(second, claims, actor="bob", ok=False)
            self.publish(second, claims)
            self.publish(first, claims, ok=False)
            self.publish(second, claims, name="unconfigured", ok=False)
            third = self.commit("Source/code.cpp", b"int main() {}\n")
            self.publish(third, claims, actor="bob", ok=False)
            self.publish(third, claims)
            # A forbidden transient addition must be checked even if the final
            # snapshot removes it and exactly matches the published snapshot.
            self.commit("Private/secret.bin", b"forbidden")
            (self.project / "Private/secret.bin").unlink()
            hidden = self.cmd("native", "commit", "-m", "removed transient").stdout.decode().strip()
            self.publish(hidden, claims, ok=False)
            root = {"kind": "revision", "id": "0" * 64}
            previous = self.http("GET", "/v1/refs")["main"]
            self.http("PUT", "/v1/refs/main", {"root": root, "expected": previous}, expected=404)
            self.assertEqual(self.http("GET", "/v1/refs")["main"]["id"], third)
        self.cmd("gc", "--delete", store=self.server, key=True)
        self.cmd("fsck", store=self.server, key=True)
        with self.serving(strict=True):
            target = self.root / "clone"
            self.cmd("init", store=target)
            self.cmd("agent", "pull", self.url, "main", "download", "--require-read-lease", actor="reader", store=target)
            self.cmd("fsck", store=target)

    def test_native_push_fetch_preserve_workspace_and_archived_tracking_heads(self):
        first = self.commit("Content/map.uasset", b"asset first")
        with self.serving(strict=True):
            held = self.lock("Content/map.uasset")
            path = self.root / "native-claims.json"
            path.write_text(json.dumps(self.claims(held)), encoding="utf-8")
            pushed = json.loads(self.cmd("native", "push", self.url, "--lock-claims", path,
                                        "--require-lease", actor="alice").stdout)
            self.assertEqual(pushed["revision"], first)
            clone = self.root / "clone-project"
            clone.mkdir()
            target = clone / ".syrnck"
            self.cmd("init", store=target)
            local_file = clone / "untouched.txt"
            local_file.write_bytes(b"untracked local content")
            fetched = json.loads(self.cmd("native", "fetch", self.url, "--require-read-lease",
                                          actor="reader", store=target).stdout)
            self.assertEqual(fetched["reference"], "remotes/origin/main")
            self.assertEqual(fetched["revision"], first)
            refs = json.loads(self.cmd("refs", store=target).stdout)
            self.assertNotIn("heads/main", refs)
            second = self.commit("Content/map.uasset", b"asset second")
            self.cmd("native", "push", self.url, "--lock-claims", path, actor="alice")
            self.cmd("native", "fetch", self.url, actor="reader", store=target)
            refs = json.loads(self.cmd("refs", store=target).stdout)
            self.assertEqual(refs["remotes/origin/main"]["id"], second)
            self.assertEqual(refs["archive/fetch/" + first]["id"], first)
            self.assertEqual(local_file.read_bytes(), b"untracked local content")
            self.assertEqual(sorted(p.name for p in clone.iterdir()), [".syrnck", "untouched.txt"])
            output = self.root / "fetched-export"
            self.cmd("native", "export", "remotes/origin/main", output, store=target)
            self.assertEqual((output / "Content/map.uasset").read_bytes(), b"asset second")
            self.cmd("gc", "--delete", store=target)
            self.cmd("native", "export", first, self.root / "old-export", store=target)
            self.assertEqual((self.root / "old-export/Content/map.uasset").read_bytes(), b"asset first")

    def test_scoped_transfer_leases_and_write_rejections(self):
        revision = self.commit("Source/code.cpp", b"source")
        root = {"kind": "revision", "id": revision}
        with self.serving():
            identifier = "1" * 64
            request = {"root": root, "reference": "main", "expected": None, "fence": None}
            acquired = self.http("PUT", "/v1/uploads/" + identifier, request)
            self.assertEqual(acquired["lease"]["id"], identifier)
            self.http("GET", "/v1/uploads/" + identifier, actor="bob", expected=404)
            self.http("GET", "/v1/uploads/" + identifier, actor="reader", expected=403)
            self.http("POST", "/v1/uploads/" + identifier + "/confirm",
                      {"fence": acquired["lease"]["fence"]}, actor="bob", expected=404)
            self.http("PUT", "/v1/objects/chunk/" + "0" * 64, {}, actor="reader", expected=403)
            self.http("PUT", "/v1/refs/main", {"root": root, "expected": None}, actor="reader", expected=403)
            self.http("PUT", "/v1/uploads/" + "2" * 64, dict(request, reference="forbidden"), expected=403)


if __name__ == "__main__":
    unittest.main()
