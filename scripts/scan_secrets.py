"""Run Gitleaks on the publishable working tree, staged changes, or Git history."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gitleaks", default="gitleaks")
    parser.add_argument("--mode", choices=("working-tree", "staged", "history"), default="working-tree")
    parser.add_argument("--report", type=Path, default=Path(".scratch/gitleaks.json"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    git = ["git", "-c", f"safe.directory={root.as_posix()}", "-C", str(root)]
    scanner = shutil.which(args.gitleaks) or str(Path(args.gitleaks).resolve(strict=True))
    report = args.report.resolve()
    # Scanner output may contain sensitive context even with secret redaction.
    # Keep reports out of the publishable tree.
    report.relative_to(root / ".scratch")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)
    common = ["--no-banner", "--no-color", "--redact=100", "--report-format", "json", "--report-path", str(report),
              "--config", str(root / ".gitleaks.toml")]
    environment = os.environ.copy()
    for name in ("GITLEAKS_CONFIG", "GITLEAKS_CONFIG_TOML"):
        environment.pop(name, None)
    summary = {"mode": args.mode, "scanner": subprocess.check_output([scanner, "version"], text=True).strip()}
    if args.mode == "working-tree":
        raw = subprocess.check_output([*git, "ls-files", "-z", "--cached", "--others", "--exclude-standard"])
        paths = sorted(set(os.fsdecode(name) for name in raw.split(b"\0") if name))
        digest = hashlib.sha256()
        total = copied = 0
        with tempfile.TemporaryDirectory(prefix="syrnck-secret-scan-") as temporary:
            snapshot = Path(temporary)
            for name in paths:
                source = root / name
                if not source.exists() and not source.is_symlink():
                    continue  # A tracked deletion has no working-tree bytes to publish.
                if source.is_symlink() or not source.is_file():
                    raise RuntimeError(f"Cannot snapshot non-regular file: {name}")
                source.resolve(strict=True).relative_to(root)
                if source.stat().st_size >= 100 * 1024 * 1024:
                    raise RuntimeError(f"File reaches GitHub's 100 MiB Git object limit: {name}")
                data = source.read_bytes()
                total += len(data)
                copied += 1
                digest.update(name.replace("\\", "/").encode("utf-8") + b"\0")
                digest.update(hashlib.sha256(data).digest())
                target = snapshot / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            result = subprocess.run([scanner, "dir", *common, str(snapshot)], env=environment, cwd=root)
        summary.update(files=copied, bytes=total, tree_sha256=digest.hexdigest())
    else:
        options = ["--staged"] if args.mode == "staged" else ["--log-opts=--all --full-history"]
        environment["GIT_CONFIG_COUNT"] = "1"
        environment["GIT_CONFIG_KEY_0"] = "safe.directory"
        environment["GIT_CONFIG_VALUE_0"] = root.as_posix()
        result = subprocess.run([scanner, "git", *common, *options, str(root)], env=environment, cwd=root)
    summary.update(exit_code=result.returncode, findings=len(json.loads(report.read_text())) if report.exists() else None)
    report.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
