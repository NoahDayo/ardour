# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the private Windows AAF writer runtime into a new package directory.

Example: --destination PACKAGE/lib/ardour9/project-exchange --cache BUILD/downloads
Dependencies are pinned, hash-checked, and isolated from the user's Python.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile

DEPENDENCIES = [
    ("python-3.14.8-embed-amd64.zip",
     "https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip",
     "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310", "python"),
    ("pyaaf2-1.7.1-py2.py3-none-any.whl",
     "https://files.pythonhosted.org/packages/21/a6/4d89acdb0cfa4791166103e4c8d4392fb1026bdb339705189d472c107d3c/pyaaf2-1.7.1-py2.py3-none-any.whl",
     "67870dcce5239f463767f7acedcfab50f0a1e193525e3b6fa0889ebbeb0c55e6", "vendor"),
]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(cache, name, url, expected):
    path = cache / name
    if not path.exists():
        with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as work:
            temporary = Path(work) / name
            assert temporary.resolve().is_relative_to(cache.resolve())
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open("xb") as output:
                size = 0
                while block := response.read(1024 * 1024):
                    size += len(block)
                    if size > 64 * 1024 * 1024:
                        raise ValueError("Dependency download exceeds size limit")
                    output.write(block)
            if digest(temporary) != expected:
                raise ValueError("Dependency checksum mismatch: " + name)
            temporary.rename(path)
    if digest(path) != expected:
        raise ValueError("Cached dependency checksum mismatch: " + name)
    return path


def extract(archive, directory):
    with zipfile.ZipFile(archive) as source:
        if sum(item.file_size for item in source.infolist()) > 256 * 1024 * 1024:
            raise ValueError("Dependency archive exceeds size limit")
        for item in source.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or ".." in path.parts or ":" in item.filename or "\\" in item.filename or stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("Unsafe dependency archive member")
            target = directory.joinpath(*path.parts)
            assert target.resolve().is_relative_to(directory.resolve())
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(item) as input_file, target.open("xb") as output:
                    shutil.copyfileobj(input_file, output)


def install(destination, cache):
    if os.name != "nt":
        raise ValueError("This runtime packager requires Windows")
    destination = destination.resolve()
    if destination.exists():
        raise ValueError("Runtime destination must not already exist")
    cache.mkdir(parents=True, exist_ok=True)
    sources = [(fetch(cache, name, url, expected), folder) for name, url, expected, folder in DEPENDENCIES]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".logic-runtime-", dir=destination.parent) as work:
        stage = Path(work) / "runtime"
        assert stage.resolve().is_relative_to(destination.parent)
        stage.mkdir()
        for archive, folder in sources:
            extract(archive, stage / folder)
        helper = Path(__file__).resolve().parents[2] / "share/project-exchange/aaf_writer.py"
        shutil.copy2(helper, stage / helper.name)
        version = subprocess.run([str(stage / "python/python.exe"), "-I", "--version"],
                                 capture_output=True, text=True, timeout=30,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        if version.returncode or version.stdout.strip() != "Python 3.14.8":
            raise RuntimeError("Bundled Python validation failed: " + version.stderr)
        record = {"dependencies": [{"name": name, "url": url, "sha256": checksum}
                                   for name, url, checksum, _ in DEPENDENCIES],
                  "files": {str(path.relative_to(stage)).replace("\\", "/"): digest(path)
                            for path in stage.rglob("*") if path.is_file()}}
        (stage / "runtime-manifest.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        # Windows refuses to rename a directory over an existing destination.
        stage.rename(destination)
    print("Installed private AAF writer runtime:", destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    install(args.destination, args.cache)
