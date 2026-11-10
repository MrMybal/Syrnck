"""Plan complete, disjoint Go test runs from the toolchain's JSON listing."""
import json
import re


def listed_cases(output):
    cases = set()
    for line in output.splitlines():
        event = json.loads(line)
        if event.get("Action") == "fail":
            raise ValueError("Go test discovery failed")
        if event.get("Action") != "output":
            continue
        name = event.get("Output", "").rstrip("\r\n")
        if not name.startswith(("Test", "Example", "Fuzz")) or not name.isidentifier():
            continue
        package = event.get("Package")
        if not isinstance(package, str) or not package:
            raise ValueError("Go test listing has no package")
        pair = (package, name)
        if pair in cases:
            raise ValueError("Go test listing repeats a case")
        cases.add(pair)
    if not cases:
        raise ValueError("Go test discovery found no cases")
    return sorted(cases)


def plan(cases, count=2):
    if not isinstance(count, int) or count < 1 or count > 64:
        raise ValueError("Go test shard count must be in 1..64")
    names = sorted({name for _, name in cases})
    if not names:
        raise ValueError("Go test discovery found no cases")
    count = min(count, len(names))
    groups = [names[len(names) * index // count:len(names) * (index + 1) // count] for index in range(count)]
    return [("^(" + "|".join(re.escape(name) for name in group) + ")$",
             sum(name in group for _, name in cases)) for group in groups]
