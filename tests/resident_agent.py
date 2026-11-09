"""Resident native publication against the real encrypted, authenticated server."""
import contextlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import unittest
import server_signatures as signed
import workflows


class Frames:
    def __init__(self, process, errors):
        self.process, self.errors = process, errors
        self.rows = []
        self.queue = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            try:
                self.queue.put(json.loads(line))
            except ValueError as error:
                self.queue.put(error)

    def wait(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                row = self.queue.get(timeout=.05)
            except queue.Empty:
                if self.process.poll() is not None:
                    self.errors.seek(0)
                    raise AssertionError((self.process.returncode, self.rows, self.errors.read()))
                continue
            if isinstance(row, Exception):
                raise row
            self.rows.append(row)
            if predicate(row):
                return row
        raise AssertionError(("resident status timeout", self.rows))

    def state(self, value, **fields):
        return self.wait(lambda row: row["state"] == value and all(row.get(k) == v for k, v in fields.items()))


class ResidentAgent(unittest.TestCase):
    setUp = signed.ServerSignatures.setUp
    http = signed.ServerSignatures.http
    serving = signed.ServerSignatures.serving
    commit = signed.ServerSignatures.commit
    publish = signed.ServerSignatures.publish
    signer = signed.ServerSignatures.signer
    binding = staticmethod(signed.ServerSignatures.binding)
    save_policy = signed.ServerSignatures.save_policy
    metadata = signed.ServerSignatures.metadata

    def test_repeated_capture_reuses_assets_and_complete_observation(self):
        self.write("Content/large.bin", b"a" * (8 * 1024 * 1024))
        self.write("Source/code.cpp", b"first\n")
        with self.serving(strict=True), self.agent() as agent:
            first = agent.state("pushed")
            self.assertEqual(first["capture"]["reused_files"], 0)
            self.assertGreaterEqual(first["capture"]["source_bytes_read"], 8 * 1024 * 1024)
            agent.state("synced", working_clean=True)
            time.sleep(3.1)
            self.write("Source/code.cpp", b"second\n")
            second = agent.state("pushed")
            self.assertNotEqual(first["local_revision"], second["local_revision"])
            capture = second["capture"]
            self.assertEqual(capture["source_bytes_read"] + capture["source_bytes_reused"], 8 * 1024 * 1024 + 7)
            if capture["reused_files"]:
                self.assertEqual(capture["source_bytes_read"], 7)
                self.assertEqual(capture["source_bytes_reused"], 8 * 1024 * 1024)
            self.assertFalse(second["full_scan"], "capture forced a redundant complete observer audit")
            agent.state("synced", working_clean=True)
        self.cmd("fsck")

    def cmd(self, *arguments, actor=None, store=None, key=False, ok=True):
        env = dict(self.env)
        if actor:
            env["SYRNCK_TOKEN"] = self.tokens[actor]
        if key:
            env["SYRNCK_KEY"] = "39" * 32
        deadline = time.monotonic() + 10
        while True:
            result = subprocess.run([workflows.CLI, "--store", str(store or self.local), *map(str, arguments)],
                cwd=self.project, env=env, capture_output=True, timeout=25)
            if b"store busy: another process holds its lock" not in result.stderr or time.monotonic() >= deadline:
                break
            time.sleep(.03)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode("utf-8", "replace"))
        return result

    @contextlib.contextmanager
    def agent(self, *args, workspace=None, directory=None, env=None, store=None):
        arguments = [workflows.CLI, "--store", str(store or self.local)]
        if workspace:
            arguments += ["--workspace", workspace]
        arguments += ["agent", "run", self.url, "--remote-branch", "scratch", "--interval-ms", "50",
                      "--debounce-ms", "25", "--remote-poll-ms", "100", "--duration-ms", "60000"]
        if directory is not None:
            arguments += ["--directory", str(directory)]
        arguments += list(map(str, args))
        errors = (self.root / ("agent-" + str(time.monotonic_ns()) + ".log")).open("w+", encoding="utf-8")
        process = subprocess.Popen(arguments, cwd=self.project,
            env=env or dict(self.env, SYRNCK_TOKEN=self.tokens["alice"]),
            stdout=subprocess.PIPE, stderr=errors, text=True, encoding="utf-8",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        frames = Frames(process, errors)
        try:
            yield frames
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            frames.reader.join(timeout=5)
            process.stdout.close()
            errors.close()

    def write(self, name, data):
        path = self.project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def refs(self):
        return json.loads(self.cmd("refs").stdout)

    @contextlib.contextmanager
    def locked_store(self):
        with (self.local / "store.lock").open("r+b") as lock:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                if os.name == "nt":
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def test_store_contention_at_startup_and_between_cycles_preserves_signer(self):
        self.write("Source/code.cpp", b"before startup\n")
        with self.serving(strict=True), contextlib.ExitStack() as pending:
            pending.enter_context(self.locked_store())
            with self.agent("--polling") as agent:
                agent.state("unavailable", reason="store_busy")
                pending.close()
                first = agent.state("pushed")["local_revision"]
                agent.state("synced", working_clean=True)
                with self.locked_store():
                    agent.state("unavailable", reason="operation_failed")
                    self.write("Source/code.cpp", b"during contention\n")
                second = agent.state("pushed")["local_revision"]
                self.assertNotEqual(first, second)
                self.assertEqual(self.metadata(first)["signature"]["public_key"],
                                 self.metadata(second)["signature"]["public_key"])

    def test_invalid_checkpoint_blocks_before_capturing_dirty_files(self):
        first = self.commit("Source/code.cpp", b"committed\n")
        with self.serving(strict=True):
            with self.agent("--max-updates", "1") as agent:
                agent.state("pushed", local_revision=first)
                self.assertEqual(agent.process.wait(timeout=10), 0)
            state_ref, = [name for name in self.refs() if name.startswith("sync/push/") and name.endswith("/state")]
            self.cmd("pin", state_ref, "revision", first)
            self.write("Source/code.cpp", b"uncommitted\n")
            with self.agent("--max-updates", "1") as agent:
                error = agent.state("unavailable")
                self.assertIn("invalid native push checkpoint", error["message"])
                self.assertEqual(agent.process.wait(timeout=10), 1)
            self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], first)
            self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], first)

    def test_portable_backup_restores_checkpoint_without_old_physical_destination(self):
        self.write("Source/code.cpp", b"before portable backup\n")
        with self.serving(strict=True):
            with self.agent("--max-updates", "1") as agent:
                first = agent.state("pushed")["local_revision"]
                self.assertEqual(agent.process.wait(timeout=10), 0)
            backup = self.root / "backup"
            self.cmd("backup", "create", backup)
            self.cmd("backup", "verify", store=backup)
            restored = self.root / "restored-project"
            self.cmd("native", "export", first, restored)
            target = restored / ".syrnck"
            self.cmd("backup", "restore", target, store=backup)
            self.assertFalse((target / "native-identity.json").exists())
            before = json.loads(self.cmd("refs", store=target).stdout)
            env = dict(self.env, SYRNCK_TOKEN=self.tokens["alice"], SYRNCK_USER_NAME="Alice",
                       SYRNCK_USER_EMAIL="alice@example.invalid")
            with self.agent(directory=restored, store=target, env=env) as agent:
                agent.state("synced", local_revision=first)
                self.assertEqual(json.loads(self.cmd("refs", store=target).stdout), before)
                (restored / "Source/code.cpp").write_bytes(b"after portable restore\n")
                second = agent.state("pushed")["local_revision"]
                self.assertEqual(self.metadata(second, target)["parents"], [first])
            self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"before portable backup\n")
            self.cmd("gc", "--delete", store=target)
            self.cmd("fsck", store=target)

    def test_native_and_polling_watch_publish_renames_deletions_and_preserve_signer(self):
        for polling in (False, True):
            with self.subTest(polling=polling), self.serving(strict=True):
                self.write("Source/code.cpp", b"first\n")
                with self.agent(*(["--polling"] if polling else [])) as agent:
                    first = agent.state("pushed")["local_revision"]
                    agent.state("synced", working_clean=True)
                    self.write("Source/code.cpp", b"second\n")
                    second = agent.state("pushed")["local_revision"]
                    self.assertNotEqual(first, second)
                    (self.project / "Source/code.cpp").rename(self.project / "Source/renamed.cpp")
                    third = agent.state("pushed")["local_revision"]
                    (self.project / "Source/renamed.cpp").unlink()
                    fourth = agent.state("pushed")["local_revision"]
                    agent.state("synced", working_clean=True)
                    self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], fourth)
                    # Store lock is released during resident waiting.
                    self.cmd("native", "status")
                for revision in (first, second, third, fourth):
                    metadata = self.metadata(revision)
                    self.assertEqual(metadata["committer"]["name"], "Alice")
                    self.assertIsNotNone(metadata["signature"])
                self.assertEqual(self.metadata(second)["parents"], [first])
                self.assertEqual(self.metadata(third)["parents"], [second])
                self.assertEqual(self.metadata(fourth)["files"], {})
                refs = self.refs()
                self.assertTrue(any(name.startswith("archive/agent-push/") and root["id"] == first
                                    for name, root in refs.items()))
                self.cmd("gc", "--delete")
                self.cmd("fsck")

    def test_offline_snapshots_resume_on_server_return_and_after_process_restart(self):
        self.write("Source/code.cpp", b"offline first\n")
        with self.agent("--polling") as agent:
            first = agent.state("unavailable")["local_revision"]
            self.assertIsNotNone(first)
            self.write("Source/code.cpp", b"offline second\n")
            second = agent.wait(lambda row: row["state"] == "unavailable" and row.get("local_revision") != first)["local_revision"]
            self.assertEqual(self.metadata(second)["parents"], [first])
            with self.serving(strict=True):
                agent.state("pushed", local_revision=second)
                agent.state("synced", working_clean=True)
        self.write("Source/code.cpp", b"offline after restart\n")
        with self.serving(strict=True), self.agent() as agent:
            third = agent.state("pushed")["local_revision"]
            self.assertEqual(self.metadata(third)["parents"], [second])
            agent.state("synced", working_clean=True)
        self.cmd("fsck")

    def test_staging_blocks_capture_until_index_is_cleared(self):
        first = self.commit("Source/code.cpp", b"committed\n")
        self.write("Source/code.cpp", b"staged\n")
        self.cmd("native", "add", "Source/code.cpp")
        with self.serving(strict=True), self.agent("--polling") as agent:
            blocked = agent.state("blocked", reason="staging_in_progress")
            self.assertEqual(blocked["local_revision"], first)
            self.assertEqual(self.http("GET", "/v1/refs"), {})
            self.cmd("native", "reset")
            next_revision = agent.state("pushed")["local_revision"]
            self.assertNotEqual(first, next_revision)

    def test_commits_only_preserves_dirty_files_and_does_not_create_revisions(self):
        first = self.commit("Source/code.cpp", b"committed\n")
        self.write("Source/code.cpp", b"uncommitted\n")
        with self.serving(strict=True), self.agent("--commits-only", "--polling") as agent:
            agent.state("pushed", local_revision=first, working_clean=False)
            agent.state("uncommitted", local_revision=first, working_clean=False)
            self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"uncommitted\n")
            self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], first)
        self.assertEqual(self.metadata(first)["parents"], [])

    def test_remote_rewind_and_deletion_are_not_undone_by_agent(self):
        first = self.commit("Source/code.cpp", b"one\n")
        with self.serving(strict=True):
            self.publish(first, name="scratch")
            with self.agent("--commits-only", "--polling") as agent:
                agent.state("synced", local_revision=first)
                second = self.commit("Source/code.cpp", b"two\n")
                agent.state("pushed", local_revision=second)
                self.publish(first, name="scratch")
                agent.state("blocked", reason="remote_history_rewritten")
                self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], first)
        # Server ref administration while stopped; next resident session must
        # remember the acknowledged baseline rather than recreate a deleted ref.
        self.cmd("unpin", "scratch", store=self.server, key=True)
        with self.serving(strict=True), self.agent("--commits-only") as agent:
            agent.state("blocked", reason="remote_deleted")
            self.assertNotIn("scratch", self.http("GET", "/v1/refs"))

    def test_divergent_remote_is_fetched_without_replacing_local_files(self):
        first = self.commit("Source/code.cpp", b"base\n")
        self.cmd("native", "branch", "other", first)
        with self.serving(strict=True), self.agent("--commits-only", "--polling") as agent:
            agent.state("pushed", local_revision=first)
            local = self.commit("Source/code.cpp", b"local\n")
            agent.state("pushed", local_revision=local)
            # Construct an alternate history in a separate linked workspace.
            other = self.root / "other"
            self.cmd("native", "workspace-add", "other", other, "--branch", "other")
            (other / "Source/code.cpp").write_bytes(b"remote\n")
            alternate = self.cmd("--workspace", "other", "native", "commit", "-m", "alternate").stdout.decode().strip()
            self.publish(alternate, name="scratch")
            agent.state("blocked", reason="remote_history_rewritten")
            self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"local\n")
            self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], local)
            self.assertTrue(any(name.startswith("remotes/agent-") and key["id"] == alternate
                                for name, key in self.refs().items()))

    def test_linked_partial_detached_workspace_preserves_hidden_files_and_primary(self):
        self.write("Content/hidden.uasset", b"hidden large asset\n")
        initial = self.commit("Source/code.cpp", b"first\n")
        linked = self.root / "partial"
        self.cmd("native", "workspace-add", "partial", linked, "--revision", initial, "--include", "Source")
        self.assertFalse((linked / "Content").exists())
        with self.serving(strict=True), self.agent(workspace="partial") as agent:
            agent.state("pushed", local_revision=initial)
            (linked / "Source/code.cpp").write_bytes(b"linked update\n")
            updated = agent.state("pushed")["local_revision"]
            self.assertNotEqual(initial, updated)
            self.assertEqual(self.metadata(updated)["files"]["Content/hidden.uasset"],
                             self.metadata(initial)["files"]["Content/hidden.uasset"])
            self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], initial)
            self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"first\n")
        self.cmd("fsck")

    def test_empty_workspace_and_quiet_restart_do_not_write_checkpoints(self):
        with self.serving(strict=True), self.agent("--max-updates", "1") as agent:
            agent.state("empty")
            self.assertEqual(agent.process.wait(timeout=10), 0)
        self.assertEqual(self.refs(), {})
        first = self.commit("Source/code.cpp", b"one\n")
        with self.serving(strict=True), self.agent("--max-updates", "1") as agent:
            agent.state("pushed", local_revision=first)
            self.assertEqual(agent.process.wait(timeout=10), 0)
        before = self.refs()
        with self.serving(strict=True), self.agent("--max-updates", "1") as agent:
            agent.state("synced", local_revision=first)
            self.assertEqual(agent.process.wait(timeout=10), 0)
        self.assertEqual(self.refs(), before)

    def test_completed_legacy_receipt_cannot_recreate_a_deleted_remote_branch(self):
        first = self.commit("Source/code.cpp", b"published outside resident mode\n")
        with self.serving(strict=True):
            self.publish(first, name="scratch")
        self.assertFalse(any(name.startswith("sync/push/") for name in self.refs()))
        self.cmd("unpin", "scratch", store=self.server, key=True)
        with self.serving(strict=True), self.agent("--commits-only", "--max-updates", "1") as agent:
            blocked = agent.state("blocked", reason="remote_deleted")
            self.assertEqual(blocked["last_observed_remote"], first)
            self.assertEqual(agent.process.wait(timeout=10), 2)
            self.assertNotIn("scratch", self.http("GET", "/v1/refs"))


if __name__ == "__main__":
    unittest.main()
