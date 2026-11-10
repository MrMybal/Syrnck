"""Audit actual CTest registrations against every named native test scenario."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def document(command):
    result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8", timeout=20)
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--declarations", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--suite", action="append", nargs=4, required=True, metavar=("NAME", "EXECUTABLE", "SHARDS", "CASES"))
    parser.add_argument("--campaign", action="append", nargs=3, default=[], metavar=("NAME", "SHARDS", "SCENARIOS"))
    args = parser.parse_args()
    # CTest's --show-only rewrites LastTest.log, even without executing tests.
    # Generate the plan during build and consume it without nesting CTest here.
    snapshot = json.loads(args.plan.read_text(encoding="utf-8"))
    assert snapshot["version"] == 1 and snapshot["configuration"] == args.config, snapshot
    assert snapshot["declarations_sha256"] == hashlib.sha256(args.declarations.read_bytes()).hexdigest(), "stale CTest case plan; rebuild test_case_plan"
    plan = snapshot["plan"]
    planned = {test["name"]: test for test in plan["tests"]}
    exclusive = [name for name in ("go_binding", "syncthing_adapter") if name in planned]
    if args.sanitizers:
        exclusive += [name for name in ("reference_tables", "server_authentication", "documentation_help") if name in planned]
    for name in exclusive:
        properties = {item["name"]: item["value"] for item in planned[name].get("properties", [])}
        assert properties.get("RUN_SERIAL") is True, (name, "load-sensitive test requires exclusive CTest scheduling")
    total = 0
    for name, executable, shards, expected in args.suite:
        shards, expected = int(shards), int(expected)
        full = document([executable, "--list"])
        cases = full["cases"]
        assert full["version"] == 1 and full["total"] == len(cases) == expected, (name, full)
        assert len(set(cases)) == expected, (name, "duplicate case identity")
        covered = []
        for index in range(shards):
            test = planned[f"{name}_{index}"]
            command = test["command"]
            assert os.path.normcase(str(Path(command[0]).resolve())) == os.path.normcase(str(Path(executable).resolve())), test
            assert command[1:] == ["--shard", f"{index}/{shards}"], test
            properties = {item["name"]: item["value"] for item in test.get("properties", [])}
            assert not properties.get("DISABLED") and not properties.get("WILL_FAIL"), test
            assert "SKIP_RETURN_CODE" not in properties and "SKIP_REGULAR_EXPRESSION" not in properties, test
            subset = document([*command, "--list"])
            assert subset["index"] == index and subset["count"] == shards and subset["total"] == expected, (name, subset)
            assert subset["cases"], (name, "empty shard")
            covered.extend(subset["cases"])
        assert covered == cases and len(set(covered)) == expected, (name, "dropped, repeated or reordered scenarios")
        total += expected
    crash_total = 0
    for name, shards, expected in args.campaign:
        shards, expected = int(shards), int(expected)
        command = planned[f"{name}_0"]["command"]
        at = command.index("--shard")
        full = document([*command[:at], *command[at+2:], "--list-scenarios"])
        cases = full["scenarios"]
        assert full["version"] == 1 and full["index"] == 0 and full["count"] == 1 and len(cases) == expected, (name, full)
        assert len({json.dumps(case) for case in cases}) == expected, (name, "duplicate scenario")
        covered = []
        for index in range(shards):
            test = planned[f"{name}_{index}"]
            command = test["command"]
            at = command.index("--shard")
            assert command[at+1] == f"{index}/{shards}", test
            properties = {item["name"]: item["value"] for item in test.get("properties", [])}
            assert not properties.get("DISABLED") and not properties.get("WILL_FAIL"), test
            assert "SKIP_RETURN_CODE" not in properties and "SKIP_REGULAR_EXPRESSION" not in properties, test
            subset = document([*command, "--list-scenarios"])
            assert subset["version"] == 1 and subset["index"] == index and subset["count"] == shards and subset["scenarios"], (name, subset)
            covered.extend(subset["scenarios"])
        assert covered == cases, (name, "dropped or repeated crash scenario")
        crash_total += expected
    print(json.dumps({"suites": len(args.suite), "scenarios": total, "crash_campaigns": len(args.campaign),
                      "crash_scenarios": crash_total, "exactly_once": True}))


if __name__ == "__main__":
    main()
