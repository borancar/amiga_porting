#include "lua_script.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "libretro.h"

static lua_State *L, *co;
static const ami_host_api *api;
static long wake_frame;        /* frame at which the script runs again */
static int wait_ref = LUA_NOREF; /* ami.wait_until predicate, or none */
static long wait_limit;        /* give up waiting after this frame; -1 for never */
static int exit_code;
static int finished; /* the script chunk returned or failed */
static int failed;   /* a Lua error: hooks stop too */
static ami_machine *machine_out;
static char *script_dir;
static int booted, boot_called;
static int script_argc;
static const char **script_argv;
static const char *files_root;

static char *dup_str(const char *v) { return strdup(v); }
static char *join_dir(const char *path)
{
    if (path[0] == '/') return dup_str(path);
    size_t n = strlen(script_dir) + strlen(path) + 2;
    char *out = malloc(n);
    snprintf(out, n, "%s/%s", script_dir, path);
    return out;
}


/* ---- control socket -----------------------------------------------------------------------------------------
 * `--control PATH`: a Unix stream socket whose clients send one Lua line at a time and get the result back as
 * `ok LEN\n` or `err LEN\n` plus LEN bytes. A line runs in the script's own state (so its globals, `ami.import`ed
 * modules and every ami.* function that does not wait), between frames, also while the machine is paused. */
#define CTL_CLIENTS 4
#define CTL_LINE 65536
static const char *ctl_path;
static int ctl_listen = -1;
static struct { int fd; size_t n; char buf[CTL_LINE]; } ctl_client[CTL_CLIENTS];

/* Runs `code` as an expression (its values are shown) or else as a statement; print() output comes first. */
static const char ctl_prelude[] =
    "local function show(v, depth)\n"
    "  if type(v) == 'string' then return (('%q'):format(v)) end\n"
    "  if type(v) ~= 'table' or depth > 2 then return tostring(v) end\n"
    "  local keys = {}\n"
    "  for k in pairs(v) do keys[#keys + 1] = k end\n"
    "  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)\n"
    "  local parts = {}\n"
    "  for i, k in ipairs(keys) do\n"
    "    if i > 64 then parts[#parts + 1] = '...' break end\n"
    "    parts[#parts + 1] = (type(k) == 'string' and k or '[' .. tostring(k) .. ']') .. '=' .. show(v[k], depth + 1)\n"
    "  end\n"
    "  return '{' .. table.concat(parts, ', ') .. '}'\n"
    "end\n"
    "function ami.inspect(v) return show(v, 0) end\n"
    "function ami._control(code)\n"
    "  local out, real_print = {}, print\n"
    "  print = function(...)\n"
    "    local t = table.pack(...)\n"
    "    for i = 1, t.n do t[i] = tostring(t[i]) end\n"
    "    out[#out + 1] = table.concat(t, '\\t')\n"
    "  end\n"
    "  local f, err = load('return ' .. code, '=control')\n"
    "  if not f then f, err = load(code, '=control') end\n"
    "  local ok, res = false, err\n"
    "  if f then\n"
    "    res = table.pack(pcall(f))\n"
    "    ok = res[1]\n"
    "    if ok then\n"
    "      for i = 2, res.n do out[#out + 1] = show(res[i], 0) end\n"
    "      res = nil\n"
    "    else res = res[2] end\n"
    "  end\n"
    "  print = real_print\n"
    "  if not ok then out[#out + 1] = tostring(res) end\n"
    "  return ok, table.concat(out, '\\n')\n"
    "end\n";

void lua_script_set_control(const char *path) { ctl_path = path; }

static void ctl_open(void)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(ctl_path) >= sizeof addr.sun_path) { fprintf(stderr, "[script] --control path too long\n"); ctl_path = NULL; return; }
    strcpy(addr.sun_path, ctl_path);
    unlink(ctl_path);
    mode_t old = umask(077);
    ctl_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    int bad = ctl_listen < 0 || bind(ctl_listen, (struct sockaddr *)&addr, sizeof addr) || listen(ctl_listen, 2);
    umask(old);
    if (bad) {
        fprintf(stderr, "[script] cannot serve %s: %s\n", ctl_path, strerror(errno));
        if (ctl_listen >= 0) close(ctl_listen);
        ctl_listen = -1; ctl_path = NULL;
        return;
    }
    fcntl(ctl_listen, F_SETFL, O_NONBLOCK);
    for (int i = 0; i < CTL_CLIENTS; ++i) ctl_client[i].fd = -1;
    fprintf(stderr, "[script] control socket %s\n", ctl_path);
}

static void ctl_reply(int fd, int ok, const char *text, size_t n)
{
    char head[32];
    int h = snprintf(head, sizeof head, "%s %zu\n", ok ? "ok" : "err", n);
    struct iovec_like { const char *p; size_t n; } parts[2] = {{head, (size_t)h}, {text, n}};
    for (int i = 0; i < 2; ++i)
        for (size_t done = 0; done < parts[i].n;) {
            ssize_t w = send(fd, parts[i].p + done, parts[i].n - done, MSG_NOSIGNAL);
            if (w < 0 && errno == EAGAIN) { struct pollfd p = {fd, POLLOUT, 0}; poll(&p, 1, 1000); continue; }
            if (w <= 0) return;
            done += (size_t)w;
        }
}

static void ctl_run(int fd, const char *code)
{
    lua_getglobal(L, "ami");
    lua_getfield(L, -1, "_control");
    lua_pushstring(L, code);
    if (lua_pcall(L, 1, 2, 0)) {
        size_t n; const char *m = lua_tolstring(L, -1, &n);
        ctl_reply(fd, 0, m ? m : "error", m ? n : 5);
        lua_pop(L, 2);
        return;
    }
    size_t n; const char *text = lua_tolstring(L, -1, &n);
    ctl_reply(fd, lua_toboolean(L, -2), text ? text : "", text ? n : 0);
    lua_pop(L, 3);
}

void lua_script_poll_control(int paused)
{
    if (!ctl_path) return;
    if (ctl_listen < 0) ctl_open();
    if (ctl_listen < 0) return;
    struct pollfd fds[1 + CTL_CLIENTS];
    int slot[1 + CTL_CLIENTS], n = 0;
    fds[n] = (struct pollfd){ctl_listen, POLLIN, 0}; slot[n++] = -1;
    for (int i = 0; i < CTL_CLIENTS; ++i)
        if (ctl_client[i].fd >= 0) { fds[n] = (struct pollfd){ctl_client[i].fd, POLLIN, 0}; slot[n++] = i; }
    if (poll(fds, (nfds_t)n, paused ? 2 : 0) <= 0) return;
    for (int k = 0; k < n; ++k) {
        if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (slot[k] < 0) {
            int fd = accept(ctl_listen, NULL, NULL);
            if (fd < 0) continue;
            int i = 0;
            while (i < CTL_CLIENTS && ctl_client[i].fd >= 0) ++i;
            if (i == CTL_CLIENTS) { close(fd); continue; }
            ctl_client[i].fd = fd; ctl_client[i].n = 0;
            continue;
        }
        int i = slot[k];
        ssize_t got = recv(ctl_client[i].fd, ctl_client[i].buf + ctl_client[i].n, CTL_LINE - ctl_client[i].n, 0);
        if (got <= 0) { close(ctl_client[i].fd); ctl_client[i].fd = -1; continue; }
        ctl_client[i].n += (size_t)got;
        for (char *nl; ctl_client[i].fd >= 0 && (nl = memchr(ctl_client[i].buf, '\n', ctl_client[i].n));) {
            *nl = 0;
            ctl_run(ctl_client[i].fd, ctl_client[i].buf);
            size_t used = (size_t)(nl - ctl_client[i].buf) + 1;
            memmove(ctl_client[i].buf, ctl_client[i].buf + used, ctl_client[i].n - used);
            ctl_client[i].n -= used;
        }
        if (ctl_client[i].fd >= 0 && ctl_client[i].n == CTL_LINE) { close(ctl_client[i].fd); ctl_client[i].fd = -1; }
    }
}

static void fail(const char *what, lua_State *s);
static lua_State *current_hook;
/* luaL_error has no %x: format addresses ourselves. */
static int addr_error(lua_State *s, const char *what, uint32_t addr)
{
    char text[96];
    snprintf(text, sizeof text, "%s at $%06x", what, addr);
    return luaL_error(s, "%s", text);
}

static unsigned check_addr(lua_State *s, int i) { return (unsigned)(uint32_t)luaL_checkinteger(s, i); }

static int l_frame(lua_State *s) { lua_pushinteger(s, api->frame); return 1; }
static int l_width(lua_State *s) { lua_pushinteger(s, api->screen_width); return 1; }
static int l_height(lua_State *s) { lua_pushinteger(s, api->screen_height); return 1; }

static int peek(lua_State *s, size_t size)
{
    uint8_t b[4] = {0};
    if (api->read_memory(check_addr(s, 1), b, size) != size) return addr_error(s, size == 1 ? "cannot read guest byte" : size == 2 ? "cannot read guest word" : "cannot read guest long", check_addr(s, 1));
    uint32_t v = 0;
    for (size_t i = 0; i < size; ++i) v = v << 8 | b[i]; /* 68000 is big-endian */
    lua_pushinteger(s, v);
    return 1;
}
static int l_peek8(lua_State *s) { return peek(s, 1); }
static int l_peek16(lua_State *s) { return peek(s, 2); }
static int l_peek32(lua_State *s) { return peek(s, 4); }

static int poke(lua_State *s, size_t size)
{
    if (!api->write_memory(check_addr(s, 1), (uint32_t)luaL_checkinteger(s, 2), size))
        return addr_error(s, "cannot write guest memory", check_addr(s, 1));
    return 0;
}
static int l_poke8(lua_State *s) { return poke(s, 1); }
static int l_poke16(lua_State *s) { return poke(s, 2); }
static int l_poke32(lua_State *s) { return poke(s, 4); }

static int l_read(lua_State *s)
{
    lua_Integer n = luaL_checkinteger(s, 2);
    luaL_argcheck(s, n >= 0 && n <= (1 << 24), 2, "length out of range");
    luaL_Buffer buf;
    uint8_t *out = (uint8_t *)luaL_buffinitsize(s, &buf, (size_t)n);
    if (api->read_memory(check_addr(s, 1), out, (size_t)n) != (size_t)n) return addr_error(s, "cannot read guest memory", check_addr(s, 1));
    luaL_pushresultsize(&buf, (size_t)n);
    return 1;
}

static int l_pixel(lua_State *s)
{
    lua_pushinteger(s, api->pixel((unsigned)luaL_checkinteger(s, 1), (unsigned)luaL_checkinteger(s, 2)));
    return 1;
}

/* FNV-1a over the last presented frame, optionally limited to a rectangle x, y, w, h. */
static int l_frame_hash(lua_State *s)
{
    unsigned x0 = 0, y0 = 0, w = api->screen_width, h = api->screen_height;
    if (!lua_isnoneornil(s, 1)) {
        x0 = (unsigned)luaL_checkinteger(s, 1);
        y0 = (unsigned)luaL_checkinteger(s, 2);
        w = (unsigned)luaL_checkinteger(s, 3);
        h = (unsigned)luaL_checkinteger(s, 4);
    }
    uint64_t hash = 1469598103934665603ull;
    for (unsigned y = y0; y < y0 + h; ++y)
        for (unsigned x = x0; x < x0 + w; ++x) {
            hash = (hash ^ api->pixel(x, y)) * 1099511628211ull;
        }
    lua_pushinteger(s, (lua_Integer)(hash & 0x7fffffffffffffffull));
    return 1;
}

/* ami.save_file(path, bytes): writes a host file (creating missing parent directories). Like ami.screenshot it is
 * not confined: the script is the user's own. */
static int l_save_file(lua_State *s)
{
    const char *path = luaL_checkstring(s, 1);
    size_t n;
    const char *data = luaL_checklstring(s, 2, &n);
    char *dir = strdup(path);
    if (!dir) return luaL_error(s, "out of memory");
    for (char *c = dir + 1; *c; ++c)
        if (*c == '/') { *c = 0; mkdir(dir, 0777); *c = '/'; }
    free(dir);
    FILE *file = fopen(path, "wb");
    if (!file) return luaL_error(s, "cannot write %s", path);
    size_t written = fwrite(data, 1, n, file);
    if (fclose(file) != 0 || written != n) return luaL_error(s, "cannot write %s", path);
    return 0;
}

/* ami.append_file(path, bytes): appends to a host file (created when missing; a named pipe may be given: the reader sees the bytes at once).  For what a script streams out as it runs. */
static int l_append_file(lua_State *s)
{
    const char *path = luaL_checkstring(s, 1);
    size_t n;
    const char *data = luaL_checklstring(s, 2, &n);
    FILE *file = fopen(path, "ab");
    if (!file) return luaL_error(s, "cannot append to %s", path);
    size_t written = fwrite(data, 1, n, file);
    if (fclose(file) != 0 || written != n) return luaL_error(s, "cannot append to %s", path);
    return 0;
}

/* ami.cycles(): CPU cycles run since power on, the machine's own clock (for timing code: a frame is 141,875 on a PAL A500). */
static int l_cycles(lua_State *s)
{
    lua_pushinteger(s, (lua_Integer)(api->read_cycles ? api->read_cycles() : 0));
    return 1;
}

/* ami.trace_writes(on): record every custom register write (needs the trace core); ami.take_writes() returns the writes
 * since the last call as lines `cycles offset value pc copper` and clears them. */
static int l_trace_writes(lua_State *s)
{
    if (api->trace_writes) api->trace_writes(lua_toboolean(s, 1));
    return 0;
}

static int l_take_writes(lua_State *s)
{
    size_t length = 0;
    char *text = api->take_writes ? api->take_writes(&length) : NULL;
    lua_pushlstring(s, text ? text : "", length);
    free(text);
    return 1;
}

/* ami.snapshot_save([name]) / ami.snapshot_load(name): the machine's complete state to or from a file. A name without
 * '/' is a file in the machine's `snapshots` directory (default: the save directory); no name saves frame-N.state
 * there, as F11 does. Returns whether it worked. Not from a hook. */
static int l_snapshot_save(lua_State *s)
{
    lua_pushboolean(s, api->snapshot_save && api->snapshot_save(luaL_optstring(s, 1, NULL)));
    return 1;
}

static int l_snapshot_load(lua_State *s)
{
    lua_pushboolean(s, api->snapshot_load && api->snapshot_load(luaL_optstring(s, 1, NULL)));
    return 1;
}

/* ami.disk_count(), ami.disk_index() (0-based), ami.disk_eject(bool), ami.disk_select(i): swap floppies of an .m3u;
 * eject, ami.wait(~20), select, eject(false). nil when the core has no disk control. */
static int disk_result(lua_State *s, int r)
{
    if (r < 0) lua_pushnil(s); else lua_pushinteger(s, r);
    return 1;
}
static int l_disk_count(lua_State *s) { return disk_result(s, api->disk_count ? api->disk_count() : -1); }
static int l_disk_index(lua_State *s) { return disk_result(s, api->disk_index ? api->disk_index() : -1); }
static int l_disk_eject(lua_State *s)
{
    return disk_result(s, api->disk_eject ? api->disk_eject(lua_toboolean(s, 1)) : -1);
}
static int l_disk_select(lua_State *s)
{
    return disk_result(s, api->disk_select ? api->disk_select((int)luaL_checkinteger(s, 1)) : -1);
}

static int l_screenshot(lua_State *s)
{
    const char *path = luaL_checkstring(s, 1);
    FILE *file = fopen(path, "wb");
    if (!file) return luaL_error(s, "cannot write %s", path);
    fprintf(file, "P6\n%u %u\n255\n", api->screen_width, api->screen_height);
    for (unsigned y = 0; y < api->screen_height; ++y)
        for (unsigned x = 0; x < api->screen_width; ++x) {
            uint32_t p = api->pixel(x, y);
            fputc((p >> 16) & 255, file); fputc((p >> 8) & 255, file); fputc(p & 255, file);
        }
    fclose(file);
    return 0;
}

static unsigned check_key(lua_State *s, int i)
{
    if (lua_isinteger(s, i)) return (unsigned)lua_tointeger(s, i);
    luaL_checkstring(s, i);
    lua_getglobal(s, "ami");
    lua_getfield(s, -1, "keys");
    lua_pushvalue(s, i);
    lua_gettable(s, -2);
    if (!lua_isinteger(s, -1)) luaL_error(s, "unknown key '%s'", lua_tostring(s, i));
    unsigned key = (unsigned)lua_tointeger(s, -1);
    lua_pop(s, 3);
    return key;
}
static int l_key(lua_State *s)
{
    api->key(check_key(s, 1), lua_toboolean(s, 2));
    return 0;
}
/* ami.joystick{ up = true, down = true, left = true, right = true, fire = true }: press those directions and the fire
 * button of the joystick in port 1 (anything not named is released); ami.joystick() releases all */
static int l_joystick(lua_State *s)
{
    static const struct { const char *name; unsigned id; } dirs[] = {
        {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN}, {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},
        {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT}, {"fire", RETRO_DEVICE_ID_JOYPAD_B}};
    unsigned mask = 0;
    if (lua_istable(s, 1)) {
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
            lua_getfield(s, 1, dirs[i].name);
            if (lua_toboolean(s, -1)) mask |= 1u << dirs[i].id;
            lua_pop(s, 1);
        }
    }
    api->joystick(mask);
    return 0;
}
static int l_mouse_move(lua_State *s) { api->mouse_move((int)luaL_checkinteger(s, 1), (int)luaL_checkinteger(s, 2)); return 0; }
static int l_mouse_button(lua_State *s)
{
    api->mouse_button((int)luaL_checkinteger(s, 1), lua_toboolean(s, 2));
    return 0;
}
static int l_log(lua_State *s)
{
    int n = lua_gettop(s);
    luaL_Buffer buf;
    luaL_buffinit(s, &buf);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&buf, ' ');
        luaL_addstring(&buf, luaL_tolstring(s, i, NULL));
        lua_pop(s, 1);
    }
    luaL_pushresult(&buf);
    if (api) api->log("%s", lua_tostring(s, -1));
    else fprintf(stderr, "[script] %s\n", lua_tostring(s, -1));
    return 0;
}
static int l_quit(lua_State *s)
{
    exit_code = (int)luaL_optinteger(s, 1, 0);
    api->quit();
    if (s == current_hook) return 0; /* a hook finishes normally; the host exits after the frame */
    return lua_yield(s, 0); /* stop running this script; the host exits after the frame */
}

/* ami.wait(n): resume after n more frames (default 1). */
static int l_wait(lua_State *s)
{
    lua_Integer n = luaL_optinteger(s, 1, 1);
    luaL_argcheck(s, n >= 0, 1, "negative frame count");
    wake_frame = api->frame + (n ? n : 1);
    wait_limit = -1;
    return lua_yield(s, 0);
}

/* ami.wait_until(fn, max_frames): resume once fn() is true; returns false on timeout. */
static int l_wait_until(lua_State *s)
{
    luaL_checktype(s, 1, LUA_TFUNCTION);
    lua_Integer max = luaL_optinteger(s, 2, -1);
    lua_pushvalue(s, 1);
    luaL_unref(s, LUA_REGISTRYINDEX, wait_ref);
    wait_ref = luaL_ref(s, LUA_REGISTRYINDEX);
    wake_frame = api->frame + 1;
    wait_limit = max < 0 ? -1 : api->frame + max;
    return lua_yield(s, 0);
}



/* ---- CPU hooks and registers ---------------------------------------------- */
static const char *const reg_names[18] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "sr", "pc"};

static int check_reg(lua_State *s, int i)
{
    const char *name = luaL_checkstring(s, i);
    if (!strcmp(name, "sp")) return 15;
    for (int r = 0; r < 18; ++r) if (!strcmp(name, reg_names[r])) return r;
    return luaL_error(s, "unknown register '%s'", name);
}

/* ami.reg(name): register value; "d0".."d7", "a0".."a7" (or "sp"), "sr", "pc". */
static int l_reg(lua_State *s)
{
    int r = check_reg(s, 1);
    uint32_t regs[18] = {0};
    api->read_regs(regs, 18);
    lua_pushinteger(s, regs[r]);
    return 1;
}

static int l_set_reg(lua_State *s)
{
    int r = check_reg(s, 1);
    if (!api->write_reg((unsigned)r, (uint32_t)luaL_checkinteger(s, 2))) return luaL_error(s, "cannot write register %s", lua_tostring(s, 1));
    return 0;
}

/* ami.hook(addr, fn): run fn() when the CPU reaches addr, before the instruction there executes. Nothing is halted. */
static int l_hook(lua_State *s)
{
    uint32_t addr = (uint32_t)luaL_checkinteger(s, 1) & 0xffffff;
    luaL_checktype(s, 2, LUA_TFUNCTION);
    lua_getfield(s, LUA_REGISTRYINDEX, "ami_hooks");
    lua_pushinteger(s, addr);
    lua_pushvalue(s, 2);
    lua_settable(s, -3);
    api->add_breakpoint(addr);
    return 0;
}

static int l_unhook(lua_State *s)
{
    uint32_t addr = (uint32_t)luaL_checkinteger(s, 1) & 0xffffff;
    lua_getfield(s, LUA_REGISTRYINDEX, "ami_hooks");
    lua_pushinteger(s, addr);
    lua_pushnil(s);
    lua_settable(s, -3);
    api->remove_breakpoint(addr);
    return 0;
}

/* ami.rts([d0]): return from the subroutine the CPU is in at the hook (pop PC), optionally setting D0. */
static int l_rts(lua_State *s)
{
    uint32_t regs[18] = {0};
    api->read_regs(regs, 18);
    uint8_t b[4];
    if (api->read_memory(regs[15], b, 4) != 4) return addr_error(s, "cannot read the return address", regs[15]);
    api->write_reg(17, (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    api->write_reg(15, regs[15] + 4);
    if (!lua_isnoneornil(s, 1)) api->write_reg(0, (uint32_t)luaL_checkinteger(s, 1));
    return 0;
}

/* Hooks run as coroutines so that a hook can call guest routines (ami.call) and continue when they
 * return. A call pushes a return address of its own and hooks it. Calls can be outstanding
 * together: a call made in one task may be preempted by another task whose hook calls too. */
#define CALL_TRAP_BASE 0x00fc00d2u /* the ROM's cold-start entry, which never runs again */
#define MAX_CALLS 8
static struct {
    int active;
    lua_State *thread;
    int ref; /* registry reference that keeps the suspended thread alive */
    uint32_t saved[18];
} pending[MAX_CALLS];
static uint32_t call_trap(int slot) { return CALL_TRAP_BASE + 2u * (uint32_t)slot; }
static lua_State *current_hook;
static int current_ref;
static int call_started; /* set by ami.call while a hook runs: the hook yielded for it */
static int step_ref = LUA_NOREF; /* ami.step callback while single-stepping */

static void run_hook(lua_State *th, int ref, int nargs)
{
    lua_State *previous = current_hook;
    int previous_ref = current_ref;
    current_hook = th;
    current_ref = ref;
    call_started = 0;
    int nres = 0;
    int status = lua_resume(th, L, nargs, &nres);
    current_hook = previous;
    current_ref = previous_ref;
    if (status == LUA_YIELD) {
        lua_pop(th, nres);
        if (!call_started) { lua_pushstring(th, "a hook may only yield through ami.call"); fail("hook", th); }
        return;
    }
    if (status != LUA_OK) fail("hook", th);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

/* ami.call(addr, {d0=..., a1=...}): run the guest routine at addr (it returns with RTS) from a
 * hook and return a table of the registers it left. The CPU state is restored afterwards. */
static int l_call(lua_State *s)
{
    if (s != current_hook) return luaL_error(s, "ami.call is only available inside a hook");
    int slot = 0;
    while (slot < MAX_CALLS && pending[slot].active) ++slot;
    if (slot == MAX_CALLS) return luaL_error(s, "too many outstanding ami.call");
    uint32_t addr = check_addr(s, 1);
    api->read_regs(pending[slot].saved, 18);
    if (!lua_isnoneornil(s, 2)) {
        luaL_checktype(s, 2, LUA_TTABLE);
        for (lua_pushnil(s); lua_next(s, 2); lua_pop(s, 1)) {
            lua_pushvalue(s, -2);
            int r = check_reg(s, -1);
            lua_pop(s, 1);
            api->write_reg((unsigned)r, (uint32_t)luaL_checkinteger(s, -1));
        }
    }
    uint32_t regs[18];
    api->read_regs(regs, 18);
    uint32_t sp = regs[15] - 4;
    api->write_memory(sp, call_trap(slot), 4);
    api->write_reg(15, sp);
    api->write_reg(17, addr);
    api->add_breakpoint(call_trap(slot));
    pending[slot].active = 1;
    pending[slot].thread = s;
    pending[slot].ref = current_ref;
    call_started = 1;
    return lua_yield(s, 0);
}

int lua_script_stepping(void) { return step_ref != LUA_NOREF; }

int lua_script_step(const ami_host_api *host, uint32_t pc)
{
    (void)host;
    if (!L || failed || step_ref == LUA_NOREF) return 0;
    /* fn(pc) returns true to go on, anything else ends the trace */
    lua_rawgeti(L, LUA_REGISTRYINDEX, step_ref);
    lua_pushinteger(L, pc & 0xffffff);
    if (lua_pcall(L, 1, 1, 0)) { fail("step", L); return 0; }
    int more = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!more) { luaL_unref(L, LUA_REGISTRYINDEX, step_ref); step_ref = LUA_NOREF; }
    return more;
}

void lua_script_breakpoint(const ami_host_api *host, uint32_t pc)
{
    (void)host;
    if (!L || failed) return; /* hooks keep running after the script chunk has returned */
    pc &= 0xffffff;
    for (int slot = 0; slot < MAX_CALLS; ++slot) {
        if (!pending[slot].active || pc != call_trap(slot)) continue;
        uint32_t regs[18];
        api->read_regs(regs, 18);
        lua_State *th = pending[slot].thread;
        int ref = pending[slot].ref;
        uint32_t saved[18];
        memcpy(saved, pending[slot].saved, sizeof saved);
        pending[slot].active = 0;
        lua_createtable(th, 0, 18);
        for (int i = 0; i < 18; ++i) {
            lua_pushinteger(th, regs[i]);
            lua_setfield(th, -2, reg_names[i]);
        }
        for (int i = 0; i < 18; ++i) api->write_reg((unsigned)i, saved[i]); /* sr before pc: a user/supervisor switch moves a7 */
        run_hook(th, ref, 1);
        return;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "ami_hooks");
    lua_pushinteger(L, pc);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_State *th = lua_newthread(L); /* anchored on L's stack until the hook ends or yields */
        lua_pushvalue(L, -2);
        lua_xmove(L, th, 1);
        int new_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* keep the thread alive while suspended */
        run_hook(th, new_ref, 0);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* ami.step(fn): pause the machine and single-step it: fn(pc) is called before each instruction and returns true
 * to keep stepping; when it does not, the machine resumes. Slow, and the frames are cut short; for tracing a short
 * stretch while debugging. */
static int l_step(lua_State *s)
{
    luaL_checktype(s, 1, LUA_TFUNCTION);
    luaL_unref(s, LUA_REGISTRYINDEX, step_ref);
    lua_pushvalue(s, 1);
    step_ref = luaL_ref(s, LUA_REGISTRYINDEX);
    api->pause();
    return 0;
}

/* ami.pause(): freeze the whole machine (silent) until ami.resume(). Nothing else pauses it. */
static int l_pause(lua_State *s)
{
    (void)s;
    api->pause();
    return 0;
}

static int l_resume(lua_State *s)
{
    (void)s;
    api->resume();
    return 0;
}

static int l_paused(lua_State *s)
{
    lua_pushboolean(s, api->is_paused());
    return 1;
}

/* ami.custom(offset): the last value written to a custom register ($096 = DMACON, ...) and the PC
 * of the instruction that wrote it. Custom registers are write-only, so this is how to see them. */
static int l_custom(lua_State *s)
{
    uint16_t value;
    uint32_t pc;
    if (!api->custom_write((unsigned)luaL_checkinteger(s, 1), &value, &pc)) return luaL_error(s, "custom register state is not available");
    lua_pushinteger(s, value);
    lua_pushinteger(s, pc);
    return 2;
}


extern char **environ;

/* ---- peers: processes the machine config names, driven over pipes ---------------------------------------------- */
typedef struct {
    pid_t pid;
    int to, from; /* write end of its stdin, read end of its stdout */
    unsigned char *buf;
    size_t len, cap;
} peer_t;
#define PEER_MT "ami.peer"

static peer_t *check_peer(lua_State *s)
{
    peer_t *p = luaL_checkudata(s, 1, PEER_MT);
    if (p->pid <= 0) luaL_error(s, "the peer has been closed");
    return p;
}

static void peer_close(peer_t *p)
{
    if (p->pid <= 0) return;
    close(p->to);
    close(p->from);
    int status;
    waitpid(p->pid, &status, 0);
    p->pid = 0;
    free(p->buf);
    p->buf = NULL;
    p->len = p->cap = 0;
}

/* Read what is available (blocking until something is); 0 at end of file. */
static int peer_fill(peer_t *p)
{
    if (p->len == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 1 << 16;
        p->buf = realloc(p->buf, p->cap);
    }
    ssize_t n;
    do n = read(p->from, p->buf + p->len, p->cap - p->len);
    while (n < 0 && errno == EINTR);
    if (n <= 0) return 0;
    p->len += (size_t)n;
    return 1;
}

static int l_peer_send(lua_State *s)
{
    peer_t *p = check_peer(s);
    size_t n, done = 0;
    const char *data = luaL_checklstring(s, 2, &n);
    while (done < n) {
        ssize_t w = write(p->to, data + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return luaL_error(s, "the peer is gone");
        done += (size_t)w;
    }
    return 0;
}

/* peer:readline() -> the next line without its newline, or nil at end of file */
static int l_peer_readline(lua_State *s)
{
    peer_t *p = check_peer(s);
    for (size_t from = 0;;) {
        unsigned char *nl = memchr(p->buf + from, '\n', p->len - from);
        if (nl) {
            size_t n = (size_t)(nl - p->buf);
            lua_pushlstring(s, (const char *)p->buf, n);
            memmove(p->buf, p->buf + n + 1, p->len - n - 1);
            p->len -= n + 1;
            return 1;
        }
        from = p->len;
        if (!peer_fill(p)) { lua_pushnil(s); return 1; }
    }
}

/* peer:read(n) -> exactly n bytes, or nil at end of file */
static int l_peer_read(lua_State *s)
{
    peer_t *p = check_peer(s);
    lua_Integer n = luaL_checkinteger(s, 2);
    luaL_argcheck(s, n >= 0 && n <= (1 << 26), 2, "length out of range");
    while (p->len < (size_t)n)
        if (!peer_fill(p)) { lua_pushnil(s); return 1; }
    lua_pushlstring(s, (const char *)p->buf, (size_t)n);
    memmove(p->buf, p->buf + n, p->len - (size_t)n);
    p->len -= (size_t)n;
    return 1;
}

static int l_peer_close(lua_State *s)
{
    peer_close(luaL_checkudata(s, 1, PEER_MT));
    return 0;
}

/* ami.peer(name): start the process machine.peers[name] names; its stdin and stdout are the pipes. */
static int l_peer(lua_State *s)
{
    const char *name = luaL_checkstring(s, 1);
    const char *const *argv = NULL;
    for (int i = 0; machine_out && i < machine_out->peer_count; ++i)
        if (!strcmp(machine_out->peers[i].name, name)) argv = machine_out->peers[i].argv;
    if (!argv) return luaL_error(s, "no peer '%s' in ami.machine{ peers = ... }", name);
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return luaL_error(s, "pipe: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN); /* a dead peer is an error from send, not a signal */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addclose(&actions, in[1]);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (rc) {
        close(in[1]);
        close(out[0]);
        return luaL_error(s, "cannot start %s: %s", argv[0], strerror(rc));
    }
    peer_t *p = lua_newuserdatauv(s, sizeof *p, 0);
    memset(p, 0, sizeof *p);
    p->pid = pid;
    p->to = in[1];
    p->from = out[0];
    luaL_setmetatable(s, PEER_MT);
    return 1;
}

static const luaL_Reg peer_methods[] = {
    {"send", l_peer_send}, {"readline", l_peer_readline}, {"read", l_peer_read}, {"close", l_peer_close},
    {"__gc", l_peer_close}, {NULL, NULL}};

/* ---- input, panels and RGB helpers --------------------------------------------------------------------------- */
/* ami.input(): the input the host processed since the last call, oldest first, each {kind, a, b}:
 * {"k", libretro key, down}, {"m", dx, dy}, {"b", 0 left / 1 right, down}. */
static int l_input(lua_State *s)
{
    ami_input_event events[64];
    size_t n = api->take_input(events, 64);
    lua_createtable(s, (int)n, 0);
    for (size_t i = 0; i < n; ++i) {
        lua_createtable(s, 3, 0);
        char kind[2] = {(char)events[i].kind, 0};
        lua_pushstring(s, kind);
        lua_rawseti(s, -2, 1);
        lua_pushinteger(s, events[i].a);
        lua_rawseti(s, -2, 2);
        lua_pushinteger(s, events[i].b);
        lua_rawseti(s, -2, 3);
        lua_rawseti(s, -2, (lua_Integer)i + 1);
    }
    return 1;
}

/* ami.panel(slot, rgb, w, h [, sx, sy, y]): show an RGB image beside the emulator frame (see loader_api.h). */
static int l_panel(lua_State *s)
{
    size_t n;
    const char *rgb = luaL_checklstring(s, 2, &n);
    int w = (int)luaL_checkinteger(s, 3), h = (int)luaL_checkinteger(s, 4);
    luaL_argcheck(s, w > 0 && h > 0 && n == (size_t)w * (size_t)h * 3, 2, "needs w * h * 3 bytes");
    api->panel((int)luaL_checkinteger(s, 1), (const uint8_t *)rgb, w, h, (int)luaL_optinteger(s, 5, 1),
               (int)luaL_optinteger(s, 6, 1), (int)luaL_optinteger(s, 7, 0));
    return 0;
}

/* ami.frame_rgb(x, y, w, h [, sx]): a w x h image of the last frame as RGB bytes, taking every sx-th column. */
static int l_frame_rgb(lua_State *s)
{
    int x0 = (int)luaL_checkinteger(s, 1), y0 = (int)luaL_checkinteger(s, 2);
    int w = (int)luaL_checkinteger(s, 3), h = (int)luaL_checkinteger(s, 4), sx = (int)luaL_optinteger(s, 5, 1);
    luaL_argcheck(s, w > 0 && h > 0 && sx > 0 && (size_t)w * (size_t)h <= (1u << 24), 3, "bad size");
    luaL_Buffer buf;
    unsigned char *out = (unsigned char *)luaL_buffinitsize(s, &buf, (size_t)w * (size_t)h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = api->pixel((unsigned)(x0 + x * sx), (unsigned)(y0 + y));
            *out++ = (unsigned char)(p >> 16);
            *out++ = (unsigned char)(p >> 8);
            *out++ = (unsigned char)p;
        }
    luaL_pushresultsize(&buf, (size_t)w * (size_t)h * 3);
    return 1;
}

/* ami.rgb_diff(a, b): compare two equal-sized RGB strings. Returns the number of differing pixels, the 1-based
 * index of the first one (nil if none) and an RGB image of the difference: the picture dimmed, differences red. */
static int l_rgb_diff(lua_State *s)
{
    size_t na, nb;
    const unsigned char *a = (const unsigned char *)luaL_checklstring(s, 1, &na);
    const unsigned char *b = (const unsigned char *)luaL_checklstring(s, 2, &nb);
    luaL_argcheck(s, na == nb && na % 3 == 0, 2, "images must be the same size");
    luaL_Buffer buf;
    unsigned char *out = (unsigned char *)luaL_buffinitsize(s, &buf, na);
    lua_Integer count = 0, first = 0;
    for (size_t i = 0; i < na; i += 3) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) {
            if (!count++) first = (lua_Integer)(i / 3) + 1;
            out[i] = 255, out[i + 1] = 0, out[i + 2] = 0;
        } else {
            out[i] = a[i] / 4, out[i + 1] = a[i + 1] / 4, out[i + 2] = a[i + 2] / 4;
        }
    }
    luaL_pushresultsize(&buf, na); /* finish the buffer before pushing anything else */
    lua_pushinteger(s, count);
    if (first) lua_pushinteger(s, first); else lua_pushnil(s);
    lua_rotate(s, -3, -1); /* image, count, first -> count, first, image */
    return 3;
}

/* ami.write(addr, bytes): copy a Lua string into guest memory. */
static int l_write(lua_State *s)
{
    size_t n;
    uint32_t addr = check_addr(s, 1);
    const unsigned char *data = (const unsigned char *)luaL_checklstring(s, 2, &n);
    size_t i = 0;
    for (; i < n && (addr + i) % 4 == 0 && n - i >= 4; i += 4)
        if (!api->write_memory(addr + (uint32_t)i, (uint32_t)data[i] << 24 | data[i + 1] << 16 | data[i + 2] << 8 | data[i + 3], 4)) goto fail_write;
    for (; i < n; ++i)
        if (!api->write_memory(addr + (uint32_t)i, data[i], 1)) goto fail_write;
    return 0;
fail_write:
    return addr_error(s, "cannot write guest memory", addr + (uint32_t)i);
}

/* ---- host files (read-only, rooted at machine.files) ------------------------ */
/* Resolve a relative, AmigaDOS-style (case-insensitive) path under files_root. */
static char *resolve_file(lua_State *s, const char *rel)
{
    if (!files_root) luaL_error(s, "set machine.files to use host files");
    char *path = strdup(files_root);
    char *copy = strdup(rel), *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, "..") || !strcmp(part, ".")) { free(path); free(copy); return NULL; }
        DIR *d = opendir(path);
        char *found = NULL;
        for (struct dirent *e; d && (e = readdir(d));) {
            if (!strcasecmp(e->d_name, part)) { found = strdup(e->d_name); break; }
        }
        if (d) closedir(d);
        if (!found) { free(path); free(copy); return NULL; }
        char *next = malloc(strlen(path) + strlen(found) + 2);
        sprintf(next, "%s/%s", path, found);
        free(path); free(found);
        path = next;
    }
    free(copy);
    return path;
}

/* ami.file_size(path): bytes, or nil if absent; -1 for a directory. */
static int l_file_size(lua_State *s)
{
    char *path = resolve_file(s, luaL_checkstring(s, 1));
    struct stat st;
    int ok = path && !stat(path, &st);
    free(path);
    if (!ok) { lua_pushnil(s); return 1; }
    lua_pushinteger(s, S_ISDIR(st.st_mode) ? -1 : (lua_Integer)st.st_size);
    return 1;
}

/* ami.file_read(path [, offset [, length]]): bytes read (a string), or nil if absent. */
static int l_file_read(lua_State *s)
{
    char *path = resolve_file(s, luaL_checkstring(s, 1));
    lua_Integer off = luaL_optinteger(s, 2, 0), len = luaL_optinteger(s, 3, -1);
    FILE *f = path ? fopen(path, "rb") : NULL;
    free(path);
    if (!f) { lua_pushnil(s); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (off < 0 || off > size) off = size;
    if (len < 0 || len > size - off) len = size - off;
    fseek(f, (long)off, SEEK_SET);
    luaL_Buffer buf;
    char *out = luaL_buffinitsize(s, &buf, (size_t)len);
    size_t got = fread(out, 1, (size_t)len, f);
    fclose(f);
    luaL_pushresultsize(&buf, got);
    return 1;
}

static int cmp_names(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* ami.dir(path): sorted array of {name=, size=, dir=} for a directory ("" is the root), or nil. */
static int l_dir(lua_State *s)
{
    char *path = resolve_file(s, luaL_checkstring(s, 1));
    DIR *d = path ? opendir(path) : NULL;
    if (!d) { free(path); lua_pushnil(s); return 1; }
    char *names[4096];
    int count = 0;
    for (struct dirent *e; count < 4096 && (e = readdir(d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names[count++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)count, sizeof *names, cmp_names);
    lua_createtable(s, count, 0);
    for (int i = 0; i < count; ++i) {
        char *full = malloc(strlen(path) + strlen(names[i]) + 2);
        sprintf(full, "%s/%s", path, names[i]);
        struct stat st;
        int ok = !stat(full, &st);
        free(full);
        lua_createtable(s, 0, 3);
        lua_pushstring(s, names[i]); lua_setfield(s, -2, "name");
        lua_pushinteger(s, ok && !S_ISDIR(st.st_mode) ? (lua_Integer)st.st_size : 0); lua_setfield(s, -2, "size");
        lua_pushboolean(s, ok && S_ISDIR(st.st_mode)); lua_setfield(s, -2, "dir");
        lua_rawseti(s, -2, i + 1);
        free(names[i]);
    }
    free(path);
    return 1;
}


/* ami.import("amiga/hunk"): load NAME.lua from the script's directory or the shared library
 * directory (ami9000/host/lua) once, and return what it returns. */
#ifndef AMI_LUA_DIR
#define AMI_LUA_DIR "."
#endif
static int l_import(lua_State *s)
{
    const char *name = luaL_checkstring(s, 1);
    if (name[0] == '/' || strstr(name, "..")) return luaL_error(s, "bad module name '%s'", name);
    lua_getfield(s, LUA_REGISTRYINDEX, "ami_modules");
    lua_pushvalue(s, 1);
    if (lua_gettable(s, -2) != LUA_TNIL) return 1;
    lua_pop(s, 1);
    const char *dirs[] = {script_dir, AMI_LUA_DIR};
    for (int i = 0; i < 2; ++i) {
        char *path = malloc(strlen(dirs[i]) + strlen(name) + 6);
        sprintf(path, "%s/%s.lua", dirs[i], name);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        int ok = f != NULL && luaL_loadfile(s, path) == LUA_OK;
        if (f && !ok) { free(path); return lua_error(s); }
        free(path);
        if (!ok) continue;
        lua_call(s, 0, 1);
        if (lua_isnil(s, -1)) { lua_pop(s, 1); lua_pushboolean(s, 1); }
        lua_pushvalue(s, 1);
        lua_pushvalue(s, -2);
        lua_settable(s, -4);
        return 1;
    }
    return luaL_error(s, "module '%s' not found in %s or %s", name, script_dir, AMI_LUA_DIR);
}

/* Optional string field `name` of table at stack index 1; NULL if absent. */
static const char *opt_string(lua_State *s, const char *name, int path)
{
    lua_getfield(s, 1, name);
    if (lua_isnil(s, -1)) { lua_pop(s, 1); return NULL; }
    if (lua_type(s, -1) != LUA_TSTRING && !lua_isnumber(s, -1)) luaL_error(s, "machine.%s must be a string", name);
    const char *v = lua_tostring(s, -1);
    char *out = path ? join_dir(v) : dup_str(v);
    lua_pop(s, 1);
    return out;
}

static void add_opt(lua_State *s, ami_machine *m, const char *key, const char *value)
{
    for (int i = 0; i < m->opt_count; ++i)
        if (!strcmp(m->opt[i].key, key)) { m->opt[i].value = value; return; }
    if (m->opt_count == AMI_MAX_OPTS) luaL_error(s, "too many machine options");
    m->opt[m->opt_count].key = key;
    m->opt[m->opt_count++].value = value;
}

/* ami.machine{ name, model, system, save, snapshots, content, files, core, scale, joystick_keys,
 *              opt = {puae_fastmem_size = 8}, roms = {{"kick.rom", 0xcrc32}}, plugin, plugin_args } */
static int l_machine(lua_State *s)
{
    if (booted || boot_called) return luaL_error(s, "ami.machine must be called before ami.boot");
    luaL_checktype(s, 1, LUA_TTABLE);
    ami_machine *m = machine_out;
    static const char *known[] = {"name", "model", "system", "save", "snapshots", "content", "core", "files", "scale",
        "joystick_keys", "opt", "roms", "plugin", "plugin_args", "peers", NULL};
    for (lua_pushnil(s); lua_next(s, 1); lua_pop(s, 1)) {
        int ok = 0;
        if (lua_type(s, -2) == LUA_TSTRING)
            for (const char **k = known; *k; ++k) ok |= !strcmp(*k, lua_tostring(s, -2));
        if (!ok) return luaL_error(s, "unknown machine field '%s'", lua_tostring(s, -2));
    }
    m->name = opt_string(s, "name", 0);
    m->model = opt_string(s, "model", 0);
    m->system = opt_string(s, "system", 1);
    m->save = opt_string(s, "save", 1);
    m->snapshots = opt_string(s, "snapshots", 1);
    m->content = opt_string(s, "content", 1);
    m->core = opt_string(s, "core", 1);
    m->files = opt_string(s, "files", 1);
    files_root = m->files;
    m->plugin = opt_string(s, "plugin", 1);
    m->plugin_args = opt_string(s, "plugin_args", 0);
    lua_getfield(s, 1, "scale");
    m->scale = (int)luaL_optinteger(s, -1, 0);
    lua_pop(s, 1);
    lua_getfield(s, 1, "joystick_keys");
    m->joystick_keys = lua_toboolean(s, -1);
    lua_pop(s, 1);
    lua_getfield(s, 1, "opt");
    if (!lua_isnil(s, -1)) {
        luaL_checktype(s, -1, LUA_TTABLE);
        for (lua_pushnil(s); lua_next(s, -2); lua_pop(s, 1)) {
            if (lua_type(s, -2) != LUA_TSTRING) return luaL_error(s, "machine.opt keys must be strings");
            lua_pushvalue(s, -2); /* tostring on a copy so lua_next's key stays a string */
            const char *key = dup_str(lua_tostring(s, -1));
            lua_pop(s, 1);
            const char *value = dup_str(luaL_tolstring(s, -1, NULL));
            lua_pop(s, 1);
            add_opt(s, m, key, value);
        }
    }
    lua_pop(s, 1);
    lua_getfield(s, 1, "peers");
    if (!lua_isnil(s, -1)) {
        luaL_checktype(s, -1, LUA_TTABLE);
        for (lua_pushnil(s); lua_next(s, -2); lua_pop(s, 1)) {
            if (lua_type(s, -2) != LUA_TSTRING) return luaL_error(s, "machine.peers keys must be names");
            if (m->peer_count == AMI_MAX_PEERS) return luaL_error(s, "too many peers");
            luaL_checktype(s, -1, LUA_TTABLE);
            int p = m->peer_count++;
            m->peers[p].name = dup_str(lua_tostring(s, -2));
            int n = (int)luaL_len(s, -1);
            if (n < 1 || n > AMI_MAX_PEER_ARGS) return luaL_error(s, "a peer needs a program and up to %d arguments", AMI_MAX_PEER_ARGS - 1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(s, -1, i);
                const char *arg = luaL_checkstring(s, -1);
                /* a relative program path is the script's */
                m->peers[p].argv[i - 1] = i == 1 && arg[0] != '/' && strchr(arg, '/') ? join_dir(arg) : dup_str(arg);
                lua_pop(s, 1);
            }
            m->peers[p].argv[n] = NULL;
        }
    }
    lua_pop(s, 1);
    lua_getfield(s, 1, "roms");
    if (!lua_isnil(s, -1)) {
        luaL_checktype(s, -1, LUA_TTABLE);
        for (lua_Integer i = 1; lua_rawgeti(s, -1, i) != LUA_TNIL; ++i, lua_pop(s, 1)) {
            if (m->rom_count == AMI_MAX_ROMS) return luaL_error(s, "too many roms");
            luaL_checktype(s, -1, LUA_TTABLE);
            lua_rawgeti(s, -1, 1);
            lua_rawgeti(s, -2, 2);
            m->roms[m->rom_count].name = dup_str(luaL_checkstring(s, -2));
            m->roms[m->rom_count++].crc = (uint32_t)luaL_checkinteger(s, -1);
            lua_pop(s, 2);
        }
        lua_pop(s, 1); /* the nil that ended the loop */
    }
    lua_pop(s, 1);
    return 0;
}

/* ami.boot(): end of configuration. The host loads the core, then resumes the script. */
static int l_boot(lua_State *s)
{
    if (booted || boot_called) return luaL_error(s, "ami.boot was already called");
    boot_called = 1;
    return lua_yield(s, 0);
}

/* Machine functions need the core: wrap them so that using one earlier is a clear error. */
static int guarded(lua_State *s)
{
    if (!api) return luaL_error(s, "ami functions that touch the machine only work after ami.boot()");
    return lua_tocfunction(s, lua_upvalueindex(1))(s);
}

static const luaL_Reg funcs[] = {
    {"frame", l_frame}, {"width", l_width}, {"height", l_height},
    {"peek8", l_peek8}, {"peek16", l_peek16}, {"peek32", l_peek32},
    {"poke8", l_poke8}, {"poke16", l_poke16}, {"poke32", l_poke32}, {"read", l_read},
    {"pixel", l_pixel}, {"frame_hash", l_frame_hash}, {"screenshot", l_screenshot}, {"save_file", l_save_file}, {"append_file", l_append_file}, {"cycles", l_cycles}, {"trace_writes", l_trace_writes}, {"take_writes", l_take_writes}, {"snapshot_save", l_snapshot_save}, {"snapshot_load", l_snapshot_load}, {"disk_count", l_disk_count}, {"disk_index", l_disk_index}, {"disk_eject", l_disk_eject}, {"disk_select", l_disk_select},
    {"key", l_key}, {"joystick", l_joystick}, {"mouse_move", l_mouse_move}, {"mouse_button", l_mouse_button},
    {"log", l_log}, {"quit", l_quit}, {"wait", l_wait}, {"wait_until", l_wait_until},
    {"peer", l_peer}, {"input", l_input}, {"panel", l_panel}, {"frame_rgb", l_frame_rgb}, {"rgb_diff", l_rgb_diff}, {"call", l_call}, {"pause", l_pause}, {"resume", l_resume}, {"paused", l_paused}, {"custom", l_custom}, {"step", l_step}, {"reg", l_reg}, {"set_reg", l_set_reg}, {"hook", l_hook}, {"unhook", l_unhook}, {"rts", l_rts},
    {"write", l_write}, {"file_size", l_file_size}, {"file_read", l_file_read}, {"dir", l_dir},
    {NULL, NULL}};
static const luaL_Reg config_funcs[] = {{"import", l_import}, {"machine", l_machine}, {"boot", l_boot}, {"log", l_log}, {NULL, NULL}};

#define KEY(name, code) {name, code}
static const struct { const char *name; unsigned code; } key_names[] = {
    KEY("return", RETROK_RETURN), KEY("escape", RETROK_ESCAPE), KEY("space", RETROK_SPACE),
    KEY("tab", RETROK_TAB), KEY("backspace", RETROK_BACKSPACE), KEY("delete", RETROK_DELETE),
    KEY("up", RETROK_UP), KEY("down", RETROK_DOWN), KEY("left", RETROK_LEFT), KEY("right", RETROK_RIGHT),
    KEY("lctrl", RETROK_LCTRL), KEY("lshift", RETROK_LSHIFT), KEY("lalt", RETROK_LALT),
    KEY("kp_multiply", RETROK_KP_MULTIPLY), KEY("kp_enter", RETROK_KP_ENTER),
    KEY("f1", RETROK_F1), KEY("f2", RETROK_F2), KEY("f3", RETROK_F3), KEY("f4", RETROK_F4),
    KEY("f5", RETROK_F5), KEY("f6", RETROK_F6), KEY("f7", RETROK_F7), KEY("f8", RETROK_F8),
    KEY("f9", RETROK_F9), KEY("f10", RETROK_F10),
};

static void set_keys(lua_State *s)
{
    lua_newtable(s);
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; ++i) {
        lua_pushinteger(s, key_names[i].code);
        lua_setfield(s, -2, key_names[i].name);
    }
    for (char c = 'a'; c <= 'z'; ++c) { /* RETROK_a.. are the ASCII codes */
        char name[2] = {c, 0};
        lua_pushinteger(s, c);
        lua_setfield(s, -2, name);
    }
    for (char c = '0'; c <= '9'; ++c) {
        char name[2] = {c, 0};
        lua_pushinteger(s, c);
        lua_setfield(s, -2, name);
    }
    lua_setfield(s, -2, "keys");
}

static void fail(const char *what, lua_State *s)
{
    const char *msg = lua_tostring(s, -1);
    /* a traceback of the failing coroutine, to see which hook or script line called what */
    luaL_traceback(L, s, msg ? msg : "(error object)", 0);
    fprintf(stderr, "[script] %s: %s\n", what, lua_tostring(L, -1));
    lua_pop(L, 1);
    exit_code = 3;
    finished = failed = 1;
    if (api) api->quit();
}

void lua_script_set_argv(int count, const char **argv)
{
    script_argc = count;
    script_argv = argv;
}

int lua_script_configure(const char *path, const char *args, ami_machine *m)
{
    machine_out = m;
    script_dir = strdup(path);
    char *slash = strrchr(script_dir, '/');
    if (slash) *slash = 0; else strcpy(script_dir, ".");
    char *absolute = realpath(script_dir, NULL); /* peers and other processes get paths that do not depend on the cwd */
    if (absolute) { free(script_dir); script_dir = absolute; }
    L = luaL_newstate();
    if (!L) return 1;
    /* No io, os, package, debug or load from file: a script drives the machine and nothing else. */
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_COLIBNAME, luaopen_coroutine}, {NULL, NULL}};
    for (const luaL_Reg *lib = libs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_newtable(L);
    luaL_setfuncs(L, config_funcs, 0);
    for (const luaL_Reg *f = funcs; f->name; ++f) {
        if (!strcmp(f->name, "log")) continue;
        lua_pushcfunction(L, f->func);
        lua_pushcclosure(L, guarded, 1);
        lua_setfield(L, -2, f->name);
    }
    set_keys(L);
    lua_pushstring(L, args ? args : "");
    lua_setfield(L, -2, "args");
    lua_pushstring(L, script_dir);
    lua_setfield(L, -2, "script_dir");
    lua_createtable(L, script_argc, 0);
    for (int i = 0; i < script_argc; ++i) {
        lua_pushstring(L, script_argv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "argv");
    lua_setglobal(L, "ami");
    if (luaL_dostring(L, ctl_prelude)) { fprintf(stderr, "[script] prelude: %s\n", lua_tostring(L, -1)); return 1; }
    luaL_newmetatable(L, PEER_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, peer_methods, 0);
    lua_pop(L, 1);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ami_hooks");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ami_modules");
    co = lua_newthread(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "ami_script"); /* anchor the thread */
    if (luaL_loadfile(co, path)) { fail("load", co); return 1; }
    int nres = 0;
    int status = lua_resume(co, L, 0, &nres);
    if (status == LUA_YIELD && boot_called) { lua_pop(co, nres); return 0; }
    if (status == LUA_YIELD || status == LUA_OK) {
        fprintf(stderr, "[script] %s: finished without calling ami.boot()\n", path);
        exit_code = 3;
        return 1;
    }
    fail("configure", co);
    return 1;
}

void lua_script_start(const ami_host_api *host)
{
    api = host;
    booted = 1;
}

void lua_script_before_frame(const ami_host_api *host)
{
    if (finished || host->frame < wake_frame) return;
    int nargs = 0;
    if (wait_ref != LUA_NOREF) {
        /* Run the predicate on the main state: co is suspended inside its yield. */
        lua_rawgeti(L, LUA_REGISTRYINDEX, wait_ref);
        if (lua_pcall(L, 0, 1, 0)) { fail("wait_until", L); return; }
        int done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (!done && (wait_limit < 0 || host->frame < wait_limit)) { wake_frame = host->frame + 1; return; }
        luaL_unref(L, LUA_REGISTRYINDEX, wait_ref);
        wait_ref = LUA_NOREF;
        lua_pushboolean(co, done); /* becomes the result of ami.wait_until */
        nargs = 1;
    }
    int nres = 0;
    int status = lua_resume(co, L, nargs, &nres);
    if (status == LUA_YIELD) { lua_pop(co, nres); return; }
    if (status != LUA_OK) { fail("run", co); return; }
    finished = 1; /* script returned; the game keeps running */
}

void lua_script_shutdown(void)
{
    if (ctl_listen >= 0) { close(ctl_listen); unlink(ctl_path); ctl_listen = -1; }
    for (int i = 0; ctl_path && i < CTL_CLIENTS; ++i) if (ctl_client[i].fd >= 0) close(ctl_client[i].fd);
    if (L) lua_close(L);
    L = co = NULL;
}

int lua_script_exit_code(void) { return exit_code; }
