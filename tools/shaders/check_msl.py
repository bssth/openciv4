#!/usr/bin/env python3
"""Compiles the checked-in Metal shaders (src/render/MslShaders.inc) with Apple's compiler.

The renderer hands these sources to SDL, which compiles them only when the game starts
(newLibraryWithSource, default options): a translation that Metal rejects would otherwise
first fail on a player's Mac. This writes each shader to a .metal file, checks that its
entry point is there under the name the renderer asks for, and compiles it with
`xcrun -sdk macosx metal -c` (needs Xcode with its Metal toolchain; macOS only).

    python3 tools/shaders/check_msl.py [--keep DIR]

--keep DIR leaves the .metal files in DIR (otherwise a temporary folder is used).
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / "src/render/MslShaders.inc"
STAGES = {"VS": "vertex", "PS": "fragment"}


def shaders():
    text = INC.read_text(encoding="utf-8")
    found = re.findall(r'\{ "([^"]+)", "([^"]+)", R"msl\((.*?)\)msl" \},', text, re.S)
    if not found:
        sys.exit(f"no shaders found in {INC.relative_to(ROOT).as_posix()}")
    return found


def check(folder):
    failed = []
    for file, entry, source in shaders():
        name = f"{Path(file).stem}_{entry}"
        metal = folder / f"{name}.metal"
        metal.write_text(source, encoding="utf-8")
        stage = STAGES.get(entry[:2])
        if not stage or not re.search(rf"^{stage} \S+ {re.escape(entry)}\(", source, re.M):
            print(f"{file} {entry}: no {stage or 'vertex/fragment'} function named {entry}")
            failed.append(name)
            continue
        r = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-c", metal, "-o", folder / f"{name}.air"],
                           capture_output=True, text=True)
        print(f"{file} {entry}: {'ok' if r.returncode == 0 else 'FAILED'}")
        if r.returncode:
            print(r.stdout + r.stderr)
            failed.append(name)
    if failed:
        sys.exit(f"{len(failed)} Metal shader(s) did not compile: {', '.join(failed)}")


def main():
    args = sys.argv[1:]
    if args[:1] == ["--keep"] and len(args) == 2:
        folder = Path(args[1])
        folder.mkdir(parents=True, exist_ok=True)
        check(folder)
    elif not args:
        with tempfile.TemporaryDirectory() as d:
            check(Path(d))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
