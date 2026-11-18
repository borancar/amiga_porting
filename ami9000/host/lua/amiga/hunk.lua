-- AmigaDOS hunk executables: parse, and place into guest memory like LoadSeg.
local hunk = {}

local HUNK_UNIT, HUNK_NAME, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3e7, 0x3e8, 0x3e9, 0x3ea, 0x3eb
local HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END, HUNK_HEADER = 0x3ec, 0x3f0, 0x3f1, 0x3f2, 0x3f3
local HUNK_DREL32, HUNK_RELOC32SHORT = 0x3f7, 0x3fc

-- parse(data) -> { hunks = { {kind="code"|"data"|"bss", size=bytes, memflags=0|1|2, data=string|nil,
--                            relocs={ {hunk=n, offsets={...}} } } } }
-- memflags: 0 any, 1 chip, 2 fast (the top bits of the hunk size in the header).
function hunk.parse(data)
    local p = 1
    local function u32()
        local v = string.unpack(">I4", data, p)
        p = p + 4
        return v
    end
    assert(u32() == HUNK_HEADER, "not a hunk executable")
    while true do -- resident library names (unused)
        local n = u32()
        if n == 0 then break end
        p = p + 4 * n
    end
    local table_size, first, last = u32(), u32(), u32()
    local hunks = {}
    for i = first, last do
        local v = u32()
        local flags = v >> 30
        if flags == 3 then u32() end -- extended memory attributes follow
        hunks[i + 1] = { size = (v & 0x3fffffff) * 4, memflags = flags, relocs = {} }
    end
    local index = first
    while p <= #data do
        local t = u32() & 0x3fffffff
        local h = hunks[index + 1]
        if t == HUNK_CODE or t == HUNK_DATA then
            local n = (u32() & 0x3fffffff) * 4
            h.kind = t == HUNK_CODE and "code" or "data"
            h.data = data:sub(p, p + n - 1)
            p = p + n
        elseif t == HUNK_BSS then
            u32()
            h.kind = "bss"
        elseif t == HUNK_RELOC32 then
            while true do
                local count = u32()
                if count == 0 then break end
                local target = u32()
                local offs = {}
                for i = 1, count do offs[i] = u32() end
                h.relocs[#h.relocs + 1] = { hunk = target, offsets = offs }
            end
        elseif t == HUNK_SYMBOL then
            while true do
                local n = u32()
                if n == 0 then break end
                p = p + 4 * (n & 0xffffff) + 4
            end
        elseif t == HUNK_DEBUG then
            p = p + 4 * u32()
        elseif t == HUNK_END then
            index = index + 1
        elseif t == HUNK_NAME or t == HUNK_UNIT then
            p = p + 4 * u32()
        else
            error(("unsupported hunk type $%x at offset %d"):format(t, p - 5))
        end
    end
    return { hunks = hunks }
end

-- load(exe, alloc) -> { base = {hunk base addresses (first code/data byte)}, seglist = BPTR,
--                        entry = address of the first hunk }
-- Places each hunk like LoadSeg: a block [size][next BPTR] followed by the hunk, links the
-- blocks into a seglist and applies the relocations. alloc(bytes, memflags) returns the
-- address of guest memory (memflags 0 any, 1 chip, 2 fast; zero filled).
function hunk.load(exe, alloc)
    local base, block = {}, {}
    for i, h in ipairs(exe.hunks) do
        block[i] = alloc(h.size + 8, h.memflags)
        base[i] = block[i] + 8
    end
    for i, h in ipairs(exe.hunks) do
        ami.poke32(block[i], h.size + 8)
        ami.poke32(block[i] + 4, exe.hunks[i + 1] and (block[i + 1] + 4) >> 2 or 0)
        if h.data and #h.data > 0 then ami.write(base[i], h.data) end
    end
    for i, h in ipairs(exe.hunks) do
        for _, r in ipairs(h.relocs) do
            local target = base[r.hunk + 1]
            for _, off in ipairs(r.offsets) do
                local at = base[i] + off
                ami.poke32(at, ami.peek32(at) + target)
            end
        end
    end
    return { base = base, seglist = (block[1] + 4) >> 2, entry = base[1] }
end

return hunk
