#include "host/state.h"
#include "host/import.h"

#include <stdio.h>
#include <string.h>

bool mrc_state_path_for(const char *rom, char *out, size_t cap)
{
    if (!rom || !out || !cap) return false;
    size_t n = strlen(rom), stem = n;
    /*
     * The extension, if the name has one. A dot in a directory along the way
     * is not one, and neither is a leading dot: "roms/PIC-2000.rom" has an
     * extension and ".hidden" does not.
     */
    const char *slash = strrchr(rom, '/'), *dot = strrchr(rom, '.');
    if (dot && (!slash || dot > slash + 1) && dot != rom) stem = (size_t)(dot - rom);
    if (stem + sizeof(".state") > cap) return false;
    memcpy(out, rom, stem);
    memcpy(out + stem, ".state", sizeof(".state"));
    /* A device launched from its own state file would save over it with a
     * machine that never booted. */
    if (!strcmp(out, rom)) {
        fprintf(stderr, "state: a device cannot be launched from its own "
                "state file\n");
        return false;
    }
    return true;
}

/* One machine to a process, so one path. Copied rather than borrowed: the
 * command line it came from outlives this, but a caller building the name
 * on its stack does not. */
static char path[4096];
static bool have_path;

void mrc_state_set_path(const char *p)
{
    if (!p || strlen(p) >= sizeof(path)) { have_path = false; return; }
    memcpy(path, p, strlen(p) + 1);
    have_path = true;
}

const char *mrc_state_path(void)
{
    return have_path ? path : NULL;
}

static char device_id[128];
static char wanted_id[128];
static bool wanted;
static bool stop_requested;

void mrc_state_set_device_id(const char *id)
{
    if (!id || strlen(id) >= sizeof(device_id)) { device_id[0] = 0; return; }
    memcpy(device_id, id, strlen(id) + 1);
}

const char *mrc_state_device_id(void)
{
    return device_id[0] ? device_id : NULL;
}

void mrc_state_request_device(const char *id)
{
    if (!id || strlen(id) >= sizeof(wanted_id)) return;
    memcpy(wanted_id, id, strlen(id) + 1);
    wanted = true;
    stop_requested = false;
}

bool mrc_state_device_requested(void)
{
    return wanted || stop_requested;
}

void mrc_state_request_stop(void)
{
    wanted = false;
    stop_requested = true;
}

bool mrc_state_take_stop(void)
{
    bool stop = stop_requested;
    stop_requested = false;
    return stop;
}

const char *mrc_state_take_requested_device(void)
{
    if (!wanted) return NULL;
    wanted = false;
    return wanted_id;
}

#ifndef __ANDROID__
/*
 * A desktop has no picker of its own worth preferring: the emulator's own
 * file list can open anything the person can, and putting a system dialog in
 * front of it would only be a second way to do the same thing. Android
 * replaces these in its own entry point, where the intent and the activity
 * result live.
 */
bool mrc_import_available(void) { return false; }
bool mrc_import_request(mrc_import_kind kind) { (void)kind; return false; }
bool mrc_import_pending(mrc_import_kind kind) { (void)kind; return false; }
bool mrc_import_result(mrc_import_kind kind, char *path, size_t path_size,
                       char *error, size_t error_size) {
    (void)kind; (void)path; (void)path_size; (void)error; (void)error_size;
    return false;
}
void mrc_import_cancel(mrc_import_kind kind) { (void)kind; }
#endif
