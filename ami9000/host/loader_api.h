/*
 * ami9000_host loader plugin ABI.
 *
 * A per-game loader is a shared object compiled natively for the host. It
 * replaces what an in-guest launcher (WHDLoad slave, boot script) would do:
 * it observes the running machine through guest memory and the video frame,
 * and drives it with injected input and memory writes. Nothing runs inside
 * the emulated 68000 for it. Export ami9000_loader() and name the object with
 * `plugin = "FILE.so"` in ami.machine{} (or --plugin).
 */
#ifndef AMI9000_LOADER_API_H
#define AMI9000_LOADER_API_H

#include <stddef.h>
#include <stdint.h>

#define AMI9000_LOADER_ABI 2

/* Input the host processed, for a loader that forwards it (e.g. to a peer): 'k' key (a = libretro key, b = 1 down),
 * 'm' mouse motion (a = dx, b = dy), 'b' button (a = 0 left / 1 right, b = 1 down). */
typedef struct ami_input_event {
    int kind;
    int a, b;
} ami_input_event;

typedef struct ami_host_api {
    uint32_t abi;
    long frame;   /* frames run so far (the frame about to run in before_frame) */
    unsigned screen_width, screen_height; /* last presented frame, 0 before the first */

    /* Guest bus memory (Chip/Slow/Fast, as the 68000 sees it). */
    size_t (*read_memory)(uint32_t address, void *out, size_t size);
    int (*write_memory)(uint32_t address, uint32_t value, size_t size); /* size 1, 2 or 4 */

    /* Last presented frame as 0xAARRGGBB; (x, y) outside the frame gives 0. */
    uint32_t (*pixel)(unsigned x, unsigned y);

    /* Input injected on top of the user's own: mouse counts and buttons (0 left, 1 right). */
    void (*mouse_move)(int dx, int dy);
    void (*mouse_button)(int button, int down);

    /* Keyboard injection by libretro key code (RETROK_*, from libretro.h). */
    void (*key)(unsigned retro_key, int down);

    void (*log)(const char *fmt, ...);
    void (*quit)(void);

    /* ABI 2: hooks. add_breakpoint(address) makes the core call the loader's breakpoint callback when the 68000 is
     * about to execute the instruction at address. It does not stop anything: the machine keeps running, the frame
     * is not cut short, audio and video carry on. In the callback read_regs/write_reg (0-7 D0-D7, 8-15 A0-A7,
     * 16 SR, 17 PC) inspect or change the CPU: writing PC redirects it (the instruction at address is then not
     * executed). To stop the whole machine, call pause() (e.g. from the callback); a paused machine is frozen and
     * silent until resume(). The host does not stop the machine for you. */
    void (*add_breakpoint)(uint32_t address);
    void (*remove_breakpoint)(uint32_t address);
    size_t (*read_regs)(uint32_t *out, size_t count); /* up to 18 values, as write_reg indexes them */
    int (*write_reg)(unsigned index, uint32_t value);
    void (*pause)(void);
    void (*resume)(void);
    int (*is_paused)(void);
    /* Last value written to a custom chip register (offset from $DFF000, e.g. $096 for DMACON)
     * and the PC of that write. Returns 0 if the core does not track it. */
    int (*custom_write)(unsigned offset, uint16_t *value, uint32_t *pc);
    /* Input events processed since the last call (oldest first); returns how many were copied, at most max. */
    size_t (*take_input)(ami_input_event *out, size_t max);
    /* Show an image beside the emulator frame: slot 0..3 left to right, RGB bytes (3 per pixel, w x h), each pixel drawn
     * sx by sy frame pixels, y frame rows below the top. The image stays until replaced. No effect headless. */
    void (*panel)(int slot, const uint8_t *rgb, int w, int h, int sx, int sy, int y);
    /* Machine snapshots (the core's serialised state: RAM, chips, CPU, disk). a name without '/' is a file in the
     * snapshot directory (--snapshots DIR / machine.snapshots, default the save directory), NULL saves frame-N.state. Call between frames (before_frame or a script
     * line), never from a hook. Return 1 on success. Appended after ABI 2 was fixed: a loader built earlier never
     * reads them. */
    int (*snapshot_save)(const char *path);
    int (*snapshot_load)(const char *path);
    /* CPU cycles run since power on (0 if the core does not count them): the machine's own clock. Appended after the
     * snapshot calls: a loader built earlier never reads it. */
    uint64_t (*read_cycles)(void);
    /* Custom register write trace (needs a core built with patches/ami9000-custom-write-observer.patch): trace_writes(1)
     * starts recording every write, with the cycle count, the register, the value, the PC and whether the copper made it;
     * take_writes returns them as text lines `cycles offset value pc copper` and clears the record (the caller frees). */
    void (*trace_writes)(int on);
    char *(*take_writes)(size_t *length);
    /* Joystick (port 1) injected on top of the user's own: a mask of 1 << RETRO_DEVICE_ID_JOYPAD_UP, _DOWN, _LEFT, _RIGHT
     * and _B (the fire button); the state stays until the next call. Appended after take_writes: a loader built
     * earlier never reads it. */
    void (*joystick)(unsigned mask);
} ami_host_api;

typedef struct ami_loader {
    uint32_t abi;
    const char *name;
    /* args is the `plugin_args` text of ami.machine{}, or "". Return 0 on success. */
    int (*init)(const ami_host_api *host, const char *args);
    /* Called before every core frame. */
    void (*before_frame)(const ami_host_api *host);
    /* Optional (ABI 2): the 68000 reached a hook added with add_breakpoint; see above. */
    void (*breakpoint)(const ami_host_api *host, uint32_t pc);
    void (*shutdown)(void);
} ami_loader;

typedef const ami_loader *(*ami9000_loader_fn)(void);

#endif
