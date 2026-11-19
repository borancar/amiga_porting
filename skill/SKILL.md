---
name: amiga-porting
description: Method for porting an Amiga game to C (Amiga build plus SDL3/browser) so it matches the original bytes. Use when reconstructing game functions or variables, turning absolute-address variables into a struct, removing padding or fixed addresses, running oracle/lockstep/sweep checks against the original, or working on the SDL and web builds of a port.
---

# Porting an Amiga game to C

Goal: C source that builds for the Amiga and for SDL3 (desktop, browser) and behaves like the original bytes. The original is the authority at every step; never "fix" something because it looks wrong.

## Layout of a port

The framework repository (amiga-decomp) holds shared tools; each game is `projects/<id>/`, selected explicitly (`python -m amiga_decomp --project <id>`), with no default game and no dependence on another project's layout. Game addresses, names, routines, data, media, reports and game-specific emulator forks belong under the project.

```
projects/<id>/
  AGENTS.md  README.md  DECOMPILING.md  PLAN.md  project.json   notes and the method as applied to this game
  host/        ami9000 Lua scripts (machine description, boot, lockstep, oracle cases)
  scripts/     generators and test drivers for this game (symbols, struct, amiga.asm, build, rename, lockstep, sweep)
  local/       git-ignored: Kickstart, original disks, installs, saves
  captures/    git-ignored: recordings, inputs, RAM dumps (bounded windows, compressed)
  native/      the reconstruction: its own repository, a submodule of the project
```

The reconstruction goes under `native/` and is self-contained, so it can be split out and published on its own:

```
native/
  README.md  COPYING  .gitignore  .envrc  flake.nix  flake.lock  sdl3-emscripten.nix
  Makefile                      Amiga build (m68k-amigaos-gcc): intro and game programs; `make sdl`, `make sdl MATCH_MEMORY_LAYOUT=0`
  src/                          the game in C: game.c, menu.c, boot.c, file.c ..., the state struct and declarations in game.h,
                                game.symbols (the single source of the variables), data.c and game.ld (generated), the intro's data
  src/amiga/                    only what the Amiga build has: loader.asm (the only assembly), hardware, display, memory.c
  src/sdl/                      the SDL3 port of the machine: host, display, irq, sound, files, guest.c / guest_load.c / guest_members.h,
                                gen_fixups.py, Makefile, Makefile.web, README.md
  web/                          the browser page (shell.html, assets)
  scripts/                      what the build needs at run time (e.g. serve_web.py)
```

Rules:
- One game module (`game.c`) with address comments, not one file per routine; asm only in the loader.
- Generated files (`game.h` between its BEGIN/END markers, `data.c`, `game.ld`, `amiga.asm`, fixups) are checked in so the native repository builds alone. The generators and the tests that compare the port with the original live in the project (`projects/<id>/scripts`, `host/`).
- Original disks, ROMs, recordings and binaries are never part of `native/`; the port asks for the disk images at run time (`--disks DIR`, the page's file picker).
- License the native repository as the user chooses (`COPYING` in `native/`).

## 1. Start from the original

- Original disks are never committed. Run the original in an emulator driven by scripts; it is the oracle.
- Reconstruct executed code only (coverage of scripted and human runs). A branch to code that never ran ends the C function with `/* goes on at $ADDR */`; fill it in when a later run reaches it.
- The executed code comes from an **exact trace**, not from hooks: the core's hooks report an address that is sometimes two bytes off while the machine free-runs (the prefetch), a hook on an extension word fires too, and `ami.hook` keeps one function per address (a coverage hook silently replaces your control hook). Free-run to the frame you care about, then `ami.step(fn)` (exact instruction addresses; about 30,000 steps a second) recording each address, its count, the frames it first and last ran and its observed successors (successors expose interrupt entries and indirect jumps). Do the inputs of the run from the step callback by frame number so the run is the same one.
- The low RAM is overlaid by programs (the intro, the game, a level's code): record the first frame of every instruction and take RAM dumps at several frames; decode each address from the dump of its phase. Find where a program image comes from on disk by searching the ADF for a block of the RAM dump.
- Python tooling runs through `uv` (`uv run script.py`, dependencies declared in the script's PEP 723 header or the project's `pyproject.toml`), never a global `pip install`. Read the original with capstone (`Cs(CS_ARCH_M68K, CS_MODE_M68K_000)`) on a RAM dump taken at the moment the code runs, with the coverage marking which instructions executed: disassemble, then write the C by hand from that listing. Code loaded by the game's own loader overwrites earlier code, so dump RAM after it is in place.
- Every function carries `/* orig $ADDR: what it does */`. Name by what the code does; a provisional name is `fn_` plus the address.

## 2. Variables are one struct

- Each variable at an absolute address is one line in a symbols file (`name 0xADDR type`). Generators turn that into the declarations, the single state struct, the linker script, the constant-data file and the Amiga assembly shim. Never edit generated regions by hand; regenerate.
- Members sit at the offsets of their original addresses; gaps are a `PAD(n)` macro that expands to a byte array only under a `MATCH_MEMORY_LAYOUT` switch. A compile-time `LAYOUT_ASSERT` checks the offsets.
- Code must not depend on absolute addresses or byte offsets: use struct members. The build without padding must behave identically to the padded one. Records that still carry always-present padding are converted field-wise when loaded.
- Flatten unions into named structs (one struct per variant) so nothing needs a cast. Put data and tables into structs rather than helpers or macros, unless raw (pixels, buffers). Constant tables are data, not code that fills them.
- Clear a struct with `memset(&x, 0, sizeof x)`; clear per member if the struct overlaps other state.

## 3. C only, 68000 semantics

- No inline assembly; the only assembly is the Amiga loader. The C runs from boot, as the original does.
- Keep original widths and signedness (16-bit word arithmetic; `blt/bgt` signed, `bcs/bhi` unsigned). Register arguments become parameters; results the caller uses become return values.
- Convert endianness only where data enters from disk or a memory image, never in game logic.
- Amiga-specific code lives in its own directory; the SDL port reimplements it.

### Hazards of running the C on a little-endian host

The same C builds for the big-endian 68000 and the little-endian host, and four things only go wrong on the host (the oracle on the Amiga build cannot see them; run the port headless with a scripted input and look at game variables, e.g. the positions of the enemies and the score):
- A raw byte buffer (screens, graphics, sector buffers, level data) holds the original's byte order: read and write its words and longs only through `rd16/rd32/wr16/wr32`; typed members are in host order.
- A long that overlays two words in another view of the same record (a 16.16 position used as a long by one routine and as two words by the others) is wrong on the host: declare it as the pair of words, high word first, and compose it in the one routine.
- An address of the original's code stored in memory (a handler, an animation op) is turned into the C function through one generated table, in the Amiga build too (never the identity: it would jump into the original's machine code); in the host build an address with no C is a logging stub.
- Loops that wait for an interrupt or the beam call `HOST_IDLE()`; the beam registers (`vposr`, `vhposr`) are given values that put the beam in the vertical blank; the disk HLE takes emulated frames (the game's retry loops count them).

## 4. Verify in layers

1. Oracle per function: hook the original at entry, call original and C on the same machine state, compare the result register and chip RAM. Every function gets a case. Routines that wait for the vertical blank cannot be called with interrupts masked: have the oracle make the waits return at once on both sides (generate the list from the C: the functions that contain `HOST_IDLE`), and put a watchdog on every call (restore the state and report HUNG after some frames, or the first hang silently blocks every later case; each hung call also leaks one of the emulator's eight call slots). The scenarios must really exercise the code: take the savestates where the thing runs (check hit counts of a few routines per 500 frames of a cold run) and press the fire button as the game expects.
2. Lockstep: run the original and a dev build of the port side by side and compare at the same point of the program (section 4a), memory and picture. Exclude timing counters. Apply keys by sync point so the run is deterministic.
3. Sweep: run a fixed set of saved states against a stored reference; list known non-equal states in the reference. Never rebuild while a sweep runs.
4. A/B of build variants: the padded and unpadded builds must produce identical frames and audio. Delete audio dump files first if the writer appends.
5. Cold start: run the SDL build headless from the first screen with scripted keys and compare its dumps.

Do not skip ahead by poking variables; play the regular run so the state is one the original reaches. Start processes through a pidfd-based runner, not `pkill -f` / `pgrep -f`.

### 4a. Lockstep with a dev build and an interactive Lua shell

The shipping port stays free of tooling (the port's folder holds only the port); the dev build is a separate target in a folder beside it that compiles the same game C and machine sources plus the Lua host (`dev/src`, `dev/lua`), and vendored third-party code lives in a top-level `third_party/`.

- **The dev build hosts Lua and nothing else.** It embeds Lua (vendored in the repository's top-level `third_party/`, never in the port's folder) and gives the scripts the machine as a global `port`: step and run frames, run to or hook a function by the address it has in the original (with callbacks), stop inside it, memory in the original's byte order (whole image, `peek8/16/32`), the picture, the stick and keys, load memory, disks, and raw sockets (`listen`, `accept`, `readline`, `read`, `write`, `close`). It has no command protocol of its own: `--lua SCRIPT` runs a script and `--lua-args TEXT` is the script's global `args`.
- **Interactivity is Lua's.** The control socket is served by a Lua script (`dev/lua/shell.lua`, started with `--lua dev/lua/shell.lua --lua-args PATH`), not by the dev build, and it is a regular interactive Lua shell, no framing and no commands of its own: a client (`rlwrap nc -U PATH`, or `socat`) gets the `> ` prompt, `>> ` for unfinished statements, expression values printed, errors with a traceback, `print`/`io.write` going to the client; globals and functions defined stay for the next lines; the port moves only while a line runs and stands still between lines, so a stop can be inspected at leisure. Binary results go to files from Lua (`io.open`), not through the socket.
- **The emulator side is the same kind of shell.** `ami9000_host --control PATH` is a Lua shell inside the lockstep script, with both sides in reach (the original's memory, the port's through a peer); put the helpers that read, poke and compare both sides (`port_read`, `port_poke`, `mem_diff`, a pause at an address) in the lockstep script, as the Gods port does. The lockstep drives the port through that same shell: the emulator can only start a peer as a process with pipes, so the shell script also serves stdin/stdout (`--lua-args -`), and the lockstep script sends lines of Lua (`port.step()`, a `sync(addr)` function it defines, `ram_out()`), waits for the prompt (set to a line it can wait for) and reads binary results as `BIN LEN` plus bytes. There is no separate peer protocol and nothing in the port's C knows one. A stop at a function can arrive without the hook having counted an entry: wait for the count, not only for the stop.
- **Savestates first.** Both sides need save and load of a complete state, and lockstep is built on them: the emulator has them (`snapshot_save/load`), so the dev build gets `port.save_state(PATH)` / `port.load_state(PATH)` (memory in the original's byte order, plus everything the machine layer keeps: custom registers, copper list pointers, blitter and Paula registers, CIA and keyboard state, the disk position, the frame count, pending interrupts). Priority order: (1) start both sides from the same state (the original's savestate imported into the port) instead of replaying from the cold start; (2) lockstep from any state the original has, so a late scenario (a level being played, a boss) is reached by loading, not by a long run; (3) on a first difference save both sides, so the stop can be reloaded and bisected; (4) a stored set of states is the sweep's input (section 4, item 3). A port that can only load memory (`loadram`) starts well from a state at a point where the machine layer has no pending work (a frame flip); anything else the state holds that is not in memory is a source of false differences, so list it.
- **The game runs as a coroutine** that yields at each `HOST_IDLE`, so the controller steps the port one frame at a time; `-finstrument-functions` (never on the generated code table) gives hooks by the original's address through the same table that maps addresses to C functions.
- **Sync points are program points, not instants:** the original takes real cycles and the port none, so compare at an entry of a function the game passes once a frame (the frame-flip routine; hook the entries that jump to it, not a wait loop that jumps to itself). The port starts from the original's memory captured at the game's first instruction.
- **Baseline of junk:** at the first sync point record which members already differ (what the original's memory held before the game, pointer fields with stale bits) and report only changes after that. Leave out members that depend on interrupt timing (frame counters; with a slow original also the music player and the random generator), as an option.
- **Picture:** the original's frame one frame after the sync point (the flip takes effect at the next vertical blank) against the port's picture; find the window of the port's picture in the emulator's frame once, by the shift and columns-per-pixel at which the lit pixels coincide. A constant mismatch of the whole picture is an alignment problem, not a game difference: check with the frame before and after.
- **Frame cadence:** a port stopped inside a hooked function is mid-frame; do not run its frame-end work again on resume, or the port takes extra frames per sync point.

## 5. The SDL port reimplements only the machine

Display, copper, blitter, interrupts, sound mixer, disk access and the memory image are the port's. Game files load member by member through a member table, with generated fixups for structs inside disk records. The Amiga build has no libc: supply `memset`/`memcpy` yourself (and keep the compiler from turning their loops into calls to themselves).

High-level emulation, not chip emulation: no beam, no DMA slots, no chip timing. The C that drives a chip stays faithful (it runs on the Amiga and in the oracle) and the few registers whose write starts an action go through one macro each that the port intercepts: `BLTSIZE_WRITE` (the host performs the whole blit at once from the registers: area mode with shifts, masks, minterm and modulos; the blitter is never busy) and `SETCLR_WRITE` for the set/clear registers (`dmacon`, `intena`, `intreq`). A routine that is better replaced as a whole (the game's disk loader, a chip delay loop) is declared `HLE_WEAK` and the port links a strong definition over it. The picture is made once a frame from the registers the CPU set and the copper list the game leaves in memory, read as data (MOVE and WAIT per scanline). Sound is a mixer fed from the channel registers (lc, len, per, vol, DMACON). Waits for an interrupt are written `while (!flag) HOST_IDLE();`: the host runs the due interrupts there, one video frame of emulated time per call. Where the picture cannot be derived this way, the oracle layers (section 4) compare frames. Gods (`src/sdl/display.c`) and Xenon 2 (`src/sdl/display.c`, `blitter.c`, `sound.c`, `disk.c`) are references.

## 6. Browser build

Emscripten (wasm32, pthreads). The program runs on a worker thread; `emscripten_set_main_loop` does vblank, drawing and sound; disk images go to the in-memory file system from the page; persistent data uses IDBFS; a phase switch joins the worker and starts the next phase. Turn the renderer's vsync on, or SDL runs the main loop on a timer. Serve with a script that sets the cross-origin-isolation headers threads need.

## 7. Working rules

- Build inside the reproducible shell: `flake.nix`, `flake.lock`, `sdl3-emscripten.nix` and `.envrc` (direnv `use flake`, with the retrofoundry cachix cache) in this folder are the reference; copy them into the port's repository and adjust the names (the flake's `GAME_*`/`game.*` names must match the port's Makefiles). It provides the m68k-amigaos-gcc toolchain, SDL3, Emscripten and `nix build .#web`. Run builds with `nix develop --command ...`.
- Small commits, each with a summary; pushes carry `[skip ci]` where the project asks for it; push only when asked.
- Framework tools and per-game analysis stay in the framework repository; the port's repository holds the reconstruction and the generated files its build needs.

## 8. The oracle: ami9000 (this repository)

Multi-disk games and runtime checks: `content` can be an `.m3u` (the first image in DF0); `ami.disk_count/index/eject/select` swap floppies (eject, wait ~20 frames, select, insert; a savestate restores disk 1 again, so scripts that load a disk-2 level start from a cold boot or swap after the load). A host script ends with `ami.quit()`, or the process runs until its timeout.

This repository, `git@github.com:borancar/amiga_porting.git`, carries the emulator host every check above runs on. Clone with `--recurse-submodules` (`ami9000/engine9000-src` is the pinned Engine9000/PUAE source). Binaries are never committed; build them:

```
ami9000/host/build_core.sh     # patched PUAE core -> ami9000/build/system/ami9000.so (patches listed in ami9000/patches/series)
make -C ami9000/host           # host -> ami9000/build/ami9000_host   (needs SDL3 and Lua 5.4)
ami9000/build/ami9000_host game.lua [--headless] [the script's own flags]
```

- A game is one Lua script: `ami.machine{ model, content (adf, .slave or "@blank"), files, roms = {{"kickNNN.rom", crc32}}, opt = {...} }` then `ami.boot()`. Kickstarts and disks are never shipped; keep them in the project's ignored `local/` folder.
- After `ami.boot()` the script is a coroutine resumed once per core frame: `ami.wait(n)`, `wait_until(fn, max)`, `peek/poke 8/16/32`, `read/write`, `reg/set_reg`, `key`, `pixel/frame_hash/screenshot`, `snapshot_save/load`, `pause/resume`. Hooks run a Lua body when the CPU reaches an address (this is how the per-function oracle calls the original and the C on one state). `ami.import("amiga/lockstep")` is the frame-locked lockstep driver against a native peer (the peer speaks `key`/`step`/`image`/`quit` over stdin/stdout); `amiga/hunk`, `amiga/exec`, `amiga/dos13` load hunk executables and take over Kickstart 1.3 boot.
- `--control PATH` serves a socket; `ami9000/host/ctl.py PATH 'ami.reg("pc")'` runs Lua lines in the live script, including while it is paused. Use it to inspect a stop.
- Run it under `ami9000/pidrun.py [--timeout S] -- COMMAND` so it can be waited on, timed out and killed through a pidfd.
- Full API, flags and the core patches: `ami9000/host/README.md`; how a Lua loader takes over a WHDLoad-style game: `ami9000/WHDLOAD.md`. Each port keeps its own scripts under its project (e.g. `projects/<id>/host/*.lua`), not here.
- Patches that do not apply to the pinned source (checkpoint, clock, transfer, live-display, pc-observer) are kept in `ami9000/patches/` but are not in `series`.
