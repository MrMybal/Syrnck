"""Kill owned checkouts/merges and resume their exact durable targets offline."""
import json
import queue
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
import authorship_crashes as base


class OwnedCheckoutCrashes(unittest.TestCase):
    command = base.AuthorshipCrashes.command

    def scenarios(self):
        return [(encrypted, scope, action, transition)
                for encrypted in (False, True)
                for scope in ("primary", "linked", "partial", "detached")
                for action in ("checkout", "merge") for transition in (False, True)]

    def test_observed_owned_transaction_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-owned-checkout-crash-") as temporary:
            parent = Path(temporary)
            count = verified = resumed_without_key = 0
            for encrypted, scope, action, transition in base.SHARD.select(self.scenarios()):
                label = f"{encrypted}-{scope}-{action}-{transition}"
                seed = parent / ("seed-" + label)
                self.command(base.READER, "prepare", seed, "encrypted" if encrypted else "plain",
                             scope, action, "transition" if transition else "ordinary")
                trace = parent / ("trace-" + label)
                shutil.copytree(seed, trace)
                events = self.command(base.PROBE, "trace", trace)
                self.assertTrue(events)
                self.assertEqual(len({(row["phase"], row["kind"], row["occurrence"]) for row in events}), len(events))
                self.assertTrue(any(row["kind"] == "config" for row in events), "owned feature was not durably gated")
                self.assertTrue(any(row["kind"] == "refs" for row in events))
                for event in events:
                    with self.subTest(encrypted=encrypted, scope=scope, action=action, transition=transition, event=event):
                        root = parent / f"case-{count}"
                        count += 1
                        shutil.copytree(seed, root)
                        process = subprocess.Popen([base.PROBE, "run", str(root), event["phase"],
                            event["kind"], str(event["occurrence"])], env=base.ENV,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                        lines = queue.Queue()
                        reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
                        reader.start()
                        try:
                            line = lines.get(timeout=20)
                            if not line:
                                _, errors = process.communicate(timeout=8)
                                self.fail("owned checkpoint not reached: " + errors.decode("utf-8", "replace"))
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
                        untouched = base.snapshot(root)
                        before = self.command(base.READER, "verify", root)
                        self.assertTrue(before["verified"])
                        self.assertEqual(base.snapshot(root), untouched, "fresh reader modified durable recovery evidence")
                        after = self.command(base.READER, "resume", root)
                        self.assertTrue(after["verified"])
                        if before["pending"]:
                            self.assertTrue(after["resumed_without_key"], "active signed target required the private signing key")
                            resumed_without_key += 1
                        verified += 1
            print(f"{verified}/{count} owned process termination boundaries; {resumed_without_key} active targets resumed offline without signing key", flush=True)


if __name__ == "__main__":
    unittest.main()
