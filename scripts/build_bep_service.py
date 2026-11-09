"""Build the optional BEP executable and retain its exact runtime source notices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def documents(text):
    decoder = json.JSONDecoder()
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        yield value


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def quoted(value):
    # cmd/go quotes whole arguments, including -L; JSON/shell escaping differs.
    quote = "'" if "'" not in value else '"'
    if quote in value:
        raise SystemExit("Go linker arguments cannot contain both quote characters")
    return quote + value + quote


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go", default=shutil.which("go"))
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime-library-relative", default="../lib")
    parser.add_argument("--download", action="store_true", help="allow checksum-verified public Go module preparation")
    args = parser.parse_args()
    candidate = args.go or str(root / ".tools/go/bin" / ("go.exe" if os.name == "nt" else "go"))
    go = shutil.which(candidate) or str(Path(candidate).resolve(strict=True))
    library = args.library.resolve(strict=True)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    module = root / "bindings/syncthing"
    before = {name: (module / name).read_bytes() for name in ("go.mod", "go.sum")}
    env = dict(os.environ, GOTOOLCHAIN="local", GOWORK="off", GOPROXY="https://proxy.golang.org" if args.download else "off", GOFLAGS="",
               GOCACHE=str(root / ".tools/go-cache"), GOPATH=str(root / ".tools/go-path"),
               CGO_ENABLED="0" if os.name == "nt" else "1")
    if args.download:
        env.update(GOSUMDB="sum.golang.org", GOPRIVATE="", GONOSUMDB="", GONOPROXY="")
    if os.name != "nt":
        header = digest(root / "include/syrnck.h")
        env["CGO_CPPFLAGS"] = env.get("CGO_CPPFLAGS", "") + " -DSYRNCK_C_ABI_HEADER_" + header + "=1"
        origin = "@executable_path" if sys.platform == "darwin" else "$ORIGIN"
        relative = Path(args.runtime_library_relative).as_posix()
        flags = ["-L" + str(library.parent), "-Wl,-rpath," + origin,
                 "-Wl,-rpath," + origin + "/" + relative]
        env["CGO_LDFLAGS"] = env.get("CGO_LDFLAGS", "") + " " + " ".join(quoted(flag) for flag in flags)
    command = "./cmd/syrnck-bep"
    def capture(*arguments):
        return subprocess.check_output([go, *arguments], cwd=module, env=env, text=True, encoding="utf-8")
    # Verification needs graph metadata for every required module, including
    # platform-excluded dependencies. Only --download permits fetching it.
    subprocess.run([go, "mod", "download"], cwd=module, env=env, check=True)
    packages = documents(capture("list", "-mod=readonly", "-buildvcs=false", "-deps", "-json", command))
    runtime = {}
    for package in packages:
        dependency = package.get("Module")
        if dependency and not dependency["Path"].startswith("syrnck.local/"):
            if dependency.get("Replace"):
                raise SystemExit("Runtime dependency replacements need explicit packaging support")
            runtime[dependency["Path"]] = dependency
    downloads = list(documents(capture("mod", "download", "-json",
                                      *(path + "@" + item["Version"] for path, item in sorted(runtime.items())))))
    if any(item.get("Error") for item in downloads):
        raise SystemExit("Pinned runtime source archives are unavailable; prepare dependencies first")
    subprocess.run([go, "mod", "verify"], cwd=module, env=env, check=True, stdout=subprocess.DEVNULL)
    licenses = output.parent / "bep-licenses"
    (licenses / "sources").mkdir(parents=True, exist_ok=True)
    (licenses / "modules").mkdir(exist_ok=True)
    records = []
    for item in sorted(downloads, key=lambda item: item["Path"]):
        identity = item["Path"] + "@" + item["Version"]
        slug = hashlib.sha256(identity.encode("utf-8")).hexdigest()
        archive = Path(item["Zip"])
        source_hash = digest(archive)
        destination = licenses / "sources" / (slug + ".zip")
        if not destination.is_file() or digest(destination) != source_hash:
            shutil.copyfile(archive, destination)
        notices = []
        notice_directory = licenses / "modules" / slug
        notice_directory.mkdir(exist_ok=True)
        for source in sorted(Path(item["Dir"]).iterdir()):
            if source.is_file() and source.name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "COPYRIGHT")):
                shutil.copyfile(source, notice_directory / source.name)
                notices.append("modules/" + slug + "/" + source.name)
        if not notices:
            raise SystemExit("No runtime module license found: " + identity)
        records.append({"module": item["Path"], "version": item["Version"], "sum": item["Sum"],
                        "source": "sources/" + slug + ".zip", "sha256": source_hash, "notices": notices})
    goroot = Path(capture("env", "GOROOT").strip())
    shutil.copyfile(goroot / "LICENSE", licenses / "Go.LICENSE")
    subprocess.run([go, "build", "-mod=readonly", "-buildvcs=false", "-trimpath", "-o", str(output), command],
                   cwd=module, env=env, check=True)
    if any((module / name).read_bytes() != data for name, data in before.items()):
        raise SystemExit("BEP build modified pinned dependency manifests")
    manifest = {"version": 1, "binary": output.name, "sha256": digest(output),
                "go": capture("version").strip(), "modules": records}
    (licenses / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"binary": str(output), "licenses": str(licenses), "modules": len(records),
                      "sha256": manifest["sha256"]}))


if __name__ == "__main__":
    main()
