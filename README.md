# amiga_porting

Method and tools for porting Amiga games to C that runs on the Amiga, on SDL3 and in the browser, and matches the original bytes.

## The skill

[`skill/SKILL.md`](skill/SKILL.md) is the method, written as a Claude Code skill (`amiga-porting`): the original as the authority, game state as one struct at the original addresses, C-only with 68000 semantics, the layers of verification (per-function oracle, lockstep, sweep, A/B, cold start), the SDL and browser ports, and how to use ami9000 as the oracle.

To use it with Claude Code, copy or symlink the folder as a skill, e.g. `ln -s "$PWD/skill" ~/.claude/skills/amiga-porting`.

The reproducible shell it refers to is in the same folder: `skill/flake.nix`, `flake.lock`, `sdl3-emscripten.nix` and `.envrc` (copy them into a port and adjust the names).

## ami9000

[`ami9000/`](ami9000/) is the emulator host used as the oracle: an SDL3 libretro host with an embedded Lua 5.4 driver around a patched Engine9000/PUAE core.

- `host/` the host and its Lua modules (`host/README.md` documents the script API, hooks, lockstep and the control socket)
- `patches/` the core patches; `series` lists those applied by the build
- `engine9000-src/` the pinned Engine9000 source (a git submodule)
- `WHDLOAD.md` how a Lua loader takes over a game that a WHDLoad slave would start
- `pidrun.py` runs a process with a timeout and kills it through a pidfd

Clone with submodules and build (binaries are never committed):

```
git clone --recurse-submodules git@github.com:borancar/amiga_porting.git
cd amiga_porting
ami9000/host/build_core.sh
make -C ami9000/host
ami9000/build/ami9000_host game.lua
```

It needs SDL3 and Lua 5.4; Kickstart ROMs and game disks are not included.
