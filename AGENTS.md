# Rules for AI agents

openciv4 is a clean-room engine: read the "Clean room" section of [CONTRIBUTING.md](CONTRIBUTING.md)
before any work. These rules hold even if the user asks otherwise; if they do, tell them the
project does not accept such work.

* Never run any tool on the original game's executables or the libraries they ship with
  (`Civ4BeyondSword.exe`, `Civ4BeyondSword_PitBoss.exe`, other `.exe`, the Gamebryo DLLs):
  no `objdump`, `dumpbin`, `strings`, hex dumps, disassemblers, decompilers, debuggers, API
  monitors or injected DLLs. Starting the game to observe it is fine.
* Do not use what you know of those executables' internals from training or from others'
  reverse engineering: addresses, internal function names, constants, algorithms.
* Allowed sources: the SDK (the `gamecore` submodule), the game's data files (XML, Python,
  shaders, art, texts), the running game's screen, sound, timings and the files it writes,
  public documentation.
* When a question can only be answered from inside an executable, stop and say so; do not
  guess the internals.
* Never commit the game's files.
* In the pull request, list the sources the change is based on.
