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
parser.add_argument("--module", choices=("core", "syncthing"), default="core")
parser.add_argument("--benchmark", action="store_true")
args = parser.parse_args()
library = args.library.resolve(strict=True)
go = args.go or str(root / ".tools/go/bin" / ("go.exe" if os.name == "nt" else "go"))
provider_names = {"SYRNCK_KEY_FILE", "SYRNCK_KEY_PASSWORD", "SYRNCK_NEW_KEY_PASSWORD",
                  "SYRNCK_SIGNING_KEY_FILE", "SYRNCK_SIGNING_KEY_PASSWORD", "SYRNCK_NEW_SIGNING_KEY_PASSWORD"}
# Environment names are case insensitive on Windows.
env = {name: value for name, value in os.environ.items() if name.upper() not in provider_names}
env["GOTOOLCHAIN"] = "local"
env["GOCACHE"] = str(root / ".tools/go-cache")
env["GOPATH"] = str(root / ".tools/go-path")
test_cli = library.parent / ("syrnck.exe" if os.name == "nt" else "syrnck")
if test_cli.is_file():
    env["SYRNCK_TEST_CLI"] = str(test_cli)
test_edit_helper = library.parent / ("syrnck_checkout_edit_tests.exe" if os.name == "nt" else "syrnck_checkout_edit_tests")
if test_edit_helper.is_file():
    env["SYRNCK_TEST_CHECKOUT_EDIT_HELPER"] = str(test_edit_helper)
test_bep_service = library.parent / ("syrnck-bep.exe" if os.name == "nt" else "syrnck-bep")
if test_bep_service.is_file() and not env.get("SYRNCK_TEST_BEP_SERVICE"):
    env["SYRNCK_TEST_BEP_SERVICE"] = str(test_bep_service)
if os.name == "nt":
    env["SYRNCK_LIBRARY"] = str(library)
    env["CGO_ENABLED"] = "0"
else:
    env["CGO_ENABLED"] = "1"
    # cmd/go recognizes quotes only around an ENTIRE argument. Quotes after
    # -L survive parsing and break the generated go:cgo_ldflag directive.
    flag = "-L" + str(library.parent)
    quote = "'" if "'" not in flag else '"'
    if quote in flag:
        parser.error("cgo library path cannot contain both single and double quotes")
    env["CGO_LDFLAGS"] = env.get("CGO_LDFLAGS", "") + " " + quote + flag + quote
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        env[key] = str(library.parent) + os.pathsep + env.get(key, "")
command = [go, "test", "-mod=readonly", "-v", "-count=1", "-timeout=90s"]
if args.module == "syncthing":
    # Tests use the pinned module/sums and never fetch dependencies implicitly.
    env["GOPROXY"] = "off"
if args.benchmark:
    if args.module != "syncthing":
        parser.error("--benchmark requires --module syncthing")
    command.extend(["-run", "^$", "-bench", ".", "-benchmem", "-benchtime=300ms"])
if args.check_pointers:
    command.append("-gcflags=all=-d=checkptr=2")
command.append("./...")
directory = root / "bindings" / ("go" if args.module == "core" else "syncthing")
raise SystemExit(subprocess.call(command, cwd=directory, env=env))
