#ifndef MRC_RUNTIME_MACHINE_H
#define MRC_RUNTIME_MACHINE_H
#include <stdbool.h>
#include <stdint.h>
/* Borrowed, coarse-grained view of a board. The caller owns its lifetime.
 * No callbacks are dispatched for individual CPU instructions or bus accesses.
 * run_slots preserves the legacy CLI budget, including CPU32 idle slots.
 * Time is emulated time, not host wall time or retired instruction count.
 * Optional callbacks are NULL when a board does not implement that capability.
 * Frame format is board-described (0..255 grayscale or 0..3 LCD levels).
 */
typedef void (*mrc_runtime_audio_sink)(void *, int16_t);
typedef struct mrc_runtime_ops {
    uint64_t (*run_slots)(void *, uint64_t);
    uint64_t (*slots)(void *);
    uint64_t (*elapsed_ns)(void *);
    bool (*stopped)(void *);
    bool (*frame)(void *, uint8_t *, unsigned *, unsigned *);
    bool (*pen)(void *, bool, unsigned, unsigned);
    void (*option)(void *, bool);
    /*
     * Whether the option key is held. It is a line the guest samples, kept in
     * a device register and carried in the state, so a window that remembered
     * its own answer would contradict a restored machine: the guest would
     * still be seeing the key down while the button showed it up.
     */
    bool (*option_held)(void *);
    void (*key_byte)(void *, uint8_t);
    unsigned (*audio_rate)(void *);
    void (*audio_sink)(void *, mrc_runtime_audio_sink, void *);
    bool (*save_state)(void *, const char *);
    /*
     * Start sending a package over the guest's own link, and hand back the
     * link so a window can watch it. NULL when this board has no package-link
     * implementation.
     */
    struct mrc_pclink *(*install)(void *, const char *path);
    bool (*save_screen)(void *, const char *);
    void (*power_button)(void *, bool);
    /* USB HID keyboard usage ID, physical make/break; true selects the
     * accessory path even for an unsupported key (never leaks to UART). */
    bool (*keyboard_key)(void *, unsigned, bool, bool);
    void (*keyboard_release)(void *);
    /* Optional removable SRAM-card controls for frontends. */
    bool (*card_present)(void *, unsigned);
    const char *(*card_name)(void *, unsigned);
    const char *(*card_type)(void *, unsigned);
    bool (*card_insert_sram)(void *, unsigned, const char *);
    bool (*card_insert_ne2000)(void *, unsigned);
    bool (*card_eject)(void *, unsigned);
    /* False means unsupported or an explicit sensor override is active. */
    bool (*host_battery)(void *, int);
    bool (*host_adapter)(void *, bool);
} mrc_runtime_ops;
typedef enum { MRC_FRAME_GRAY8, MRC_FRAME_LCD2 } mrc_frame_format;
typedef struct {
    void *board;
    const char *machine_id;
    uint32_t nominal_slots_hz;
    const mrc_runtime_ops *ops;
    mrc_frame_format frame_format;
} mrc_runtime;
/* Integer conversion that avoids multiplying the entire cumulative counter.
 * For clock rates in this emulator, the remainder multiplication fits uint64_t.
 */
static inline uint64_t mrc_time_ns(uint64_t cycles, uint32_t hz) {
    return cycles / hz * 1000000000ull + cycles % hz * 1000000000ull / hz;
}
#endif
