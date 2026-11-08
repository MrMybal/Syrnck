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
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
TRANSIENT_HTTP = {408, 429, 500, 502, 503, 504}
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024


@dataclass(frozen=True)
class Archive:
    name: str
    url: str
    sha256: str
    cache_name: str = ""
    fallback: Optional["Archive"] = None

    @property
    def filename(self):
        name = self.cache_name or urlsplit(self.url).path.rsplit("/", 1)[-1]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", name) or name.endswith("."):
            raise ValueError("invalid dependency archive filename")
        return name


def parse_archive(name, entry, allow_fallback=True):
    required = {"url", "sha256"} if allow_fallback else {"url", "sha256", "filename"}
    allowed = required | {"fallback"} if allow_fallback else required
    if not isinstance(entry, dict) or not required.issubset(entry) or not set(entry).issubset(allowed):
        raise ValueError("invalid dependency manifest entry")
    url, sha, filename = entry["url"], entry["sha256"], entry.get("filename", "")
    if not isinstance(url, str) or not isinstance(sha, str) or not re.fullmatch(r"[a-f0-9]{64}", sha) or not isinstance(filename, str):
        raise ValueError("invalid pinned dependency")
    if not allow_fallback and not filename:
        raise ValueError("dependency fallback requires an archive filename")
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username is not None or parsed.password is not None or parsed.query or parsed.fragment:
        raise ValueError("dependency URL must use HTTPS without credentials or query parameters")
    fallback = None
    if entry.get("fallback") is not None:
        if not allow_fallback:
            raise ValueError("nested dependency fallbacks are not supported")
        fallback = parse_archive(name + "-fallback", entry["fallback"], False)
    archive = Archive(name, url, sha, filename, fallback)
    archive.filename
    if fallback and fallback.filename == archive.filename:
        raise ValueError("dependency variants must use distinct cache filenames")
    return archive


def read_manifest(path=ROOT / "cmake/dependencies.json"):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or type(data.get("version")) is not int or data["version"] != 1:
        raise ValueError("unsupported dependency manifest")
    result = []
    for name, entry in data.items():
        if name == "version":
            continue
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", name):
            raise ValueError("invalid dependency manifest entry")
        result.append(parse_archive(name, entry))
    if not {"curl", "zstd"}.issubset({item.name for item in result}):
        raise ValueError("missing required dependency")
    return result


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def cached_archive(archive, directory):
    target = directory / archive.filename
    if target.is_symlink() or target.resolve().parent != directory:
        raise ValueError("dependency cache path escapes its directory")
    if target.exists():
        if not target.is_file() or file_hash(target) != archive.sha256:
            raise ValueError("cached dependency checksum mismatch: " + archive.name)
        return target
    return None


def fetch_archive(archive, directory, retries=4, delay=2, timeout=15):
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    for candidate in (archive, archive.fallback):
        if candidate is not None:
            path = cached_archive(candidate, directory)
            if path is not None:
                return path, False
    try:
        return download_archive(archive, directory, retries, delay, timeout), True
    except (HTTPError, URLError, TimeoutError, ConnectionError, HTTPException) as error:
        if archive.fallback is None or (isinstance(error, HTTPError) and error.code not in TRANSIENT_HTTP):
            raise
        print("Using verified source fallback for " + archive.name, file=sys.stderr, flush=True)
        return fetch_archive(archive.fallback, directory, retries, delay, timeout)


def download_archive(archive, directory, retries, delay, timeout):
    target = directory / archive.filename
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
            return target
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
