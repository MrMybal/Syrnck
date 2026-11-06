"""Resident workspace observation, released Store locks and recovery."""
import contextlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import unittest

# Import the existing CLI fixture first; workflows consumes the CLI argument.
import native_workflows as base
from workflows import CLI
HOLDER = str(Path(sys.argv.pop(1)).resolve())
JOURNAL_HELPER = str(Path(sys.argv.pop(1)).resolve())


class Running:
    def __init__(self, test, *args):
        self.test = test
        self.lines = queue.Queue()
        self.frames = []
        self.process = subprocess.Popen([CLI, "--store", str(test.store), "native", "watch", *map(str, args)],
                                        cwd=test.root, env=test.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def read():
            for line in self.process.stdout:
                try:
                    value = json.loads(line)
                except Exception as error:
                    value = {"invalid_json": str(error), "line": repr(line)}
                self.frames.append(value)
                self.lines.put(value)
            self.lines.put(None)
        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()

    def until(self, predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                value = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            self.test.assertIsNotNone(value, "watch exited before expected frame")
            self.test.assertNotIn("invalid_json", value)
            if predicate(value):
                return value
        self.test.fail("expected watch frame missing: " + repr(self.frames[-3:]))

    def finish(self, expected=0):
        self.process.wait(timeout=8)
        self.thread.join(timeout=2)
        self.test.assertEqual(self.process.returncode, expected, self.process.stderr.read().decode("utf-8", "replace"))
        return self.frames

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.thread.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


class NativeWatch(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    commit = base.NativeWorkflows.commit

    @contextlib.contextmanager
    def watch(self, *args):
        run = Running(self, *args)
        try:
            yield run
        finally:
            run.close()

    def baseline(self):
        (self.root / "asset").write_bytes(b"baseline")
        return self.commit("baseline")

    @contextlib.contextmanager
    def hold_store(self):
        holder = subprocess.Popen([HOLDER, "hold", str(self.store)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready = queue.Queue()
        reader = threading.Thread(target=lambda: ready.put(holder.stdout.readline()), daemon=True)
        reader.start()
        try:
            self.assertEqual(ready.get(timeout=3).strip(), b"held")
            yield
            holder.stdin.write(b"release\n")
            holder.stdin.flush()
            holder.wait(timeout=3)
            self.assertEqual(holder.returncode, 0, holder.stderr.read().decode("utf-8", "replace"))
        finally:
            if holder.poll() is None:
                holder.kill()
            holder.wait(timeout=3)
            reader.join(timeout=2)
            holder.stdin.close()
            holder.stdout.close()
            holder.stderr.close()

    def test_plain_encrypted_custom_store_and_equal_timestamp_edit(self):
        for encrypted in (False, True):
            with self.subTest(encrypted=encrypted):
                if encrypted:
                    self.store = self.root / "vault"
                    self.env["SYRNCK_KEY"] = "47" * 32
                    self.call("init", "--encrypted")
                baseline = self.baseline()
                with self.watch("--max-updates", 2, "--duration-ms", 5000) as run:
                    initial = run.until(lambda f: f["type"] == "status")
                    self.assertEqual(initial["reason"], "initial")
                    self.assertTrue(initial["full_scan"])
                    self.assertTrue(initial["status"]["clean"])
                    self.assertEqual(json.loads(self.cmd("refs"))["heads/main"]["id"], baseline)
                    asset = self.root / "asset"
                    before = asset.stat()
                    asset.write_bytes(b"changed!")
                    os.utime(asset, ns=(before.st_atime_ns, before.st_mtime_ns))
                    changed = run.until(lambda f: f["type"] == "status" and not f["status"]["clean"])
                    self.assertEqual(changed["status"]["counts"]["modified"], 1)
                    self.assertGreaterEqual(changed["status"]["hashed_bytes"], 8)
                    self.assertGreater(changed["sequence"], initial["sequence"])
                    run.finish()

    def test_store_is_not_opened_during_native_idle_wait(self):
        self.baseline()
        with self.watch("--duration-ms", 1300, "--interval-ms", 40) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("idle notification assertion requires a native backend")
            with self.hold_store():
                time.sleep(0.35)
            self.assertEqual(len(run.finish()), 1, "idle watcher attempted extra observations or fed back on cache writes")

    def test_existing_file_edit_does_not_reread_large_unrelated_asset(self):
        (self.root / "large").write_bytes(b"x" * (4 * 1024 * 1024))
        self.baseline()
        with self.watch("--max-updates", 2, "--duration-ms", 4000) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("targeted observation requires native notifications")
            (self.root / "asset").write_bytes(b"changed!")
            frame = run.until(lambda f: f["type"] == "status" and not f["status"]["clean"])
            self.assertFalse(frame["full_scan"])
            self.assertEqual(frame["status"]["hashed_bytes"], 8)
            self.assertEqual(frame["status"]["observation"]["mode"], "targeted")
            self.assertEqual(frame["status"]["observation"]["retained_files"], 1)
            run.finish()

    def test_file_create_rename_delete_are_targeted_and_journaled(self):
        (self.root / "large").write_bytes(b"x" * (4 * 1024 * 1024))
        self.baseline()
        with self.watch("--journal", "--max-updates", 4, "--duration-ms", 5000) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("targeted namespace observation requires native notifications")
            (self.root / "new").write_bytes(b"new")
            created = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "new" for r in f["status"]["changes"]))
            (self.root / "new").rename(self.root / "renamed")
            renamed = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "renamed" for r in f["status"]["changes"]))
            (self.root / "renamed").unlink()
            removed = run.until(lambda f: f["type"] == "status" and f["status"]["clean"])
            for frame, expected in ((created, 3), (renamed, 3), (removed, 0)):
                self.assertFalse(frame["full_scan"])
                self.assertEqual(frame["status"]["observation"]["mode"], "targeted")
                self.assertEqual(frame["status"]["hashed_bytes"], expected)
                self.assertTrue(frame["journal"]["durable"])
            self.assertEqual(removed["status"]["workspace_files"], 2)
            run.finish()

    def test_subtree_rename_nested_ignores_and_directory_to_file_transition(self):
        (self.root / "large").write_bytes(b"x" * (4 * 1024 * 1024))
        (self.root / "folder").mkdir()
        (self.root / "folder/first").write_bytes(b"one")
        (self.root / "folder/second").write_bytes(b"two")
        self.baseline()
        with self.watch("--journal", "--duration-ms", 5000) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("targeted namespace observation requires native notifications")
            (self.root / "folder").rename(self.root / "moved")
            renamed = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "moved/first" for r in f["status"]["changes"]))
            if initial["backend"] == "windows":
                self.assertFalse(renamed["full_scan"])
                self.assertEqual(renamed["status"]["hashed_bytes"], 6)
            else:
                self.assertTrue(renamed["full_scan"], "inotify registration gap needs full recovery")
            (self.root / "moved/.syrnckignore").write_bytes(b"second\n")
            ignored = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "moved/.syrnckignore" for r in f["status"]["changes"]))
            self.assertFalse(ignored["full_scan"])
            self.assertEqual(ignored["status"]["hashed_bytes"], 10)
            self.assertEqual(ignored["status"]["observation"]["scanned_scopes"], ["moved"])
            self.assertNotIn("moved/second", [r["path"] for r in ignored["status"]["changes"]])
            (self.root / "moved/.syrnckignore").unlink()
            restored = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "moved/second" for r in f["status"]["changes"]))
            self.assertFalse(restored["full_scan"])
            self.assertEqual(restored["status"]["hashed_bytes"], 6)
            (self.root / "moved/first").unlink()
            (self.root / "moved/second").unlink()
            (self.root / "moved").rmdir()
            (self.root / "moved").write_bytes(b"file")
            transitioned = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "moved" for r in f["status"]["changes"]))
            if initial["backend"] == "windows":
                self.assertFalse(transitioned["full_scan"])
                self.assertEqual(transitioned["status"]["hashed_bytes"], 4)
            self.assertEqual(transitioned["status"]["workspace_files"], 3)
            self.assertTrue(transitioned["journal"]["durable"])

    def test_invalid_ignore_range_reports_error_without_terminating_resident_observer(self):
        self.baseline()
        with self.watch("--journal", "--max-updates", 2, "--duration-ms", 4000) as run:
            run.until(lambda f: f["type"] == "status")
            (self.root / ".syrnckignore").write_bytes(b"[z-a]\n")
            error = run.until(lambda f: f["type"] == "error")
            self.assertEqual(error["message"], "invalid ignore pattern")
            self.assertTrue(error["journal"]["durable"])
            self.assertIsNone(run.process.poll())
            (self.root / ".syrnckignore").unlink()
            recovered = run.until(lambda f: f["type"] == "status" and f["reason"] == "recovered")
            self.assertTrue(recovered["full_scan"])
            self.assertTrue(recovered["status"]["clean"])
            run.finish()

    def test_busy_store_after_event_is_retried_without_losing_working_change(self):
        self.baseline()
        with self.watch("--journal", "--duration-ms", 5000, "--interval-ms", 40) as run:
            run.until(lambda f: f["type"] == "status")
            journal = (self.store / "watch-journal").read_bytes()
            with self.hold_store():
                (self.root / "asset").write_bytes(b"changed!")
                frame = run.until(lambda f: f["type"] == "error")
                self.assertEqual(frame["code"], "scan_failed")
                self.assertFalse(frame["journal"]["durable"])
                self.assertEqual((self.store / "watch-journal").read_bytes(), journal)
            frame = run.until(lambda f: f["type"] == "status" and f["reason"] == "recovered")
            self.assertTrue(frame["full_scan"])
            self.assertTrue(frame["journal"]["durable"])
            self.assertFalse(frame["status"]["clean"])
            self.assertEqual((self.root / "asset").read_bytes(), b"changed!")

    def test_hint_overflow_forces_byte_scan_and_drops_partial_path_list(self):
        self.baseline()
        with self.watch("--duration-ms", 5000, "--max-paths", 1) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("native path overflow requires a native backend")
            for number in range(128):
                (self.root / f"burst-{number}").write_bytes(b"x")
            overflow = run.until(lambda f: f["type"] == "status" and f["overflow"])
            self.assertEqual(overflow["reason"], "overflow")
            self.assertTrue(overflow["full_scan"])
            self.assertEqual(overflow["paths"], [])
            self.assertEqual(overflow["status"]["working_index"]["reused_files"], 0)
            self.assertEqual(overflow["status"]["counts"]["untracked"], 128)

    def test_restart_uses_full_observation_for_offline_edits(self):
        self.baseline()
        with self.watch("--max-updates", 1) as run:
            self.assertTrue(run.finish()[0]["status"]["clean"])
        asset = self.root / "asset"
        original = asset.stat()
        asset.write_bytes(b"offline!")
        os.utime(asset, ns=(original.st_atime_ns, original.st_mtime_ns))
        with self.watch("--max-updates", 1) as run:
            frame = run.finish()[0]
            self.assertTrue(frame["full_scan"])
            self.assertFalse(frame["status"]["clean"])
            self.assertEqual(frame["status"]["hashed_bytes"], 8)
            self.assertEqual(frame["status"]["working_index"]["reused_files"], 0)

    def test_journal_plain_encrypted_persistence_offline_edit_and_clear(self):
        for encrypted in (False, True):
            with self.subTest(encrypted=encrypted):
                if encrypted:
                    self.store = self.root / "vault"
                    self.env["SYRNCK_KEY"] = "47" * 32
                    self.call("init", "--encrypted")
                self.baseline()
                with self.watch("--journal", "--max-updates", 1) as run:
                    first = run.finish()[0]
                self.assertTrue(first["journal"]["durable"])
                self.assertEqual(first["journal"]["generation"], 2)
                self.assertEqual(first["journal"]["previous"]["state"], "missing")
                journal = json.loads(self.cmd("native", "watch-journal"))
                self.assertEqual(journal["state"], "observed")
                self.assertEqual(journal["session"], first["journal"]["session"])
                self.assertEqual(journal["observation"]["base"], first["status"]["base"])
                self.assertTrue(journal["observation"]["clean"])
                self.call("gc", "--delete")
                self.assertEqual(json.loads(self.cmd("native", "watch-journal")), journal)
                (self.root / "asset").write_bytes(b"offline!")
                with self.watch("--journal", "--max-updates", 1) as run:
                    restarted = run.finish()[0]
                self.assertTrue(restarted["full_scan"])
                self.assertEqual(restarted["status"]["hashed_bytes"], 8)
                self.assertFalse(restarted["status"]["clean"])
                self.assertEqual(restarted["journal"]["previous"],
                                 {"state": "observed", "session": first["journal"]["session"]})
                self.assertNotEqual(restarted["journal"]["session"], first["journal"]["session"])
                self.assertEqual(json.loads(self.cmd("native", "watch-journal", "--clear"))["state"], "cleared")
                self.assertEqual(json.loads(self.cmd("native", "watch-journal"))["state"], "absent")

    def test_journal_pending_recovery_and_quiet_polling(self):
        self.baseline()
        seed = subprocess.run([JOURNAL_HELPER, "pending", str(self.store)], capture_output=True, timeout=5)
        self.assertEqual(seed.returncode, 0, seed.stderr.decode("utf-8", "replace"))
        with self.watch("--journal", "--polling", "--interval-ms", 40, "--duration-ms", 1200) as run:
            frame = run.until(lambda f: f["type"] == "status")
            self.assertEqual(frame["journal"]["previous"]["state"], "pending")
            self.assertTrue(frame["full_scan"])
            original = (self.store / "watch-journal").read_bytes()
            time.sleep(0.25)
            self.assertEqual((self.store / "watch-journal").read_bytes(), original,
                             "quiet polling rewrote durable checkpoint")
            self.assertEqual(len(run.finish()), 1)

    def test_journal_session_lock_allows_inspection_but_refuses_clear_or_second_owner(self):
        self.baseline()
        with self.watch("--journal", "--duration-ms", 1400) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("idle Store assertion requires a native backend")
            journal = json.loads(self.cmd("native", "watch-journal"))
            self.assertEqual(journal["session"], initial["journal"]["session"])
            self.call("native", "watch-journal", "--clear", ok=False)
            self.call("native", "watch", "--journal", "--max-updates", 1, ok=False)
            self.assertEqual(len(run.finish()), 1)

    def test_corrupt_journal_is_preserved_until_explicit_clear(self):
        self.baseline()
        (self.store / "watch-journal").write_bytes(b"corrupt")
        with self.watch("--journal", "--duration-ms", 1200) as run:
            frames = run.finish(expected=1)
            self.assertEqual([f["type"] for f in frames], ["error"])
            self.assertFalse(frames[0]["journal"]["durable"])
        self.assertEqual((self.store / "watch-journal").read_bytes(), b"corrupt")
        self.call("native", "watch-journal", ok=False)
        self.call("native", "watch-journal", "--clear")
        with self.watch("--journal", "--max-updates", 1) as run:
            self.assertTrue(run.finish()[0]["status"]["clean"])

    def test_failed_scan_persists_unavailable_before_recovering(self):
        self.baseline()
        rules = self.root / ".syrnckignore"
        with self.watch("--journal", "--duration-ms", 4500, "--interval-ms", 40) as run:
            run.until(lambda f: f["type"] == "status")
            rules.write_bytes(b"[\n")
            frame = run.until(lambda f: f["type"] == "error")
            self.assertTrue(frame["journal"]["durable"])
            journal = json.loads(self.cmd("native", "watch-journal"))
            self.assertEqual(journal["state"], "unavailable")
            self.assertIsNone(journal["observation"])
            rules.unlink()
            run.until(lambda f: f["type"] == "status" and f["reason"] == "recovered")

    def test_journal_modified_during_session_is_preserved(self):
        self.baseline()
        with self.watch("--journal", "--duration-ms", 2200) as run:
            run.until(lambda f: f["type"] == "status")
            (self.store / "watch-journal").write_bytes(b"modified externally")
            (self.root / "asset").write_bytes(b"changed!")
            frame = run.until(lambda f: f["type"] == "error")
            self.assertFalse(frame["journal"]["durable"])
            run.finish(expected=1)
        self.assertEqual((self.store / "watch-journal").read_bytes(), b"modified externally")

    def test_directory_rename_and_file_directory_transition_are_observed(self):
        (self.root / "folder").mkdir()
        (self.root / "folder/file").write_bytes(b"tracked")
        self.baseline()
        with self.watch("--duration-ms", 5000) as run:
            run.until(lambda f: f["type"] == "status")
            (self.root / "folder").rename(self.root / "intermediate")
            (self.root / "intermediate").rename(self.root / "Folder")
            run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "Folder/file" for r in f["status"]["changes"]))
            (self.root / "asset").unlink()
            (self.root / "asset").mkdir()
            (self.root / "asset/child").write_bytes(b"child")
            frame = run.until(lambda f: f["type"] == "status" and any(
                r["path"] == "asset/child" for r in f["status"]["changes"]))
            self.assertTrue(any(r["path"] == "asset" and r["status"] == "removed" for r in frame["status"]["changes"]))

    def test_combined_observes_new_staging_reference(self):
        self.baseline()
        with self.watch("--combined", "--duration-ms", 3500) as run:
            run.until(lambda f: f["type"] == "status")
            (self.root / "asset").write_bytes(b"prepared")
            staged = self.call("native", "add", "asset").stdout.decode().strip()
            frame = run.until(lambda f: f["type"] == "status" and f["status"]["staged"]["snapshot"] == staged)
            self.assertFalse(frame["status"]["clean"])
            self.assertEqual(frame["status"]["staged"]["counts"]["modified"], 1)
            self.assertTrue(frame["status"]["unstaged"]["clean"])

    def test_polling_stays_quiet_but_full_audit_is_not_postponed(self):
        self.baseline()
        with self.watch("--polling", "--interval-ms", 40, "--audit-ms", 1000, "--duration-ms", 1600) as run:
            frames = run.finish()
            self.assertEqual([f["reason"] for f in frames], ["initial", "audit"])
            self.assertTrue(all(f["backend"] == "polling" and f["full_scan"] for f in frames))
            self.assertTrue(all(f["status"]["hashed_bytes"] == 8 for f in frames))

    def test_failed_observation_is_reported_then_recovered(self):
        self.baseline()
        refs = self.store / "refs.json"
        original = refs.read_bytes()
        with self.watch("--duration-ms", 5000, "--interval-ms", 40) as run:
            run.until(lambda f: f["type"] == "status")
            refs.write_bytes(b"{")
            error = run.until(lambda f: f["type"] == "error")
            self.assertEqual(error["code"], "scan_failed")
            self.assertIsNone(error["status"])
            refs.write_bytes(original)
            recovered = run.until(lambda f: f["type"] == "status" and f["reason"] == "recovered")
            self.assertTrue(recovered["full_scan"])
            self.assertTrue(recovered["status"]["clean"])
            self.assertEqual(refs.read_bytes(), original)

    def test_unresolved_failure_exits_nonzero_without_repeating_identical_errors(self):
        self.baseline()
        (self.store / "refs.json").write_bytes(b"{")
        with self.watch("--duration-ms", 1300, "--interval-ms", 40) as run:
            frames = run.finish(expected=1)
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0]["type"], "error")

    def test_limits_and_french_diagnostic(self):
        result = self.call("--lang", "fr", "native", "watch", "--interval-ms", 1, ok=False)
        self.assertIn("intervalle de surveillance", result.stderr.decode("utf-8"))
        for args in (("--debounce-ms", "5001"), ("--duration-ms", "1"), ("--audit-ms", "0"),
                     ("--max-updates", "1000001"), ("--max-paths", "0"), ("--combined", "--revision", "HEAD")):
            self.call("native", "watch", *args, ok=False)

    @unittest.skipIf(os.name == "nt", "non-UTF-8 byte names require POSIX")
    def test_invalid_utf8_ignored_name_requests_rescan_without_breaking_json(self):
        (self.root / ".syrnckignore").write_bytes(b"*.tmp\n")
        self.baseline()
        with self.watch("--duration-ms", 5000) as run:
            initial = run.until(lambda f: f["type"] == "status")
            if initial["backend"] == "polling":
                self.skipTest("native malformed-name hints require a native backend")
            raw = os.fsencode(self.root) + b"/ignored-\xff.tmp"
            descriptor = os.open(raw, os.O_WRONLY | os.O_CREAT, 0o600)
            os.write(descriptor, b"ignored")
            os.close(descriptor)
            frame = run.until(lambda f: f["type"] == "status" and f["full_scan"])
            self.assertTrue(frame["status"]["clean"])
            self.assertEqual(frame["paths"], [])


if __name__ == "__main__":
    unittest.main()
