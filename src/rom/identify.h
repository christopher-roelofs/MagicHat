#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MH_ROM_UNKNOWN, MH_ROM_DATAROVER840, MH_ROM_PIC2000,
    MH_ROM_PIC1000, MH_ROM_HIX300, MH_ROM_ENVOY
} mh_rom_device;

mh_rom_device mh_rom_identify(const uint8_t *data, size_t size);
/* What the machine is, for a report: the product and the architecture. */
const char *mh_rom_device_name(mh_rom_device device);
/*
 * What the machine is called, for a person: the product on its own. This is
 * what a new device is named after, because "Sony PIC-2000" is what someone
 * owns and "PIC-2000.rom" is only what the file on their disk happened to be
 * called -- often a download with a version or a mirror's name stuck to it.
 */
const char *mh_rom_device_product(mh_rom_device device);
/* The same machine as a short word: "pic2000", "datarover840". These are the
 * values --device takes, and the folder a device of that kind is kept in, so
 * that a listing on disk reads the same as the command line. */
const char *mh_rom_device_slug(mh_rom_device device);
mh_rom_device mh_rom_device_from_slug(const char *slug);
