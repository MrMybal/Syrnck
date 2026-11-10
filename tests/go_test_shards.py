"""Every discovered Go test/example/fuzz seed is selected exactly once."""
import json
from pathlib import Path
import random
import re
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from go_test_shards import listed_cases, plan


def output(cases):
    return "\n".join(json.dumps({"Action": "output", "Package": package, "Output": name + "\n"})
                     for package, name in cases)


class GoTestShards(unittest.TestCase):
    def test_shared_names_examples_fuzz_and_unicode_are_covered_once(self):
        cases = [("adapter", "TestShared"), ("cli", "TestShared"), ("adapter", "TestProtocol"),
                 ("cli", "TestConfig"), ("adapter", "ExampleStore"), ("adapter", "FuzzDecode"), ("cli", "TestÉtat")]
        groups = plan(listed_cases(output(cases)))
        self.assertEqual(sum(count for _, count in groups), len(cases))
        for pair in cases:
            self.assertEqual(sum(bool(re.fullmatch(pattern, pair[1])) for pattern, _ in groups), 1, pair)
        self.assertFalse(any(re.fullmatch(pattern, "TestSharedExtra") for pattern, _ in groups))

    def test_generated_plans_are_disjoint_complete_and_nonempty(self):
        rng = random.Random(1871)
        for total in range(1, 81):
            cases = [("pkg" + str(rng.randrange(4)), "TestCase" + str(at)) for at in range(total)]
            for count in (1, 2, 3, 8, 32):
                groups = plan(listed_cases(output(cases)), count)
                self.assertEqual(sum(size for _, size in groups), total)
                self.assertTrue(all(size for _, size in groups))
                for _, name in cases:
                    self.assertEqual(sum(bool(re.fullmatch(pattern, name)) for pattern, _ in groups), 1)

    def test_listing_noise_is_not_a_case(self):
        value = output([("pkg", "TestActual"), ("pkg", "ok pkg"), ("pkg", "BenchmarkIgnored")])
        self.assertEqual(listed_cases(value), [("pkg", "TestActual")])

    def test_failed_missing_or_ambiguous_listing_is_refused(self):
        for value in ("", "not JSON", '{"Action":"fail"}', '{"Action":"output","Output":"TestNoPackage"}',
                      output([("pkg", "TestRepeated"), ("pkg", "TestRepeated")])):
            with self.assertRaises(ValueError): listed_cases(value)
        for count in (0, -1, 65, "2"):
            with self.assertRaises(ValueError): plan([("pkg", "TestCase")], count)


if __name__ == "__main__": unittest.main()
