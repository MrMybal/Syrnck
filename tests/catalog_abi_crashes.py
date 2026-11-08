"""Terminate C ABI catalog publication and verify old/new atomic state and GC."""
import json
import queue
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
import authorship_crashes as base


class CatalogCrashes(unittest.TestCase):
    command = base.AuthorshipCrashes.command

    def test_catalog_publication_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="syrnck-catalog-cas-") as temporary:
            parent = Path(temporary)
            count = 0
            scenarios = [(encrypted, layout) for encrypted in (False, True) for layout in (0, 1, 2)]
            for encrypted, layout in base.SHARD.select(scenarios):
                seed = parent / f"seed-{encrypted}-{layout}"
                self.command(base.READER, "prepare", seed, "encrypted" if encrypted else "plain", layout)
                trace = parent / f"trace-{encrypted}-{layout}"
                shutil.copytree(seed, trace)
                events = self.command(base.PROBE, "trace", trace)
                self.assertTrue(events)
                for event in events:
                    with self.subTest(encrypted=encrypted, layout=layout, event=event):
                        case = parent / f"case-{count}"
                        count += 1
                        shutil.copytree(seed, case)
                        process = subprocess.Popen([base.PROBE, "run", str(case), event["phase"], str(event["occurrence"])],
                            env=base.ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                        output = queue.Queue()
                        reader = threading.Thread(target=lambda: output.put(process.stdout.readline()), daemon=True)
                        reader.start()
                        try:
                            line = output.get(timeout=20)
                            if not line:
                                _, errors = process.communicate(timeout=8)
                                self.fail("catalog checkpoint missing: " + errors.decode("utf-8", "replace"))
                            self.assertEqual(json.loads(line), event)
                            self.assertIsNone(process.poll())
                            process.kill()
                            process.communicate(timeout=8)
                        finally:
                            if process.poll() is None:
                                process.kill()
                            process.communicate(timeout=8)
                            reader.join(timeout=5)
                        before = base.snapshot(case)
                        self.assertTrue(self.command(base.READER, "verify", case)["verified"])
                        self.assertEqual(base.snapshot(case), before, "reader changed catalog publication evidence")
                        self.assertTrue(self.command(base.READER, "resume", case)["verified"])
            print(f"{count}/{count} catalog CAS termination boundaries verified, including data/state reconstruction after GC", flush=True)


if __name__ == "__main__":
    unittest.main()
