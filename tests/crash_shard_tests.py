"""Coverage invariants for partitioned crash campaigns."""
import sys
import unittest
from unittest.mock import patch
from crash_shard import CrashShard


class CrashShardTests(unittest.TestCase):
    def test_every_case_is_selected_once_in_original_order(self):
        for length in (0, 1, 2, 3, 7, 8, 9, 127, 1025, 4096):
            cases = list(range(length))
            for count in (1, 2, 4, 8, 64):
                with self.subTest(length=length, count=count):
                    shards = [CrashShard(f"{index}/{count}").select(cases) for index in range(count)]
                    self.assertEqual([case for shard in shards for case in shard], cases)
                    self.assertLessEqual(max(map(len, shards)) - min(map(len, shards)), 1)

    def test_only_owned_cli_arguments_are_consumed(self):
        args = ["crash_probe.py", "--shard", "3/8", "Suite.test_case", "-v"]
        with patch.object(sys, "argv", args):
            shard = CrashShard.from_argv()
            self.assertEqual((shard.index, shard.count), (3, 8))
            self.assertEqual(sys.argv, ["crash_probe.py", "Suite.test_case", "-v"])
        with patch.object(sys, "argv", ["crash_probe.py"]):
            self.assertEqual(CrashShard.from_argv().select([1, 2]), [1, 2])

    def test_invalid_partition_fails_before_a_probe_runs(self):
        for value in ("", "0", "a/2", "-1/2", "1/1", "0/0", "0/65", "1/2/3"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                CrashShard(value)


if __name__ == "__main__":
    unittest.main()
