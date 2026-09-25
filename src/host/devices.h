/*
 * devices.h — the virtual devices a person owns.
 *
 * A device is not a ROM file. It is a machine with a history: the firmware it
 * came with, everything it has been doing since, and whatever is plugged into
 * it. You make one once, from a ROM image, and after that you never think
 * about that file again -- you pick the device and carry on where you were.
 *
 * That is why a device owns a copy of its firmware rather than pointing at
 * one. A ROM referred to by path is a device that stops existing when the
 * file is renamed, moved to another machine, or tidied into a different
 * folder, and a device that can be broken by tidying up is not a device. The
 * copy costs a few megabytes and buys a thing that can be moved, backed up
 * and deleted as one piece.
 *
 * On disk, one directory each, all together:
 *
 *   devices/<id>/rom        the firmware, copied in when the device was made
 *   devices/<id>/rom.state  the machine, written whenever it is put down
 *   devices/<id>/name       what to call it
 *   devices/<id>/machine    which board it is: pic2000, datarover840, ...
 *
 * Flat rather than filed by machine. Sorting them into folders would put
 * structure on disk that nobody asked to navigate: what a person wants is to
 * see their devices and which kind each one is, and that is a column in a
 * list, not a directory to walk into.
 *
 * The machine is written down rather than worked out on demand, because
 * working it out means reading several megabytes of firmware, and the list is
 * rebuilt every time it is opened.
 *
 * Plain files rather than one record, because each is written at a different
 * moment by a different part of the program, and a single record would mean
 * reading and rewriting the whole thing to change any of them.
 *
 * The state is named for the firmware beside it on purpose. A device is then
 * nothing more than a ROM path to everything downstream -- the same
 * <rom>.state a ROM given on the command line gets -- so saving, resuming and
 * remembering which card was in a slot all work on a device without knowing
 * that devices exist.
 *
 * The id is derived from the name, so a directory listing is legible and a
 * device can be found by hand. Nothing in here knows about SDL or about any
 * particular machine: which board a device is, is decided by looking at its
 * ROM, the same way it is decided for a ROM given on the command line.
 */
#ifndef MRC_HOST_DEVICES_H
#define MRC_HOST_DEVICES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MRC_DEVICE_ID_MAX   64
#define MRC_DEVICE_NAME_MAX 96
#define MRC_DEVICE_MAX      64      /* more than anyone will make by hand */

#define MRC_DEVICE_MACHINE_MAX 24

typedef struct {
    char id[MRC_DEVICE_ID_MAX];      /* the directory name */
    char name[MRC_DEVICE_NAME_MAX];  /* what to show */
    char machine[MRC_DEVICE_MACHINE_MAX]; /* pic2000, datarover840, ... */
    bool has_state;                  /* false until it has been run once */
} mrc_device;

/*
 * Where devices are kept. MRC_DEVICES_DIR overrides it; otherwise it is
 * under the usual place for application data on this platform. Created on
 * first use. Returns NULL only when no directory can be established, which
 * leaves the program working exactly as it did before devices existed.
 */
const char *mrc_devices_root(void);

/* Every device, newest first. Returns how many were written to `out`. */
unsigned mrc_devices_list(mrc_device *out, unsigned max);

/*
 * Make a device from a ROM image, copying the firmware in.
 *
 * `name` may be NULL, in which case the image's filename is used -- that is
 * the default a person is offered and can change, not the only choice.
 *
 * Which kind of machine it is comes from the image itself, the same way it
 * does for a ROM given on the command line. An image nothing recognises is
 * refused rather than filed under "unknown": a device is a thing you can
 * start, and the list is only worth trusting if everything in it starts.
 * Experimental images still run from a path with --device.
 *
 * Fails too if a device of that name already exists, across all machines --
 * two devices with one name is a choice nobody can make from a list.
 */
bool mrc_device_create(const char *rom_path, const char *name, mrc_device *out);

/* Rename, keeping the id and so keeping the directory. */
bool mrc_device_rename(const mrc_device *d, const char *name);

/* Remove a device and everything in it. There is no undo: the machine, its
 * firmware copy and its history all go. */
bool mrc_device_delete(const mrc_device *d);

/*
 * The files inside a device. Either pointer may be NULL when that path is
 * not wanted. The state path is returned whether or not it exists yet --
 * it is where the state will be written.
 */
bool mrc_device_paths(const mrc_device *d, char *rom, size_t rom_cap,
                      char *state, size_t state_cap);

/* Find one by id, for resuming whatever was last in use. */
bool mrc_device_by_id(const char *id, mrc_device *out);

/*
 * Why the last thing failed, in words meant for a person.
 *
 * Everything here reports on stderr as well, which is the right place when a
 * command line is what asked. A window has no stderr anyone is looking at, so
 * it needs the reason back in its hand: the alternative is a button that
 * sometimes does nothing and never says why, which is what this interface did
 * until a device store was deleted underneath a running emulator and the
 * window sat there looking fine.
 */
const char *mrc_devices_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* MRC_HOST_DEVICES_H */
