"""Qualify actual OS vaults with isolated, disposable credentials across processes."""
import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


def run(command, timeout_seconds=60, **kwargs):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout_seconds, **kwargs)
    except subprocess.TimeoutExpired:
        raise RuntimeError("owned vault fixture subprocess exceeded " + str(timeout_seconds) + " seconds") from None
    if result.returncode:
        # Never echo commands: the temporary macOS keychain setup has an
        # ephemeral fixture password, not a deployment credential.
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


@dataclass(frozen=True)
class Probe:
    executable: str
    go: bool = False
    cli: bool = False


def go_environment(library):
    environment = os.environ.copy()
    environment["GOTOOLCHAIN"] = "local"
    environment["GOPROXY"] = "off"
    source = Path(__file__).resolve().parent.parent
    environment["GOCACHE"] = str(source / ".tools/go-cache")
    environment["GOPATH"] = str(source / ".tools/go-path")
    if os.name == "nt":
        environment["SYRNCK_LIBRARY"] = str(library)
        environment["CGO_ENABLED"] = "0"
    else:
        environment["CGO_ENABLED"] = "1"
        flag = "-L" + str(library.parent)
        quote = "'" if "'" not in flag else '"'
        if quote in flag:
            raise RuntimeError("fixture cgo library path contains both quote styles")
        environment["CGO_LDFLAGS"] = environment.get("CGO_LDFLAGS", "") + " " + quote + flag + quote
        for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            environment[name] = str(library.parent) + os.pathsep + environment.get(name, "")
    return environment


def build_go_probe(go, library, root, bep=False):
    name = "vault-bep-go-probe" if bep else "vault-go-probe"
    executable = root / (name + (".exe" if os.name == "nt" else ""))
    environment = go_environment(library)
    source = Path(__file__).resolve().parent.parent
    run([str(go), "test", "-mod=readonly", "-c", "-gcflags=all=-d=checkptr=2", "-o", str(executable),
         "./cmd/syrnck-bep" if bep else "."],
        cwd=source / ("bindings/syncthing" if bep else "bindings/go"), env=environment, timeout_seconds=120)
    return executable


def probe_run(probe, mode, root, keychain, library):
    if probe.cli:
        return run([sys.executable, str(Path(__file__).with_name("vault_cli.py")), probe.executable,
                    "--system", mode, str(root), *([str(keychain)] if keychain else [])])
    if probe.go:
        command = [probe.executable, "-test.run=^TestSystemVaultRealProvider$", "-test.v",
                   "-syrnck-vault-mode=" + mode, "-syrnck-vault-root=" + str(root),
                   "-syrnck-vault-keychain=" + (str(keychain) if keychain else "")]
        result = run(command, env=go_environment(library))
        if "--- PASS: TestSystemVaultRealProvider" not in result or "--- SKIP:" in result:
            raise RuntimeError("real Go vault fixture did not execute successfully")
        return result
    return run([probe.executable, mode, str(root), *([str(keychain)] if keychain else [])])


def exercise(probe, root, keychain=None, after_create=None, library=None):
    try:
        for mode in ("create", "verify", "erase"):
            print(probe_run(probe, mode, root, keychain, library), end="", flush=True)
            if mode == "create" and after_create:
                after_create()
    finally:
        if (root / "state.json").exists():
            probe_run(probe, "cleanup", root, keychain, library)


def exercise_all(probes, root, keychain=None, keychain_password=None, library=None):
    for index, probe in enumerate(probes):
        fixture = root / ("probe-" + str(index))
        fixture.mkdir(mode=0o700)
        def locked_check(probe=probe, fixture=fixture):
            if not probe.go and not probe.cli:
                print(probe_run(probe, "policy-required", fixture, keychain, library), end="", flush=True)
            run(["security", "lock-keychain", str(keychain)])
            try:
                print(probe_run(probe, "inaccessible", fixture, keychain, library), end="", flush=True)
            finally:
                run(["security", "unlock-keychain", "-p", keychain_password, str(keychain)])
        exercise(probe, fixture, keychain, locked_check if keychain else None, library)


def linux_session(probes, root, library):
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR"):
        destination = root / name.lower()
        destination.mkdir(mode=0o700)
        os.environ[name] = str(destination)
    control = root / "keyring-control"
    control.mkdir(mode=0o700)
    daemon = subprocess.Popen(["gnome-keyring-daemon", "--foreground", "--unlock", "--components=secrets",
                               "--control-directory", str(control)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        daemon.stdin.write(secrets.token_hex(24).encode())
        daemon.stdin.close()
        deadline = time.monotonic() + 15
        while True:
            if daemon.poll() is not None:
                raise RuntimeError("isolated Secret Service daemon exited before readiness")
            owner = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                                    "--object-path", "/org/freedesktop/DBus", "--method",
                                    "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.secrets"],
                                    capture_output=True, text=True, timeout=3)
            # Do not auto-activate a second daemon while our foreground owner
            # is still starting on the private bus.
            if owner.returncode == 0 and owner.stdout.strip() == "(true,)":
                result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.secrets",
                                         "--object-path", "/org/freedesktop/secrets", "--method",
                                         "org.freedesktop.Secret.Service.ReadAlias", "default"],
                                        capture_output=True, text=True, timeout=3)
                if result.returncode == 0 and "objectpath '/'" not in result.stdout:
                    break
            if time.monotonic() >= deadline:
                raise RuntimeError("isolated Secret Service did not expose its default collection")
            time.sleep(0.1)  # Fixture readiness only; engine operations are never retried.
        exercise_all(probes, root, library=library)
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=5)
        daemon.stdout.close()
        daemon.stderr.close()


def macos_session(probes, root, library):
    keychain = root / "qualification.keychain-db"
    password = secrets.token_hex(24)
    try:
        run(["security", "create-keychain", "-p", password, str(keychain)])
        run(["security", "unlock-keychain", "-p", password, str(keychain)])
        exercise_all(probes, root, keychain, password, library)
    finally:
        if keychain.exists():
            run(["security", "delete-keychain", str(keychain)])
        # Some OS versions register a newly created keychain. Remove only our
        # exact disposable path; preserve the order and any unrelated changes.
        current = shlex.split(run(["security", "list-keychains", "-d", "user"]))
        if str(keychain) in current:
            run(["security", "list-keychains", "-d", "user", "-s",
                 *[entry for entry in current if entry != str(keychain)]])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="+")
    parser.add_argument("--inside-linux", type=Path)
    go_selection = parser.add_mutually_exclusive_group()
    go_selection.add_argument("--go", type=Path)
    go_selection.add_argument("--go-probe", type=Path, action="append", default=[])
    parser.add_argument("--bep-go", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--cli", type=Path)
    args = parser.parse_args()
    executables = [str(path.resolve()) for path in args.executable]
    library = args.library.resolve(strict=True) if args.library else None
    if (args.go or args.go_probe or args.bep_go) and library is None:
        parser.error("the Go fixture requires --library")
    probes = [Probe(path) for path in executables]
    if args.cli:
        probes.append(Probe(str(args.cli.resolve(strict=True)), cli=True))
    for probe in args.go_probe:
        probes.append(Probe(str(probe.resolve(strict=True)), True))
    if args.inside_linux:
        linux_session(probes, args.inside_linux.resolve(), library)
        return
    root = Path(tempfile.mkdtemp(prefix="syrnck-vault-")).resolve()
    try:
        if args.go:
            probes.append(Probe(str(build_go_probe(args.go.resolve(strict=True), library, root)), True))
        if args.bep_go:
            probes.append(Probe(str(build_go_probe(args.bep_go.resolve(strict=True), library, root, bep=True)), True))
        if sys.platform == "linux":
            for tool in ("dbus-run-session", "gnome-keyring-daemon", "gdbus"):
                if not shutil.which(tool):
                    raise RuntimeError("system vault qualification requires " + tool)
            command = ["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                       *executables, "--inside-linux", str(root)]
            if library:
                command.extend(["--library", str(library)])
            if args.cli:
                command.extend(["--cli", str(args.cli.resolve(strict=True))])
            for probe in probes:
                if probe.go:
                    command.extend(["--go-probe", probe.executable])
            print(run(command, timeout_seconds=150), end="")
        elif sys.platform == "darwin":
            macos_session(probes, root, library)
        elif sys.platform == "win32":
            exercise_all(probes, root, library=library)
        else:
            raise RuntimeError("no system vault qualification fixture for this platform")
    except BaseException:
        print("Retained vault fixture for explicit recovery: " + str(root), file=sys.stderr)
        raise
    else:
        root.relative_to(Path(tempfile.gettempdir()).resolve())
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
