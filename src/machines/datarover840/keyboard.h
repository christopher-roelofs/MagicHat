#ifndef MRC_DATAROVER_KEYBOARD_H
#define MRC_DATAROVER_KEYBOARD_H
#include "devices/magicbus/keyboard.h"
#include "soc/tx39/tx39.h"

/* One reconstructed MBKB accessory. Host wiring is deliberately separate from
 * serial-console input. See docs/KEYBOARD.md for evidence and fidelity limits. */
typedef struct {
    mrc_mb_keyboard keys;
    tx39_mbus_port port;
    tx39_mbus *controller;
    bool assigned, notified;
    uint8_t selection, pending_read, write_size;
    uint8_t write_data[8];
    uint8_t held[32], release[32]; /* host-held keys; deferred break codes */
    uint64_t unknown_commands, rejected_writes, receive_errors;
} mrc_dr_keyboard;

void mrc_dr_keyboard_init(mrc_dr_keyboard *k, tx39_mbus *controller);
/* Connect explicitly with mrc_mbus_connect(controller, &k->port). */
bool mrc_dr_keyboard_key(mrc_dr_keyboard *k, uint8_t code, bool extended, bool down);
/* Service after guest execution, when the controller has switched to receive. */
void mrc_dr_keyboard_service(mrc_dr_keyboard *k);
bool mrc_dr_keyboard_host_key(mrc_dr_keyboard *k, unsigned usage, bool down, bool repeat);
void mrc_dr_keyboard_release(mrc_dr_keyboard *k);
#define MRC_DR_KEYBOARD_STATE_SIZE 384u
void mrc_dr_keyboard_encode(const mrc_dr_keyboard *k, uint8_t *data);
/* Validate before changing the device; restoration produces no line edges. */
bool mrc_dr_keyboard_decode(mrc_dr_keyboard *k, const uint8_t *data);
#endif
