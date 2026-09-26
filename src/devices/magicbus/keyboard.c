#include "devices/magicbus/keyboard.h"
#include <string.h>

void mh_mb_keyboard_init(mh_mb_keyboard *k)
{
    memset(k, 0, sizeof(*k));
}

bool mh_mb_keyboard_key(mh_mb_keyboard *k, uint8_t code, bool extended, bool down)
{
    /* The ROM decodes E0/E1 and F0 prefixes at 13C27714..13C27770.
     * E1/Pause needs a separate multi-byte sequence and isn't accepted here. */
    if (!code || code == 0xe0 || code == 0xe1 || code == 0xf0 || code == 0xf8)
        return false;
    unsigned n = 1 + (extended ? 1 : 0) + (down ? 0 : 1);
    if (n > MH_MBKEY_CAPACITY - k->count) {
        k->dropped_events++;
        return false;
    }
    uint8_t bytes[3]; unsigned i = 0;
    if (extended) bytes[i++] = 0xe0;
    if (!down) bytes[i++] = 0xf0;
    bytes[i++] = code;
    for (unsigned j = 0; j < n; ++j)
        k->queue[(k->head + k->count++) % MH_MBKEY_CAPACITY] = bytes[j];
    return true;
}

bool mh_mb_keyboard_write(mh_mb_keyboard *k, const uint8_t *data, size_t size)
{
    if (size < 2 || data[0] != 'K' || data[1] > size - 2)
        return false;
    /* Accept only complete sequences actually emitted by the stock driver:
     * 13C27C38 reset, 13C27C84 LEDs, 13C27CD4 repeat + enable scanning.
     * This is the Magic Bus adapter envelope, not raw PS/2 ACK traffic. */
    if (data[1] == 1 && data[2] == 0xff) {
        k->head = k->count = 0;
        k->leds = k->repeat = 0;
        return true;
    }
    if (data[1] == 2 && data[2] == 0xed && !(data[3] & ~7u)) {
        k->leds = data[3];
        return true;
    }
    if (data[1] == 3 && data[2] == 0xf3) {
        /* The three command bytes are F3, rate, F4. The ROM pads the
         * complete Magic Bus envelope to eight bytes. */
        if (size < 5 || data[4] != 0xf4 || (data[3] & 0x80)) return false;
        k->repeat = data[3];
        return true;
    }
    return false;
}

size_t mh_mb_keyboard_read(mh_mb_keyboard *k, uint8_t *data, size_t capacity)
{
    if (!capacity) return 0;
    /* The guest requests 16 bytes and requires count < returned length
     * at 13C27BA0. Prefixes may span reads: modifiers persist in the driver. */
    size_t n = k->count;
    if (n > capacity - 1) n = capacity - 1;
    if (n > 255) n = 255;
    data[0] = (uint8_t)n;
    for (size_t i = 0; i < n; ++i)
        data[i + 1] = k->queue[(k->head + i) % MH_MBKEY_CAPACITY];
    k->head = (k->head + n) % MH_MBKEY_CAPACITY;
    k->count -= n;
    return n + 1;
}
