---@meta
-- Type stubs for the `ami` API that ami9000_host gives every script and module (lua-language-server only;
-- never loaded). Keep in step with tools/ami9000_host/lua_script.c and the README.

---@alias ami.Reg "d0"|"d1"|"d2"|"d3"|"d4"|"d5"|"d6"|"d7"|"a0"|"a1"|"a2"|"a3"|"a4"|"a5"|"a6"|"a7"|"sp"|"sr"|"pc"
---@alias ami.Regs table<ami.Reg, integer>

---@class ami.Machine
---@field name? string label used in error messages
---@field model? string puae_model, e.g. "A500"
---@field system? string Kickstart directory
---@field save? string writable save directory
---@field snapshots? string directory for machine snapshots (ami.snapshot_save/load, F11)
---@field content? string floppy image, WHDLoad .slave, or "@blank" (an empty disk)
---@field files? string host directory that ami.file_size/file_read/dir may read
---@field core? string path to ami9000.so
---@field opt? table<string, string|number> core options
---@field roms? { [1]: string, [2]: integer }[] Kickstart name in `system` and its CRC32
---@field plugin? string native loader plugin
---@field plugin_args? string
---@field scale? integer
---@field joystick_keys? boolean

---@class ami.DirEntry
---@field name string
---@field size integer
---@field dir boolean

---@class ami
---@field args string the --script-args text
---@field script_dir string
---@field argv string[] the command-line flags the host did not recognise (the script's own, e.g. "--max")
---@field keys table<string, integer> libretro key codes by name ("return", "a", "kp_multiply", "f1", ...)
ami = {}

-- Configuration phase (before ami.boot) ----------------------------------------------------------

---Describe the machine. Only before `ami.boot`.
---@param machine ami.Machine
function ami.machine(machine) end

---End configuration: the host checks the ROMs, loads the core and starts the content.
function ami.boot() end

---Load `name.lua` from the script's directory or tools/ami9000_host/lua, once, and return what it returns.
---@param name string e.g. "amiga/hunk"
---@return any
function ami.import(name) end

---@param ... any
function ami.log(...) end

-- Time -------------------------------------------------------------------------------------------

---Presented frames so far, one per emulated video frame (hooks do not cut a frame short).
---@return integer
function ami.frame() end

---Yield for `n` frames. Not available inside a hook.
---@param n? integer default 1
function ami.wait(n) end

---Yield until `fn()` is true. Returns false if `max` frames pass first.
---@param fn fun(): boolean
---@param max? integer default: never give up
---@return boolean
function ami.wait_until(fn, max) end

---Stop the host with an exit status.
---@param code? integer default 0
function ami.quit(code) end

-- Memory (guest bus, big-endian) -----------------------------------------------------------------

---@param address integer
---@return integer
function ami.peek8(address) end
---@param address integer
---@return integer
function ami.peek16(address) end
---@param address integer
---@return integer
function ami.peek32(address) end
---@param address integer
---@param value integer
function ami.poke8(address, value) end
---@param address integer
---@param value integer
function ami.poke16(address, value) end
---@param address integer
---@param value integer
function ami.poke32(address, value) end

---@param address integer
---@param length integer
---@return string
function ami.read(address, length) end

---Copy a string into guest memory.
---@param address integer
---@param bytes string
function ami.write(address, bytes) end

---Last value written to a custom chip register (offset from $DFF000, $096 is DMACON) and the PC that wrote it.
---@param offset integer
---@return integer value
---@return integer pc
function ami.custom(offset) end

-- CPU and hooks ----------------------------------------------------------------------------------

---@param name ami.Reg
---@return integer
function ami.reg(name) end

---@param name ami.Reg
---@param value integer
function ami.set_reg(name, value) end

---Run `fn()` when the 68000 reaches `address`, from inside the CPU loop. Nothing is stopped: the rest of the machine
---(chipset, audio, video) keeps running as it was. `fn` may use ami.call; if it changes `pc` the CPU continues there
---and the instruction at `address` is not executed. Hooks stay active after the script chunk returns.
---@param address integer
---@param fn fun()
---CPU cycles run since power on: the machine's own clock (a PAL A500 frame is 141,875).
---@return integer
function ami.cycles() end

---Start (true) or stop recording every custom register write; needs a core with the custom-write observer.
---@param on boolean
function ami.trace_writes(on) end

---The recorded writes as lines `cycles offset value pc copper`, then clear them.
---@return string
function ami.take_writes() end

function ami.hook(address, fn) end

---@param address integer
function ami.unhook(address) end

---Freeze the whole machine (silent) until `ami.resume`. Hooks never do this on their own; nothing else pauses it.
function ami.pause() end

function ami.resume() end

---@return boolean
function ami.paused() end

---From a hook: return from the subroutine the CPU stopped in (pop PC), optionally setting D0.
---@param d0? integer
function ami.rts(d0) end

---From a hook: run the guest routine at `address` (it must end with RTS) and return the registers it left.
---The CPU state is restored afterwards.
---@param address integer
---@param regs? ami.Regs registers to set first
---@return ami.Regs
function ami.call(address, regs) end

---Pause the machine and single-step it: `fn(pc)` runs before each instruction and returns true to keep going; when it
---does not, the machine resumes. Slow, and frames are cut short; for debugging.
---@param fn fun(pc: integer): boolean?
function ami.step(fn) end

-- Video and input --------------------------------------------------------------------------------

---@return integer
function ami.width() end
---@return integer
function ami.height() end

---Pixel of the last presented frame as 0xAARRGGBB (0 outside the frame).
---@param x integer
---@param y integer
---@return integer
function ami.pixel(x, y) end

---FNV-1a hash of the last presented frame, or of a rectangle of it.
---@param x? integer
---@param y? integer
---@param w? integer
---@param h? integer
---@return integer
function ami.frame_hash(x, y, w, h) end

---Write the last presented frame as a PPM.
---@param path string
function ami.screenshot(path) end

---Writes a host file, creating missing parent directories.
---@param path string
---@param bytes string
function ami.save_file(path, bytes) end

---Save the machine's whole state (the core's savestate). A name without "/" is a file in the machine's `snapshots`
---directory (default: the save directory); no name saves frame-N.state there, as F11 does. Not from a hook.
---@param path? string
---@return boolean
function ami.snapshot_save(path) end
---@param path? string
---@return boolean
function ami.snapshot_load(path) end

---@param key integer|string libretro key code, or a name from ami.keys
---@param down boolean
function ami.key(key, down) end

---@param dx integer
---@param dy integer
function ami.mouse_move(dx, dy) end

---@param button integer 0 left, 1 right
---@param down boolean
function ami.mouse_button(button, down) end

---Press joystick directions and the fire button on port 1 (what is not named is released; no argument releases all).
---@param state? { up?: boolean, down?: boolean, left?: boolean, right?: boolean, fire?: boolean }
function ami.joystick(state) end

-- Host files (read-only, under machine.files, case-insensitive) ------------------------------------

---@param path string
---@return integer? bytes, -1 for a directory, nil if absent
function ami.file_size(path) end

---@param path string
---@param offset? integer
---@param length? integer
---@return string? bytes nil if absent
function ami.file_read(path, offset, length) end

---@param path string "" is the root
---@return ami.DirEntry[]? entries sorted by name, nil if absent
function ami.dir(path) end
