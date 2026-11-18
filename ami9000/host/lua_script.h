/*
 * Lua driver for ami9000_host. A script is the whole per-game setup: it first
 * describes the machine with ami.machine{...} and calls ami.boot(); the host
 * then loads the core and runs the rest of the script, one resume per core
 * frame, so it reads linearly: ami.wait(n) yields until n more frames have
 * run. It is built on the same ami_host_api as native loader plugins. A script
 * patches a game by poking bytes into guest memory, by hooks (Lua run at an
 * instruction, from inside the core's instruction loop, stopping nothing) and by
 * placing small 68000 routines in guest memory.
 */
#ifndef AMI9000_LUA_SCRIPT_H
#define AMI9000_LUA_SCRIPT_H

#include <stdint.h>

#include "loader_api.h"

#define AMI_MAX_OPTS 64
#define AMI_MAX_ROMS 8
#define AMI_MAX_PEERS 4
#define AMI_MAX_PEER_ARGS 24

/* What ami.machine{} declared. Strings are owned by the script driver; paths
 * are already resolved against the script's directory. NULL/0 means "not set". */
typedef struct {
    const char *name, *model, *system, *save, *snapshots, *content, *core, *files;
    const char *plugin, *plugin_args;
    int scale, joystick_keys;
    struct { const char *key, *value; } opt[AMI_MAX_OPTS];
    int opt_count;
    struct { const char *name; uint32_t crc; } roms[AMI_MAX_ROMS];
    int rom_count;
    /* peers = { name = { "program", "arg", ... } }: processes the script may start with ami.peer(name) */
    struct { const char *name; const char *argv[AMI_MAX_PEER_ARGS + 1]; } peers[AMI_MAX_PEERS];
    int peer_count;
} ami_machine;

/* The command-line flags the host did not recognise, for the script as ami.argv. Call before lua_script_configure. */
void lua_script_set_argv(int count, const char **argv);
/* Load PATH and run it up to ami.boot(), collecting its ami.machine{} in *m.
 * args is exposed to the script as ami.args. Returns 0 on success. */
int lua_script_configure(const char *path, const char *args, ami_machine *m);
/* The core is up: from now on the ami.* machine functions work. */
void lua_script_start(const ami_host_api *host);
/* ami9000_host --control PATH: serve a Unix socket (mode 0600) at PATH. Call before lua_script_start. */
void lua_script_set_control(const char *path);
/* Run the Lua lines clients have sent (see ctl.py); once per host loop. When `paused` it also waits a little for
 * input, so a paused headless host does not spin. */
void lua_script_poll_control(int paused);
/* Resume the script for the frame about to run. */
void lua_script_before_frame(const ami_host_api *host);
/* The 68000 reached a hook added by ami.hook(): run its Lua function (from inside the core's instruction loop). */
void lua_script_breakpoint(const ami_host_api *host, uint32_t pc);
/* ami.step: whether a trace is active, and one step of it (the machine is paused at pc). Returns 1 to keep
 * stepping, 0 when the trace ended. */
int lua_script_stepping(void);
int lua_script_step(const ami_host_api *host, uint32_t pc);
void lua_script_shutdown(void);
/* Exit status requested by ami.quit(code), or 3 if the script raised an error. */
int lua_script_exit_code(void);

#endif
