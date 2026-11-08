"""Prefetch SHA-256-pinned CMake archives with bounded transient retries."""
import argparse
from dataclasses import dataclass
import hashlib
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent
TRANSIENT_HTTP = {408, 429, 500, 502, 503, 504}
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024


@dataclass(frozen=True)
class Archive:
    name: str
    url: str
    sha256: str

    @property
    def filename(self):
        name = urlsplit(self.url).path.rsplit("/", 1)[-1]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", name) or name.endswith("."):
            raise ValueError("invalid dependency archive filename")
        return name


def read_manifest(path=ROOT / "cmake/dependencies.json"):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or type(data.get("version")) is not int or data["version"] != 1:
        raise ValueError("unsupported dependency manifest")
    result = []
    for name, entry in data.items():
        if name == "version":
            continue
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", name) or not isinstance(entry, dict) or set(entry) != {"url", "sha256"}:
            raise ValueError("invalid dependency manifest entry")
        url, sha = entry["url"], entry["sha256"]
        if not isinstance(url, str) or not isinstance(sha, str) or not re.fullmatch(r"[a-f0-9]{64}", sha):
            raise ValueError("invalid pinned dependency")
        parsed = urlsplit(url)
        if parsed.scheme != "https" or not parsed.hostname or parsed.username is not None or parsed.password is not None or parsed.query or parsed.fragment:
            raise ValueError("dependency URL must use HTTPS without credentials or query parameters")
        archive = Archive(name, url, sha)
        archive.filename
        result.append(archive)
    if not {"curl", "zstd"}.issubset({item.name for item in result}):
        raise ValueError("missing required dependency")
    return result


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_archive(archive, directory, retries=4, delay=2, timeout=15):
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / archive.filename
    if target.is_symlink() or target.resolve().parent != directory:
        raise ValueError("dependency cache path escapes its directory")
    if target.exists():
        if not target.is_file() or file_hash(target) != archive.sha256:
            raise ValueError("cached dependency checksum mismatch: " + archive.name)
        return target, False
    for attempt in range(retries + 1):
        temporary = None
        try:
            digest = hashlib.sha256()
            size = 0
            deadline = time.monotonic() + timeout
            request = Request(archive.url, headers={"User-Agent": "Syrnck-dependency-fetch/1"})
            with tempfile.NamedTemporaryFile(mode="wb", delete=False, dir=directory,
                                             prefix="." + archive.filename + ".", suffix=".tmp") as output:
                temporary = Path(output.name)
                with urlopen(request, timeout=timeout) as response:
                    while True:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("dependency download deadline exceeded")
                        block = response.read1(65536)
                        if not block:
                            break
                        size += len(block)
                        if size > MAX_ARCHIVE_BYTES:
                            raise ValueError("dependency archive exceeds 128 MiB")
                        digest.update(block)
                        output.write(block)
                output.flush()
                os.fsync(output.fileno())
            if digest.hexdigest() != archive.sha256:
                raise ValueError("downloaded dependency checksum mismatch: " + archive.name)
            os.replace(temporary, target)
            return target, True
        except HTTPError as error:
            error.close()
            if error.code not in TRANSIENT_HTTP or attempt == retries:
                raise
            reason = "HTTP " + str(error.code)
        except (URLError, TimeoutError, ConnectionError, HTTPException) as error:
            if attempt == retries:
                raise
            reason = type(error).__name__
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except PermissionError:
                    # A transient scanner lock must not hide the original error;
                    # this random temporary name is never a trusted cache entry.
                    print("Could not remove dependency temporary file: " + str(temporary), file=sys.stderr)
        print(f"Retrying {archive.name} after {reason} ({attempt + 1}/{retries})", file=sys.stderr, flush=True)
        time.sleep(delay)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-only", action="store_true")
    args = parser.parse_args()
    try:
        records = []
        for archive in read_manifest():
            if args.core_only and archive.name != "zstd":
                continue
            path, downloaded = fetch_archive(archive, ROOT / ".tools/downloads")
            records.append({"name": archive.name, "path": str(path), "downloaded": downloaded})
        print(json.dumps(records, indent=2))
    except (ValueError, OSError, URLError, HTTPException) as error:
        parser.exit(1, "Dependency prefetch failed: " + str(error) + "\n")


if __name__ == "__main__":
    main()
