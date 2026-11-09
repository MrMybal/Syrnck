"""Capture CTest's actual case declarations during build, before test logging."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    output = args.output.resolve()
    if output.parent != build or not output.name.startswith("test-case-plan-") or output.suffix != ".json":
        raise SystemExit("test plan output must be a generated JSON file in its build directory")
    declarations = build / "CTestTestfile.cmake"
    before = declarations.read_bytes()
    result = subprocess.run([args.ctest, "--test-dir", str(build), "-C", args.config, "--show-only=json-v1"],
                            check=True, capture_output=True, text=True, encoding="utf-8", timeout=30)
    plan = json.loads(result.stdout)
    if declarations.read_bytes() != before:
        raise SystemExit("CTest declarations changed while generating case plan; rebuild")
    snapshot = {"version": 1, "configuration": args.config,
                "declarations_sha256": hashlib.sha256(before).hexdigest(), "plan": plan}
    temporary = output.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(snapshot, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output)


if __name__ == "__main__":
    main()
