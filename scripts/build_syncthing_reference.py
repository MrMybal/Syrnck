"""Build the exact upstream daemon used for optional BEP interoperability tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--go", default=shutil.which("go"))
parser.add_argument("--output", type=Path, default=root / "build/qualification/syncthing")
parser.add_argument("--download", action="store_true", help="allow verified public module downloads")
args = parser.parse_args()
go = args.go or str(root / ".tools/go/bin" / ("go.exe" if os.name == "nt" else "go"))
fixture = root / "tests/syncthing-reference"
module = "github.com/syncthing/syncthing"
pattern = rf"(?m)^\s*(?:require )?{re.escape(module)}\s+(\S+)\s*$"
expected = re.search(pattern, (fixture / "go.mod").read_text(encoding="utf-8"))
adapter = re.search(pattern, (root / "bindings/syncthing/go.mod").read_text(encoding="utf-8"))
if not expected or not adapter or expected[1] != adapter[1]:
    parser.error("reference daemon and adapter must use exactly the same Syncthing module version")
before = {name: (fixture / name).read_bytes() for name in ("go.mod", "go.sum")}
checksum = re.search(rf"(?m)^{re.escape(module)} {re.escape(expected[1])} (h1:\S+)\s*$",
                     before["go.sum"].decode("utf-8"))
if not checksum:
    parser.error("reference source checksum is missing")
output = args.output.resolve()
if os.name == "nt" and output.suffix.lower() != ".exe":
    output = output.with_name(output.name + ".exe")
output.parent.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.update(GOTOOLCHAIN="local", GOWORK="off", GOPATH=str(root / ".tools/go-path"),
           GOCACHE=str(root / ".tools/go-cache"), CGO_ENABLED="0", GOFLAGS="",
           GOPROXY="https://proxy.golang.org" if args.download else "off",
           GOSUMDB="sum.golang.org", GOPRIVATE="", GONOSUMDB="", GONOPROXY="")
# The reference daemon is built without cgo; the native adapter uses cgo on
# macOS/Linux. In particular Darwin's notify backend is excluded from the
# daemon build. Resolve the actual adapter/test graph before offline CTest.
adapter_root = root / "bindings/syncthing"
adapter_before = {name: (adapter_root / name).read_bytes() for name in ("go.mod", "go.sum")}
adapter_env = dict(env, CGO_ENABLED="0" if os.name == "nt" else "1")
subprocess.run([go, "list", "-mod=readonly", "-buildvcs=false", "-deps", "-test", "./..."],
               cwd=adapter_root, env=adapter_env, check=True, stdout=subprocess.DEVNULL)
if any((adapter_root / name).read_bytes() != content for name, content in adapter_before.items()):
    raise SystemExit("adapter dependency preparation modified its pinned manifests")
subprocess.run([go, "build", "-mod=readonly", "-tags=noassets", "-o", str(output),
                module + "/cmd/syncthing"], cwd=fixture, env=env, check=True)
if any((fixture / name).read_bytes() != content for name, content in before.items()):
    raise SystemExit("reference build modified its pinned dependency manifests")
metadata = subprocess.check_output([go, "version", "-m", str(output)], env=env, text=True, encoding="utf-8")
if (not re.search(rf"(?m)^\s*mod\s+{re.escape(module)}\s+{re.escape(expected[1])}\s+{re.escape(checksum[1])}\s*$", metadata)
        or not re.search(rf"(?m)^\s*path\s+{re.escape(module)}/cmd/syncthing\s*$", metadata)):
    raise SystemExit("reference executable does not contain the expected upstream module")
print(json.dumps({"binary": str(output), "module": module, "version": expected[1],
                  "tags": ["noassets"], "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}))
