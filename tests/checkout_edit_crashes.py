"""Terminate regular-file edit preservation at each observed durable boundary."""
import json
import queue
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
import authorship_crashes as base


class CheckoutEditCrashes(unittest.TestCase):
    command = base.AuthorshipCrashes.command

    def scenarios(self):
        return [(encrypted, scope, transition) for encrypted in (False, True)
                for scope in ("primary", "partial") for transition in (False, True)]

    def kill_at(self, root, event):
        process = subprocess.Popen([base.PROBE, "run", str(root), event["phase"], event["kind"],
                                    str(event["occurrence"])], env=base.ENV,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        lines = queue.Queue()
        reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = lines.get(timeout=20)
            if not line:
                _, errors = process.communicate(timeout=8)
                self.fail("preservation checkpoint not reached: " + errors.decode("utf-8", "replace"))
            self.assertEqual(json.loads(line), event)
            self.assertIsNone(process.poll())
            process.kill()
            process.communicate(timeout=8)
            self.assertNotEqual(process.returncode, 0)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=8)
            reader.join(timeout=5)

    def test_all_durable_boundaries(self):
        verified = 0
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-edits-") as temporary:
            parent = Path(temporary)
            for encrypted, scope, transition in base.SHARD.select(self.scenarios()):
                label = f"{encrypted}-{scope}-{transition}"
                seed = parent / ("seed-" + label)
                self.command(base.READER, "prepare", seed, "encrypted" if encrypted else "plain",
                             scope, "transition" if transition else "ordinary")
                trace = parent / ("trace-" + label)
                shutil.copytree(seed, trace)
                events = self.command(base.PROBE, "trace", trace)
                for phase in ("checkout-edit.prepared", "checkout-edit.moved", "checkout-edit.restored"):
                    self.assertTrue(any(row["phase"] == phase for row in events), phase)
                for event in events:
                    with self.subTest(scenario=label, event=event):
                        root = parent / f"case-{verified}"
                        shutil.copytree(seed, root)
                        self.kill_at(root, event)
                        untouched = base.snapshot(root)
                        self.assertTrue(self.command(base.READER, "verify", root)["verified"])
                        self.assertEqual(base.snapshot(root), untouched, "read-only validation changed recovery state")
                        result = self.command(base.READER, "resume", root)
                        self.assertTrue(result["verified"])
                        self.assertEqual(result["copies"], 2)
                        verified += 1
            print(f"{verified}/{verified} preservation termination points resumed keylessly with two exact copies", flush=True)

    def test_namespace_relocation_boundaries(self):
        if (True, "partial", False) not in list(base.SHARD.select(self.scenarios())):
            self.skipTest("namespace relocation scenarios belong to a different shard")
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-edit-layouts-") as temporary:
            parent = Path(temporary)
            for layout in ("reverse", "case"):
                for phase in ("checkout-edit.prepared", "checkout-edit.moved", "checkout-edit.restored"):
                    with self.subTest(layout=layout, phase=phase):
                        root = parent / (layout + phase)
                        self.command(base.READER, "prepare", root, "encrypted", "partial", layout)
                        self.kill_at(root, {"phase": phase, "kind": "filesystem", "occurrence": 1})
                        result = self.command(base.READER, "resume", root)
                        self.assertTrue(result["verified"])
                        self.assertEqual(result["copies"], 2)

    def test_edits_during_preservation_keep_every_version(self):
        if (False, "primary", False) not in list(base.SHARD.select(self.scenarios())):
            self.skipTest("concurrent writer scenarios belong to a different shard")
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-edit-races-") as temporary:
            parent = Path(temporary)
            for phase, location in [("checkout-edit.prepared", "original"),
                                    ("checkout-edit.moved", "original"),
                                    ("checkout-edit.restored", "original"),
                                    ("checkout-edit.moved", "copy")]:
                with self.subTest(phase=phase, location=location):
                    root = parent / (phase + "-" + location)
                    self.command(base.READER, "prepare", root, "encrypted", "partial", "transition")
                    result = self.command(base.PROBE, "race", root, phase, location)
                    self.assertTrue(result["verified"])
                    self.assertEqual(result["copies"], 2 if location == "copy" else 3)

    def test_existing_conflict_copy_is_never_replaced(self):
        if (False, "primary", False) not in list(base.SHARD.select(self.scenarios())):
            self.skipTest("collision scenario belongs to a different shard")
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-edit-collision-") as temporary:
            root = Path(temporary)
            self.command(base.READER, "prepare", root, "plain", "primary", "ordinary")
            self.kill_at(root, {"phase": "checkout-edit.prepared", "kind": "filesystem", "occurrence": 1})
            path = Path(self.command(base.READER, "inject-collision", root)["path"])
            result = self.command(base.READER, "resume", root)
            self.assertTrue(result["verified"])
            self.assertEqual(result["copies"], 3)
            self.assertEqual(path.read_bytes(), b"foreign conflict copy")



if __name__ == "__main__":
    unittest.main()
