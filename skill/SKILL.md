---
name: amiga-porting
description: Method for porting an Amiga game to C (Amiga build plus SDL3/browser) so it matches the original bytes. Use when reconstructing game functions or variables, turning absolute-address variables into a struct, removing padding or fixed addresses, running oracle/lockstep/sweep checks against the original, or working on the SDL and web builds of a port.
---

# Porting an Amiga game to C

Goal: C source that builds for the Amiga and for SDL3 (desktop, browser) and behaves like the original bytes. The original is the authority at every step; never "fix" something because it looks wrong.

## 1. Start from the original

- Original disks are never committed. Run the original in an emulator driven by scripts; it is the oracle.
- Reconstruct executed code only (coverage of scripted and human runs). A branch to code that never ran ends the C function with `/* goes on at $ADDR */`; fill it in when a later run reaches it.
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

## 4. Verify in layers

1. Oracle per function: hook the original at entry, call original and C on the same machine state, compare the result register and chip RAM. Every function gets a case.
2. Lockstep: replay a recording on the original and on the C build; compare game variables at the top of each loop iteration, plus screenshots. Exclude timing counters. Apply keys by iteration so the replay is deterministic.
3. Sweep: run a fixed set of saved states against a stored reference; list known non-equal states in the reference. Never rebuild while a sweep runs.
4. A/B of build variants: the padded and unpadded builds must produce identical frames and audio. Delete audio dump files first if the writer appends.
5. Cold start: run the SDL build headless from the first screen with scripted keys and compare its dumps.

Do not skip ahead by poking variables; play the regular run so the state is one the original reaches. Start processes through a pidfd-based runner, not `pkill -f` / `pgrep -f`.

## 5. The SDL port reimplements only the machine

Display, copper, blitter, interrupts, sound mixer, disk access and the memory image are the port's. Game files load member by member through a member table, with generated fixups for structs inside disk records. The Amiga build has no libc: supply `memset`/`memcpy` yourself (and keep the compiler from turning their loops into calls to themselves).

## 6. Browser build

Emscripten (wasm32, pthreads). The program runs on a worker thread; `emscripten_set_main_loop` does vblank, drawing and sound; disk images go to the in-memory file system from the page; persistent data uses IDBFS; a phase switch joins the worker and starts the next phase. Turn the renderer's vsync on, or SDL runs the main loop on a timer. Serve with a script that sets the cross-origin-isolation headers threads need.

## 7. Working rules

- Build inside the reproducible shell: `flake.nix`, `flake.lock`, `sdl3-emscripten.nix` and `.envrc` (direnv `use flake`, with the retrofoundry cachix cache) in this folder are the reference; copy them into the port's repository and adjust the names (the flake's `GAME_*`/`game.*` names must match the port's Makefiles). It provides the m68k-amigaos-gcc toolchain, SDL3, Emscripten and `nix build .#web`. Run builds with `nix develop --command ...`.
- Small commits, each with a summary; pushes carry `[skip ci]` where the project asks for it; push only when asked.
- Framework tools and per-game analysis stay in the framework repository; the port's repository holds the reconstruction and the generated files its build needs.
