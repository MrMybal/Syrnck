"""Native resident reception and causal merges across independent workspaces."""
import json
import unittest
import resident_agent as base


class ResidentSync(unittest.TestCase):
    setUp = base.ResidentAgent.setUp
    cmd = base.ResidentAgent.cmd
    http = base.ResidentAgent.http
    serving = base.ResidentAgent.serving
    commit = base.ResidentAgent.commit
    publish = base.ResidentAgent.publish
    signer = base.ResidentAgent.signer
    binding = staticmethod(base.ResidentAgent.binding)
    save_policy = base.ResidentAgent.save_policy
    metadata = base.ResidentAgent.metadata
    agent = base.ResidentAgent.agent
    write = base.ResidentAgent.write

    def peer(self):
        path = self.root / "peer"
        path.mkdir()
        store = path / ".syrnck"
        self.cmd("init", store=store)
        self.cmd("native", "identity", "--name", "Alice", "--email", "alice@example.invalid", store=store)
        return path, store

    def peer_commit(self, path, store, data, name="Source/code.cpp"):
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return self.cmd("native", "commit", "--directory", path, "-m", "peer offline revision", store=store).stdout.decode().strip()

    def test_independent_agents_receive_and_publish_edits_renames_and_deletions(self):
        self.write("Source/code.cpp", b"initial\n")
        peer, store = self.peer()
        with self.serving(strict=True), self.agent("--bidirectional") as left:
            first = left.state("pushed")["local_revision"]
            left.state("synced", working_clean=True)
            with self.agent("--bidirectional", directory=peer, store=store) as right:
                right.state("pulled", local_revision=first)
                right.state("synced", working_clean=True)
                self.assertEqual((peer / "Source/code.cpp").read_bytes(), b"initial\n")
                (peer / "Source/code.cpp").write_bytes(b"peer edit\n")
                second = right.state("pushed")["local_revision"]
                left.state("pulled", local_revision=second)
                self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"peer edit\n")
                (peer / "Source/code.cpp").rename(peer / "Source/renamed.cpp")
                renamed = right.state("pushed")["local_revision"]
                left.state("pulled", local_revision=renamed)
                self.assertFalse((self.project / "Source/code.cpp").exists())
                self.assertEqual((self.project / "Source/renamed.cpp").read_bytes(), b"peer edit\n")
                (self.project / "Source/renamed.cpp").unlink()
                deleted = left.state("pushed")["local_revision"]
                right.state("pulled", local_revision=deleted)
                self.assertFalse((peer / "Source/renamed.cpp").exists())
        for target in (self.local, store):
            self.cmd("gc", "--delete", store=target)
            self.cmd("fsck", store=target)

    def test_offline_text_changes_merge_with_both_parents_and_converge(self):
        initial = self.commit("Source/code.cpp", b"one\nkeep\nlast\n")
        peer, store = self.peer()
        with self.serving(strict=True):
            self.publish(initial, name="scratch")
            with self.agent("--bidirectional", "--max-updates", "1", directory=peer, store=store) as right:
                right.state("pulled", local_revision=initial)
                self.assertEqual(right.process.wait(timeout=10), 0)
        ours = self.commit("Source/code.cpp", b"ONE\nkeep\nlast\n")
        theirs = self.peer_commit(peer, store, b"one\nkeep\nLAST\n")
        with self.serving(strict=True):
            self.publish(ours, name="scratch")
            with self.agent("--bidirectional", "--commits-only", directory=peer, store=store) as right:
                merged = right.state("pushed", merged=True)["local_revision"]
                self.assertEqual(self.metadata(merged, store)["parents"], [theirs, ours])
                self.assertIsNotNone(self.metadata(merged, store)["signature"])
                right.state("synced", working_clean=True)
                with self.agent("--bidirectional", "--commits-only") as left:
                    left.state("pulled", local_revision=merged)
                    left.state("synced", working_clean=True)
                    for path in (peer, self.project):
                        self.assertEqual((path / "Source/code.cpp").read_bytes(), b"ONE\nkeep\nLAST\n")
                    self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], merged)
        for target in (self.local, store):
            self.cmd("gc", "--delete", store=target)
            self.cmd("fsck", store=target)

    def test_binary_conflict_retains_both_histories_and_local_bytes(self):
        initial = self.commit("Source/asset.bin", b"\0base")
        peer, store = self.peer()
        with self.serving(strict=True):
            self.publish(initial, name="scratch")
            with self.agent("--bidirectional", "--max-updates", "1", directory=peer, store=store) as right:
                right.state("pulled", local_revision=initial)
                self.assertEqual(right.process.wait(timeout=10), 0)
            ours = self.commit("Source/asset.bin", b"\0remote")
            theirs = self.peer_commit(peer, store, b"\0local", "Source/asset.bin")
            self.publish(ours, name="scratch")
            with self.agent("--bidirectional", directory=peer, store=store) as right:
                blocked = right.state("blocked", reason="merge_conflict")
                self.assertFalse(blocked["merge"]["ready"])
                self.assertEqual((peer / "Source/asset.bin").read_bytes(), b"\0local")
                self.assertEqual(json.loads(self.cmd("native", "head", store=store).stdout)["revision"], theirs)
                self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], ours)
        self.cmd("gc", "--delete", store=store)
        self.cmd("fsck", store=store)
        for revision in (initial, ours, theirs):
            self.metadata(revision, store)

    def test_commits_only_reception_waits_for_dirty_working_file(self):
        initial = self.commit("Source/code.cpp", b"base\n")
        peer, store = self.peer()
        with self.serving(strict=True):
            self.publish(initial, name="scratch")
            with self.agent("--bidirectional", "--commits-only", directory=peer, store=store) as right:
                right.state("pulled", local_revision=initial)
                right.state("synced", working_clean=True)
                (peer / "Source/code.cpp").write_bytes(b"dirty local\n")
                right.state("uncommitted", local_revision=initial)
                incoming = self.commit("Source/code.cpp", b"new remote\n")
                self.publish(incoming, name="scratch")
                right.state("blocked", reason="working_changes")
                self.assertEqual((peer / "Source/code.cpp").read_bytes(), b"dirty local\n")
                self.assertEqual(json.loads(self.cmd("native", "head", store=store).stdout)["revision"], initial)
                (peer / "Source/code.cpp").write_bytes(b"base\n")
                right.state("pulled", local_revision=incoming)
                self.assertEqual((peer / "Source/code.cpp").read_bytes(), b"new remote\n")

    def test_partial_detached_reception_preserves_hidden_content_and_primary_head(self):
        self.write("Content/hidden.uasset", b"old hidden")
        initial = self.commit("Source/code.cpp", b"base\n")
        partial = self.root / "partial"
        self.cmd("native", "workspace-add", "partial", partial, "--revision", initial, "--include", "Source")
        with self.serving(strict=True):
            self.publish(initial, name="scratch")
            with self.agent("--bidirectional", "--commits-only", workspace="partial") as agent:
                agent.state("synced", local_revision=initial)
                self.write("Content/hidden.uasset", b"new hidden")
                incoming = self.commit("Source/code.cpp", b"remote update\n")
                self.publish(incoming, name="scratch")
                agent.state("pulled", local_revision=incoming)
                self.assertEqual((partial / "Source/code.cpp").read_bytes(), b"remote update\n")
                self.assertFalse((partial / "Content").exists())
                self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], incoming)
                self.assertEqual(json.loads(self.cmd("--workspace", "partial", "native", "head").stdout)["revision"], incoming)
        self.cmd("fsck")

    def test_bidirectional_agent_does_not_reconcile_a_deliberate_remote_rewind(self):
        first = self.commit("Source/code.cpp", b"one\n")
        with self.serving(strict=True), self.agent("--bidirectional", "--commits-only") as agent:
            agent.state("pushed", local_revision=first)
            second = self.commit("Source/code.cpp", b"two\n")
            agent.state("pushed", local_revision=second)
            self.publish(first, name="scratch")
            agent.state("blocked", reason="remote_history_rewritten")
            self.assertEqual((self.project / "Source/code.cpp").read_bytes(), b"two\n")
            self.assertEqual(json.loads(self.cmd("native", "head").stdout)["revision"], second)
            self.assertEqual(self.http("GET", "/v1/refs")["scratch"]["id"], first)


if __name__ == "__main__":
    unittest.main()
