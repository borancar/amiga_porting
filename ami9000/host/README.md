# ami9000_host

SDL3 libretro host for Engine9000's `ami9000.so` (PUAE). No debugger needed.

    ami9000/host/build_core.sh      # once: the core with the host patches (see below)
    make -C ami9000/host            # -> ami9000/build/ami9000_host
    ami9000/build/ami9000_host projects/<id>/host/<name>.lua

Libraries: SDL3 and the system Lua 5.4 (`liblua`).

## Game scripts

Each game keeps its whole setup in one Lua script under `projects/<id>/host/`.
It describes the machine, boots it, then optionally drives it:

```lua
ami.machine{
    name = "...", model = "A500",                  -- model = puae_model
    system = "../local/system", save = "../local/save",
    content = "../local/game.adf",                 -- floppy image, WHDLoad .slave, or "@blank"
    files = "../local/data",                       -- host directory ami.file_*/ami.dir may read
    opt = { puae_fastmem_size = 8 },               -- any core option
    roms = { { "kick34005.A500", 0xc4f0f55f } },   -- Kickstart in `system` with that CRC32
    -- also: core (path to ami9000.so), plugin / plugin_args (native plugin), scale, joystick_keys
}
ami.boot()
```

Relative paths resolve against the script (`ami.script_dir`); unknown fields are
errors. Before `ami.boot()` only `ami.machine`, `ami.boot`, `ami.import`, `ami.log` and the
values `ami.args`, `ami.argv`, `ami.script_dir` and `ami.keys` exist; machine functions raise an error. `ami.boot()` checks the ROM
CRCs (a missing or wrong ROM stops the launch naming the file), loads the core
and starts the content. Command-line flags given as well override the script.
Kickstarts are never shipped. A script that ends without calling `ami.boot()` is
an error. Without a script, `ami9000_host game.adf` runs an image with defaults.

`content = "@blank"` is an empty floppy (written to `save/blank.adf`): the machine
boots to "insert disk" and a script takes over. Example, running a game from a host
directory with only the Kickstart: `projects/fa18-interceptor/host/fa18.lua`.

Command-line flags the host does not recognise are passed to the script in `ami.argv` (a list of strings), so a
script defines its own, such as `fa18.lua --max`. `--flag=value` carries a value; everything after a bare `--`
is the script's too. The script decides what they mean, including the machine: it builds the `opt` table of
`ami.machine{}` from them, e.g. `puae_cpu_compatibility = "exact"` with `puae_cpu_multiplier = 16` for a cycle-exact
56.75 MHz CPU (the non-exact throttle modes cost far more host time and fall below real time). An explicit
`--opt KEY=VALUE` still overrides the script.

Window: click grabs the mouse, Ctrl+Alt releases, Alt+Enter fullscreen, F12 quits.

## Driving the machine from Lua

After `ami.boot()` the script runs as a coroutine resumed once per core frame,
so it reads linearly. It has no `io`, `os`, `debug` or file loading, so it can
only act through `ami`.

| `ami.` | meaning |
| --- | --- |
| `frame()`, `width()`, `height()` | presented frames so far; size of the last presented frame |
| `wait(n)` | yield for `n` frames (default 1) |
| `wait_until(fn, max)` | yield until `fn()` is true; `false` after `max` frames (default: never) |
| `peek8/16/32(a)`, `poke8/16/32(a, v)`, `read(a, n)`, `write(a, bytes)` | guest bus memory, big-endian; `read` returns a string |
| `cycles()` | CPU cycles run since power on: the machine's own clock, for timing code between hooks |
| `pause()`, `resume()`, `paused()` | freeze the whole machine (silent) until resumed; nothing else pauses it |
| `reg(name)`, `set_reg(name, v)` | `d0`-`d7`, `a0`-`a7` (`sp`), `sr`, `pc` |
| `custom(offset)` | last value written to a custom register (`$096` is DMACON) and the PC that wrote it |
| `pixel(x, y)`, `frame_hash([x, y, w, h])`, `screenshot(path)` | last frame as `0xAARRGGBB`; FNV-1a hash; PPM |
| `disk_count()`, `disk_index()`, `disk_eject(bool)`, `disk_select(i)` | swap floppies of an `.m3u` content (first image in DF0; the core's disk control): `disk_eject(true); wait(20); disk_select(1); disk_eject(false)`. In the window **PageDown** does that for the next image. nil without disk control |
| `snapshot_save([name])`, `snapshot_load(name)` | the machine's whole state (the core's savestate) to or from a file; a name without `/` is a file in the machine's `snapshots` directory (`--snapshots DIR`; default the save directory), and `snapshot_save()` with no name writes `frame-N.state`. Returns whether it worked. In the window **F11 captures** `frame-N.state` the same way (between frames); restoring is the script's job. Restoring does not rewind a lockstep peer |
| `save_file(path, bytes)` | writes a host file, creating parent directories (memory snapshots: `save_file(p, ami.read(0, 0x80000))`) |
| `key(k, down)` | `k` is a libretro key code or a name from `ami.keys` (`"return"`, `"a"`, `"kp_multiply"`, `"f1"`...) |
| `mouse_move(dx, dy)`, `mouse_button(b, down)` | injected mouse input |
| `file_size(p)`, `file_read(p [, off [, n]])`, `dir(p)` | read-only host files under `machine.files`, case-insensitive; `nil` if absent |
| `import(name)` | load `name.lua` from the script's directory or `ami9000/host/lua` once |
| `log(...)`, `quit([code])` | stderr log; stop the host with an exit status |
| `args`, `argv` | the `--script-args` text; the list of command-line flags the host did not recognise |

`ami.frame()` counts presented frames, one per emulated video frame. Hooks (below) do not cut a frame short,
so they do not change it; pausing the machine and `ami.step` do.

A script error or a failed load prints the Lua message and exits with status 3.

## Lockstep: a native port against the emulator

Run a native port and the original side by side, frame-locked and fed the same input, and stop on the first visual
difference. `ami.machine{ peers = { name = { program, args... } } }` declares a process the script may start with
`ami.peer(name)` (`send`, `readline`, `read`, `close` over pipes); `ami.input()` returns the keyboard and mouse events the
host processed; `ami.panel(slot, rgb, w, h ...)` shows images right of the frame (redrawn while paused);
`ami.frame_rgb` extracts an image of the frame and `ami.rgb_diff(a, b)` returns the count, the first index and a
difference image. `lua/amiga/lockstep.lua` is the driver and documents the peer protocol: each emulated frame it
forwards input, sends `step`, reads the peer's image and compares it exactly; on the first difference it calls
`ami.pause()` (silent, frozen) and stops stepping the peer. Options: `start` (the frame the peer's initial state
belongs to), `window` (where the peer's image sits in the emulator frame), `keymap`, `schedule` (lines for the peer
before step N, for what the emulated side does by itself), `on_start`, `on_frame` (a true result stops there), `ignore` (regions left out of the comparison), `on_mismatch`, `continue_for` (N frames past the first difference, or -1 to never stop); a `schedule` entry may be a function (it may call `ami.key` and return the peer's lines).

Text mode (`text = { emulated = fn, idle = 45 }`) matches the two sides at the start and at the end of each burst of text
and compares only then, so how fast each side types does not matter. The peer's `frame` line lists the screen rows of
the glyphs it wrote since its previous frame (`GLYPHS`, comma-separated or `-`) and `text.emulated()` returns the same
for the emulated machine. A burst (a line or several) begins with the first glyph after a quiet spell and ends when no
glyph has been written for `idle` frames. Each side keeps its screen as it was the frame before the burst's first
glyph (start) and on the frame of its last glyph (end); the Nth start is compared with the Nth start and the Nth end
with the Nth end, whole frames. Every burst logs the glyph count and rows of each side, the timing of its start and end
and the skew between the sides, and whether the start and end matched. A difference stops the run unless
`continue_for` is -1. In `fa18.lua` add `text` (and `idle=N`) to `--script-args`; `dump=PATH` also writes, for every step, the number of glyphs each side wrote (step, native tick, native glyphs, emulated glyphs), for fitting typing speeds. Pick `idle` longer than the longest
pause either side makes inside one piece of text, or the sides split it into different bursts and stop pairing up.

A game script wires it up (`projects/fa18-interceptor/host/fa18.lua --lockstep`). A peer implements the protocol in its
own `--lockstep` mode (stdin `key`, `mouse`, `button`, `step`, `image`, `quit`; one `frame` line plus RGB per reply).
Use the machine the peer models (not a sped-up one); see the control socket below for debugging a stop.

## A control socket for debugging

`--control PATH` serves a Unix socket (mode 0600) at PATH. A client sends one Lua line at a time and gets `ok LEN\n` or
`err LEN\n` plus LEN bytes back. A line runs in the script's own state, so its globals, `ami.import`ed modules and every
`ami.*` call that does not wait work, between frames and also while the machine is paused (the host keeps looping, a
headless one sleeps 2 ms a turn). A line is tried as an expression first (values are printed, tables with
`ami.inspect`), then as a statement; `print` output comes back too. `ami.wait`, `ami.wait_until` and `ami.call` need
a coroutine and do not work from here; use `ami.pause()` and `ami.resume()` and read state.

```
ami9000/host/ctl.py PATH 'ami.reg("pc")' 'ami.paused()' 'ami.import("amiga/lockstep").report.different'
```

Lockstep keeps `lockstep.report` (the first difference) and, with `continue_for = N`, `lockstep.history` (every frame
from the first difference, with both images) for this.

## Hooks: a Lua body at an instruction

`ami.hook(addr, fn)` runs `fn()` when the 68000 reaches `addr`, from inside the core's instruction loop. It stops
nothing: chipset, audio and video carry on exactly as without it, frames are not cut short, and a hooked machine
sounds and times like an unhooked one. Hooks stay active after the script chunk returns, and `ami.unhook(addr)`
removes one. In a hook, `ami.reg`/`ami.set_reg` read and change the CPU, so a hook can replace a routine
(`ami.rts(d0)` pops the return address and sets D0) or redirect code by setting `pc`; the instruction at `addr` is
then not executed. Without changing `pc` it runs as usual after the hook.

A hook runs as a coroutine, so it can call guest routines and continue when they return:
`ami.call(addr, { d0 = ..., a6 = ... })` runs the routine at `addr` (it must end with `RTS`) and returns a table of
the registers it left; the CPU state is restored afterwards. This is how `amiga/exec.lua` calls `AllocMem` and
`AddTask` from Lua. Several calls can be outstanding at once (up to 8): a call made in one task may be preempted by
another task whose hook calls too, and each call returns to its own trap address.

Stopping the machine is separate and explicit: `ami.pause()` freezes it (silent, nothing runs) until
`ami.resume()`. To halt at an address while debugging, pause from the hook:
`ami.hook(addr, function() ami.pause() end)`; the hook does not fire again at that PC when it resumes. `ami.step(fn)`
pauses and single-steps, calling `fn(pc)` before each instruction until it returns something other than `true`,
then resumes (slow, and the frames are cut short: for debugging).

Limits: a hook is skipped once at the PC a paused machine resumes at; hooks are per address, not per task, and `ami.hook(a, f)` replaces any hook already at `a`. While the machine free-runs the reported address is sometimes two bytes before the instruction (and extension words report too): for an exact trace use `ami.step`, whose callback gets the true address of each instruction (`host/trace_steps.lua` of a port records counts, frames and successors; about 30,000 steps a second). A script's main chunk ending does not stop the host: finish with `ami.quit()`.

## Shared Lua modules (`lua/`)

Reusable across games; `ami.import("amiga/hunk")` and so on. Everything that touches the
guest (`alloc`, `call`) must run inside a hook.

| module | content |
| --- | --- |
| `amiga/hunk` | `parse(data)` an AmigaDOS hunk executable; `load(exe, alloc)` places it like `LoadSeg` (relocations, seglist) |
| `amiga/exec` | `alloc`/`free`, `add_library`, `create_process{...}` (a Process started with `AddTask`, with a CLI structure), `cstr` |
| `amiga/dos13` | a Kickstart 1.3 `dos.library` served from `machine.files`: `Open`, `Read`, `Seek`, `Lock`, `Examine`, `LoadSeg`, ... Read-only; writes fail with "write protected" |

## The core

`ami9000.so` is Engine9000's PUAE core. Register writes need an export the pinned core lacks
(`e9k_debug_write_reg`, `patches/ami9000-write-reg.patch`), and hooks are a callback from its instruction loop
(`e9k_debug_add_hook` and friends, `patches/ami9000-hooks.patch`). Its own breakpoints stop the whole machine, and
drop the audio generated up to each stop, so they are not used for hooks.
`build_core.sh` applies the patches listed in `patches/series` to a copy under `build/e9k` (the
pinned `ami9000/engine9000-src` is not touched), builds it (about a minute) and installs
`ami9000/build/system/ami9000.so`.

## Editing the Lua

Editor settings live at the repository root and apply to every `.lua` file:

- `.stylua.toml`: stylua (what `conform.nvim` runs on save): 4 spaces, 120 columns, double quotes, calls
  keep their parentheses except a single table argument (`ami.machine{ ... }`), and simple one-statement
  `if` and `function` bodies stay on one line.
- `.editorconfig`: the same indentation and width for nvim's built-in editorconfig support and for
  lua-language-server's formatter.
- `.luarc.json`: lua-language-server for Lua 5.4 with `io`, `os`, `debug` and `package` disabled, because the
  script sandbox has none of them (an `io.open` is flagged as an undefined global). It loads
  `lua/meta/ami.lua`, the type stubs for the `ami` API. Add a function there when the host gains one.

Format with `stylua ami9000/host/lua projects/*/host`.
