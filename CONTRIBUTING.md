# Contributing

Bug reports are most useful with `openciv4.log` and an autosave from before the problem: the
engine is deterministic, so a save replays the same turn. The issue forms ask for both.

## Clean room

openciv4 is written without looking inside the original game's programs. Its sources are:

* the SDK Firaxis released: the game core's source and headers;
* the game's own data files, which it loads as they are: XML, Python, shaders, art, texts;
* the original game running, under Windows or Wine: what it shows and plays, the files it
  reads and writes, its timings, and what a game core built from the SDK logs of the calls
  the engine makes into it;
* public documentation, modding guides and forum posts that do not quote disassembly.

Not allowed, for you and for any tool or AI agent working for you:

* Running anything but the game itself on the original executables and the libraries they
  ship with (`Civ4BeyondSword.exe`, the PitBoss and other `.exe`, the Gamebryo DLLs):
  disassemblers, decompilers, debuggers, `strings`, `dumpbin`, `objdump`, hex editors, memory
  dumps, API monitors or DLLs injected to trace the executable's own calls.
* Using what others learned that way: addresses, internal function names, constants or
  algorithms quoted from disassembly, in forum posts, other projects or an AI's memory.
* Committing any of the game's files (art, XML, Python, saves).

What can only be learned from inside an executable stays open: describe what is observed in
an issue instead. A pull request that shows such knowledge, in its code or its description,
is closed, and such code found in the repository is rewritten from observation.

## Pull requests

* The description says what the change is based on (the SDK, which data files, what was
  observed in the running game and how), so that a reviewer can tell it is clean.
* Build and test as in the [README](README.md#building-windows-x64); `ctest` passes.
* Game results stay the same: with a fixed seed an AI autoplay prints the same sync checksum
  every turn before and after the change, unless the change fixes what made them differ.
  For example, `oc4core.exe --turns 100 --map Fractal --size STANDARD --seed 7`.
* Original saves keep loading.
* Changes to the game core go to [openciv4-dll](https://github.com/bssth/openciv4-dll) (the
  `gamecore` submodule) and follow its rules.
* Comments explain what the code cannot say itself; no history of the change in them, that
  is what the commit message is for.
