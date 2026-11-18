-- Lockstep: drive a native port (a peer process) one step per emulated video frame, with the same input, and compare
-- what each shows. On the first difference the emulator is paused (silent, frozen) and the peer is not stepped again,
-- so both can be inspected.
--
--   local lockstep = ami.import("amiga/lockstep")
--   lockstep.run{
--       peer = ami.peer("native"),            -- speaks the protocol below
--       width = 320, height = 256,            -- the native image
--       window = { x = 40, y = 16, sx = 2 },  -- where it sits in the emulator frame, sx columns per pixel
--       start = function() return ... end,    -- true on the emulator frame the peer's initial state belongs to
--       keymap = function(key) return key end -- libretro key -> the peer's key code (nil to drop it)
--       on_frame = function(count) end,       -- optional: called after every comparison (count: differing pixels, then the compared native and emulated images); a
--                                             -- true result stops here (pauses the emulator, peer no longer stepped)
--       ignore = { { x, y, w, h, screen = "menu" }, ... }, -- optional: image regions left out of the comparison (both
--                                             -- sides blanked), only while the peer reports that screen name if given;
--                                             -- the panels still show the real images
--       on_start = function() end,            -- optional: called on that frame, before the first comparison
--       continue_for = 0,                     -- optional: keep stepping this many frames past the first difference, or
--                                             -- -1 to never stop (differences are logged as they start and stop, with a
--                                             -- summary every 250 frames); lockstep.history holds the native/emulated
--                                             -- images and count of those frames (the last 64 when never stopping)
--       fast_forward = { steps = 1690, schedule = { [755] = "key 32 1\n" } },  -- optional, when the emulator was restored
--                                             -- from a snapshot: step the peer this many times first (its lines per
--                                             -- step as in `schedule`, strings only) so both are at the same state
--       text = { emulated = function() return { 20, 20, 28 } end, idle = 45 },
--                                             -- optional: compare at the start and end of each burst of text, not
--                                             -- every frame (see "Text mode" below)
--       schedule = { [755] = "key 13 1\nkey 13 0\n" }  -- optional: lines for the peer before its Nth step (N counts
--                                                      -- from the first step), for what the emulated side does by itself;
--                                                      -- a function runs instead and may return those lines (and ami.key)
--   }
--
-- The peer protocol (stdin/stdout, see fa18_native --lockstep): `key CODE DOWN`, `mouse DX DY`, `button B DOWN` and
-- `step` (one video frame) or `image` (the current state) on lines; every `step` and `image` is answered by the line
-- `frame TICKS WIDTH HEIGHT NAME GLYPHS` and WIDTH*HEIGHT*3 bytes of RGB. GLYPHS lists the screen rows of the glyphs
-- the peer wrote since its previous frame, comma-separated and in order, or "-".
--
-- Text mode (`text`): instead of comparing every frame, the two sides are matched at the start and at the end of each
-- burst of text. A burst begins with the first glyph written after a quiet spell and ends when no glyph has been written
-- for `idle` frames (default 45): it may hold a line or several. Each side keeps its screen as it was the frame before
-- the burst's first glyph (the start) and on the frame of its last glyph (the end); the Nth start is compared with the
-- Nth start and the Nth end with the Nth end, whole frames, as soon as both sides have them. Nothing in between is
-- compared, so how fast each side types does not matter; each burst logs its glyph count, rows and timing. 
-- `text.emulated()` is called once per frame and returns the rows of the glyphs the emulated machine wrote during that
-- frame. A difference stops the run unless `continue_for` is -1.
local lockstep = {}

local function read_frame(peer, width, height)
    local header = peer:readline()
    if not header then return nil, "the peer closed its output" end
    local ticks, w, h, name, glyphs = header:match("^frame (%d+) (%d+) (%d+) (%S+) (%S+)")
    if not ticks then ticks, w, h, name = header:match("^frame (%d+) (%d+) (%d+) (%S+)") end
    if not ticks then return nil, ("unexpected reply from the peer: %q"):format(header) end
    assert(tonumber(w) == width and tonumber(h) == height, "the peer's image is not the configured size")
    local rgb = peer:read(width * height * 3)
    if not rgb then return nil, "the peer closed its output in an image" end
    local rows = {}
    for row in (glyphs or ""):gmatch("[%d.]+") do rows[#rows + 1] = tonumber(row) end
    return rgb, tonumber(ticks), name, rows
end

-- `img` (RGB, `w` pixels wide) with each region {x, y, w, h} set to black.
local function blank(img, w, regions, screen)
    local parts = {}
    for _, r in ipairs(regions) do
        if not r.screen or r.screen == screen then
            for y = r[2], r[2] + r[4] - 1 do
                local from = (y * w + r[1]) * 3
                parts[#parts + 1] = { from + 1, from + r[3] * 3, string.rep("\0", r[3] * 3) }
            end
        end
    end
    table.sort(parts, function(a, b) return a[1] < b[1] end)
    local out, at = {}, 1
    for _, p in ipairs(parts) do
        out[#out + 1] = img:sub(at, p[1] - 1)
        out[#out + 1] = p[3]
        at = p[2] + 1
    end
    out[#out + 1] = img:sub(at)
    return table.concat(out)
end

local function forward_input(peer, keymap)
    for _, e in ipairs(ami.input()) do
        if e[1] == "k" then
            local key = keymap(e[2])
            if key then peer:send(("key %d %d\n"):format(key, e[3])) end
        elseif e[1] == "m" then
            peer:send(("mouse %d %d\n"):format(e[2], e[3]))
        else
            peer:send(("button %d %d\n"):format(e[2], e[3]))
        end
    end
end

-- Letters are upper case in the native ports' key codes, everything else is the libretro code.
function lockstep.upper_case_letters(key)
    if key >= 97 and key <= 122 then return key - 32 end
    return key
end

-- Follows the glyph rows one side wrote, frame by frame, and collects its bursts of text: side.bursts[n] = { start,
-- finish, glyphs, lo, hi } where start and finish are { image, shown, frame, tick } (see "Text mode").
local function feed(side, rows, now, idle)
    if #rows > 0 then
        local burst = side.burst
        if not burst then
            burst = { start = side.previous or now, glyphs = 0, lo = math.huge, hi = -math.huge }
            side.burst = burst
        end
        for _, row in ipairs(rows) do
            local line = math.floor(row)
            burst.glyphs, burst.lo, burst.hi = burst.glyphs + 1, math.min(burst.lo, line), math.max(burst.hi, line)
        end
        side.idle, side.active = 0, now
    else
        side.idle = side.idle + 1
        if side.burst and side.idle >= idle then
            side.burst.finish = side.active
            side.bursts[#side.bursts + 1] = side.burst
            side.burst = nil
        end
    end
    side.previous = now
end

-- Runs until the first difference (then pauses the emulator and returns the report) or until the peer ends.
function lockstep.run(cfg)
    local peer, w, h, win = cfg.peer, cfg.width, cfg.height, cfg.window
    lockstep.peer = peer -- for a control socket: peer:send(...) while paused
    local keymap = cfg.keymap or lockstep.upper_case_letters
    local panels = cfg.panels ~= false

    while not cfg.start() do ami.wait(1) end
    ami.log(("lockstep starts at emulator frame %d"):format(ami.frame()))
    ami.input() -- what happened before the start is not the peer's
    if cfg.on_start then cfg.on_start() end

    local ff = cfg.fast_forward
    if ff then
        ami.log(("lockstep: stepping the peer %d times to reach the emulator's state"):format(ff.steps))
        for step = 1, ff.steps do
            if ff.schedule and ff.schedule[step] then peer:send(ff.schedule[step]) end
            peer:send("step\n")
            assert(read_frame(peer, w, h))
        end
    end
    local text = cfg.text
    local idle = text and text.idle or 45
    if text then
        text.sides = { native = { bursts = {}, idle = 0 }, emulated = { bursts = {}, idle = 0 } }
        text.compared, text.different = 0, 0
    end
    local first = true
    local compared = 0
    while true do
        if first then
            peer:send("image\n")
        else
            forward_input(peer, keymap)
            local extra = cfg.schedule and cfg.schedule[compared]
            if type(extra) == "function" then extra = extra() end -- may act on the emulator (ami.key) and return lines
            if extra then peer:send(extra) end
            peer:send("step\n")
        end
        local native, ticks, name, glyph_rows = read_frame(peer, w, h)
        if not native then
            ami.log("lockstep ended: " .. ticks)
            return { ended = true }
        end
        local emulated = ami.frame_rgb(win.x, win.y, w, h, win.sx)
        local shown = native
        if cfg.ignore then
            native, emulated = blank(native, w, cfg.ignore, name), blank(emulated, w, cfg.ignore, name)
        end
        if text then
            compared = compared + 1 -- the step counter that `schedule` follows
            text.frames = (text.frames or 0) + 1
            local stamp = function(image) return { image = image, frame = ami.frame(), tick = ticks } end
            feed(text.sides.native, glyph_rows, { image = native, shown = shown, frame = ami.frame(), tick = ticks }, idle)
            local erows = text.emulated()
            if text.dump then
                text.log = text.log or {}
                text.log[#text.log + 1] = ("%d %d %s %s"):format(text.frames, ticks, #glyph_rows > 0 and table.concat(glyph_rows, ",") or "-", #erows > 0 and table.concat(erows, ",") or "-")
                if text.frames % 200 == 0 then ami.save_file(text.dump, table.concat(text.log, "\n")) end
            end
            feed(text.sides.emulated, erows, stamp(emulated), idle)
            text.base = text.base or { frame = ami.frame(), tick = ticks }
            local ours, theirs = text.sides.native.bursts, text.sides.emulated.bursts
            while text.compared < math.min(#ours, #theirs) do
                text.compared = text.compared + 1
                local n, e = ours[text.compared], theirs[text.compared]
                local function at(side, point) return (side[point].tick or side[point].frame) end
                local start_diffs, start_at = ami.rgb_diff(n.start.image, e.start.image)
                local end_diffs, end_at, end_picture = ami.rgb_diff(n.finish.image, e.finish.image)
                local line = ("burst %d: native glyphs %d rows %d-%d, ticks %d-%d (+%d); emulator glyphs %d rows %d-%d, frames %d-%d (+%d);"
                    .. " skew start %+d end %+d"):format(
                    text.compared, n.glyphs, n.lo, n.hi, n.start.tick, n.finish.tick, n.start.tick - text.base.tick,
                    e.glyphs, e.lo, e.hi, e.start.frame, e.finish.frame, e.start.frame - text.base.frame,
                    (n.start.tick - text.base.tick) - (e.start.frame - text.base.frame),
                    (n.finish.tick - text.base.tick) - (e.finish.frame - text.base.frame))
                if panels then
                    ami.panel(0, n.finish.shown or n.finish.image, w, h, win.sx, 1, win.y)
                    ami.panel(1, end_picture, w, h, win.sx, 1, win.y)
                end
                local function where(index) return ("(%d,%d)"):format((index - 1) % w, (index - 1) // w) end
                if start_diffs > 0 or end_diffs > 0 then
                    ami.log(("%s: start %s, end %s"):format(line,
                        start_diffs > 0 and ("%d pixels different, first at %s"):format(start_diffs, where(start_at)) or "equal",
                        end_diffs > 0 and ("%d pixels different, first at %s"):format(end_diffs, where(end_at)) or "equal"))
                    text.different = text.different + 1
                    lockstep.report = { burst = text.compared, start = { native = n.start.image, emulated = e.start.image,
                        different = start_diffs }, finish = { native = n.finish.image, emulated = e.finish.image,
                        different = end_diffs }, frame = ami.frame(), ticks = ticks, screen = name }
                    if cfg.continue_for ~= -1 then
                        ami.log("lockstep stopped at the first differing burst")
                        ami.pause()
                        return lockstep.report
                    end
                else
                    ami.log(line .. ": start and end equal")
                end
            end
            if text.stop_after and text.compared >= text.stop_after then
                ami.log(("lockstep paused after burst %d"):format(text.compared))
                ami.pause()
                lockstep.report = lockstep.report or {}
                lockstep.report.native, lockstep.report.emulated = native, emulated
                return lockstep.report
            end
            lockstep.stats = { compared = text.compared, different = text.different, native_bursts = #ours,
                               emulated_bursts = #theirs }
            first = false
            ami.wait(1)
            goto continue
        end
        local count, index, diff = ami.rgb_diff(native, emulated)
        if panels then
            ami.panel(0, shown, w, h, win.sx, 1, win.y)
            ami.panel(1, diff, w, h, win.sx, 1, win.y)
        end
        compared = compared + 1
        if name ~= lockstep.screen then
            ami.log(("peer screen %s from emulator frame %d (native tick %d)"):format(name, ami.frame(), ticks))
            lockstep.screen = name
        end
        if cfg.on_frame and cfg.on_frame(count, native, emulated) then
            ami.log(("lockstep stopped at emulator frame %d (native tick %d, %s): %d pixels different")
                :format(ami.frame(), ticks, name, count))
            ami.pause()
            lockstep.report = { frame = ami.frame(), ticks = ticks, screen = name, different = count,
                                native = native, emulated = emulated }
            return lockstep.report
        end
        if cfg.continue_for == -1 then
            -- never stop: log each run of different frames, keep the latest ones to inspect
            lockstep.stats = lockstep.stats or { compared = 0, different = 0, runs = 0, in_run = false }
            local st = lockstep.stats
            st.compared = st.compared + 1
            if count > 0 then
                st.different = st.different + 1
                lockstep.history = lockstep.history or {}
                lockstep.history[#lockstep.history + 1] = { frame = ami.frame(), ticks = ticks, screen = name, different = count,
                    x = (index - 1) % w, y = (index - 1) // w, native = native, emulated = emulated }
                if #lockstep.history > 64 then table.remove(lockstep.history, 1) end
                lockstep.report = lockstep.history[#lockstep.history]
                if not st.in_run then
                    st.runs = st.runs + 1
                    ami.log(("different from emulator frame %d (native tick %d, %s): %d pixels, first at (%d,%d)")
                        :format(ami.frame(), ticks, name, count, (index - 1) % w, (index - 1) // w))
                end
            elseif st.in_run then
                ami.log(("equal again at emulator frame %d (native tick %d, %s)"):format(ami.frame(), ticks, name))
            end
            st.in_run = count > 0
            if st.compared % 250 == 0 then
                ami.log(("lockstep: %d frames compared, %d different, %d runs; now %s"):format(
                    st.compared, st.different, st.runs, st.in_run and ("%d pixels different"):format(count) or "equal"))
            end
        elseif count > 0 or lockstep.after then
            local report = { frame = ami.frame(), ticks = ticks, screen = name, different = count,
                             x = index and (index - 1) % w or 0, y = index and (index - 1) // w or 0, compared = compared, native = native, emulated = emulated }
            if not lockstep.after then
                lockstep.first = report
                lockstep.history = {}
                lockstep.after = 0
                ami.log(("DIFFERENT at emulator frame %d (native tick %d, %s): %d of %d pixels, first at (%d,%d)%s")
                    :format(report.frame, ticks, name, count, w * h, report.x, report.y,
                        (cfg.continue_for or 0) > 0 and "" or "; paused"))
            end
            lockstep.history[#lockstep.history + 1] = report
            if lockstep.after >= (cfg.continue_for or 0) then
                if lockstep.after > 0 then ami.log(("%d frames after the first difference; paused"):format(lockstep.after)) end
                ami.pause()
                lockstep.report = lockstep.first -- for a control socket: ami.import('amiga/lockstep').report
                if cfg.on_mismatch then cfg.on_mismatch(lockstep.first) end
                return lockstep.first
            end
            lockstep.after = lockstep.after + 1
        end
        first = false
        ami.wait(1)
        ::continue::
    end
end

return lockstep
