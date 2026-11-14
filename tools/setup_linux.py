#!/usr/bin/env python3
"""Fetch verified sources and build the private Python 2.7 runtime for Linux.

Run with Python 3. No system Python installation is changed. Shader generation
and game assets are outside this setup; SDL and FreeType are build dependencies
even when the resulting executable is used without a display.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
EXTERNAL = ROOT / "external"
# filename, source URL, SHA-256, extracted directory
PACKAGES = [
    ("Python-2.7.18.tgz", "https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz", "da3080e3b488f648a3d7a4560ddee895284c3380b11d6de75edb986526b9a814", "Python-2.7.18"),
    ("pybind11-2.9.2.tar.gz", "https://codeload.github.com/pybind/pybind11/tar.gz/refs/tags/v2.9.2", "6bd528c4dbe2276635dc787b6b1f2e5316cf6b49ee3e150264e455a0d68d19c1", "pybind11-2.9.2"),
    ("zlib-1.3.1.tar.gz", "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz", "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23", "zlib-1.3.1"),
    ("SDL3-3.4.18.tar.gz", "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz", "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3", "SDL3-3.4.18"),
    ("freetype-2.13.3.tar.gz", "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz", "5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747", "freetype-2.13.3"),
    ("DirectXMath.tar.gz", "https://codeload.github.com/microsoft/DirectXMath/tar.gz/refs/tags/oct2024", "69af50e165e8458b0422068cff24dbdaf7d649c3174f140acbb71c39f3c93716", "DirectXMath-oct2024"),
]


def download(name, url, digest):
    path = EXTERNAL / "downloads" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".part")
        print("Downloading", name, flush=True)
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError("SHA-256 mismatch: " + str(path))
    return path


def unpack(package):
    name, url, digest, folder = package
    archive = download(name, url, digest)
    destination = EXTERNAL / folder
    if not destination.exists():
        with tarfile.open(archive) as source:
            source.extractall(EXTERNAL, filter="data")
    return destination


def run(*args, **kwargs):
    print(" ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    core = ROOT / "gamecore"
    if not (core / "GameCore.cmake").exists():
        raise SystemExit("Initialize the game core first: git submodule update --init")
    for package in PACKAGES:
        unpack(package)
    math = EXTERNAL / "DirectXMath"
    if not math.exists():
        math.symlink_to("DirectXMath-oct2024", target_is_directory=True)
    mp3 = download("dr_mp3.h", "https://raw.githubusercontent.com/mackron/dr_libs/aec2dd20667f54acfdb66031217df6d240f56c52/dr_mp3.h", "f5c8020eeebb79877184432efdd2651f24869f5bb8f379d45450fd0aed1ee915")
    (EXTERNAL / "dr_libs").mkdir(exist_ok=True)
    installed_mp3 = EXTERNAL / "dr_libs/dr_mp3.h"
    if not installed_mp3.exists() or installed_mp3.read_bytes() != mp3.read_bytes():
        installed_mp3.write_bytes(mp3.read_bytes())
    prefix = EXTERNAL / "python27"
    marker = prefix / ".oc4-complete"
    build_config = "Python-2.7.18 shared ucs4 gnu17 v1\n"
    if not marker.exists() or marker.read_text() != build_config:
        build = EXTERNAL / "python27-build"
        build.mkdir(exist_ok=True)
        # Modern GCC defaults to C23, which the Python 2 sources predate.
        env = dict(os.environ, CFLAGS="-O2 -fPIC -std=gnu17", LDFLAGS="-Wl,-rpath," + str(prefix / "lib"))
        run(EXTERNAL / "Python-2.7.18/configure", "--enable-shared", "--enable-unicode=ucs4", "--disable-ipv6", "--without-ensurepip", "--prefix=" + str(prefix), cwd=build, env=env)
        run("make", "-j" + str(args.jobs), cwd=build, env=env)
        run("make", "install", cwd=build, env=env)
        marker.write_text(build_config)
    print("Dependencies ready. Next: cmake --preset linux-headless", flush=True)


if __name__ == "__main__":
    main()
