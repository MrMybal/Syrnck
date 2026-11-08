"""Partition private crash campaigns without dropping or repeating scenarios."""
import argparse
import sys


class CrashShard:
    def __init__(self, value):
        try:
            self.index, self.count = map(int, value.split("/"))
            if not 0 <= self.index < self.count <= 64:
                raise ValueError()
        except ValueError:
            raise ValueError("--shard must be INDEX/COUNT with 0 <= INDEX < COUNT <= 64") from None

    @classmethod
    def from_argv(cls):
        parser = argparse.ArgumentParser(add_help=False)
        parser.add_argument("--shard", default="0/1")
        selection, remaining = parser.parse_known_args()
        sys.argv[1:] = remaining
        try:
            return cls(selection.shard)
        except ValueError as error:
            raise SystemExit(str(error)) from None

    def select(self, cases):
        # Contiguous groups also keep expensive seed preparation local to each
        # shard, instead of preparing every operation once per test process.
        start = len(cases) * self.index // self.count
        end = len(cases) * (self.index + 1) // self.count
        return cases[start:end]
