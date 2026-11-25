## What it changes

## What it is based on

<!-- The SDK, which data files, what was observed in the running game and how, documentation links. -->

## How it was checked

- [ ] clean room: neither I nor any tool or AI agent I used looked inside the original game's executables or libraries (no disassembly, decompiling, debugging, `strings`/`dumpbin`/`objdump`), and nothing here comes from someone else's ([CONTRIBUTING](https://github.com/bssth/openciv4/blob/master/CONTRIBUTING.md#clean-room))
- [ ] `ctest --test-dir build\Release` passes
- [ ] game results unchanged: an AI autoplay with a fixed seed gives the same per-turn checksums as before (or the change fixes what made them differ)
- [ ] original saves still load
