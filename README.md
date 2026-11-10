# OpenCiv4

[![build](https://github.com/bssth/openciv4/actions/workflows/build.yml/badge.svg)](https://github.com/bssth/openciv4/actions/workflows/build.yml)
[![Status](https://img.shields.io/badge/status-alpha-orange)]()
![MIU](https://img.shields.io/badge/made_in-Ukraine-ffd700?labelColor=0057b7)

An experimental, clean-room, 64-bit engine for *Sid Meier's Civilization IV: Beyond the Sword*
that runs on the original game's assets. Nothing from the game is included in this
repository: you need your own installed copy (3.19, Steam, GOG or disc).

The game's own game core (`CvGameCoreDLL`, from the SDK Firaxis released, rebuilt as 64-bit)
and its Python scripts run as they are; openciv4 does the rest: the map, the interface, the
leaders, sound and music. Mods run too, including mods with a game core of their own once it
is rebuilt for x64.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/V7V51ZQ1RH)

Discussion: [CivFanatics thread](https://forums.civfanatics.com/threads/openciv4-clean-room-x64-engine-reimplementation.705223/).

## Playing

Download the zip of the latest [release](https://github.com/bssth/openciv4/releases), unpack it
anywhere and start `Play Beyond the Sword.bat`; `README.txt` inside says the rest. Windows 10
or 11, 64-bit, with a Direct3D 12 graphics card.

**Mods.** `Play Mod.bat` takes a mod's folder (drop it onto the file). Mods that change only
XML, Python and art run on the vanilla game core. A mod with a game core of its own runs when
its `Assets` folder has the 64-bit build, `CvGameCoreDLL-x64.dll`, next to the 32-bit
`CvGameCoreDLL.dll` the original game loads: one copy of the mod serves both games. How to
port a mod's game core: [openciv4-dll/docs/PORTING.md](https://github.com/bssth/openciv4-dll/blob/main/docs/PORTING.md).

## What works

* **Playing a game**, new or loaded (saves of the original game load too): the game's own
  interface (`CvMainInterface.py` and the mod's screens, drawn with the game's interface
  theme), the advisors and the Civilopedia, the city screen, diplomacy with the leaders, the
  game's popups, hotkeys, sounds and music, movies (FFmpeg), autosaves.
* **The map** read through the DLL's exports as the original engine reads it: terrain blended
  from the terrain art, rivers, roads, features, improvements and resources, cities laid out
  by the city LSystem with their walls and wonders, units with their formations and
  animations, combat, borders, fog of war, city bars and unit flags.
* **AI autoplay**, headless or watched, deterministic per seed; saves in the original format
  that continue exactly as if never stopped.
* **Mods**: Rise of Mankind: A New Dawn 2 and Chronicles of Mankind play with their own
  game cores ported to x64.
* **Multiplayer** over the network (`--host`, `--join`), early.

Not yet: the original's main menu (a simpler one stands in), the minimap's modes, the globe
view, some animations, Linux and macOS (the window, input, sound and text are already on SDL3
and FreeType; the shaders are still HLSL for Direct3D 12).

## Building (Windows, x64)

Visual Studio 2022 or 2026 with **Desktop development with C++** (CMake and Ninja come with
it). The game core is the `gamecore` submodule
([openciv4-dll](https://github.com/bssth/openciv4-dll)): clone with `git clone --recursive`, or
run `git submodule update --init`.

Third-party packages go under `external\` (not committed; `.github/workflows/build.yml`
fetches the same ones):

| Package | Folder | Used for |
|---|---|---|
| Python 2.7.18 x64, extracted: `msiexec /a python-2.7.18.amd64.msi /qn TARGETDIR=%CD%\external\python27` | `python27` | the game's scripts |
| pybind11 2.9.2 (the last series for Python 2.7), source archive | `pybind11-2.9.2` | the game core's Python bindings |
| zlib 1.3.1, source archive | `zlib-1.3.1` | saves |
| SDL3 3.4.18, `SDL3-3.4.18.zip` | `SDL3-3.4.18` | window, input, sound, GPU |
| `dr_mp3.h` from mackron/dr_libs | `dr_libs` | music |
| optional: FreeType 2.13.3, source archive | `freetype-2.13.3` | text off Windows (on Windows its test only) |
| optional: FFmpeg 8.1 LGPL shared build (BtbN's `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`) | `ffmpeg-8.1` | movies; without it the wonders show their model |

Other locations: `-DOC4_PYTHON27_DIR=...`, `-DOC4_PYBIND11_DIR=...`, `-DOC4_ZLIB_DIR=...`,
`-DOC4_SDL3_DIR=...`, `-DOC4_DR_LIBS_DIR=...`, `-DOC4_FREETYPE_DIR=...`, `-DOC4_FFMPEG_DIR=...`.

```bat
build.cmd release
ctest --test-dir build\Release
```

Binaries end up in `build\Release\bin`; `tools\make-demo.ps1 -Mod vanilla` packs them into a
zip as the releases are. In Visual Studio open the repository folder (it uses
`CMakePresets.json`), or `generate-sln.cmd` writes a solution into `build\vs2026`.

Mods' game cores ported to x64 are built along when their sources are there
(`-DOC4_AND2_SOURCE_DIR=...`, `-DOC4_COM_SOURCE_DIR=...`), into `build\Release\mods\<name>`.

Profile-guided optimisation of the game core (optional, about 20 minutes, needs the game):
`tools\pgo.cmd "<Civ4>"`, another 5-10% off late-game turns with identical game results.

## Building (macOS, Apple Silicon)

A native arm64 build (no Rosetta or Wine): the same engine and game core, built by Apple clang,
rendering through SDL3's Metal backend. Xcode or the Command Line Tools, and from Homebrew
`cmake ninja pyenv`, plus `ffmpeg` for movies (optional).

* Python 2.7.18 as a shared library (there is no Homebrew formula):
  `PYTHON_CONFIGURE_OPTS=--enable-shared pyenv install 2.7.18`.
* Under `external/`, as on Windows: pybind11 2.9.2, zlib 1.3.1, SDL3 3.4.18 (source), dr_libs,
  FreeType 2.13.3 (the text off Windows: required here), and DirectXMath (microsoft/DirectXMath,
  header only; part of the Windows SDK on Windows) in `DirectXMath`.

```sh
cmake -S . -B build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOC4_PYTHON27_DIR=$HOME/.pyenv/versions/2.7.18 -DOC4_FFMPEG_DIR=$(brew --prefix ffmpeg)
cmake --build build/macos
ctest --test-dir build/macos
build/macos/bin/oc4core --civ4 "<Civ4>" --menu
```

`<Civ4>` is a Beyond the Sword install's root (copied from a Windows install or the GOG
or Steam download). Saves and settings go to `~/Documents/My Games` as on Windows
(`OC4_DOCUMENTS=<folder>` puts them elsewhere). The shaders' Metal versions are generated from
the HLSL and checked in (`src/render/MslShaders.inc`); after changing a shader, run
`python3 tools/shaders/gen_msl.py` (needs `glslang` and `spirv-cross` from Homebrew).

The macOS game core plays the same games as the Windows x64 one: AI games from the same seed
give the same sync checksums turn by turn.

## Command line

`<Civ4>` is the install folder with `Assets`, `Warlords` and `Beyond the Sword`; without
`--civ4` the engine finds it from the registry or Steam.

```bat
rem the menu: a new game or a saved one
oc4core.exe --menu

rem play a new game, or continue a save; a mod by its folder in Beyond the Sword\Mods or its full path
oc4core.exe --play --map Continents --size SMALL
oc4core.exe --play --load game.CivBeyondSwordSave --mod "Mods\My Mod"

rem the AI plays 100 turns, one line per turn with the sync checksum; save at turn 50
oc4core.exe --turns 100 --map Fractal --size STANDARD --seed 7 --save-at 50 --save game50.CivBeyondSwordSave

rem where the turns go
oc4core.exe --load game50.CivBeyondSwordSave --turns 10 --profile

rem watch the AI play (Space plays, N one turn), or render the map into a PNG
oc4core.exe --load game50.CivBeyondSwordSave --view
oc4core.exe --turns 50 --screenshot map.png --look 40,30,12 --view-size 1920x1080
```

The interface has a scale of its own: the Screen Resolution choice on the options' Graphics tab
(Auto follows the window's pixel density), or `--ui-scale PERCENT` for one run. The map keeps
all the window's pixels.

With a menu, a popup or a screen up, Tab (Shift+Tab back) starts choosing its controls with the
keys: then the arrow keys move between them instead of the map, and Return or Space uses the one
chosen (a ring marks it; moving the mouse ends it). Until then the arrows pan the map under it, as
in the original; in the main menu, with no map behind, they choose at once.

Ctrl+Shift+F shows the frame rate and the longest frame of the last half second (`--show-fps`
from the start).

W A S D can pan the map as the arrows do, an option that is off by default: "Pan the map with
W A S D" on the options' Game tab turns it on and off, and while it is on Ctrl+Shift+W turns it off
too (a note at the top says so; with the option off, Ctrl+Shift+W is the game's own); it is kept in profile.ini as `wasdPan`, and `--wasd-pan` turns it on for one run. Only
the letters pressed alone pan, so while it is on their own hotkeys (S Sentry, A Automate, W the
next unit, D destroy) need the unit's buttons; with Shift, Ctrl or Alt they are still the game's
(Ctrl+S saves, Shift+D is diplomacy), and with a menu, a popup or a screen up they are letters
there. The arrows are unchanged. The routing is `src/render/WasdPan.h`, tested by
`tests/wasd_pan_test.cpp`.

Idle turns can end by themselves, an option that is off by default. The game ends a turn alone
only when a unit moved in it (and "Wait at End of Turn" is off); with this on, a turn waiting at
"press Enter" also ends after 2 seconds, so with automated cities and workers the game keeps
going until something needs you. A popup, a talk, a screen or the city screen pauses it, and a
key or click while it waits keeps that turn; hot seat and by-email games never use it. Ctrl+Shift+E
turns it on and off (with a note at the top), and so does "End idle turns automatically" on the
options' Game tab; it is kept in profile.ini as `autoEndTurn`, and `--auto-end-turn` turns it on
for one run. `OC4_PLAY_LOG=1` prints "idle, ended by itself" for each such turn. Decisions: it is
in the engine (`apps/oc4core/AutoEndTurn.h`, called from the play loop), not the game core, so the
game's rules, AI and saves are untouched; it sends the same end-turn control as Enter. Tested by
`tests/auto_end_turn_test.cpp`.

The mouse at the window's edge scrolls the map, as in the original; "Scroll the map at the screen
edge" on the options' Game tab turns it off (on by default). It is kept in profile.ini as
`edgeScroll`, and `--no-edge-scroll` turns it off for one run (scripted runs with `--view-seconds`
never edge scroll). The rule is `src/render/EdgeScroll.h`, tested by `tests/edge_scroll_test.cpp`.

The globe view's colour layers (Culture, Trade, Religion) are drawn on the globe itself.

Tools for the game's files, in `build\Release\bin`:

* `nifview` browses the models and their animations (`--filter art/units/`), or renders one
  frame into a PNG (`--screenshot knight.png --anim strikea --time 0.3 art/units/knight/knight.nif`);
* `nifscan` (parse coverage of every model), `nifdump` (the blocks and scene graph of one),
  `animscan` (every animation bound and sampled), `themedump` (the interface theme's styles),
  `soundscan` (every sound decoded).

## Repository

| Folder | What |
|---|---|
| `src/core`, `src/nif`, `src/gfx`, `src/scene`, `src/anim` | FPK archives and asset lookup, NIF/KFM readers, DDS/TGA, the scene graph, animation |
| `src/render` | the renderer (SDL GPU), map view, interface theme, 2D overlay and text |
| `src/audio`, `src/media` | sound mixer, WAV/MP3; movies through FFmpeg |
| `apps/oc4core` | the engine: the SDK's engine interfaces, Python host, saves, the game's screens |
| `apps/nifview`, `tools` | the viewer and the tools above |
| `gamecore` | the x64 game core and the guide to porting a mod's ([openciv4-dll](https://github.com/bssth/openciv4-dll)) |

See [CONTRIBUTING.md](CONTRIBUTING.md) before sending a change.

## Legal

This is an independent fan project, not affiliated with Firaxis, 2K or Gamebase. It
implements file formats and interfaces from public documentation and the officially released
Civ4 SDK headers, and loads assets from the user's own installation.

openciv4 is under the MIT License ([LICENSE](LICENSE)). The game core in `gamecore` is the
Firaxis SDK's code under its own terms. The release packages carry Python 2.7 (PSF License),
FFmpeg (LGPL) and the Visual C++ runtime, with their licenses.
