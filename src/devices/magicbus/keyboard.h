#ifndef MH_MAGICBUS_KEYBOARD_H
#define MH_MAGICBUS_KEYBOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Peripheral endpoint only. The MBUS controller owns transport and IRQs.
 * Stock DataRover driver evidence: docs/KEYBOARD.md. */
#define MH_MBKEY_CAPACITY 256
#define MH_MBKEY_ID UINT32_C(0x4d424b42) /* MBKB */
typedef struct {
    uint8_t queue[MH_MBKEY_CAPACITY];
    unsigned head, count;
    uint8_t leds, repeat;
    uint64_t dropped_events;
} mh_mb_keyboard;

bool mh_mb_keyboard_usage(unsigned usage, uint8_t *code, bool *extended);

void mh_mb_keyboard_init(mh_mb_keyboard *k);
/* AT set 2 make code, optionally E0 extended. Whole events are enqueued
 * atomically so overflow cannot strand a break or extended prefix. */
bool mh_mb_keyboard_key(mh_mb_keyboard *k, uint8_t code, bool extended, bool down);
/* ROM WritePeripheral command 5 payload: 'K', byte count, AT command bytes.
 * Unsupported or malformed packets return false without changing state. */
bool mh_mb_keyboard_write(mh_mb_keyboard *k, const uint8_t *data, size_t size);
/* Keyboard ReadPeripheral command 2 result: count byte then scan bytes.
 * Returns bytes written, including the count; never consumes without space. */
size_t mh_mb_keyboard_read(mh_mb_keyboard *k, uint8_t *data, size_t capacity);
#endif
