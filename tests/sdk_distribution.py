"""Build and run independent consumers after moving an installed native SDK."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--generator", required=True)
    parser.add_argument("--network", choices=("ON", "OFF"), required=True)
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--cxx-compiler", required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="syrnck-sdk-") as temporary:
        root = Path(temporary).resolve()
        original, relocated = root / "original SDK", root / "relocated SDK"
        run([args.cmake, "--install", str(args.build.resolve()), "--config", args.config,
             "--prefix", str(original)])
        # Only our newly created temporary installation is moved.
        original.resolve().relative_to(root)
        relocated.resolve().relative_to(root)
        original.rename(relocated)
        configs = list(relocated.rglob("Syrnck*.cmake"))
        assert configs, "SDK target export is missing"
        for config in configs:
            text = config.read_text(encoding="utf-8").replace("\\", "/")
            for forbidden in (source, args.build.resolve(), original):
                assert forbidden.as_posix() not in text, "SDK retains a source/build/original-install path"
        environment = os.environ.copy()
        # Exercise only find_package from the relocated prefix, not a user's
        # CMake package registry, include flags, or this checkout's libraries.
        for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
            environment.pop(name, None)
        for mode in ("full", "C-only"):
            consumer = root / f"{mode} consumer build"
            configure = [args.cmake, "-S", str(source / "tests/sdk_consumer"), "-B", str(consumer),
                 "-G", args.generator, f"-DCMAKE_BUILD_TYPE={args.config}",
                 f"-DCMAKE_C_COMPILER={args.c_compiler}", f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}",
                 f"-DSyrnck_DIR={configs[0].parent}", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                 "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF", f"-DSYRNCK_CONSUMER_NETWORK={args.network}",
                 f"-DSYRNCK_CONSUMER_C_ONLY={'ON' if mode == 'C-only' else 'OFF'}"]
            if args.network == "OFF":
                configure.extend(["-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON"])
            run(configure, env=environment)
            run([args.cmake, "--build", str(consumer), "--config", args.config, "--parallel", "2"], env=environment)
            output = run([args.ctest, "--test-dir", str(consumer), "-C", args.config,
                          "--output-on-failure"], env=environment)
            print(output, end="")
        if args.network == "OFF":
            # A real optional-feature request must be refused, rather than
            # silently offering targets whose archives were never distributed.
            requested = [arg for arg in configure if not arg.startswith("-DSYRNCK_CONSUMER_")]
            requested.extend(["-DSYRNCK_CONSUMER_C_ONLY=OFF", "-DSYRNCK_CONSUMER_NETWORK=ON"])
            result = subprocess.run(requested, capture_output=True, text=True, env=environment)
            if result.returncode == 0 or "built without network support" not in result.stdout + result.stderr:
                raise RuntimeError("core-only SDK did not reject a required network component\n" + result.stdout + result.stderr)
            print("Core-only SDK rejected the required Network component.")


if __name__ == "__main__":
    main()
