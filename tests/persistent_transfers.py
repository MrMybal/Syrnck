"""Kill production agents, reopen their journals and preserve references and blocks."""
from collections import Counter
import json
import os
import subprocess
import unittest
import network_operations as base
import parallel_transfers as parallel


class JournalHandler(parallel.ParallelHandler):
    def authorized(self):
        if not super().authorized():
            return False
        if (self.command, self.path.split("/", 4)[:4]) == self.server.gate_request:
            self.server.entered.set()
            self.server.release.wait(8)
        return True

    def reply(self, value, status=200):
        if self.command == "PUT" and self.path == self.server.gate_publication:
            # The mock has applied its CAS, but has not sent an acknowledgement.
            self.server.entered.set()
            self.server.release.wait(8)
        super().reply(value, status)


class JournalRepository(parallel.ParallelRepository):
    def __init__(self):
        super().__init__()
        self.RequestHandlerClass = JournalHandler
        self.gate_request = None
        self.gate_publication = None


class PersistentTransfers(unittest.TestCase):
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe
    setUp = parallel.ParallelTransfers.setUp

    def sessions(self, target=False):
        return json.loads(self.cmd("agent", "sessions", store=self.target if target else self.source,
                                   encrypted=target).stdout)

    def resume(self, remote, identifier, target=False, expected=0):
        return self.cmd("agent", "resume", remote.url, identifier, "--requests", 4,
                        store=self.target if target else self.source, encrypted=target, expected=expected)

    def killed(self, remote, operation):
        env = dict(self.env)
        if operation == "pull":
            env["SYRNCK_KEY"] = "39" * 32
        store = self.target if operation == "pull" else self.source
        source = "asset" if operation == "pull" else self.source_ref
        name = "received" if operation == "pull" else "asset"
        command = [base.CLI, "--store", str(store), "agent", operation, remote.url, source, name, "--requests", "4"]
        process = subprocess.Popen(command, cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            self.assertTrue(remote.entered.wait(8), "agent did not reach the deterministic HTTP gate")
            self.assertIsNone(process.poll())
            process.kill()
            output, _ = process.communicate(timeout=5)
            self.assertEqual(output, b"")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            remote.gate_request = remote.gate_publication = None
            remote.release.set()
            remote.drain()

    def test_killed_push_survives_unpin_and_resumes_without_the_original_reference(self):
        with base.server(JournalRepository) as remote:
            remote.gate_request = ("PUT", ["", "v1", "objects", "manifest"])
            self.killed(remote, "push")
            record, = self.sessions()
            self.assertEqual(record["state"], "transferring")
            self.assertEqual(record["root"], self.root_key)
            self.assertNotIn(base.TOKEN, json.dumps(record))
            self.assertNotIn(remote.url, json.dumps(record))
            self.cmd("unpin", self.source_ref)
            for options in ((), ("--delete",)):
                result = self.cmd("gc", *options, expected=1, lang="fr")
                self.assertIn("sessions de transfert inachevées", result.stderr.decode())
            result = json.loads(self.resume(remote, record["id"]).stdout)
            self.assertEqual(result["session"], record["id"])
            self.assertGreater(result["reused_objects"], 0)
            self.assertEqual(remote.refs["asset"], self.root_key)
            self.assertEqual(self.sessions()[0]["state"], "complete")
            self.cmd("gc", "--delete")
            self.cmd("fsck")

    def test_killed_pull_reuses_metadata_and_publishes_complete_encrypted_file(self):
        with base.server(JournalRepository) as remote:
            self.probe(remote, "ok", requests=4)
            remote.gate_request = ("GET", ["", "v1", "objects", "chunk"])
            self.killed(remote, "pull")
            record, = self.sessions(target=True)
            self.assertEqual(record["state"], "transferring")
            self.assertEqual(self.cmd("refs", store=self.target, encrypted=True).stdout.strip(), b"{}")
            self.cmd("gc", "--delete", store=self.target, encrypted=True, expected=1)
            before = Counter(remote.calls)
            result = json.loads(self.resume(remote, record["id"], target=True).stdout)
            self.assertGreater(result["reused_objects"], 0)
            path = "/v1/objects/manifest/" + self.identifier
            self.assertEqual(Counter(remote.calls)["GET", path], before["GET", path])
            destination = self.root / "restored.bin"
            self.cmd("get", self.identifier, destination, store=self.target, encrypted=True)
            self.assertEqual(destination.read_bytes(), self.data)
            self.cmd("fsck", store=self.target, encrypted=True)
            self.cmd("gc", "--delete", store=self.target, encrypted=True)

    def test_process_killed_after_remote_commit_reconciles_without_replaying_cas(self):
        with base.server(JournalRepository) as remote:
            remote.gate_publication = "/v1/refs/asset"
            self.killed(remote, "push")
            record, = self.sessions()
            self.assertEqual(record["state"], "publishing")
            self.assertEqual(remote.refs["asset"], self.root_key)
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
            result = json.loads(self.resume(remote, record["id"]).stdout)
            self.assertTrue(result["reconciled"])
            self.assertEqual(result["transferred_objects"], 0)
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
            self.assertEqual(self.sessions()[0]["state"], "complete")
            calls = list(remote.calls)
            self.resume(remote, record["id"])
            self.assertEqual(remote.calls, calls)  # Completed receipt does not republish.

    def test_lost_acknowledgement_and_unknown_outcome_cannot_overwrite_a_reverted_reference(self):
        with base.server(JournalRepository) as remote:
            remote.disconnect = True
            self.assertTrue(self.probe(remote, "uncertain")["uncertain"])
            record, = self.sessions()
            self.assertEqual(record["state"], "uncertain")
            remote.disconnect = False
            remote.refs.clear()  # Could be a deliberate change after the lost acknowledgement.
            calls = Counter(remote.calls)
            result = self.resume(remote, record["id"], expected=3)
            self.assertEqual(result.stdout, b"")
            self.assertEqual(Counter(remote.calls)["PUT", "/v1/refs/asset"], calls["PUT", "/v1/refs/asset"])
            self.assertEqual(remote.refs, {})
            self.cmd("agent", "forget", record["id"])
            self.assertEqual(self.sessions(), [])
            self.probe(remote, "ok")
            self.assertEqual(remote.refs["asset"], self.root_key)

    def test_local_reference_changed_between_processes_is_preserved_until_explicit_discard(self):
        with base.server(JournalRepository) as remote:
            self.probe(remote, "ok")
            remote.gate_request = ("GET", ["", "v1", "objects", "chunk"])
            self.killed(remote, "pull")
            record, = self.sessions(target=True)
            previous = self.root / "new-local-version.bin"
            previous.write_bytes(b"new local state after the interrupted pull")
            saved = json.loads(self.cmd("put", previous, store=self.target, encrypted=True).stdout)["manifest"]
            self.cmd("pin", "received", "manifest", saved, store=self.target, encrypted=True)
            before = self.cmd("refs", store=self.target, encrypted=True).stdout
            calls = list(remote.calls)
            self.resume(remote, record["id"], target=True, expected=1)
            self.assertEqual(remote.calls, calls)
            self.assertEqual(self.cmd("refs", store=self.target, encrypted=True).stdout, before)
            self.assertEqual(self.sessions(target=True)[0]["state"], "conflict")
            self.cmd("agent", "forget", record["id"], store=self.target, encrypted=True)
            self.cmd("gc", "--delete", store=self.target, encrypted=True)
            self.cmd("fsck", store=self.target, encrypted=True)

    def test_automatic_resume_endpoint_binding_and_secret_free_local_commands(self):
        with base.server(JournalRepository) as remote:
            self.assertTrue(self.probe(remote, "publish")["cancelled"])
            record, = self.sessions()
            before = len(remote.calls)
            result = self.cmd("agent", "resume", "http://127.0.0.1:1", record["id"], expected=1)
            self.assertIn(b"different endpoint", result.stderr)
            self.assertEqual(before, len(remote.calls))
            resumed = self.probe(remote, "ok")
            self.assertEqual(resumed["result"]["session"], record["id"])
            self.assertEqual(resumed["result"]["transferred_objects"], 0)
            self.assertEqual(len(self.sessions()), 1)
            self.env.pop("SYRNCK_TOKEN")
            self.assertEqual(self.sessions()[0]["state"], "complete")
            self.cmd("agent", "forget", record["id"])
            self.assertEqual(self.sessions(), [])

    def test_local_publication_recovers_without_http_or_reference_rewrite(self):
        with base.server(JournalRepository) as remote:
            self.probe(remote, "ok")
            self.probe(remote, "ok", "pull", "received")
            record, = self.sessions(target=True)
            # Model the durable state just after local refs publication and
            # before the completion-record replacement, using the real journal.
            record["state"] = "publishing"
            path = self.target / "transfers" / (record["id"] + ".json")
            path.write_text(json.dumps(record), encoding="utf-8")
            refs = self.target / "refs.json"
            timestamp = refs.stat().st_mtime_ns
            calls = list(remote.calls)
            recovered = json.loads(self.resume(remote, record["id"], target=True).stdout)
            self.assertTrue(recovered["reconciled"])
            self.assertEqual(remote.calls, calls)
            self.assertEqual(refs.stat().st_mtime_ns, timestamp)
            self.assertEqual(self.sessions(target=True)[0]["state"], "complete")


if __name__ == "__main__":
    unittest.main()
