"""Bound successful crash-fixture lifetime without weakening verification."""
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import sys
import time

WINDOWS = sys.platform == "win32"


def report(name, state, started):
    print(json.dumps({"case": name, "state": state,
                      "elapsed_ms": (time.monotonic_ns() - started) // 1_000_000}), flush=True)


def owned_path(owner, name):
    owner = Path(owner).resolve(strict=True)
    if not name or name.startswith(".") or Path(name).name != name or "/" in name or "\\" in name:
        raise ValueError("fixture must be a direct owned child")
    path = owner / name
    if path.is_symlink() or path.parent.resolve(strict=True) != owner:
        raise ValueError("linked or foreign fixture root")
    return owner, path


def remove_owned_case(owner, name, timeout=2):
    deadline = time.monotonic() + timeout
    while True:
        # Recheck the physical target on every attempt. Only these disposable
        # direct children can be retried; engine operations are never retried.
        checked_owner, checked = owned_path(owner, name)
        if not checked.exists():
            return
        if not checked.is_dir() or checked.resolve(strict=True).parent != checked_owner:
            raise ValueError("fixture cleanup target changed")
        try:
            shutil.rmtree(checked)
            return
        except OSError as error:
            # Windows deletion can remain pending after its last child handle
            # closes. Persistent entries, access denial and other errors fail.
            if not (WINDOWS and getattr(error, "winerror", None) in (32, 145)
                    and time.monotonic() < deadline):
                raise
            time.sleep(0.01)


@contextmanager
def case_directory(owner, name):
    owner, path = owned_path(owner, name)
    if path.exists():
        raise FileExistsError("case fixture already exists: " + name)
    started = time.monotonic_ns()
    report(name, "running", started)
    try:
        yield path
    except BaseException:
        report(name, "failed", started)
        raise
    else:
        report(name, "verified", started)
        cleanup = time.monotonic_ns()
        report(name, "cleaning", cleanup)
        # Recheck the absolute physical target immediately before recursive
        # deletion. Never follow a substituted root or retire an unrelated path.
        remove_owned_case(owner, name)
        report(name, "cleaned", cleanup)
