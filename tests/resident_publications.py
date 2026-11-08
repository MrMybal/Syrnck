"""Kill the real resident process across an unacknowledged remote publication."""
import json
import os
import queue
import signal
import subprocess
import threading
import unittest
import network_operations as base
import persistent_transfers as journals


class ResidentPublications(unittest.TestCase):
    setUp = base.NetworkOperations.setUp
    cmd = base.NetworkOperations.cmd

    def arguments(self, remote, *extra):
        return [base.CLI, "--store", str(self.source), "agent", "run", remote.url,
                "--directory", str(self.root), "--remote-branch", "asset",
                "--interval-ms", "50", "--remote-poll-ms", "100", "--polling", *extra]

    def interrupted(self, remote):
        remote.gate_publication = "/v1/refs/asset"
        process = subprocess.Popen(self.arguments(remote), cwd=self.root, env=self.env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            self.assertTrue(remote.entered.wait(12), "resident agent never reached its deterministic CAS gate")
            self.assertIsNone(process.poll())
            process.kill()
            process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            remote.gate_publication = None
            remote.release.set()
            remote.drain()
        record, = self.sessions()
        self.assertEqual(record["state"], "publishing")
        self.assertEqual(record["root"]["kind"], "revision")
        self.assertEqual(remote.refs["asset"], record["root"])
        self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
        return record

    def sessions(self):
        return json.loads(self.cmd("agent", "sessions").stdout)

    def next_commit(self):
        (self.root / "asset.bin").write_bytes(b"next local version\n" * 10000)
        return self.cmd("native", "commit", "--directory", self.root, "-m", "new root while old outcome unknown").stdout.decode().strip()

    def run_once(self, remote, expected):
        result = subprocess.run(self.arguments(remote, "--commits-only", "--max-updates", "1"),
            cwd=self.root, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stderr.decode("utf-8", "replace"))
        frame, = [json.loads(row) for row in result.stdout.splitlines()]
        return frame

    def test_new_head_cannot_bypass_unknown_old_root_after_remote_deletion(self):
        with base.server(journals.JournalRepository) as remote:
            old = self.interrupted(remote)
            new = self.next_commit()
            remote.refs.clear()  # A deliberate remote removal after the lost ACK.
            before = dict(remote.objects)
            frame = self.run_once(remote, 3)
            self.assertEqual((frame["state"], frame["reason"]), ("blocked", "publication_uncertain"))
            self.assertEqual(frame["local_revision"], new)
            self.assertEqual(remote.refs, {})
            self.assertEqual(remote.objects, before)
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
            record, = self.sessions()
            self.assertEqual(record["id"], old["id"])
            self.assertEqual(record["state"], "uncertain")
            self.cmd("gc", "--delete")
            self.cmd("fsck")

    def test_new_head_follows_reconciled_old_root_without_replaying_old_cas(self):
        with base.server(journals.JournalRepository) as remote:
            old = self.interrupted(remote)
            new = self.next_commit()
            frame = self.run_once(remote, 0)
            self.assertEqual(frame["state"], "pushed")
            self.assertEqual(frame["local_revision"], new)
            self.assertEqual(frame["expected_remote_revision"], old["root"]["id"])
            self.assertEqual(remote.refs["asset"], {"kind": "revision", "id": new})
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 2)
            self.assertEqual({record["state"] for record in self.sessions()}, {"complete"})
            self.cmd("gc", "--delete")
            self.cmd("fsck")

    def test_cooperative_os_stop_releases_locks_and_keeps_acknowledged_checkpoint(self):
        revision = self.cmd("native", "commit", "--directory", self.root, "-m", "before resident stop").stdout.decode().strip()
        with base.server(journals.JournalRepository) as remote:
            for number, signum in enumerate((signal.SIGINT, signal.SIGTERM) if os.name != "nt" else (None,)):
                request = self.source / ("interrupt-" + str(number))
                arguments = self.arguments(remote, "--commits-only", "--lang", "fr")
                startup = {}
                if base.RELAY:
                    arguments = [base.RELAY, str(request), *arguments]
                    info = subprocess.STARTUPINFO()
                    info.dwFlags = subprocess.STARTF_USESHOWWINDOW
                    info.wShowWindow = subprocess.SW_HIDE
                    startup = {"creationflags": subprocess.CREATE_NEW_CONSOLE, "startupinfo": info}
                child = subprocess.Popen(arguments, cwd=self.root, env=self.env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, **startup)
                first = queue.Queue()
                reader = threading.Thread(target=lambda: first.put(child.stdout.readline()), daemon=True)
                reader.start()
                try:
                    frame = json.loads(first.get(timeout=15))
                    reader.join(timeout=1)
                    self.assertIn(frame["state"], ("pushed", "synced"))
                    self.assertEqual(frame["local_revision"], revision)
                    if base.RELAY:
                        request.touch()
                    else:
                        child.send_signal(signum)
                    output, errors = child.communicate(timeout=10)
                    self.assertEqual(child.returncode, 130, (output, errors))
                    self.assertIn("opération annulée", errors.decode("utf-8"))
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.communicate(timeout=5)
                self.assertEqual(self.run_once(remote, 0)["state"], "synced")
                self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
                self.cmd("fsck")


if __name__ == "__main__":
    unittest.main()
