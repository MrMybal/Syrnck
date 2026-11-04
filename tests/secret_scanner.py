"""Verify that the reviewed fixture exception does not hide other findings."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gitleaks", default="gitleaks")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    scanner = shutil.which(args.gitleaks) or str(Path(args.gitleaks).resolve(strict=True))
    known = json.loads((root / "tests/fixtures/v1/encrypted/config.json").read_text())["key_check"]
    changed = hashlib.sha256(b"syrnck scanner canary, deliberately not a live credential").hexdigest()
    fixture_path = "tests/fixtures/v1/encrypted/config.json"
    cases = [(fixture_path, known, 0), (fixture_path, changed, 1), ("other/config.json", known, 1)]
    for index, (name, value, expected) in enumerate(cases, 1):
        with tempfile.TemporaryDirectory(prefix="syrnck-scanner-test-") as temporary:
            directory = Path(temporary) / "source"
            file = directory / name
            file.parent.mkdir(parents=True)
            file.write_text(json.dumps({"version": 1, "key_check": value}), encoding="utf-8")
            report = Path(temporary) / "report.json"
            result = subprocess.run([scanner, "dir", str(directory), "--no-banner", "--no-color", "--redact=100",
                                     "--config", str(root / ".gitleaks.toml"), "--report-format", "json",
                                     "--report-path", str(report)], capture_output=True, cwd=root)
            findings = json.loads(report.read_text()) if report.exists() else []
            if result.returncode != expected or len(findings) != expected:
                raise RuntimeError(f"Scanner exception test {index} failed (exit={result.returncode}, "
                                   f"findings={len(findings)}); no diagnostic context printed")
    print("PASS: exact public verifier allowed; changed value and other path detected")


if __name__ == "__main__":
    main()
