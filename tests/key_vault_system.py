"""Qualify actual OS vaults with isolated, disposable credentials across processes."""
import argparse
import os
from pathlib import Path
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        # Never echo commands: the temporary macOS keychain setup has an
        # ephemeral fixture password, not a deployment credential.
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def exercise(executable, root, keychain=None, after_create=None):
    arguments = [str(root)] + ([str(keychain)] if keychain else [])
    try:
        for mode in ("create", "verify", "erase"):
            print(run([executable, mode, *arguments]), end="", flush=True)
            if mode == "create" and after_create:
                after_create()
    finally:
        if (root / "state.json").exists():
            run([executable, "cleanup", *arguments])


def linux_session(executable, root):
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
        exercise(executable, root)
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


def macos_session(executable, root):
    keychain = root / "qualification.keychain-db"
    password = secrets.token_hex(24)
    def locked_check():
        run(["security", "lock-keychain", str(keychain)])
        try:
            print(run([executable, "inaccessible", str(root), str(keychain)]), end="", flush=True)
        finally:
            run(["security", "unlock-keychain", "-p", password, str(keychain)])
    try:
        run(["security", "create-keychain", "-p", password, str(keychain)])
        run(["security", "unlock-keychain", "-p", password, str(keychain)])
        exercise(executable, root, keychain, locked_check)
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
    parser.add_argument("executable", type=Path)
    parser.add_argument("--inside-linux", type=Path)
    args = parser.parse_args()
    executable = str(args.executable.resolve())
    if args.inside_linux:
        linux_session(executable, args.inside_linux.resolve())
        return
    root = Path(tempfile.mkdtemp(prefix="syrnck-vault-")).resolve()
    try:
        if sys.platform == "linux":
            for tool in ("dbus-run-session", "gnome-keyring-daemon", "gdbus"):
                if not shutil.which(tool):
                    raise RuntimeError("system vault qualification requires " + tool)
            print(run(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                       executable, "--inside-linux", str(root)]), end="")
        elif sys.platform == "darwin":
            macos_session(executable, root)
        elif sys.platform == "win32":
            exercise(executable, root)
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
