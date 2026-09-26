#include "host/devices.h"

#include "rom/identify.h"
#include "util/fs.h"

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char root_path[4096];
static bool root_ready;

static char last_error[256];

static bool fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, args);
    va_end(args);
    fprintf(stderr, "devices: %s\n", last_error);
    return false;
}

const char *mh_devices_last_error(void)
{
    return last_error[0] ? last_error : NULL;
}

static bool make_dir(const char *path)
{
    if (!mh_mkdir(path) || errno == EEXIST) return true;
    return fail("cannot create %s: %s", path, strerror(errno));
}

/* Every directory along a path, so a root three levels down from nothing
 * still appears. */
static bool make_dirs(const char *path)
{
    char work[4096];
    if (strlen(path) >= sizeof(work)) return false;
    snprintf(work, sizeof(work), "%s", path);
    char *start = work + 1;
#ifdef _WIN32
    /* "C:/..." -- the drive is there already and cannot be made. */
    if (work[0] && work[1] == ':') start = work + 2 + (work[2] != 0);
#endif
    for (char *p = start; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (!make_dir(work)) return false;
        *p = '/';
    }
    return make_dir(work);
}

const char *mh_devices_root(void)
{
    if (root_ready) return root_path[0] ? root_path : NULL;
    root_ready = true;

    const char *override = getenv("MH_DEVICES_DIR");
    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
#ifdef _WIN32
    /* Where Windows keeps per-user application data. HOME is set, or not,
     * differently by every shell, and would move the devices around. */
    const char *local = getenv("LOCALAPPDATA");
#endif
    if (override && *override)
        snprintf(root_path, sizeof(root_path), "%s", override);
#ifdef _WIN32
    else if (local && *local)
        snprintf(root_path, sizeof(root_path), "%s/magichat/devices", local);
#endif
    else if (data && *data)
        snprintf(root_path, sizeof(root_path), "%s/magichat/devices", data);
    else if (home && *home)
        snprintf(root_path, sizeof(root_path), "%s/.local/share/magichat/devices",
                 home);
    else {
        /* Nowhere to put them. Everything that does not need a device still
         * works, so this is not fatal. */
        root_path[0] = 0;
        return NULL;
    }
#ifdef _WIN32
    for (char *p = root_path; *p; p++)
        if (*p == '\\') *p = '/';
#endif
    if (!make_dirs(root_path)) { root_path[0] = 0; return NULL; }
    return root_path;
}

/*
 * A directory name from a device name: lowercase, with anything that is not
 * a letter or a digit becoming a dash. The point is a listing someone can
 * read and a path that needs no quoting, not a reversible encoding -- the
 * real name is kept in a file beside it.
 */
static void make_id(const char *name, char *id, size_t cap)
{
    size_t at = 0;
    bool dash = false;
    for (const char *p = name; *p && at + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            id[at++] = (char)c; dash = false;
        } else if (c >= 'A' && c <= 'Z') {
            id[at++] = (char)(c - 'A' + 'a'); dash = false;
        } else if (!dash && at) {
            id[at++] = '-'; dash = true;
        }
    }
    while (at && id[at - 1] == '-') at--;
    if (!at) at = (size_t)snprintf(id, cap, "device");
    id[at] = 0;
}

static bool path_in(const char *id, const char *leaf, char *out, size_t cap)
{
    const char *root = mh_devices_root();
    if (!root) return false;
    int n = snprintf(out, cap, "%s/%s/%s", root, id, leaf);
    return n > 0 && (size_t)n < cap;
}

static bool read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n > 0;
}

static bool write_line(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return fail("cannot write %s: %s", path, strerror(errno));
    bool ok = fprintf(f, "%s\n", text) > 0;
    return fclose(f) == 0 && ok;
}

static bool exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

/* Fill in a device from its directory. False when the directory is not one:
 * a stray folder under the root is ignored rather than listed as a device
 * that cannot be started. */
static bool load(const char *id, mh_device *out)
{
    char rom[4096], state[4096], name[4096], machine[4096];
    if (!path_in(id, "rom", rom, sizeof(rom)) ||
        !path_in(id, "rom.state", state, sizeof(state)) ||
        !path_in(id, "name", name, sizeof(name)) ||
        !path_in(id, "machine", machine, sizeof(machine)))
        return false;
    if (!exists(rom)) return false;
    snprintf(out->id, sizeof(out->id), "%s", id);
    if (!read_line(name, out->name, sizeof(out->name)))
        snprintf(out->name, sizeof(out->name), "%s", id);
    if (!read_line(machine, out->machine, sizeof(out->machine)))
        out->machine[0] = 0;       /* made before this was written down */
    out->has_state = exists(state);
    return true;
}

bool mh_device_by_id(const char *id, mh_device *out)
{
    if (!id || !*id || mh_path_last_separator(id)) return false;
    return load(id, out);
}

bool mh_device_paths(const mh_device *d, char *rom, size_t rom_cap,
                      char *state, size_t state_cap)
{
    if (!d) return false;
    if (rom && !path_in(d->id, "rom", rom, rom_cap)) return false;
    if (state && !path_in(d->id, "rom.state", state, state_cap)) return false;
    return true;
}

unsigned mh_devices_list(mh_device *out, unsigned max)
{
    const char *root = mh_devices_root();
    if (!root || !out || !max) return 0;
    DIR *dir = opendir(root);
    if (!dir) return 0;

    /* Newest first, by when the device was last put down -- which is the
     * order someone thinks of their own devices in. */
    struct { mh_device d; long when; } found[MH_DEVICE_MAX];
    unsigned n = 0;
    const struct dirent *e;
    while ((e = readdir(dir)) && n < MH_DEVICE_MAX) {
        if (e->d_name[0] == '.') continue;
        if (!load(e->d_name, &found[n].d)) continue;
        char state[4096];
        struct stat st;
        found[n].when = 0;
        if (path_in(e->d_name, "rom.state", state, sizeof(state)) && !stat(state, &st))
            found[n].when = (long)st.st_mtime;
        else if (path_in(e->d_name, "rom", state, sizeof(state)) && !stat(state, &st))
            found[n].when = (long)st.st_mtime;
        n++;
    }
    closedir(dir);

    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j &&
             (found[j].when > found[j - 1].when ||
              (found[j].when == found[j - 1].when &&
               strcmp(found[j].d.id, found[j - 1].d.id) < 0)); j--) {
            typeof(found[0]) t = found[j];
            found[j] = found[j - 1];
            found[j - 1] = t;
        }

    unsigned written = n < max ? n : max;
    for (unsigned i = 0; i < written; i++) out[i] = found[i].d;
    return written;
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) {
        fprintf(stderr, "devices: cannot read %s: %s\n", from, strerror(errno));
        return false;
    }
    FILE *out = fopen(to, "wb");
    if (!out) {
        fail("cannot write into the device store: %s", strerror(errno));
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out)) ok = false;
    if (!ok) {
        fail("copying the firmware failed; is there room?");
        unlink(to);
    }
    return ok;
}

/*
 * What board this image is. Read once, at the moment a device is made, so
 * that listing devices later costs a few bytes rather than several megabytes
 * each -- and so that the device can be named after the machine rather than
 * after whatever the file was called.
 */
static mh_rom_device identify_rom(const char *rom_path)
{
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fail("cannot read that file: %s", strerror(errno));
        return MH_ROM_UNKNOWN;
    }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return MH_ROM_UNKNOWN; }
    long size = ftell(f);
    if (size <= 0 || size > 64 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fail("that file is empty, unreadable, or over 64 MiB");
        fclose(f);
        return MH_ROM_UNKNOWN;
    }
    uint8_t *data = malloc((size_t)size);
    bool read_ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) { free(data); return MH_ROM_UNKNOWN; }
    mh_rom_device found = mh_rom_identify(data, (size_t)size);
    free(data);
    if (found == MH_ROM_UNKNOWN) {
        /*
         * Refused rather than filed as unknown. A device is a thing you can
         * start, and a list is only worth trusting when everything in it
         * starts; an experimental image still runs from a path with --device.
         */
        fail("that is not a firmware image this knows");
        return MH_ROM_UNKNOWN;
    }
    return found;
}

bool mh_device_create(const char *rom_path, const char *name, mh_device *out)
{
    last_error[0] = 0;
    const char *root = mh_devices_root();
    if (!root || !rom_path) return fail("there is nowhere to keep devices");
    /*
     * The store may have gone since it was last looked at -- deleted, or on a
     * volume that is no longer mounted. Establishing it once and assuming it
     * stays is how a device store vanishing under a running emulator turned
     * into a button that did nothing.
     */
    if (!make_dirs(root)) return false;

    mh_rom_device board = identify_rom(rom_path);
    if (board == MH_ROM_UNKNOWN) return false;
    const char *machine = mh_rom_device_slug(board);

    /*
     * What to call it. A name given by hand is taken as given, including the
     * refusal below if it is already in use -- that is a person's deliberate
     * choice and worth telling them about.
     *
     * Left to itself, the machine names it. "Sony PIC-2000" is what someone
     * owns; "PIC-2000.rom" is what a file happened to be called, which is
     * often a download with a version or a mirror's name stuck to it. Owning
     * two of the same machine is ordinary, so a second one counts up rather
     * than failing.
     */
    char chosen[MH_DEVICE_NAME_MAX];
    char id[MH_DEVICE_ID_MAX];
    char dir[4096];
    if (name && *name) {
        snprintf(chosen, sizeof(chosen), "%s", name);
        make_id(chosen, id, sizeof(id));
        if (snprintf(dir, sizeof(dir), "%s/%s", root, id) >= (int)sizeof(dir))
            return false;
        if (exists(dir))
            return fail("there is already a device called \"%s\"", chosen);
    } else {
        const char *product = mh_rom_device_product(board);
        for (unsigned n = 1; ; n++) {
            /*
             * Parenthesised, because these machines are named by number.
             * "Sony PIC-2000 2" reads as a model -- there really is a
             * PIC-1000 and a PIC-2000 -- where "Sony PIC-2000 (2)" plainly
             * means the second one you own.
             */
            if (n == 1) snprintf(chosen, sizeof(chosen), "%s", product);
            else snprintf(chosen, sizeof(chosen), "%s (%u)", product, n);
            make_id(chosen, id, sizeof(id));
            if (snprintf(dir, sizeof(dir), "%s/%s", root, id) >= (int)sizeof(dir))
                return false;
            if (!exists(dir)) break;
            if (n == 99)
                return fail("there are already a great many %s devices", product);
        }
    }
    if (!make_dir(dir)) return false;

    char rom[4096], namefile[4096], machinefile[4096];
    if (!path_in(id, "rom", rom, sizeof(rom)) ||
        !path_in(id, "name", namefile, sizeof(namefile)) ||
        !path_in(id, "machine", machinefile, sizeof(machinefile)) ||
        !copy_file(rom_path, rom) || !write_line(namefile, chosen) ||
        !write_line(machinefile, machine)) {
        /* Leave nothing half-made: a directory with no firmware in it would
         * be listed as a device that cannot start. */
        unlink(rom);
        unlink(namefile);
        unlink(machinefile);
        rmdir(dir);
        return false;
    }
    if (out) {
        snprintf(out->id, sizeof(out->id), "%s", id);
        snprintf(out->name, sizeof(out->name), "%s", chosen);
        snprintf(out->machine, sizeof(out->machine), "%s", machine);
        out->has_state = false;
    }
    fprintf(stderr, "devices: created \"%s\" (%s) from %s\n", chosen, machine,
            rom_path);
    return true;
}

bool mh_device_rename(const mh_device *d, const char *name)
{
    char namefile[4096];
    if (!d || !name || !*name) return false;
    if (!path_in(d->id, "name", namefile, sizeof(namefile))) return false;
    return write_line(namefile, name);
}

bool mh_device_delete(const mh_device *d)
{
    const char *root = mh_devices_root();
    if (!root || !d || !d->id[0] || mh_path_last_separator(d->id)) return false;
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s/%s", root, d->id) >= (int)sizeof(dir))
        return false;
    DIR *open_dir = opendir(dir);
    if (!open_dir) return false;
    const struct dirent *e;
    while ((e = readdir(open_dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) <
            (int)sizeof(path))
            unlink(path);
    }
    closedir(open_dir);
    if (rmdir(dir)) {
        fprintf(stderr, "devices: cannot remove %s: %s\n", dir, strerror(errno));
        return false;
    }
    fprintf(stderr, "devices: deleted \"%s\"\n", d->name);
    return true;
}
