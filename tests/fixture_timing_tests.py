"""Verify bounded successful fixture cleanup and refusal of foreign paths."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import fixture_timing
from fixture_timing import case_directory, remove_owned_case


class FixtureLifetime(unittest.TestCase):
    def test_only_transient_windows_cleanup_is_retried_with_a_deadline(self):
        def windows_error(code):
            error = OSError("injected cleanup failure")
            error.winerror = code
            return error

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "case-1"
            root.mkdir()
            original = fixture_timing.shutil.rmtree
            calls = []

            def once(path):
                calls.append(path)
                if len(calls) < 3:
                    raise windows_error(32 if len(calls) == 1 else 145)
                original(path)

            with patch.object(fixture_timing, "WINDOWS", True), \
                    patch.object(fixture_timing.shutil, "rmtree", side_effect=once):
                remove_owned_case(temporary, "case-1")
            self.assertEqual(len(calls), 3)
            self.assertFalse(root.exists())
            for code in (32, 145, 5):
                root.mkdir()
                marker = root / "data"
                marker.write_bytes(b"inspect persistent cleanup failure")
                with patch.object(fixture_timing, "WINDOWS", True), \
                        patch.object(fixture_timing.shutil, "rmtree", side_effect=windows_error(code)) as remover:
                    with self.assertRaises(OSError):
                        remove_owned_case(temporary, "case-1", timeout=2 if code == 5 else 0)
                    self.assertEqual(remover.call_count, 1)
                self.assertEqual(marker.read_bytes(), b"inspect persistent cleanup failure")
                original(root)
            root.mkdir()
            with patch.object(fixture_timing, "WINDOWS", False), \
                    patch.object(fixture_timing.shutil, "rmtree", side_effect=windows_error(145)) as remover:
                with self.assertRaises(OSError):
                    remove_owned_case(temporary, "case-1")
                self.assertEqual(remover.call_count, 1)

    def test_verified_case_is_removed_with_separate_timing(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = io.StringIO()
            with redirect_stdout(output), case_directory(temporary, "case-1") as root:
                root.mkdir()
                (root / "data").write_bytes(b"retained until verification")
                self.assertEqual((root / "data").read_bytes(), b"retained until verification")
            self.assertFalse(root.exists())
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual([event["state"] for event in events], ["running", "verified", "cleaning", "cleaned"])
            self.assertTrue(all(event["elapsed_ms"] >= 0 for event in events))

    def test_failed_or_preexisting_case_is_not_retired(self):
        with tempfile.TemporaryDirectory() as temporary:
            with redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "test failure"):
                with case_directory(temporary, "case-1") as root:
                    root.mkdir()
                    (root / "data").write_bytes(b"inspect failure")
                    raise RuntimeError("test failure")
            self.assertEqual((root / "data").read_bytes(), b"inspect failure")
            with self.assertRaises(FileExistsError):
                with case_directory(temporary, "case-1"):
                    self.fail("preexisting fixture accepted")
            self.assertTrue((root / "data").is_file())

    def test_parent_escape_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryDirectory() as outside:
            for name in ("..", ".", ".syrnck", ".SyRnCk", "../outside", "child/leaf", "child\\leaf", str(Path(outside))):
                with self.assertRaises(ValueError):
                    with case_directory(temporary, name):
                        self.fail("foreign fixture accepted")

    def test_linked_root_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryDirectory() as outside:
            marker = Path(outside) / "marker"
            marker.write_bytes(b"foreign bytes")
            link = Path(temporary) / "linked"
            try:
                link.symlink_to(outside, target_is_directory=True)
            except OSError as error:
                self.skipTest("local symlink creation unavailable: " + str(error))
            with self.assertRaises(ValueError):
                with case_directory(temporary, "linked"):
                    self.fail("linked fixture accepted")
            self.assertEqual(marker.read_bytes(), b"foreign bytes")


if __name__ == "__main__":
    unittest.main()
