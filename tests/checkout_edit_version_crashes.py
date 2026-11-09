"""Kill publication of additional versions of an actively edited conflict copy."""
import shutil
import tempfile
import unittest
from pathlib import Path
import checkout_edit_crashes as edit


class CheckoutEditVersionCrashes(unittest.TestCase):
    command = edit.CheckoutEditCrashes.command
    kill_at = edit.CheckoutEditCrashes.kill_at

    def test_observed_copy_version_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-checkout-copy-versions-") as temporary:
            parent = Path(temporary)
            seed = parent / "seed"
            self.command(edit.base.READER, "prepare-copy-race", seed, "encrypted", "partial", "reverse")
            trace = parent / "trace"
            shutil.copytree(seed, trace)
            events = self.command(edit.base.PROBE, "trace", trace)
            self.assertTrue(events)
            self.assertGreaterEqual(self.command(edit.base.READER, "verify", trace)["versions"], 1)
            self.assertTrue(self.command(edit.base.READER, "backup", trace)["verified"])
            for count, event in enumerate(events):
                with self.subTest(event=event):
                    root = parent / f"case-{count}"
                    shutil.copytree(seed, root)
                    self.kill_at(root, event)
                    untouched = edit.base.snapshot(root)
                    self.assertTrue(self.command(edit.base.READER, "verify", root)["verified"])
                    self.assertEqual(edit.base.snapshot(root), untouched)
                    result = self.command(edit.base.READER, "resume", root)
                    self.assertTrue(result["verified"])
                    self.assertEqual(result["copies"], 2)
            print(f"{len(events)}/{len(events)} conflict-copy version publication termination points verified", flush=True)


if __name__ == "__main__":
    unittest.main()
