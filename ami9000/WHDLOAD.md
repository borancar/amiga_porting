# ami9000_host: running Amiga games from Lua, and what a WHDLoad slave does

How the standalone `ami9000_host` runs a game from a Lua script with only a Kickstart ROM and the game's files,
what that takes over from a WHDLoad slave, and what has been learned about doing it. It is the emulator side of a
side-by-side comparison with a native port; it is not original-disk execution. Each game's own part (its data, its
patches) lives under `projects/<id>/host/`; the first game is F/A-18 Interceptor
(`projects/fa18-interceptor/host/fa18.lua`; the slave's patches are analysed in `projects/fa18-interceptor/GAME.md`).

## Components

- `ami9000/build/system/ami9000.so`: Engine9000's PUAE libretro core, rebuilt with the
  `patches/ami9000-*.patch` (register writes, hooks) by `ami9000/host/build_core.sh` (the pinned
  `ami9000/engine9000-src` stays untouched).
- `ami9000/host/`: the SDL3 libretro host with an embedded Lua 5.4 driver. Build with
  `make -C ami9000/host`. Its README documents the script API, hooks and flags.
- `ami9000/host/lua/amiga/`: reusable Lua modules (`hunk`, `exec`, `dos13`).
- `projects/<id>/host/*.lua`: one script per game and route: machine description, boot, patches.
- `projects/<id>/local/` (git-ignored): the Kickstart, save directory and the game's install.

## Running a game

```
ami9000/host/build_core.sh                  # once
make -C ami9000/host
ami9000/build/ami9000_host projects/<id>/host/<name>.lua [--headless] [the script's own flags]
```

A game needs the Kickstart it was written for (named and CRC-checked in the script's `ami.machine{ roms = ... }`;
Kickstarts are never shipped) and its files in a host directory. Unpacking those from a disk image is a per-game step,
kept in the project (for example an `install_adf` script driven by `project.json`).

## How a Lua loader works

This is the method; a game's script follows it and adds its patches.

1. `content = "@blank"` gives the machine an empty floppy, so Kickstart 1.3 boots to "insert disk" and its boot task
   (strap) polls trackdisk for a disk change in a loop at `$FE8638`, as ordinary task code. When the CPU first reaches
   it, exec, the libraries and the devices are initialised and nothing else is running. The loop is reached at any CPU
   speed, so it is a safe place to take over.
2. A hook there takes over (a hook is Lua run from inside the core's instruction loop when the CPU reaches an address;
   it stops nothing; see the host README):
   - `amiga/hunk` parses the game's hunk executable and places it like `LoadSeg` through `AllocMem` (guest calls made
     with `ami.call`), with relocations and a seglist.
   - The game's patches are applied: bytes poked into the hunks, hooks, small 68000 routines placed in guest memory.
   - Intuition is opened and `OpenWorkBench` called, because a normal boot has them (see the findings below).
   - `amiga/dos13` installs a `dos.library` replacement whose vectors are hooks served from the host directory,
     as a slave's HD mode does. It is read-only; writes fail with "write protected".
   - `amiga/exec.create_process` makes a Process with a CLI structure and starts it with `AddTask`. Its first
     instruction is hooked to set the AmigaDOS entry registers and jump to the first hunk. `AddTask` switches to the new
     task before it returns to the hook, so the program's first DOS call happens while the hook's own guest call is
     still outstanding; guest calls therefore interleave (each has its own return trap).
3. The program then runs as a normal task. Nothing runs in the guest for the loader itself.

## What a WHDLoad slave does, and where it goes

A slave boots a Kickstart emulation, assigns logical devices to the install directory, `LoadSeg`s the program and
patches it (`resload_PatchSeg`, patch lists with `PL_*` commands). The Lua route replaces each part:

| Slave | Lua route |
| --- | --- |
| Kickstart emulation (`kick13.s`) | the real Kickstart ROM on a plain A500 |
| assigns (`DF0`, a game's own device) | `assigns` of `amiga/dos13` |
| `LoadSeg` and start | `amiga/hunk` and `amiga/exec.create_process` |
| `PL_B`, `PL_W`, `PL_L`, `PL_NOP` | `ami.poke8/16/32`, `ami.write` |
| `PL_PS` / `PL_P` (a `JSR` / `JMP` into the slave's routine) | the same bytes plus a routine placed in guest memory, or an `ami.hook` that emulates it |
| tooltypes (`BW`, `C3`, ...) | flags of the script (`ami.argv`) |

A slave addresses its patches as offsets into all hunks laid end to end. A script maps such an offset to guest memory by
walking the hunk sizes (`fa18.lua` has a small `at()` for it). Patches that exist to cope with accelerated CPUs (counting
loops, speed regulation, 32-bit pointer fixes) are only needed on a faster machine; see below.

## A faster machine

`ami.machine{ model = ..., opt = { ... } }` takes any core option; `--opt KEY=VALUE` on the command line overrides the
script. Keep the A500 model for Kickstart 1.3: the `A500PLUS` preset wants the 2.04 ROM, and `A1200` and later cannot
run 1.3. A script can also choose its CPU from its own flags, which the host forwards in `ami.argv`.

| Option | Values |
| --- | --- |
| `puae_cpu_model` | `auto`, `68000`, `68010`, `68020`, `68030`, `68040`, `68060` |
| `puae_cpu_compatibility` | `exact`: cycle-exact; `memory` (cycle-exact DMA and memory); `compatible`; `normal` |
| `puae_cpu_multiplier` | cycle-exact only: `2` 7 MHz (A500), `4` 14, `8` 28, `10` 35, `12` 42, `16` 57 MHz |
| `puae_cpu_throttle` | non-cycle-exact only: tenths of a percent, `1000.0` is +100% (2x), the menu stops at `10000.0` |

- **Throttle.** The core's cost per cycle is `base * 1000 / (1000 + T)`, floored; past about `63000` on a 68000 (`7000`
  for a 68020, `3000` for a 68040) more throttle changes nothing, and `-1000` or less is invalid. It costs host time
  in proportion: on one test the 68020 at +1000% managed 34 frames per second, below the 50 a PAL machine needs.
- **Cycle-exact multiplier.** Cheap by comparison: a cycle-exact 57 MHz CPU ran at over 200 frames per second on the
  same test, so cycle-exact is the way to a fast machine that stays real time. The multiplier is ignored outside
  cycle-exact modes, and the throttle inside them.
- **No JIT.** This core build is a pure interpreter; the UAE JIT sources are in the tree but not built.
- **Address space.** The A500 preset keeps a 24-bit address space even with a 68020 or later.

## Findings worth keeping

- **Hooks, not breakpoints.** The core's own breakpoints stop the whole machine, end the frame early and drop the
  audio generated up to the stop. Hooks are a callback from the instruction loop and stop nothing, so a hooked machine
  sounds and times like an unhooked one. Stopping is explicit (`ami.pause()`).
- **A program that closes Workbench needs one.** Kickstart 1.3's `intuition.library` initialises on its first
  `OpenLibrary` and leaves the display off until a screen opens; a program that expects a Workbench screen and never
  turns the display on itself therefore shows black. Open Intuition and call `OpenWorkBench` before starting it.
- **CLI startup.** Lattice C startup code reads the stack size from `4(a7)` and copies `cli_CommandName` into
  `argv[0]`; a Process needs a CLI structure with a non-empty name, or it copies 65,536 bytes below the stack.
- **Debugging a Lua-driven run.** `ami.custom(0x096)` gives the last DMACON write and the PC that made it (custom
  registers are write-only). Frame-boundary PC samples are useless: the vertical-blank interrupt (gameport polling
  around `$FE5A70`) is always what is running. `ami.step` and a halted machine show only a frozen state.
- **Audio.** `--verbose` prints how many sample frames the core delivered against the expected count.

## The WHDLoad route (reference)

To measure something against what WHDLoad does, run the slave itself: `content` = the `.slave`, `model = "A500PLUS"`
with the 2.04 ROM as host ROM (`kick37175.A500`, CRC32 `c3bdb240`) and the 1.3 ROM for the game, `opt = {
puae_fastmem_size = 8 }`. Notes from running it:

- WHDLoad's `S/Startup-Sequence` uses `FAILAT`, which a 1.3 shell lacks, hence the 2.04 host ROM.
- Use the real 1.3 image, not a Cloanto one: the core identifies Cloanto's `amiga-os-130.rom` (even decrypted) as the
  A3000 1.3 ROM, and slaves require `34005.a500`, 0x40000 bytes, CRC16 `0xf9e3`. With a `rom.key` in the system
  directory WHDLoad also expects every Kickstart there to be encrypted.
- 8 MB Fast RAM: 0.5 MB slow RAM alone made WHDLoad report "Can't allocate ExpMem".
- The host records the core's declared option defaults and answers `GET_VARIABLE` with them plus the script's
  overrides; without that the core fell back to AROS and the picture was shifted and cropped.
- The core writes `WHDLoad.prefs` and its backup into the system directory.
- This route is not the original execution: the slave changes timing, protection and pointers.

## Loader plugins and scripts

The plugin ABI is `ami9000/host/loader_api.h` (`AMI9000_LOADER_ABI` 2). A per-game shared object exports
`ami9000_loader()`; the host calls `before_frame` before every frame and `breakpoint` when the 68000 reaches a hook it
added (nothing is halted). Lua scripts use the same host API. No native plugin ships.

## Open items

- Verify the windowed run, audio and input on the Lua route.
- Optionally redefine `--scale N` as N times the 360-px lores screen (currently N/2 of the core's base width: 540x430
  at the default 3).
- Regenerate `patches/engine9000-v062-replay-snapshot.patch` with context lines so it applies to the pinned submodule
  revision.
