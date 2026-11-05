"""Kill native writers at deterministic IO boundaries and reopen in fresh processes."""
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

PROBE = Path(sys.argv.pop(1)).resolve(strict=True)
READER = Path(sys.argv.pop(1)).resolve(strict=True)
PRODUCTION = Path(sys.argv.pop(1)).resolve(strict=True)
ENV = {key: value for key, value in os.environ.items()
       if key.upper() not in ("SYRNCK_KEY", "SYRNCK_TOKEN", "SYRNCK_LANG")}
PHASES = ("atomic.temporary_opened", "atomic.writer_finished", "atomic.file_synced",
          "atomic.published", "atomic.durable")


def cases():
    result = []

    def atomic(operation, kinds, occurrence=1):
        for kind in kinds:
            result.extend((operation, phase, kind, occurrence) for phase in PHASES)
            if os.name != "nt" and kind not in ("refs", "config"):
                result.append((operation, "atomic.linked", kind, occurrence))

    atomic("commit", ("chunk", "manifest", "revision", "refs"))
    atomic("stage-add", ("chunk", "manifest", "revision", "refs"))
    atomic("stage-commit", ("revision", "refs"))
    atomic("stage-reset", ("refs",))
    atomic("branch-create", ("refs",))
    for operation in ("branch-delete", "branch-delete-index"):
        atomic(operation, ("chunk", "refs"))
    for operation in ("branch-restore", "branch-restore-index"):
        atomic(operation, ("refs",))
    atomic("branch-restore-as", ("revision", "refs"))
    for operation in ("stage-rebase", "stage-rebase-resolved", "stage-rebase-empty", "stage-rebase-unborn"):
        atomic(operation, ("revision", "refs"))
    atomic("amend", ("chunk", "manifest", "revision", "refs"))
    atomic("amend-staged", ("revision", "refs"))
    atomic("amend-message", ("revision", "refs"))
    for occurrence in (1, 2, 3, 4):
        atomic("checkout", ("refs",), occurrence)
    atomic("checkout", ("checkout-prepared",))
    for kind in ("checkout-backup", "export-file"):
        result.extend(("checkout", phase, kind, 1) for phase in ("move.before", "move.published", "move.durable"))
    result.extend(("checkout", phase, "checkout", 1) for phase in
                  ("checkout.prepared", "checkout.applied", "checkout.committed", "checkout.cleaned"))
    for occurrence in (1, 2, 3):
        atomic("checkout-abort", ("refs",), occurrence)
    for kind in ("checkout-prepared", "export-file"):
        result.extend(("checkout-abort", phase, kind, 1) for phase in ("move.before", "move.published", "move.durable"))
    result.append(("checkout-abort", "checkout.cleaned", "checkout", 1))
    result.extend(("commit", phase, "chunk", 2) for phase in ("atomic.file_synced", "atomic.published"))
    for occurrence in (1, 2):
        atomic("detach", ("refs",), occurrence)
    result.extend(("detach", phase, "revision", 1) for phase in ("atomic.file_synced", "atomic.published"))
    for operation in ("append", "compact"):
        atomic(operation, ("pack", "index", "config"))
        result.extend(((operation, "pack.envelope_written", "pack", 2),
                       (operation, "atomic.file_synced", "pack", 2),
                       (operation, "atomic.published", "index", 2),
                       (operation, "pack.loose_removed", "manifest", 1),
                       (operation, "pack.loose_removed", "chunk", 2)))
        if operation == "compact":
            result.extend((operation, "pack.obsolete_removed", kind, 1) for kind in ("pack", "index"))
    atomic("gc", ("pack", "index", "config"))
    result.extend((("gc", "pack.envelope_written", "pack", 2),
                   ("gc", "gc.loose_removed", "chunk", 2),
                   ("gc", "gc.loose_removed", "manifest", 1)))
    result.extend(("gc", "pack.obsolete_removed", kind, 1) for kind in ("pack", "index"))
    atomic("restore", ("restore",))
    atomic("export", ("export-file",))
    result.extend(("export", phase, "export", 1) for phase in
                  ("directory.created", "directory.writer_finished", "directory.synced",
                   "directory.published", "directory.durable"))
    result.extend(("switch" if operation == "checkout" else "switch-abort", phase, kind, occurrence)
                  for operation, phase, kind, occurrence in list(result)
                  if operation in ("checkout", "checkout-abort"))
    for occurrence in (1, 2):
        atomic("switch", ("chunk",), occurrence)
    atomic("tag-create", ("chunk", "refs"))
    atomic("tag-delete", ("chunk", "refs"))
    atomic("tag-restore", ("refs",))
    for replay in ("cherry-pick", "revert"):
        result.extend((replay if operation == "checkout" else replay + "-abort", phase, kind, occurrence)
                      for operation, phase, kind, occurrence in list(result)
                      if operation in ("checkout", "checkout-abort"))
        atomic(replay, ("revision",))
    for merge in ("merge", "merge-fast-forward", "merge-resolved"):
        result.extend((merge if operation == "checkout" else merge + "-abort", phase, kind, occurrence)
                      for operation, phase, kind, occurrence in list(result)
                      if operation in ("checkout", "checkout-abort"))
        if merge != "merge-fast-forward":
            atomic(merge, ("revision",))
    for rewrite in ("reset-soft", "reset-soft-index", "reset-mixed", "reset-mixed-index", "squash", "squash-root"):
        atomic(rewrite, ("refs",))
        if rewrite.startswith("reset-soft") or rewrite.startswith("squash"):
            atomic(rewrite, ("revision",))
    for rebase in ("rebase", "rebase-resolved", "rebase-move"):
        result.extend((rebase if operation == "checkout" else rebase + "-abort", phase, kind, occurrence)
                      for operation, phase, kind, occurrence in list(result)
                      if operation in ("checkout", "checkout-abort"))
        if rebase != "rebase-move":
            for occurrence in (1, 2):
                atomic(rebase, ("revision",), occurrence)
    return result


def command(mode, root, *args, ok=True):
    result = subprocess.run([str(READER), mode, str(root), *map(str, args)],
                            capture_output=True, text=True, encoding="utf-8", env=ENV, timeout=30)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{mode} returned {result.returncode}: {result.stderr}")
    return json.loads(result.stdout) if ok else result


def snapshot(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


class CrashRecovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="syrnck-crash-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.seeds = {}
        for encrypted in (False, True):
            for operation in sorted({row[0] for row in cases()}):
                directory = cls.base / f"seed-{encrypted}-{operation}"
                root = directory / "store"
                info = command("prepare", root, "encrypted" if encrypted else "plain", operation)
                (directory / "fixture.json").write_text(json.dumps(info), encoding="utf-8")
                cls.seeds[encrypted, operation] = directory

    def launch(self, root, phase, kind, occurrence):
        process = subprocess.Popen([str(PROBE), "run", str(root), phase, kind, str(occurrence)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", env=ENV)
        lines = queue.Queue()
        reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = lines.get(timeout=25)
            reader.join(timeout=5)
            self.assertTrue(line, "Probe exited without reaching the requested checkpoint")
            event = json.loads(line)
            self.assertEqual(event.get("protocol"), "syrnck-test-io-checkpoint-v1", event)
            self.assertEqual((event["phase"], event["kind"], event["occurrence"]),
                             (phase, kind, occurrence))
            self.assertIsNone(process.poll(), "Probe did not wait at its checkpoint")
            return process, event
        except BaseException:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            reader.join(timeout=5)
            _, error = process.communicate(timeout=10)
            print(f"Checkpoint failure: {error}", file=sys.stderr)
            raise

    def assert_state(self, root, info, operation, phase, kind, occurrence, event, checked):
        published = phase in ("atomic.published", "atomic.durable", "atomic.linked",
                              "directory.published", "directory.durable")
        target = Path(event["target"]).resolve()
        root = root.resolve()
        sibling = target.relative_to(root.parent).parts[0] if target.is_relative_to(root.parent) else ""
        staging = sibling.startswith(".syrnck-export-") and len(sibling) == 47 and all(
            c in "0123456789abcdef" for c in sibling[15:])
        self.assertTrue(target.is_relative_to(root) or target in (root.parent / "restored.bin", root.parent / "exported") or
                        ((operation in ("checkout", "checkout-abort", "switch", "switch-abort", "cherry-pick", "cherry-pick-abort", "revert", "revert-abort") or operation.startswith(("merge", "rebase"))) and target == root.parent / "asset.bin") or
                        (operation == "export" and staging),
                        f"Checkpoint target {target} is outside fixture {root}")
        if kind == "config":
            document = json.loads((root / "config.json").read_text())
            self.assertEqual(document["packs"] != info["initial_catalog"], published)
        elif phase in PHASES or phase == "atomic.linked":
            if kind in ("chunk", "manifest", "revision", "pack", "index", "restore", "export-file", "checkout-prepared"):
                self.assertEqual(target.exists(), published)
        elif phase.startswith("directory."):
            self.assertEqual(target.exists(), published)
        if operation == "export":
            self.assertEqual((root.parent / "exported").exists(), kind == "export" and published)
        if operation == "commit":
            changed = checked["refs"]["heads/main"] != info["old_revision"]
            self.assertEqual(changed, kind == "refs" and published)
        elif operation.startswith("branch-"):
            changed = kind == "refs" and published
            expected = dict(info["refs"])
            name = info["branch_name"]
            if changed:
                if operation.startswith("branch-delete"):
                    expected.pop("heads/" + name)
                    expected.pop("staging/" + name, None)
                    prefix = "archive/branches/" + info["branch_archive"]
                    expected[prefix + "/record"] = {"kind": "chunk", "id": info["branch_archive"]}
                    expected[prefix + "/target"] = info["branch_target"]
                    if "branch_index" in info:
                        expected[prefix + "/index"] = info["branch_index"]
                else:
                    destination = info.get("branch_destination", name)
                    expected["heads/" + destination] = info["branch_target"]
                    if info.get("branch_restored_index") is not None:
                        expected["staging/" + destination] = {"kind": "revision", "id": info["branch_restored_index"]}
            self.assertEqual(checked["refs"], expected)
        elif operation.startswith("stage-rebase"):
            changed = kind == "refs" and published
            refs = checked["refs"]
            previous = info["old_index"]
            archive = "archive/rebase-index/" + previous["id"]
            self.assertEqual(refs["heads/main"], info["old_revision"])
            self.assertEqual(refs["staging/main"] != previous, changed)
            self.assertEqual(archive in refs, changed)
            if changed:
                self.assertEqual(refs[archive], previous)
                self.assertEqual(refs["staging/main"]["id"], info["stage_target"])
            unrelated = lambda values: {name: key for name, key in values.items()
                                         if name not in ("staging/main", archive)}
            self.assertEqual(unrelated(refs), unrelated(info["refs"]))
        elif operation.startswith("reset-") or operation in ("squash", "squash-root"):
            changed = kind == "refs" and published
            refs = checked["refs"]
            archive = "archive/" + ("reset/" if operation.startswith("reset-") else "squash/") + info["old_revision"]["id"]
            self.assertEqual(refs["heads/main"] != info["old_revision"], changed)
            self.assertEqual(archive in refs, changed)
            expected_index = operation.startswith("reset-soft") if changed else "old_index" in info
            self.assertEqual("staging/main" in refs, expected_index)
            ignored = {"heads/main", "staging/main", archive}
            if "old_index" in info:
                saved = "archive/reset-index/" + info["old_index"]["id"]
                self.assertEqual(saved in refs, changed)
                if changed:
                    self.assertEqual(refs[saved], info["old_index"])
                ignored.add(saved)
            self.assertEqual({key: value for key, value in refs.items() if key not in ignored},
                             {key: value for key, value in info["refs"].items() if key not in ignored})
        elif operation in ("amend", "amend-staged", "amend-message"):
            changed = kind == "refs" and published
            refs = checked["refs"]
            archive = "archive/amend/" + info["old_revision"]["id"]
            self.assertEqual(refs["heads/main"] != info["old_revision"], changed)
            self.assertEqual(archive in refs, changed)
            self.assertEqual("staging/main" in refs, operation == "amend-staged" and not changed)
            if changed:
                self.assertEqual(refs[archive], info["old_revision"])
            unrelated = lambda values: {name: key for name, key in values.items()
                                         if name not in ("heads/main", "staging/main", archive)}
            self.assertEqual(unrelated(refs), unrelated(info["refs"]))
        elif operation in ("tag-create", "tag-delete", "tag-restore"):
            changed = kind == "refs" and published
            tagged = "tags/release/v1" in checked["refs"]
            self.assertEqual(tagged, not changed if operation == "tag-delete" else changed)
            unrelated = lambda refs: {name: key for name, key in refs.items()
                                      if not name.startswith(("tags/", "tag-metadata/", "archive/tags/"))}
            self.assertEqual(unrelated(checked["refs"]), unrelated(info["refs"]))
        elif operation in ("checkout", "checkout-abort", "cherry-pick", "cherry-pick-abort", "revert", "revert-abort") or operation.startswith(("merge", "rebase")):
            changed = (operation in ("checkout", "cherry-pick", "revert") or (operation.startswith(("merge", "rebase")) and not operation.endswith("-abort"))) and (
                (kind == "refs" and (occurrence == 4 or (occurrence == 3 and published))) or
                phase in ("checkout.committed", "checkout.cleaned"))
            self.assertEqual(checked["refs"]["heads/main"] != info["old_revision"], changed)
        elif operation in ("switch", "switch-abort"):
            committed = operation == "switch" and (
                (kind == "refs" and (occurrence == 4 or (occurrence == 3 and published))) or
                phase in ("checkout.committed", "checkout.cleaned"))
            self.assertEqual("workspace/HEAD" in checked["refs"], committed)
            self.assertEqual(checked["refs"]["heads/main"], info["old_revision"])
            self.assertEqual(checked["refs"]["heads/feature"], info["new_revision"])
        elif operation in ("stage-add", "stage-commit", "stage-reset"):
            changed = kind == "refs" and published
            refs = checked["refs"]
            self.assertEqual(refs["heads/main"] != info["old_revision"],
                             operation == "stage-commit" and changed)
            self.assertEqual("staging/main" in refs, changed if operation == "stage-add" else not changed)
            self.assertEqual({k: v for k, v in refs.items() if k not in ("heads/main", "staging/main")},
                             {k: v for k, v in info["refs"].items() if k not in ("heads/main", "staging/main")})
        elif operation == "detach":
            archive = "archive/" + info["old_revision"]["id"]
            archived = archive in checked["refs"]
            changed = checked["refs"]["heads/main"] != info["old_revision"]
            self.assertEqual(changed, kind == "refs" and occurrence == 2 and published)
            if kind == "refs":
                self.assertEqual(archived, occurrence == 2 or published)
            else:
                self.assertTrue(archived)
        else:
            self.assertEqual(checked["refs"], info["refs"])
        if operation == "gc" and kind == "config" and published:
            # The published GC catalog no longer retains dead packed chunks.
            # Their dead loose manifest duplicate is still awaiting cleanup.
            self.assertFalse(checked["audit_ok"])
        elif operation == "gc" and (phase in PHASES or phase.startswith("pack.")):
            self.assertTrue(checked["audit_ok"])
        if phase in PHASES[:3]:
            self.assertTrue(any(path.name.startswith(".tmp") for path in target.parent.iterdir()),
                            "The killed writer did not leave its unpublished temporary file")

    def test_production_has_no_checkpoint_controls(self):
        probe = PROBE.read_bytes()
        production = PRODUCTION.read_bytes()
        reader = READER.read_bytes()
        for label in (b"atomic.temporary_opened", b"pack.envelope_written", b"gc.loose_removed", b"directory.created"):
            self.assertIn(label, probe)
            self.assertNotIn(label, production)
            self.assertNotIn(label, reader)

    def test_release_control_and_exclusive_lock(self):
        directory = self.base / "release-control"
        shutil.copytree(self.seeds[False, "commit"], directory)
        root = directory / "store"
        process, _ = self.launch(root, "atomic.file_synced", "refs", 1)
        try:
            locked = command("verify", root, ok=False)
            self.assertIn("store busy", locked.stderr)
            output, error = process.communicate("continue\n", timeout=30)
            self.assertEqual(process.returncode, 0, error)
            self.assertTrue(json.loads(output)["completed"])
            self.assertTrue((directory / "unwound.txt").is_file())
            self.assertTrue(command("verify", root)["audit_ok"])
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)

    def test_hard_kill_reopen_and_resume(self):
        completed = 0
        verified = 0
        for encrypted in (False, True):
            for operation, phase, kind, occurrence in cases():
                with self.subTest(encrypted=encrypted, operation=operation, phase=phase,
                                  kind=kind, occurrence=occurrence):
                    directory = self.base / f"case-{completed}-données"
                    # A failing subtest must not reuse the next subtest's path.
                    completed += 1
                    shutil.copytree(self.seeds[encrypted, operation], directory)
                    root = directory / "store"
                    info = json.loads((directory / "fixture.json").read_text())
                    untouched = snapshot(root)
                    process, event = self.launch(root, phase, kind, occurrence)
                    try:
                        process.kill()  # SIGKILL on Unix, TerminateProcess on Windows.
                        process.communicate(timeout=10)
                        self.assertNotEqual(process.returncode, 0)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.communicate(timeout=10)
                    self.assertFalse((directory / "unwound.txt").exists(), "Stack destructors ran")
                    after_kill = snapshot(root)
                    checked = command("verify", root)
                    self.assertEqual(snapshot(root), after_kill, "Reopening/auditing changed stored bytes")
                    self.assertTrue(checked["retained_files_verified"])
                    self.assert_state(root, info, operation, phase, kind, occurrence, event, checked)
                    recovered = command("resume", root)
                    self.assertTrue(recovered["audit_ok"] and recovered["retained_files_verified"])
                    if operation in ("append", "gc"):
                        # Append preserves every old pack. GC preserves the fully live one.
                        catalog = info["initial_catalog"] if operation == "append" else [info["initial_catalog"][1]]
                        for pack in catalog:
                            for extension in ("syp", "syi"):
                                relative = f"packs/pack-{pack['name']}.{extension}"
                                self.assertEqual(hashlib.sha256((root / relative).read_bytes()).hexdigest(),
                                                 untouched[relative])
                    if operation in ("append", "compact", "gc"):
                        document = json.loads((root / "config.json").read_text())
                        active = {f"pack-{pack['name']}.{extension}" for pack in document["packs"]
                                  for extension in ("syp", "syi")}
                        present = {path.name for path in (root / "packs").iterdir()
                                   if path.name.startswith("pack-")}
                        self.assertEqual(present, active, "Inactive generated packs were not cleaned")
                    verified += 1
        print(f"{verified}/{completed} process-kill checkpoints verified", flush=True)


if __name__ == "__main__":
    unittest.main()
