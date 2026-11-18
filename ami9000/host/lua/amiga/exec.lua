-- Kickstart exec.library helpers. Everything that calls the guest (alloc, free, add_library,
-- ...) may only run inside an ami.hook function (see ami.call).
local exec = {}

exec.MEMF = { PUBLIC = 1, CHIP = 2, FAST = 4, CLEAR = 0x10000 }

local LVO = { AllocMem = -198, FreeMem = -210, AddLibrary = -396, FindName = -276, OpenLibrary = -552,
              Forbid = -132, Permit = -138, AddTask = -282 }
exec.LVO = LVO

function exec.base() return ami.peek32(4) end

function exec.alloc(size, flags)
    local base = exec.base()
    local r = ami.call(base + LVO.AllocMem, { d0 = size, d1 = flags or 0, a6 = base })
    assert(r.d0 ~= 0, ("AllocMem(%d, $%x) failed"):format(size, flags or 0))
    return r.d0
end

function exec.free(addr, size)
    local base = exec.base()
    ami.call(base + LVO.FreeMem, { a1 = addr, d0 = size, a6 = base })
end

function exec.add_library(lib)
    local base = exec.base()
    ami.call(base + LVO.AddLibrary, { a1 = lib, a6 = base })
end

-- C string at addr.
function exec.cstr(addr)
    local t = {}
    for i = 0, 255 do
        local b = ami.peek8(addr + i)
        if b == 0 then break end
        t[#t + 1] = string.char(b)
    end
    return table.concat(t)
end

-- create_process{name=, pri=, stack=, entry=, final=, cli=true} -> task address
-- Builds a Process (a Task plus the dos.library fields) and starts it with AddTask: it runs entry
-- in user mode on its own stack, and final when entry returns. entry gets no registers; hook it
-- (ami.hook) to set them up.
function exec.create_process(o)
    local PUBLIC_CLEAR = exec.MEMF.PUBLIC | exec.MEMF.CLEAR
    local stack_size = o.stack or 8192
    local stack = exec.alloc(stack_size, PUBLIC_CLEAR)
    local name = exec.alloc(#o.name + 1, exec.MEMF.PUBLIC)
    ami.write(name, o.name .. "\0")
    local task = exec.alloc(188, PUBLIC_CLEAR)
    ami.poke8(task + 8, 13)                    -- NT_PROCESS
    ami.poke8(task + 9, o.pri or 0)
    ami.poke32(task + 10, name)
    ami.poke32(task + 58, stack)               -- tc_SPLower
    -- the top longword stays zero above the final-PC slot: a CLI program finds its stack size
    -- at 4(a7) on entry, and the WHDLoad slave leaves that zero too
    local top = stack + stack_size - 4
    ami.poke32(task + 62, top)                 -- tc_SPUpper
    ami.poke32(task + 54, top)                 -- tc_SPReg
    ami.poke32(task + 74, task + 78)           -- tc_MemEntry: empty list
    ami.poke32(task + 82, task + 74)
    -- pr_MsgPort: a signal port belonging to the process, empty list
    ami.poke8(task + 92 + 8, 4)                -- NT_MSGPORT
    ami.poke32(task + 92 + 16, task)           -- mp_SigTask
    ami.poke32(task + 112, task + 116)
    ami.poke32(task + 120, task + 112)
    ami.poke32(task + 0x90, stack >> 2)        -- pr_StackBase
    ami.poke32(task + 0x84, stack_size)        -- pr_StackSize
    if o.cli ~= false then
        local cli = exec.alloc(64, PUBLIC_CLEAR)
        -- cli_CommandName: a BSTR (length byte, then the text). C startup code copies it into argv[0]
        -- and misbehaves with an empty one.
        local command = o.command or o.name
        local bstr = exec.alloc(#command + 2, PUBLIC_CLEAR)
        ami.write(bstr, string.char(#command) .. command)
        ami.poke32(cli + 16, bstr >> 2)
        ami.poke32(task + 0xac, cli >> 2)      -- pr_CLI: started from a shell, not Workbench
    end
    local base = exec.base()
    ami.call(base + LVO.AddTask, { a1 = task, a2 = o.entry, a3 = o.final or 0, a6 = base })
    return task
end

-- AttnFlags: 0 for a 68000, bit 0 68010, bit 1 68020, ...
function exec.attn_flags() return ami.peek16(exec.base() + 0x128) end

return exec
