"""An observed remote value remains fixed through upload, retry and recovery."""
import hashlib
import json
import os
import subprocess
import unittest
import network_operations as base
import persistent_transfers as journals


class RaceHandler(journals.JournalHandler):
    def do_PUT(self):
        if self.path == "/v1/refs/asset" and self.server.advance is not None:
            self.server.refs["asset"] = self.server.advance
            self.server.advance = None
        super().do_PUT()


class RaceRepository(journals.JournalRepository):
    def __init__(self):
        super().__init__()
        self.RequestHandlerClass = RaceHandler
        self.advance = None


class ExpectedPublications(unittest.TestCase):
    setUp = base.NetworkOperations.setUp
    cmd = base.NetworkOperations.cmd
    probe = base.NetworkOperations.probe

    def test_publication_bookkeeping_precedes_completion_and_resumes_without_cas_replay(self):
        with base.server(RaceRepository) as remote:
            self.assertTrue(self.probe(remote, "record-fail")["uncertain"])
            record, = self.sessions()
            self.assertEqual(record["state"], "publishing")
            self.assertEqual(remote.refs["asset"], self.root_key)
            refs = json.loads(self.cmd("refs").stdout)
            self.assertEqual(refs["publication-proof"], self.root_key)
            self.cmd("unpin", self.source_ref)
            self.cmd("gc", "--delete")
            resumed = self.probe(remote, "record-resume-stop")
            self.assertTrue(resumed["stopped"])
            self.assertEqual(resumed["publication_events"], 1)
            self.assertTrue(resumed["result"]["reconciled"])
            self.assertEqual(self.sessions()[0]["state"], "complete")
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
            calls, before = list(remote.calls), self.snapshot()
            self.assertEqual(self.probe(remote, "record-resume")["publication_events"], 1)
            self.assertEqual(remote.calls, calls)
            self.assertEqual(self.snapshot(), before)

    def test_stop_during_publication_bookkeeping_finishes_the_committed_boundary(self):
        with base.server(RaceRepository) as remote:
            result = self.probe(remote, "record-stop")
            self.assertTrue(result["stopped"])
            self.assertEqual(result["publication_events"], 1)
            self.assertEqual(self.sessions()[0]["state"], "complete")
            self.assertEqual(remote.refs["asset"], self.root_key)

    def snapshot(self):
        return {p.relative_to(self.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.source.rglob("*") if p.is_file() and p.name != "store.lock"}

    def sessions(self):
        return json.loads(self.cmd("agent", "sessions").stdout)

    def variant(self):
        path = self.root / "variant.bin"
        path.write_bytes(b"changed blocks\n" * 50000)
        identifier = json.loads(self.cmd("put", path, "--fixed-size", 65536).stdout)["manifest"]
        return "files/" + identifier, {"kind": "manifest", "id": identifier}

    def push(self, remote, expectation, reference=None, expected=0, lang="en"):
        return self.cmd("agent", "push", remote.url, reference or self.source_ref, "asset", "--expect", expectation,
                        "--requests", 4, expected=expected, lang=lang)

    def test_stale_absent_and_wrong_kind_write_neither_store_nor_remote(self):
        with base.server(RaceRepository) as remote:
            remote.refs["asset"] = self.root_key
            before = self.snapshot()
            for expectation in ("absent", "revision:" + self.identifier, "manifest:" + "0" * 64):
                result = self.push(remote, expectation, expected=1, lang="fr")
                self.assertIn("valeur attendue", result.stderr.decode("utf-8"))
                self.assertEqual(self.snapshot(), before)
                self.assertEqual(remote.refs["asset"], self.root_key)
                self.assertEqual(remote.objects, {})
            self.assertEqual(remote.calls, [("GET", "/v1/refs")] * 3)
            self.assertEqual(self.sessions(), [])

    def test_absence_and_exact_observation_publish_and_freeze_expected_values(self):
        with base.server(RaceRepository) as remote:
            self.push(remote, "absent")
            self.assertEqual(remote.refs["asset"], self.root_key)
            reference, changed = self.variant()
            self.push(remote, "manifest:" + self.identifier, reference)
            self.assertEqual(remote.refs["asset"], changed)
            records = self.sessions()
            first = next(row for row in records if row["root"] == self.root_key)
            second = next(row for row in records if row["root"] == changed)
            self.assertIsNone(first["expected"])
            self.assertEqual(second["expected"], self.root_key)
            self.assertEqual({row["state"] for row in records}, {"complete"})
            self.cmd("gc", "--delete"); self.cmd("fsck")

    def test_mid_upload_change_cannot_retarget_the_durable_session(self):
        with base.server(RaceRepository) as remote:
            self.push(remote, "absent")
            reference, changed = self.variant()
            manifest = json.loads(self.cmd("inspect", "manifest", self.identifier).stdout)
            replacement = {"kind": "chunk", "id": manifest["blocks"][0]["id"]}
            self.assertIn("/v1/objects/chunk/" + replacement["id"], remote.objects)
            remote.advance = replacement
            self.push(remote, "manifest:" + self.identifier, reference, expected=1)
            self.assertEqual(remote.refs["asset"], replacement)
            record = next(row for row in self.sessions() if row["root"] == changed)
            self.assertEqual(record["state"], "conflict")
            self.assertEqual(record["expected"], self.root_key)
            before, calls = self.snapshot(), len(remote.calls)
            result = self.push(remote, "chunk:" + replacement["id"], reference, expected=1)
            self.assertIn(b"different expected reference", result.stderr)
            self.assertEqual(self.snapshot(), before)
            self.assertEqual(remote.calls[calls:], [("GET", "/v1/refs")])
            self.assertEqual(remote.refs["asset"], replacement)
            self.cmd("agent", "forget", record["id"])
            self.push(remote, "chunk:" + replacement["id"], reference)
            self.assertEqual(remote.refs["asset"], changed)

    def test_invalid_expectations_never_connect_or_create_sessions(self):
        with base.server(RaceRepository) as remote:
            before = self.snapshot()
            for value in ("none", "revision:", "unknown:" + self.identifier, "manifest:" + "F" * 64, ":" + self.identifier):
                self.push(remote, value, expected=1)
                self.assertEqual(self.snapshot(), before)
            self.assertEqual(remote.calls, [])
            self.assertEqual(self.sessions(), [])

    def test_killed_publication_resumes_its_original_expectation_without_new_cas(self):
        with base.server(RaceRepository) as remote:
            remote.gate_publication = "/v1/refs/asset"
            process = subprocess.Popen([base.CLI, "--store", str(self.source), "agent", "push", remote.url,
                self.source_ref, "asset", "--expect", "absent", "--requests", "4"], cwd=self.root, env=self.env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                self.assertTrue(remote.entered.wait(8), "explicit publication did not reach its deterministic HTTP gate")
                self.assertIsNone(process.poll())
                process.kill(); process.communicate(timeout=5)
            finally:
                if process.poll() is None:
                    process.kill(); process.communicate(timeout=5)
                remote.gate_publication = None
                remote.release.set(); remote.drain()
            record, = self.sessions()
            self.assertEqual(record["state"], "publishing")
            self.assertIsNone(record["expected"])
            self.assertEqual(remote.refs["asset"], self.root_key)
            before = self.snapshot()
            self.push(remote, "absent", expected=1)
            self.assertEqual(self.snapshot(), before)
            self.cmd("unpin", self.source_ref)
            self.cmd("gc", "--delete")
            result = json.loads(self.cmd("agent", "resume", remote.url, record["id"]).stdout)
            self.assertTrue(result["reconciled"])
            self.assertEqual(result["transferred_objects"], 0)
            self.assertEqual(remote.calls.count(("PUT", "/v1/refs/asset")), 1)
            self.assertEqual(self.sessions()[0]["state"], "complete")
            self.cmd("fsck")


if __name__ == "__main__":
    unittest.main()
