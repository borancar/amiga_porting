/*
 * Minimal libretro host for ami9000.so (Engine9000's PUAE core).
 *
 * Runs an Amiga floppy image, a WHDLoad slave or a Lua game script (see lua_script.h) windowed or
 * headless, with keyboard, mouse and keyboard-driven joystick input. No debugger is required.
 */
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libretro.h"
#include "loader_api.h"
#include "lua_script.h"

#define MAX_OPTIONS 64

typedef struct { const char *key; const char *value; } option_t;

static struct {
    void *lib;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*init)(void);
    void (*deinit)(void);
    void (*run)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
} core;

#define MAX_DEFAULTS 512

static option_t options[MAX_OPTIONS];
static int option_count;
static option_t defaults[MAX_DEFAULTS];
static int default_count;
static const char *system_dir = ".";
static const char *save_dir = ".";
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static retro_keyboard_event_t keyboard_cb;
static struct retro_system_av_info av;

static bool quit_requested;
static bool headless;
static bool verbose;
static bool joystick_keys;
static long dump_frame = -1;
static const char *dump_path;
static long frame_count;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int texture_w, texture_h;
static SDL_AudioStream *audio_stream;

static int mouse_dx, mouse_dy;
#define INPUT_LOG_MAX 256
static ami_input_event input_log[INPUT_LOG_MAX];
static size_t input_log_count;
static void log_input(int kind, int a, int b)
{
    if (input_log_count < INPUT_LOG_MAX) input_log[input_log_count++] = (ami_input_event){kind, a, b};
}
static int mouse_buttons;
static uint16_t joy_mask;

static void die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("ami9000_host: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

static void set_option(const char *key, const char *value)
{
    for (int i = 0; i < option_count; ++i) {
        if (!strcmp(options[i].key, key)) {
            options[i].value = value;
            return;
        }
    }
    if (option_count == MAX_OPTIONS) {
        die("too many --opt values");
    }
    options[option_count++] = (option_t){key, value};
}

static void add_default(const char *key, const char *value)
{
    if (key && value && default_count < MAX_DEFAULTS) {
        defaults[default_count++] = (option_t){key, value};
    }
}

/* ---- machine setup -------------------------------------------------------
 * A game keeps its setup in a Lua script (ami.machine{}, see lua_script.h): the
 * machine model, core options, required Kickstarts (by CRC32), the save/system
 * directories and the content to launch. Command-line flags override it.
 */
#define MAX_ROMS 8
static struct { const char *name; uint32_t crc; } roms[MAX_ROMS];
static int rom_count;
static const char *machine_name = "machine";
static const char *plugin_path, *plugin_args = "";
static const char *core_path_cfg, *content_cfg;
static int scale_cfg;
static int window_x = -1, window_y = -1;   /* --window-pos X,Y */
static const char *script_path, *script_args = "", *control_path;

static char *join_path(const char *dir, const char *path)
{
    if (path[0] == '/') {
        return strdup(path);
    }
    size_t n = strlen(dir) + strlen(path) + 2;
    char *out = malloc(n);
    snprintf(out, n, "%s/%s", dir, path);
    return out;
}

static uint32_t crc32_file(const char *path, bool *ok)
{
    FILE *file = fopen(path, "rb");
    *ok = file != NULL;
    if (!file) return 0;
    uint32_t crc = 0xffffffffu;
    unsigned char buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, file)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            crc ^= buffer[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
        }
    }
    fclose(file);
    return ~crc;
}

static void check_roms(void)
{
    for (int i = 0; i < rom_count; ++i) {
        char *path = join_path(system_dir, roms[i].name);
        bool ok;
        uint32_t crc = crc32_file(path, &ok);
        if (!ok) die("'%s' needs Kickstart %s in %s", machine_name, roms[i].name, system_dir);
        if (crc != roms[i].crc) {
            die("%s has CRC32 %08x, '%s' needs %08x", path, crc, machine_name, roms[i].crc);
        }
        free(path);
    }
}

static uint32_t to_argb(const uint8_t *row, int x);

/* ---- plugin host API ------------------------------------------------------ */
static const ami_loader *plugin;
static ami_host_api host_api;
static int injected_buttons;
static uint16_t injected_joy;   /* the joystick bits scripts press (ami.joystick) */
static uint32_t *last_argb;
static size_t (*e9k_read_memory)(uint32_t, uint8_t *, size_t);
static int (*e9k_write_memory)(uint32_t, uint32_t, size_t);
static void (*e9k_add_hook)(uint32_t);
static void (*e9k_remove_hook)(uint32_t);
static void (*e9k_set_hook_callback)(void (*)(uint32_t));
static void (*e9k_pause)(void);
static size_t (*e9k_read_regs)(uint32_t *, size_t);
static uint64_t (*e9k_read_cycles)(void);
static int (*e9k_write_reg)(uint32_t, uint32_t);
static int (*e9k_is_paused)(void);
static void (*e9k_resume)(void);
static void (*e9k_step_instr)(void);
typedef struct { uint16_t value; uint32_t pc; } custom_reg_state; /* core: e9k_debug_ami_custom_reg_state_t */
static const custom_reg_state *(*e9k_custom_regs)(void);

static size_t api_read_memory(uint32_t address, void *out, size_t size)
{
    return e9k_read_memory ? e9k_read_memory(address, out, size) : 0;
}

static int api_write_memory(uint32_t address, uint32_t value, size_t size)
{
    return e9k_write_memory ? e9k_write_memory(address, value, size) : 0;
}

static void api_add_breakpoint(uint32_t address) { if (e9k_add_hook) e9k_add_hook(address); }
static void api_remove_breakpoint(uint32_t address) { if (e9k_remove_hook) e9k_remove_hook(address); }
/* Called by the core from inside its instruction loop when the 68000 reaches a hook. Nothing is stopped. */
static void core_hook(uint32_t pc)
{
    if (plugin && plugin->breakpoint) plugin->breakpoint(&host_api, pc);
    if (script_path) lua_script_breakpoint(&host_api, pc);
}

/* Floppy changes: the core's disk control interface (set by the core while it loads an .m3u). */
static struct retro_disk_control_callback disk_control;
static bool have_disk_control;
static int disk_phase; /* hotkey cycle: frames left before the next image goes in, 0 when idle */
static int disk_next;

static int api_disk_count(void) { return have_disk_control ? (int)disk_control.get_num_images() : -1; }
static int api_disk_index(void) { return have_disk_control ? (int)disk_control.get_image_index() : -1; }
static int api_disk_eject(int eject) { return have_disk_control ? disk_control.set_eject_state(eject != 0) : -1; }
static int api_disk_select(int index) { return have_disk_control ? disk_control.set_image_index((unsigned)index) : -1; }

/* Snapshots: the core's own savestate. The F11 hotkey only sets a request (it arrives inside retro_run); the main loop
 * serves it between frames. Restoring is for scripts (ami.snapshot_load). */
static const char *snapshot_dir;
static int snapshot_request; /* 0 none, 1 capture */

/* A name with a '/' is a path; any other name is a file in the snapshot directory (default: the save directory);
 * no name is frame-N.state. */
static const char *snapshot_file(const char *name, char *buffer, size_t size)
{
    if (name && strchr(name, '/')) return name;
    const char *dir = snapshot_dir ? snapshot_dir : save_dir;
    if (name) snprintf(buffer, size, "%s/%s", dir, name);
    else snprintf(buffer, size, "%s/frame-%ld.state", dir, frame_count);
    return buffer;
}

static int api_snapshot_save(const char *path)
{
    char buffer[1024];
    path = snapshot_file(path, buffer, sizeof buffer);
    size_t size = core.serialize_size ? core.serialize_size() : 0;
    void *data = size ? malloc(size) : NULL;
    int ok = 0;
    if (data && core.serialize(data, size)) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path);
        for (char *c = dir + 1; *c; ++c)
            if (*c == '/') { *c = 0; mkdir(dir, 0777); *c = '/'; }
        FILE *file = fopen(path, "wb");
        if (file) {
            ok = fwrite(data, 1, size, file) == size;
            ok = fclose(file) == 0 && ok;
        }
    }
    free(data);
    fprintf(stderr, "ami9000_host: snapshot %s %s (frame %ld)\n", ok ? "saved to" : "FAILED to save to", path, frame_count);
    return ok;
}

static int api_snapshot_load(const char *path)
{
    char buffer[1024];
    path = snapshot_file(path, buffer, sizeof buffer);
    int ok = 0;
    FILE *file = fopen(path, "rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        rewind(file);
        void *data = size > 0 ? malloc((size_t)size) : NULL;
        if (data && fread(data, 1, (size_t)size, file) == (size_t)size) ok = core.unserialize && core.unserialize(data, (size_t)size);
        free(data);
        fclose(file);
    }
    fprintf(stderr, "ami9000_host: snapshot %s %s (frame %ld)\n", ok ? "restored from" : "FAILED to restore from", path, frame_count);
    return ok;
}

static void api_pause(void) { if (e9k_pause) e9k_pause(); }
static void api_resume(void) { if (e9k_resume) e9k_resume(); }
static int api_is_paused(void) { return e9k_is_paused ? e9k_is_paused() : 0; }
typedef struct { uint64_t cycles; uint32_t addr, value, pc, copper; } write_trace_t;
static write_trace_t *write_trace;
static size_t write_trace_n;
static const size_t write_trace_cap = 1u << 20;
static void (*e9k_set_write_observer)(void (*)(uint32_t, uint32_t, uint32_t, uint32_t));
static void on_custom_write(uint32_t addr, uint32_t value, uint32_t source, uint32_t copper)
{
    if (!write_trace || write_trace_n >= write_trace_cap) return;
    write_trace[write_trace_n++] = (write_trace_t){ e9k_read_cycles ? e9k_read_cycles() : 0, addr, value, source, copper };
}
static void api_trace_writes(int on)
{
    write_trace_n = 0;
    if (on && !write_trace) write_trace = malloc(write_trace_cap * sizeof *write_trace);
    if (e9k_set_write_observer) e9k_set_write_observer(on ? on_custom_write : NULL);
}
static char *api_take_writes(size_t *length)
{
    char *out = malloc(write_trace_n * 48 + 1);
    size_t at = 0;
    for (size_t i = 0; out && i < write_trace_n; ++i)
        at += (size_t)sprintf(out + at, "%llu %03x %04x %06x %u\n", (unsigned long long)write_trace[i].cycles,
                              write_trace[i].addr & 0xfff, write_trace[i].value & 0xffff, write_trace[i].pc & 0xffffff,
                              write_trace[i].copper);
    if (out) out[at] = 0;
    write_trace_n = 0;
    *length = at;
    return out;
}
static uint64_t api_read_cycles(void) { return e9k_read_cycles ? e9k_read_cycles() : 0; }
static size_t api_read_regs(uint32_t *out, size_t count) { return e9k_read_regs ? e9k_read_regs(out, count) : 0; }
static int api_write_reg(unsigned index, uint32_t value) { return e9k_write_reg ? e9k_write_reg(index, value) : 0; }


static int api_custom_write(unsigned offset, uint16_t *value, uint32_t *pc)
{
    if (!e9k_custom_regs || offset >= 512) return 0;
    const custom_reg_state *regs = e9k_custom_regs();
    if (!regs) return 0;
    *value = regs[offset >> 1].value;
    *pc = regs[offset >> 1].pc;
    return 1;
}

static uint32_t api_pixel(unsigned x, unsigned y)
{
    if (!last_argb || x >= host_api.screen_width || y >= host_api.screen_height) return 0;
    return last_argb[(size_t)y * host_api.screen_width + x];
}

static void api_mouse_move(int dx, int dy) { mouse_dx += dx; mouse_dy += dy; }
static void api_mouse_button(int button, int down)
{
    injected_buttons = down ? (injected_buttons | (1 << button)) : (injected_buttons & ~(1 << button));
}
static void api_joystick(unsigned mask) { injected_joy = (uint16_t)mask; }
static void api_key(unsigned key, int down)
{
    if (keyboard_cb) keyboard_cb(down != 0, key, 0, 0);
}
static void api_quit(void) { quit_requested = true; }

static void api_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", plugin ? plugin->name : "script");
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_WARN && !verbose) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    fputs("[core] ", stderr);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        return false;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;
        for (int i = 0; i < option_count; ++i) {
            if (!strcmp(options[i].key, var->key)) {
                var->value = options[i].value;
                return true;
            }
        }
        for (int i = 0; i < default_count; ++i) {
            if (!strcmp(defaults[i].key, var->key)) {
                var->value = defaults[i].value;
                return true;
            }
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        for (const struct retro_core_option_definition *d = data; d->key; ++d) {
            add_default(d->key, d->default_value ? d->default_value : d->values[0].value);
        }
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        for (const struct retro_core_option_definition *d = ((struct retro_core_options_intl *)data)->us; d && d->key; ++d) {
            add_default(d->key, d->default_value ? d->default_value : d->values[0].value);
        }
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        const struct retro_core_options_v2 *v2 = cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2
            ? data : ((struct retro_core_options_v2_intl *)data)->us;
        for (const struct retro_core_option_v2_definition *d = v2 ? v2->definitions : NULL; d && d->key; ++d) {
            add_default(d->key, d->default_value ? d->default_value : d->values[0].value);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        for (const struct retro_variable *v = data; v->key; ++v) {
            const char *colon = strchr(v->value, ';');
            if (colon) {
                const char *first = colon + 1 + strspn(colon + 1, " ");
                size_t len = strcspn(first, "|");
                add_default(v->key, strndup(first, len));
            }
        }
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: /* starts with the same members */
        disk_control = *(struct retro_disk_control_callback *)data;
        have_disk_control = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixel_format = *(enum retro_pixel_format *)data;
        return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ||
               pixel_format == RETRO_PIXEL_FORMAT_RGB565 ||
               pixel_format == RETRO_PIXEL_FORMAT_0RGB1555;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        keyboard_cb = ((struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    default:
        return false;
    }
}

static uint32_t to_argb(const uint8_t *row, int x)
{
    switch (pixel_format) {
    case RETRO_PIXEL_FORMAT_XRGB8888:
        return ((const uint32_t *)row)[x] | 0xff000000u;
    case RETRO_PIXEL_FORMAT_RGB565: {
        uint16_t p = ((const uint16_t *)row)[x];
        uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    default: {
        uint16_t p = ((const uint16_t *)row)[x];
        uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
        return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    }
}

static void dump_ppm(const void *data, unsigned w, unsigned h, size_t pitch)
{
    FILE *file = fopen(dump_path, "wb");
    if (!file) {
        die("cannot write %s", dump_path);
    }
    fprintf(file, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        for (unsigned x = 0; x < w; ++x) {
            uint32_t p = to_argb(row, (int)x);
            fputc((p >> 16) & 255, file);
            fputc((p >> 8) & 255, file);
            fputc(p & 255, file);
        }
    }
    fclose(file);
}

/* ---- panels: images drawn beside the emulator frame ------------------------------------------------------ */
#define MAX_PANELS 4
static struct { SDL_Texture *texture; int w, h, sx, sy, y; } panels[MAX_PANELS];
static int layout_width;     /* logical width the renderer is set up for: the frame plus the panels */
static int base_window_width, base_window_height;

/* The renderer's logical size: the frame at the core's aspect, with each panel to its right at the same vertical
 * scale, so a panel pixel lines up with the frame pixels it stands for. */
static void layout(void)
{
    int total = texture_w;
    for (int i = 0; i < MAX_PANELS; ++i) {
        if (panels[i].texture) total += panels[i].w * panels[i].sx;
    }
    if (total == layout_width) return;
    layout_width = total;
    const int logical_h = (int)(texture_w / (av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : 1.33333f));
    SDL_SetRenderLogicalPresentation(renderer, total, logical_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (base_window_width > 0) SDL_SetWindowSize(window, base_window_width * total / texture_w, base_window_height);
}

static void present(void)
{
    if (headless || !texture) return;
    layout();
    const float logical_h = (float)(int)(texture_w / (av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : 1.33333f));
    const float ky = logical_h / (float)texture_h;
    SDL_RenderClear(renderer);
    const SDL_FRect frame = {0, 0, (float)texture_w, logical_h};
    SDL_RenderTexture(renderer, texture, NULL, &frame);
    float x = (float)texture_w;
    for (int i = 0; i < MAX_PANELS; ++i) {
        if (!panels[i].texture) continue;
        const SDL_FRect to = {x, panels[i].y * ky, (float)(panels[i].w * panels[i].sx), panels[i].h * panels[i].sy * ky};
        SDL_RenderTexture(renderer, panels[i].texture, NULL, &to);
        x += (float)(panels[i].w * panels[i].sx);
    }
    SDL_RenderPresent(renderer);
}

static void api_panel(int slot, const uint8_t *rgb, int w, int h, int sx, int sy, int y)
{
    if (headless || !renderer || slot < 0 || slot >= MAX_PANELS || w <= 0 || h <= 0 || sx <= 0 || sy <= 0) return;
    if (!panels[slot].texture || panels[slot].w != w || panels[slot].h != h) {
        if (panels[slot].texture) SDL_DestroyTexture(panels[slot].texture);
        panels[slot].texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!panels[slot].texture) return;
        SDL_SetTextureScaleMode(panels[slot].texture, SDL_SCALEMODE_NEAREST);
        layout_width = 0;
    }
    if (panels[slot].sx != sx) layout_width = 0;
    panels[slot].w = w, panels[slot].h = h, panels[slot].sx = sx, panels[slot].sy = sy, panels[slot].y = y;
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(panels[slot].texture, NULL, &pixels, &pitch)) return;
    for (int row = 0; row < h; ++row) {
        uint32_t *out = (uint32_t *)((uint8_t *)pixels + row * pitch);
        for (int x = 0; x < w; ++x, rgb += 3) out[x] = 0xff000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
    }
    SDL_UnlockTexture(panels[slot].texture);
}

static size_t api_take_input(ami_input_event *out, size_t max)
{
    size_t n = input_log_count < max ? input_log_count : max;
    memcpy(out, input_log, n * sizeof *out);
    memmove(input_log, input_log + n, (input_log_count - n) * sizeof *input_log);
    input_log_count -= n;
    return n;
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) {
        return;
    }
    if (plugin || script_path) {
        if (host_api.screen_width != w || host_api.screen_height != h) {
            free(last_argb);
            last_argb = malloc((size_t)w * h * sizeof *last_argb);
            host_api.screen_width = w;
            host_api.screen_height = h;
        }
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                last_argb[(size_t)y * w + x] = to_argb((const uint8_t *)data + y * pitch, (int)x);
            }
        }
    }
    if (dump_path && frame_count + 1 >= dump_frame) {
        dump_ppm(data, w, h, pitch);
        dump_path = NULL;
    }
    if (headless) {
        return;
    }
    if (!texture || texture_w != (int)w || texture_h != (int)h) {
        if (texture) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        texture_w = (int)w;
        texture_h = (int)h;
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        layout_width = 0; /* the layout depends on the frame size */
    }
    void *pixels;
    int out_pitch;
    if (!SDL_LockTexture(texture, NULL, &pixels, &out_pitch)) {
        return;
    }
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint32_t *out = (uint32_t *)((uint8_t *)pixels + y * out_pitch);
        for (unsigned x = 0; x < w; ++x) {
            out[x] = to_argb(row, (int)x);
        }
    }
    SDL_UnlockTexture(texture);
    present();
}

static unsigned long audio_frames_total; /* sample frames the core delivered, for --verbose */

static size_t audio_batch(const int16_t *data, size_t frames)
{
    audio_frames_total += frames;
    if (audio_stream) {
        SDL_PutAudioStreamData(audio_stream, data, (int)(frames * 4));
    }
    return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
    int16_t pair[2] = {left, right};
    audio_batch(pair, 1);
}

static unsigned retro_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_BACKSPACE: return RETROK_BACKSPACE;
    case SDLK_TAB: return RETROK_TAB;
    case SDLK_RETURN: return RETROK_RETURN;
    case SDLK_ESCAPE: return RETROK_ESCAPE;
    case SDLK_SPACE: return RETROK_SPACE;
    case SDLK_DELETE: return RETROK_DELETE;
    case SDLK_UP: return RETROK_UP;
    case SDLK_DOWN: return RETROK_DOWN;
    case SDLK_LEFT: return RETROK_LEFT;
    case SDLK_RIGHT: return RETROK_RIGHT;
    case SDLK_LSHIFT: return RETROK_LSHIFT;
    case SDLK_RSHIFT: return RETROK_RSHIFT;
    case SDLK_LCTRL: return RETROK_LCTRL;
    case SDLK_RCTRL: return RETROK_RCTRL;
    case SDLK_LALT: return RETROK_LALT;
    case SDLK_RALT: return RETROK_RALT;
    case SDLK_LGUI: return RETROK_LSUPER;
    case SDLK_RGUI: return RETROK_RSUPER;
    case SDLK_HELP: case SDLK_INSERT: return RETROK_HELP;
    default:
        if (key >= SDLK_F1 && key <= SDLK_F10) {
            return RETROK_F1 + (key - SDLK_F1);
        }
        if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
            return RETROK_KP1 + (key - SDLK_KP_1);
        }
        if (key == SDLK_KP_0) return RETROK_KP0;
        return key < 128 ? (unsigned)key : RETROK_UNKNOWN;
    }
}

static void joystick_key(SDL_Keycode key, bool down)
{
    uint16_t bit = 0;
    switch (key) {
    case SDLK_UP: bit = 1u << RETRO_DEVICE_ID_JOYPAD_UP; break;
    case SDLK_DOWN: bit = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN; break;
    case SDLK_LEFT: bit = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT; break;
    case SDLK_RIGHT: bit = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT; break;
    case SDLK_LCTRL: case SDLK_RCTRL: bit = 1u << RETRO_DEVICE_ID_JOYPAD_B; break;
    default: return;
    }
    joy_mask = down ? (joy_mask | bit) : (joy_mask & ~bit);
}


static void input_poll(void)
{
    SDL_Event event;
    if (headless) {
        return;
    }
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            quit_requested = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = event.type == SDL_EVENT_KEY_DOWN;
            if (down && event.key.key == SDLK_F11) {
                snapshot_request = 1; /* captured between frames; scripts restore with ami.snapshot_load */
                break;
            }
            if (down && event.key.key == SDLK_PAGEDOWN && have_disk_control && !disk_phase) {
                int count = api_disk_count();
                if (count > 1) { /* next image: out for 50 frames, then in */
                    disk_next = (api_disk_index() + 1) % count;
                    api_disk_eject(1);
                    disk_phase = 50;
                    fprintf(stderr, "ami9000_host: disk ejected, inserting image %d of %d\n", disk_next + 1, count);
                }
                break;
            }
            if (down && event.key.key == SDLK_F12) {
                quit_requested = true;
                break;
            }
            if (down && event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (down && (event.key.mod & SDL_KMOD_CTRL) && (event.key.mod & SDL_KMOD_ALT)) {
                SDL_SetWindowRelativeMouseMode(window, false);
                mouse_buttons = 0;
                break;
            }
            if (event.key.repeat) {
                break;
            }
            if (joystick_keys) {
                joystick_key(event.key.key, down);
            } else if (keyboard_cb) {
                unsigned key = retro_key(event.key.key);
                if (key != RETROK_UNKNOWN) {
                    keyboard_cb(down, key, 0, 0);
                    log_input('k', (int)key, down);
                }
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (!SDL_GetWindowRelativeMouseMode(window)) {
                break;
            }
            mouse_dx += (int)event.motion.xrel;
            mouse_dy += (int)event.motion.yrel;
            log_input('m', (int)event.motion.xrel, (int)event.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (!SDL_GetWindowRelativeMouseMode(window)) {
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
                break;
            }
            int bit = event.button.button == SDL_BUTTON_LEFT ? 1 :
                      event.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
            mouse_buttons = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? (mouse_buttons | bit) : (mouse_buttons & ~bit);
            if (bit) log_input('b', bit == 1 ? 0 : 1, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        default:
            break;
        }
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (device == RETRO_DEVICE_MOUSE && port == 0) {
        int16_t value = 0;
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: value = (int16_t)mouse_dx; mouse_dx = 0; break;
        case RETRO_DEVICE_ID_MOUSE_Y: value = (int16_t)mouse_dy; mouse_dy = 0; break;
        case RETRO_DEVICE_ID_MOUSE_LEFT: value = (mouse_buttons | injected_buttons) & 1; break;
        case RETRO_DEVICE_ID_MOUSE_RIGHT: value = ((mouse_buttons | injected_buttons) >> 1) & 1; break;
        }
        return value;
    }
    if (device == RETRO_DEVICE_JOYPAD && port == 1) {
        return ((joy_mask | injected_joy) >> id) & 1;
    }
    return 0;
}

static void *symbol(const char *name)
{
    void *address = dlsym(core.lib, name);
    if (!address) {
        die("core lacks %s", name);
    }
    return address;
}

static void usage(void)
{
    fputs("usage: ami9000_host [options] game.lua | game.adf | game.slave\n"
          "  game.lua             Lua program: describes the machine (ami.machine{}) and drives it (see lua_script.h)\n"
          "  --script-args TEXT   text exposed to the script as ami.args\n"
          "  --snapshots DIR      where snapshots go: F11 writes DIR/frame-N.state, ami.snapshot_save/load name files in it (default SAVE)\n"
          "  --control PATH       serve a Unix socket at PATH that runs Lua lines in the script's state (ctl.py)\n"
          "  other --flags        passed to the script as ami.argv (it decides what they mean)\n"
          "  --core ami9000.so    libretro core (default: system/ami9000.so beside the binary)\n"
          "  --system DIR         Kickstart directory (default .)\n"
          "  --plugin FILE.so     native per-game loader plugin (see loader_api.h)\n"
          "  --save DIR           save directory (default .)\n"
          "  --opt KEY=VALUE      core option, repeatable; overrides the script (default puae_model=A500)\n"
          "  --verbose            show core info logs, the core options and the audio delivered\n"
          "  --headless           no window, audio or input\n"
          "  --frames N           exit after N core frames\n"
          "  --dump-frame N FILE  write frame N as a PPM\n"
          "  --joystick-keys      arrows + Ctrl drive the port-2 joystick (otherwise keyboard)\n"
          "  --scale N            window size as N/2 of the core width (default 3)\n"
          "  --window-pos X,Y     where the window is put on the desktop\n"
          "Click grabs the mouse, Ctrl+Alt releases it, Alt+Enter toggles fullscreen, F12 quits.\n", stderr);
    exit(2);
}

#define MAX_SCRIPT_ARGV 64
static const char *script_argv[MAX_SCRIPT_ARGV]; /* flags the host does not know: the script's own, ami.argv */
static int script_argc;

/* Flags the host itself understands. Any other option goes to the script. */
static bool host_flag(const char *arg)
{
    static const char *flags[] = {"--core", "--system", "--save", "--frames", "--scale", "--opt", "--script-args",
        "--plugin", "--control", "--snapshots", "--dump-frame", "--headless", "--verbose", "--joystick-keys", "--window-pos", NULL};
    for (const char **f = flags; *f; ++f) if (!strcmp(arg, *f)) return true;
    return false;
}


/* Flags that consume following arguments, so the .lua scan does not mistake a value for the script. */
static int flag_values(const char *arg)
{
    static const char *one[] = {"--core", "--system", "--save", "--frames", "--scale", "--opt", "--script-args", "--plugin", "--control", "--snapshots", "--window-pos", NULL};
    for (const char **f = one; *f; ++f) if (!strcmp(arg, *f)) return 1;
    return !strcmp(arg, "--dump-frame") ? 2 : 0;
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && !strcmp(text + n - m, suffix);
}

int main(int argc, char **argv)
{
    const char *core_path = NULL, *game_path = NULL;
    long max_frames = -1;
    int scale = 3;
    set_option("puae_model", "A500");
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) { /* everything after is the script's */
            for (++i; i < argc && script_argc < MAX_SCRIPT_ARGV; ++i) script_argv[script_argc++] = argv[i];
            break;
        }
        if (!strcmp(argv[i], "--script-args") && i + 1 < argc) script_args = argv[i + 1];
        if (argv[i][0] != '-' && has_suffix(argv[i], ".lua")) script_path = argv[i];
        else if (argv[i][0] == '-' && !host_flag(argv[i]) && script_argc < MAX_SCRIPT_ARGV) script_argv[script_argc++] = argv[i];
        i += flag_values(argv[i]);
    }
    if (script_path) {
        static ami_machine machine;
        lua_script_set_argv(script_argc, script_argv);
        if (lua_script_configure(script_path, script_args, &machine)) return lua_script_exit_code() ? lua_script_exit_code() : 3;
        if (machine.name) machine_name = machine.name;
        if (machine.model) set_option("puae_model", machine.model);
        for (int i = 0; i < machine.opt_count; ++i) set_option(machine.opt[i].key, machine.opt[i].value);
        if (machine.system) system_dir = machine.system;
        if (machine.save) save_dir = machine.save;
        if (machine.snapshots) snapshot_dir = machine.snapshots;
        core_path_cfg = machine.core;
        content_cfg = machine.content;
        if (machine.plugin) plugin_path = machine.plugin;
        if (machine.plugin_args) plugin_args = machine.plugin_args;
        scale_cfg = machine.scale;
        if (machine.joystick_keys) joystick_keys = true;
        for (int i = 0; i < machine.rom_count && rom_count < MAX_ROMS; ++i) {
            roms[rom_count].name = machine.roms[i].name;
            roms[rom_count++].crc = machine.roms[i].crc;
        }
    }
    game_path = content_cfg;
    core_path = core_path_cfg;
    if (scale_cfg) scale = scale_cfg;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) break;
        else if (arg[0] == '-' && script_path && !host_flag(arg)) continue; /* the script's own flag */
        else if (!strcmp(arg, "--plugin") && i + 1 < argc) plugin_path = argv[++i];
        else if (!strcmp(arg, "--script-args") && i + 1 < argc) ++i;
        else if (!strcmp(arg, "--control") && i + 1 < argc) control_path = argv[++i];
        else if (!strcmp(arg, "--snapshots") && i + 1 < argc) snapshot_dir = argv[++i];
        else if (!strcmp(arg, "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(arg, "--system") && i + 1 < argc) system_dir = argv[++i];
        else if (!strcmp(arg, "--save") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(arg, "--headless")) headless = true;
        else if (!strcmp(arg, "--verbose")) verbose = true;
        else if (!strcmp(arg, "--joystick-keys")) joystick_keys = true;
        else if (!strcmp(arg, "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(arg, "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(arg, "--window-pos") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &window_x, &window_y);
        else if (!strcmp(arg, "--dump-frame") && i + 2 < argc) { dump_frame = atol(argv[i + 1]); dump_path = argv[i + 2]; i += 2; }
        else if (!strcmp(arg, "--opt") && i + 1 < argc) {
            char *text = strdup(argv[++i]);
            char *equals = strchr(text, '=');
            if (!equals) usage();
            *equals = 0;
            set_option(text, equals + 1);
        } else if (arg[0] != '-') {
            if (!has_suffix(arg, ".lua")) game_path = arg;
        } else usage();
    }
    if (!core_path) {
        const char *base = SDL_GetBasePath();
        core_path = join_path(base ? base : ".", "system/ami9000.so");
    }
    if (!game_path) {
        usage();
    }
    if (!strcmp(game_path, "@blank")) {
        /* an empty 880 KB floppy: the machine boots to its "insert disk" screen and a script takes over */
        char *blank = join_path(save_dir, "blank.adf");
        FILE *f = fopen(blank, "rb");
        if (f) {
            fclose(f);
        } else {
            f = fopen(blank, "wb");
            if (!f) die("cannot write %s", blank);
            static const char zero[512];
            for (int i = 0; i < 1760; ++i) fwrite(zero, 1, sizeof zero, f);
            fclose(f);
        }
        game_path = blank;
    }
    check_roms();
    if (dump_path && max_frames < 0) {
        max_frames = dump_frame;
    }

    core.lib = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!core.lib) {
        die("%s", dlerror());
    }
    core.set_environment = symbol("retro_set_environment");
    core.set_video_refresh = symbol("retro_set_video_refresh");
    core.set_audio_sample = symbol("retro_set_audio_sample");
    core.set_audio_sample_batch = symbol("retro_set_audio_sample_batch");
    core.set_input_poll = symbol("retro_set_input_poll");
    core.set_input_state = symbol("retro_set_input_state");
    core.set_controller_port_device = symbol("retro_set_controller_port_device");
    core.init = symbol("retro_init");
    core.deinit = symbol("retro_deinit");
    core.run = symbol("retro_run");
    core.get_system_av_info = symbol("retro_get_system_av_info");
    core.load_game = symbol("retro_load_game");
    core.unload_game = symbol("retro_unload_game");
    core.serialize_size = dlsym(core.lib, "retro_serialize_size");
    core.serialize = dlsym(core.lib, "retro_serialize");
    core.unserialize = dlsym(core.lib, "retro_unserialize");

    if (verbose) {
        for (int i = 0; i < option_count; ++i) fprintf(stderr, "ami9000_host: core option %s = %s\n", options[i].key, options[i].value);
    }
    core.set_environment(environment);
    core.init();
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.set_controller_port_device(0, RETRO_DEVICE_MOUSE);
    core.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    struct retro_game_info game = {.path = game_path};
    if (!core.load_game(&game)) {
        die("core rejected %s (check --system holds the Kickstart ROM)", game_path);
    }
    core.get_system_av_info(&av);
    if (plugin_path || script_path) {
        e9k_read_memory = (size_t (*)(uint32_t, uint8_t *, size_t))dlsym(core.lib, "e9k_debug_read_memory");
        e9k_write_memory = (int (*)(uint32_t, uint32_t, size_t))dlsym(core.lib, "e9k_debug_write_memory");
        e9k_add_hook = (void (*)(uint32_t))dlsym(core.lib, "e9k_debug_add_hook");
        e9k_remove_hook = (void (*)(uint32_t))dlsym(core.lib, "e9k_debug_remove_hook");
        e9k_set_hook_callback = (void (*)(void (*)(uint32_t)))dlsym(core.lib, "e9k_debug_set_hook_callback");
        e9k_pause = (void (*)(void))dlsym(core.lib, "e9k_debug_pause");
        if (!e9k_add_hook || !e9k_set_hook_callback) die("the core has no hooks: run ami9000/host/build_core.sh");
        e9k_read_regs = (size_t (*)(uint32_t *, size_t))dlsym(core.lib, "e9k_debug_read_regs");
        e9k_read_cycles = (uint64_t (*)(void))dlsym(core.lib, "e9k_debug_read_cycle_count");
        e9k_set_write_observer = (void (*)(void (*)(uint32_t, uint32_t, uint32_t, uint32_t)))dlsym(core.lib, "e9k_debug_set_custom_write_observer");
        e9k_write_reg = (int (*)(uint32_t, uint32_t))dlsym(core.lib, "e9k_debug_write_reg");
        e9k_is_paused = (int (*)(void))dlsym(core.lib, "e9k_debug_is_paused");
        e9k_resume = (void (*)(void))dlsym(core.lib, "e9k_debug_resume");
        e9k_step_instr = (void (*)(void))dlsym(core.lib, "e9k_debug_step_instr");
        e9k_custom_regs = (const custom_reg_state *(*)(void))dlsym(core.lib, "e9k_debug_amiga_get_custom_regs");
        host_api = (ami_host_api){.abi = AMI9000_LOADER_ABI, .read_memory = api_read_memory,
            .write_memory = api_write_memory, .pixel = api_pixel, .mouse_move = api_mouse_move,
            .mouse_button = api_mouse_button, .joystick = api_joystick, .key = api_key, .log = api_log, .quit = api_quit,
            .add_breakpoint = api_add_breakpoint, .remove_breakpoint = api_remove_breakpoint,
            .read_regs = api_read_regs, .write_reg = api_write_reg, .pause = api_pause, .resume = api_resume,
            .is_paused = api_is_paused, .custom_write = api_custom_write, .take_input = api_take_input,
            .panel = api_panel, .snapshot_save = api_snapshot_save, .snapshot_load = api_snapshot_load,
            .read_cycles = api_read_cycles, .trace_writes = api_trace_writes, .take_writes = api_take_writes,
            .disk_count = api_disk_count, .disk_index = api_disk_index,
            .disk_eject = api_disk_eject, .disk_select = api_disk_select};
        e9k_set_hook_callback(core_hook);
    }
    if (plugin_path) {
        void *lib = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) die("%s", dlerror());
        ami9000_loader_fn entry = (ami9000_loader_fn)dlsym(lib, "ami9000_loader");
        plugin = entry ? entry() : NULL;
        if (!plugin || plugin->abi != AMI9000_LOADER_ABI) die("%s is not an ABI %d loader", plugin_path, AMI9000_LOADER_ABI);
        if (plugin->init && plugin->init(&host_api, plugin_args)) die("loader %s failed to initialise", plugin->name);
    }
    if (script_path) {
        if (control_path) lua_script_set_control(control_path);
        lua_script_start(&host_api);
    }
    double fps = av.timing.fps > 1 ? av.timing.fps : 50.0;

    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
            die("SDL: %s", SDL_GetError());
        }
        float aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : 1.33333f;
        window = SDL_CreateWindow("ami9000", (int)av.geometry.base_width * scale / 2,
                                  (int)(av.geometry.base_width / aspect) * scale / 2, 0);
        if (window && window_x >= 0) SDL_SetWindowPosition(window, window_x, window_y);
        if (verbose) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            fprintf(stderr, "ami9000_host: base %ux%u aspect %.4f window %dx%d\n", av.geometry.base_width,
                    av.geometry.base_height, aspect, ww, wh);
        }
        renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
        if (!renderer) {
            die("SDL: %s", SDL_GetError());
        }
        SDL_GetWindowSize(window, &base_window_width, &base_window_height);
        SDL_SetRenderVSync(renderer, 1);
        SDL_AudioSpec want = {.format = SDL_AUDIO_S16, .channels = 2,
                              .freq = (int)(av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100)};
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
        if (audio_stream) {
            SDL_ResumeAudioStreamDevice(audio_stream);
        }
    }

    Uint64 start = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    while (!quit_requested && (max_frames < 0 || frame_count < max_frames)) {
        host_api.frame = frame_count;
        if (snapshot_request) {
            api_snapshot_save(NULL);
            snapshot_request = 0;
        }
        if (disk_phase && --disk_phase == 0) {
            api_disk_select(disk_next);
            api_disk_eject(0);
        }
        if (plugin && plugin->before_frame) plugin->before_frame(&host_api);
        if (script_path) {
            lua_script_poll_control(e9k_is_paused && e9k_is_paused());
            lua_script_before_frame(&host_api);
        }
        core.run();
        if (!headless && e9k_is_paused && e9k_is_paused()) present(); /* a paused core presents no frame: keep the window current */
        /* ami.step pauses the machine and walks it instruction by instruction; nothing else ever stops it. */
        while (script_path && lua_script_stepping() && e9k_is_paused && e9k_is_paused() && !quit_requested) {
            uint32_t regs[18] = {0};
            if (e9k_read_regs) e9k_read_regs(regs, 18);
            if (!lua_script_step(&host_api, regs[17])) {
                e9k_resume();
                break;
            }
            e9k_step_instr();
            core.run();
        }
        ++frame_count;
        if (!headless) {
            double due = (double)frame_count / fps;
            double now = (double)(SDL_GetPerformanceCounter() - start) / freq;
            if (due > now) {
                SDL_Delay((Uint32)((due - now) * 1000.0));
            } else if (now - due > 0.1) {
                /* behind by more than a tenth of a second (a script that boots, loads a state or starts a peer takes seconds of real time inside the frames it counts): do not run
                 * flat out to catch up (the game would be seen sped up, and a peer's sound would play late), start counting from now */
                start += (Uint64)((now - due) * freq);
            }
        }
    }
    if (plugin && plugin->shutdown) plugin->shutdown();
    if (script_path) lua_script_shutdown();
    core.unload_game();
    core.deinit();
    fprintf(stderr, "ami9000_host: ran %ld frames\n", frame_count);
    if (verbose) {
        double rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100;
        fprintf(stderr, "ami9000_host: audio %lu sample frames, %.1f%% of the %.0f expected for %ld frames at %.2f Hz\n",
                audio_frames_total, 100.0 * (double)audio_frames_total / (rate / fps * (double)frame_count), rate / fps * (double)frame_count, frame_count, fps);
    }
    if (script_path && lua_script_exit_code()) return lua_script_exit_code();
    return dump_path ? 1 : 0;
}
