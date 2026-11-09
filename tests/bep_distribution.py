"""Qualify the installed BEP executable, runtime loader paths and source notices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--bindir", default="bin")
    parser.add_argument("--licenses", default="share/licenses/Syrnck/bep")
    parser.add_argument("--executable", default="syrnck-bep.exe" if os.name == "nt" else "syrnck-bep")
    args = parser.parse_args()
    for relative in (args.bindir, args.licenses, args.executable):
        if Path(relative).is_absolute() or ".." in Path(relative).parts:
            parser.error("qualification requires relative install destinations")
    with tempfile.TemporaryDirectory(prefix="syrnck-bep-distribution-") as temporary:
        prefix = Path(temporary) / "relocated distribution"
        installation = subprocess.run([args.cmake, "--install", str(args.build.resolve()), "--config", args.config,
                                       "--prefix", str(prefix)], capture_output=True, text=True)
        if installation.returncode:
            raise RuntimeError(installation.stdout + installation.stderr)
        executable = prefix / args.bindir / args.executable
        licenses = prefix / args.licenses
        manifest = json.loads((licenses / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["version"] == 1 and manifest["binary"] == executable.name
        assert digest(executable) == manifest["sha256"]
        assert (licenses / "Go.LICENSE").stat().st_size > 100
        assert manifest["modules"]
        identities = set()
        source_bytes = 0
        for module in manifest["modules"]:
            identity = module["module"], module["version"]
            assert identity not in identities and module["sum"].startswith("h1:")
            identities.add(identity)
            for name in [module["source"], *module["notices"]]:
                path = licenses / name
                assert path.resolve().is_relative_to(licenses.resolve()) and path.is_file()
            assert module["notices"]
            archive = licenses / module["source"]
            assert digest(archive) == module["sha256"]
            with zipfile.ZipFile(archive) as source:
                expected = module["module"] + "@" + module["version"] + "/"
                assert source.namelist() and all(name.startswith(expected) for name in source.namelist())
            source_bytes += archive.stat().st_size
        env = {key: value for key, value in os.environ.items() if key.upper() not in
               ("SYRNCK_LIBRARY", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "SYRNCK_LANG")}
        for language in ("en", "fr"):
            result = subprocess.run([str(executable), "--lang", language, "--help"], cwd=temporary,
                                    env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            assert "standalone-service" in result.stdout and "run --config" in result.stdout
            assert ("configuration locale" in result.stdout) == (language == "fr")
        print(json.dumps({"installed_executable": True, "environment_loader_override": False,
                          "runtime_modules": len(identities), "verified_source_bytes": source_bytes,
                          "languages": ["en", "fr"]}))


if __name__ == "__main__":
    main()
