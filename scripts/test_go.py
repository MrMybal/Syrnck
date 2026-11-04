"""Test the Go binding against an already built native shared library."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--library", type=Path, required=True)
parser.add_argument("--go", default=shutil.which("go"))
parser.add_argument("--check-pointers", action="store_true")
args = parser.parse_args()
library = args.library.resolve(strict=True)
go = args.go or str(root / ".tools/go/bin" / ("go.exe" if os.name == "nt" else "go"))
env = os.environ.copy()
env["GOTOOLCHAIN"] = "local"
env["GOCACHE"] = str(root / ".tools/go-cache")
env["GOPATH"] = str(root / ".tools/go-path")
if os.name == "nt":
    env["SYRNCK_LIBRARY"] = str(library)
    env["CGO_ENABLED"] = "0"
else:
    env["CGO_ENABLED"] = "1"
    env["CGO_LDFLAGS"] = env.get("CGO_LDFLAGS", "") + ' -L"' + str(library.parent) + '"'
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        env[key] = str(library.parent) + os.pathsep + env.get(key, "")
command = [go, "test", "-v", "-count=1"]
if args.check_pointers:
    command.append("-gcflags=all=-d=checkptr=2")
command.append("./...")
raise SystemExit(subprocess.call(command, cwd=root / "bindings/go", env=env))
