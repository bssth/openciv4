# Headless Linux builds and checks

This build/validation work complements [the Linux runtime PR](https://github.com/bssth/openciv4/pull/32).
It adds reproducible dependency setup, a Clang/Ninja preset and asset-free CI coverage.
The runtime changes in that PR are a prerequisite. No renderer changes or SDK pin
changes are included here.

## Build

Use an x86-64 Linux machine with CMake 3.28+, Ninja, Clang, a C/C++ build toolchain,
Python 3.12+ and development headers for OpenSSL, libffi, zlib, X11/Wayland and audio.
The package list in [the workflow](../.github/workflows/linux-headless.yml) targets
Ubuntu 24.04. After a recursive clone:

```sh
python3 tools/setup_linux.py --jobs 4
cmake --preset linux-headless
cmake --build --preset linux-headless --parallel 4
env -u DISPLAY -u WAYLAND_DISPLAY SDL_AUDIODRIVER=dummy \
  ctest --test-dir build/linux-headless --output-on-failure
```

The setup script downloads source archives with pinned SHA-256 checksums and builds
a private shared Python 2.7.18 with UCS-4 Unicode under `external/python27`. It also supplies the existing
pybind11, SDL3, FreeType, zlib, DirectXMath and dr_mp3 build dependencies. It does not
change system Python, fetch game assets or modify SDK sources. Its cache and outputs
stay under the ignored `external/` directory.

The executable still links the engine's SDL/FreeType renderer and audio code. The
preset enables the existing SPIR-V support because the Linux renderer requires it
at compile time. Headless commands do not open a window or audio device; shader
regeneration tools, a display server and a Vulkan device are not needed for these
commands or tests. Graphical Linux validation belongs to the runtime/renderer work.

## Local gameplay validation

Use an owned Beyond the Sword 3.19 installation with `Assets`, `Warlords` and
`Beyond the Sword` under its root. Supply that root explicitly with `--civ4`.
Keep generated saves and logs outside the source tree or under ignored `build/`:

```sh
mkdir -p build/linux-validation
env -u DISPLAY -u WAYLAND_DISPLAY OC4_DOCUMENTS="$PWD/build/linux-validation/documents" \
  build/linux-headless/bin/oc4core --civ4 /path/to/Civ4 \
  --turns 100 --map Fractal --size STANDARD --seed 7 \
  --save-at 50 --save build/linux-validation/turn50.CivBeyondSwordSave \
  > build/linux-validation/first.log
env -u DISPLAY -u WAYLAND_DISPLAY OC4_DOCUMENTS="$PWD/build/linux-validation/documents" \
  build/linux-headless/bin/oc4core --civ4 /path/to/Civ4 \
  --load build/linux-validation/turn50.CivBeyondSwordSave --turns 50 \
  > build/linux-validation/resumed.log
```

Compare every turn's sync checksum, then repeat the original 100-turn command.
For data mods, both `--mod 'Mods/Next War'` and the Windows spelling
`--mod 'Mods\Next War'` resolve to the installed directory. Save metadata retains
Windows separators, so reloading finds the same mod.

## Test scope

`asset_paths` creates its own synthetic files and checks mixed-case/backslash lookup,
UTF-8 installation paths, base/expansion/mod precedence, listings and missing layers.
`save_file` creates synthetic containers and checks empty/small/large payloads against
known MD5 digests, round trips binary metadata and crosses compressed chunk boundaries.
It also checks rejection of a truncated container. Neither test includes game data.

The CI job runs the engine's CTest suite without game assets. Owned-game autoplay,
original-game save compatibility, and graphical/hardware/audio checks remain local
validation tasks. Native Windows/macOS checks use the existing workflows.

Local validation used Fedora x86-64, Clang 22, OpenSSL 3.5.9 and the private UCS-4
Python build. The Linux runtime PR at `ec06495` passed 12 existing CTests before
these changes; with this branch applied, all 14 passed without a display. A
100-turn Fractal/Standard/seed-7 run retained every baseline checksum, ending at
`D7D43800`. The baseline's turn-50 save resumed with identical checksums through
turn 100. Next War started, saved and reloaded with both mod-path slash styles.
These combined checks also included the separate SDK map-init logging-format fix;
the engine PR does not change the SDK gitlink. Fresh dependency setup and a repeat
using its completed cache both succeeded. No game files are included in CI.
