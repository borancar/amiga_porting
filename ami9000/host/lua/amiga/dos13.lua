-- A Kickstart 1.3 dos.library replacement served from a host directory, in the way WHDLoad
-- replaces the disk: files are read through ami.file_*, nothing is ever written, and the DOS
-- functions a program calls are Lua hooks. Install from inside an ami.hook function.
--
--   local dos = ami.import("amiga/dos13")
--   local lib = dos.install{ assigns = { DF0 = "", F18 = "" }, trace = false }
--
-- assigns maps a device name (without ':') to a directory under machine.files; "" is the root.
local exec = ami.import("amiga/exec")
local hunk = ami.import("amiga/hunk")

local dos = {}

-- dos.library 1.3 vectors: LVO name by offset (-30 - 6 * n).
local FUNCTIONS = { "Open", "Close", "Read", "Write", "Input", "Output", "Seek", "DeleteFile", "Rename",
    "Lock", "UnLock", "DupLock", "Examine", "ExNext", "Info", "CreateDir", "CurrentDir", "IoErr",
    "CreateProc", "Exit", "LoadSeg", "UnLoadSeg", "GetPacket", "QueuePacket", "DeviceProc",
    "SetComment", "SetProtection", "DateStamp", "Delay", "WaitForChar", "ParentDir", "IsInteractive",
    "Execute" }

local ERROR = { OBJECT_NOT_FOUND = 205, OBJECT_WRONG_TYPE = 212, WRITE_PROTECTED = 214, NO_MORE_ENTRIES = 232,
                SEEK_ERROR = 219 }
local MODE_OLDFILE = 1005

local function reg(name) return ami.reg(name) end
local function signed(v) return v >= 0x80000000 and v - 0x100000000 or v end

-- State shared by the handlers.
local S = { ioerr = 0, files = {}, locks = {}, cwd = "" }

-- Resolve a DOS name to a path under machine.files, or nil for an unknown device.
local function resolve(name, assigns)
    local dev, rest = name:match("^([^:/]*):(.*)$")
    local dir
    if dev then
        if dev == "" then dir = ""
        else
            dir = assigns[dev] or assigns[dev:upper()]
            if not dir then return nil end
        end
    else
        dir, rest = S.cwd, name
    end
    local parts = {}
    for p in dir:gmatch("[^/]+") do parts[#parts + 1] = p end
    for p in rest:gmatch("[^/]*") do
        if p ~= "" then parts[#parts + 1] = p end
    end
    -- a leading '/' in a relative name means the parent directory
    for i = #parts, 1, -1 do
        if parts[i] == "" then table.remove(parts, i) end
    end
    return table.concat(parts, "/")
end

local function fill_cstr(addr, s, max)
    s = s:sub(1, max - 1)
    ami.write(addr, s .. "\0")
end

local function new_lock(path)
    local addr = exec.alloc(20, exec.MEMF.PUBLIC | exec.MEMF.CLEAR)
    local bptr = addr >> 2
    S.locks[bptr] = { path = path, index = 0 }
    return bptr
end

local function examine(lock, fib, first)
    local size = ami.file_size(lock.path)
    local name = lock.path:match("([^/]*)$")
    if first then
        -- Examine: describe the object itself
        ami.write(fib, string.rep("\0", 260))
        ami.poke32(fib + 4, size == -1 and 2 or -3 & 0xffffffff)
        fill_cstr(fib + 8, name == "" and "DF0" or name, 108)
        ami.poke32(fib + 120, size == -1 and 2 or -3 & 0xffffffff)
        ami.poke32(fib + 124, size == -1 and 0 or size)
        return true
    end
    local entries = lock.entries
    if not entries then entries = ami.dir(lock.path) or {}; lock.entries = entries end
    lock.index = lock.index + 1
    local e = entries[lock.index]
    if not e then S.ioerr = ERROR.NO_MORE_ENTRIES return false end
    ami.write(fib, string.rep("\0", 260))
    ami.poke32(fib + 4, e.dir and 2 or -3 & 0xffffffff)
    fill_cstr(fib + 8, e.name, 108)
    ami.poke32(fib + 120, e.dir and 2 or -3 & 0xffffffff)
    ami.poke32(fib + 124, e.size)
    return true
end

-- install{assigns=, trace=, on_open=} -> library base address; trace logs every call, on_open(path) is called for
-- every successful Open
function dos.install(opts)
    opts = opts or {}
    local assigns = opts.assigns or { DF0 = "" }
    local trace = opts.trace
    local on_open = opts.on_open -- optional: function(path) called for every successful Open
    local negsize = #FUNCTIONS * 6 + 30
    negsize = (negsize + 3) & ~3
    local possize = 64
    local name = exec.alloc(16, exec.MEMF.PUBLIC)
    ami.write(name, "dos.library\0")
    local mem = exec.alloc(negsize + possize, exec.MEMF.PUBLIC | exec.MEMF.CLEAR)
    local lib = mem + negsize
    -- trap addresses: one RTS word per vector
    local traps = exec.alloc(2 * (#FUNCTIONS + 8), exec.MEMF.PUBLIC)
    for i = 0, #FUNCTIONS + 7 do ami.poke16(traps + 2 * i, 0x4e75) end
    for k = 1, #FUNCTIONS + 4 do -- vectors at -6*k; the first four are the private slots
        local at = lib - 6 * k
        ami.poke16(at, 0x4ef9)
        ami.poke32(at + 2, traps + 2 * (k - 1))
    end
    ami.poke8(lib + 8, 9) -- NT_LIBRARY
    ami.poke32(lib + 10, name)
    ami.poke8(lib + 14, 6) -- LIBF_SUMUSED|LIBF_CHANGED: nothing checksums this one
    ami.poke16(lib + 16, negsize)
    ami.poke16(lib + 18, possize)
    ami.poke16(lib + 20, 34) -- version 34 (1.3)
    exec.add_library(lib)

    local handlers = {}

    handlers.Open = function()
        local path = resolve(exec.cstr(reg("d1")), assigns)
        local mode = reg("d2")
        if trace then ami.log("Open", exec.cstr(reg("d1")), mode, path, "frame", ami.frame()) end
        if mode ~= MODE_OLDFILE then S.ioerr = ERROR.WRITE_PROTECTED return 0 end
        local size = path and ami.file_size(path)
        if not size or size < 0 then S.ioerr = ERROR.OBJECT_NOT_FOUND return 0 end
        if on_open then on_open(path) end
        local fh = exec.alloc(44, exec.MEMF.PUBLIC | exec.MEMF.CLEAR)
        S.files[fh >> 2] = { path = path, size = size, pos = 0, data = ami.file_read(path) }
        return fh >> 2
    end
    handlers.Close = function()
        local f = S.files[reg("d1")]
        if f then S.files[reg("d1")] = nil exec.free(reg("d1") << 2, 44) end
        return 1
    end
    handlers.Read = function()
        local f = S.files[reg("d1")]
        if not f then S.ioerr = ERROR.OBJECT_WRONG_TYPE return 0xffffffff end
        local n = math.min(reg("d3"), f.size - f.pos)
        if n > 0 then ami.write(reg("d2"), f.data:sub(f.pos + 1, f.pos + n)) end
        f.pos = f.pos + math.max(n, 0)
        return math.max(n, 0)
    end
    handlers.Write = function()
        if S.files[reg("d1")] then S.ioerr = ERROR.WRITE_PROTECTED return 0xffffffff end
        return reg("d3") -- stdout and friends: swallow
    end
    handlers.Input = function() return 0 end
    handlers.Output = function() return 0 end
    handlers.Seek = function()
        local f = S.files[reg("d1")]
        if not f then S.ioerr = ERROR.OBJECT_WRONG_TYPE return 0xffffffff end
        local old, off, mode = f.pos, signed(reg("d2")), signed(reg("d3"))
        local pos = mode == -1 and off or mode == 0 and f.pos + off or f.size + off
        if pos < 0 or pos > f.size then S.ioerr = ERROR.SEEK_ERROR return 0xffffffff end
        f.pos = pos
        return old
    end
    handlers.DeleteFile = function() S.ioerr = ERROR.WRITE_PROTECTED return 0 end
    handlers.Rename = handlers.DeleteFile
    handlers.CreateDir = function() S.ioerr = ERROR.WRITE_PROTECTED return 0 end
    handlers.Lock = function()
        local path = resolve(exec.cstr(reg("d1")), assigns)
        if trace then ami.log("Lock", exec.cstr(reg("d1")), reg("d2"), path) end
        if not path or not ami.file_size(path) then S.ioerr = ERROR.OBJECT_NOT_FOUND return 0 end
        return new_lock(path)
    end
    handlers.UnLock = function()
        if S.locks[reg("d1")] then S.locks[reg("d1")] = nil exec.free(reg("d1") << 2, 20) end
        return 0
    end
    handlers.DupLock = function()
        local l = S.locks[reg("d1")]
        return l and new_lock(l.path) or 0
    end
    handlers.ParentDir = function()
        local l = S.locks[reg("d1")]
        if not l or l.path == "" then return 0 end
        return new_lock(l.path:match("^(.*)/[^/]*$") or "")
    end
    handlers.Examine = function()
        local l = S.locks[reg("d1")]
        return l and examine(l, reg("d2"), true) and 0xffffffff or 0
    end
    handlers.ExNext = function()
        local l = S.locks[reg("d1")]
        return l and examine(l, reg("d2"), false) and 0xffffffff or 0
    end
    handlers.CurrentDir = function()
        local old = S.cwd_lock or 0
        S.cwd_lock = reg("d1")
        local l = S.locks[reg("d1")]
        S.cwd = l and l.path or ""
        return old
    end
    handlers.IoErr = function() local e = S.ioerr S.ioerr = 0 return e end
    handlers.LoadSeg = function()
        local path = resolve(exec.cstr(reg("d1")), assigns)
        local data = path and ami.file_read(path)
        if not data then S.ioerr = ERROR.OBJECT_NOT_FOUND return 0 end
        local seg = hunk.load(hunk.parse(data), function(n, flags)
            return exec.alloc(n, (flags == 1 and exec.MEMF.CHIP or flags == 2 and exec.MEMF.FAST or exec.MEMF.PUBLIC)
                | exec.MEMF.CLEAR)
        end)
        return seg.seglist
    end
    handlers.UnLoadSeg = function() return 1 end
    handlers.DeviceProc = function() return 0 end
    handlers.DateStamp = function()
        local ds = reg("d1")
        ami.poke32(ds, 0) ami.poke32(ds + 4, 0) ami.poke32(ds + 8, 0)
        return ds
    end
    handlers.Delay = function() return 0 end
    handlers.WaitForChar = function() return 0 end
    handlers.IsInteractive = function() return 0 end
    handlers.Info = function() S.ioerr = ERROR.OBJECT_NOT_FOUND return 0 end
    handlers.SetComment = function() return 0 end
    handlers.SetProtection = function() return 0 end

    for i, fname in ipairs(FUNCTIONS) do
        local addr = traps + 2 * (i + 3) -- vectors are numbered from the first public one, after the 4 private slots
        local h = handlers[fname]
        ami.hook(addr, function()
            local result
            if trace then ami.log(("dos.%s d1=%x d2=%x d3=%x ret=%06x"):format(fname, reg("d1"), reg("d2"), reg("d3"), ami.peek32(reg("sp")))) end
            if h then
                result = h()
            else
                ami.log("dos.library: unimplemented " .. fname)
                result = 0
            end
            ami.rts(result)
        end)
    end
    return lib
end

return dos
