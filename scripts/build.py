"""Configure/build/test with CMake; discover the installed MSVC tools on Windows."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
env = {key.upper() if os.name == "nt" else key: value for key, value in os.environ.items()}
preset = "windows" if os.name == "nt" else "unix"
cmake = shutil.which("cmake")
if os.name == "nt":
    env["VSLANG"] = "1033"
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], env=env).decode().strip()
    if not installation:
        raise SystemExit("Install MSVC C++ build tools with a Windows SDK")
    vs = Path(installation)
    command = f'"{vs / "VC/Auxiliary/Build/vcvars64.bat"}" >nul && set'
    output = subprocess.check_output('cmd.exe /d /s /c "' + command + '"', env=env).decode("mbcs")
    for line in output.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key.upper()] = value
    env["VSLANG"] = "1033"
    tools = vs / "Common7/IDE/CommonExtensions/Microsoft/CMake"
    if not cmake:
        cmake = str(tools / "CMake/bin/cmake.exe")
    env["PATH"] = str(tools / "Ninja") + os.pathsep + env["PATH"]
if not cmake:
    raise SystemExit("Install CMake 3.25+ and Ninja")
action = sys.argv[1] if len(sys.argv) > 1 else "build"
if action == "configure":
    command = [cmake, "--preset", preset, *sys.argv[2:]]
elif action == "build":
    command = [cmake, "--build", "--preset", preset, "--parallel", "4", *sys.argv[2:]]
elif action == "test":
    ctest = str(Path(cmake).with_name("ctest.exe" if os.name == "nt" else "ctest")) if Path(cmake).is_absolute() else "ctest"
    command = [ctest, "--preset", preset, *sys.argv[2:]]
else:
    raise SystemExit("Usage: python scripts/build.py configure|build|test [extra CMake arguments]")
raise SystemExit(subprocess.call(command, cwd=root, env=env))
