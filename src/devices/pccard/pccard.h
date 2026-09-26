/*
 * pccard.h — a PC Card, as the board sees it.
 *
 * Distinct from glacier.h, which is the *controller*: two of those, one per
 * slot, with a register file we can read and write. This is the thing in the
 * slot, and the board gives each slot two 64 MB windows:
 *
 *   slot 1   0x08000000 (window A)   0x24000000 (window B)
 *   slot 2   0x0C000000 (window A)   0x28000000 (window B)
 *
 * PCMCIA cards present attribute, common-memory and I/O spaces. Observed
 * SRAM traffic uses A for even-addressed CIS bytes and B for common memory;
 * NE2000 uses A for CIS and I/O at +0x300. The complete Glacier decoder is
 * still unknown. See docs/STORAGE.md and docs/NETWORKING.md.
 *
 * A card is a device rather than a block of memory even when it holds only
 * memory, for two reasons. Reads have to be observable -- a ROM region's
 * accesses are invisible to --log-mmio, so the previous model could not say
 * what the OS read from a card or from which window. And a network card is
 * not memory at all: it is registers with side effects, and it needs this
 * interface to exist.
 *
 * Card wiring is outside the legacy snapshot layout. Card state must not be
 * silently lost: snapshots with NE2000 or writable SRAM attached are
 * rejected until a card-state serializer is implemented.
 */
#ifndef MH_PCCARD_H
#define MH_PCCARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "devices/ne2000/ne2000.h"

typedef enum {
    MH_PCCARD_WINDOW_A,
    MH_PCCARD_WINDOW_B,
    MH_PCCARD_NWINDOW,
} mh_pccard_window;

typedef struct mh_pccard mh_pccard;

/* A data modem card's UART and Hayes responder (modem_card.c). */
typedef struct {
    uint8_t  ier, lcr, mcr, fcr, scr, dll, dlm, dcd, msr_delta;
    bool     thre_pending, echo, verbose, online, connected_once;
    uint8_t  rx[4096];
    unsigned rx_head, rx_tail;
    char     line[128];
    unsigned line_len, plus;
    uint64_t now_ns, next_rx_ns, last_tx_ns, tx_bytes, rx_bytes;
    unsigned log_io, log_data;
    void    *link;                     /* an mh_serial for the far end */
} mh_modem;

/*
 * What a particular kind of card does. A memory card serves bytes; a network
 * card will do rather more. Returning false from read means "this card does
 * not drive that address", which surfaces as a floating read rather than as a
 * quietly invented zero.
 */
typedef struct {
    const char *name;
    bool (*read)(mh_pccard *c, mh_pccard_window w, uint32_t off,
                 unsigned size, uint32_t *out);
    bool (*write)(mh_pccard *c, mh_pccard_window w, uint32_t off,
                  unsigned size, uint32_t val);
} mh_pccard_kind;

struct mh_pccard {
    const mh_pccard_kind *kind;
    unsigned    slot;               /* 0 or 1, for messages */
    FILE       *log;

    /* Memory-card backing store. Mirrors when the window is larger. */
    uint8_t    *image;
    uint32_t    image_len;
    ne2000      nic;
    mh_modem   modem;
    uint8_t     config;
    uint8_t     cis[64];
    unsigned    cis_len;

    /* Per-window traffic accounting, independent of the card type. */
    uint64_t    reads[MH_PCCARD_NWINDOW];
    uint64_t    writes[MH_PCCARD_NWINDOW];
    uint32_t    first_off[MH_PCCARD_NWINDOW];
    uint32_t    high_off[MH_PCCARD_NWINDOW];
    bool        touched[MH_PCCARD_NWINDOW];

    /* Diagnostics: log the first n accesses with the site that made them. */
    unsigned    log_first;
};

/* The memory card: an image, mirrored through the window. */
extern const mh_pccard_kind mh_pccard_memory;
extern const mh_pccard_kind mh_pccard_ne2000;
/* Standard NE2000 CIS/COR attribute bytes, shared with experimental 68k slot. */
bool mh_ne2000_card_attribute_byte(uint32_t off, uint8_t config, uint8_t *value);
/* Experimental PIC-2000 variant: standard CIS plus Magic Cap's IO-card tuple. */
bool mh_ne2000_magic_attribute_byte(uint32_t off, uint8_t config, uint8_t *value);
/*
 * Which card, as a number rather than a pointer.
 *
 * The kinds themselves are addresses of static descriptors, which is right
 * inside a process and useless in a file. A state has to name a card it will
 * find again in a later run, so it stores one of these and the image's path.
 */
typedef enum {
    MH_CARD_NONE = 0,
    MH_CARD_SRAM = 1,
    MH_CARD_MEMORY = 2,
    MH_CARD_NE2000 = 3,
    MH_CARD_MODEM = 4,
} mh_card_kind;

extern const mh_pccard_kind mh_pccard_modem;
void mh_modem_init(mh_pccard *c);
bool mh_modem_irq(const mh_pccard *c);
void mh_modem_tick(mh_pccard *c, uint64_t now_ns);

extern const mh_pccard_kind mh_pccard_sram;
void mh_pccard_sram_init(mh_pccard *c, unsigned slot, uint8_t *data, uint32_t size);

void mh_pccard_init(mh_pccard *c, unsigned slot, const mh_pccard_kind *kind);
void mh_pccard_report(const mh_pccard *c);

/*
 * Bus glue. One of these per window, so the region's ctx carries both the
 * card and which of its windows this is.
 */
typedef struct {
    mh_pccard        *card;
    mh_pccard_window  window;
    const uint32_t    *pc_hint;     /* diagnostics only */
    const bool        *present;     /* board socket's physical insertion */
} mh_pccard_port;

uint32_t mh_pccard_read(void *ctx, uint32_t off, unsigned size);
void     mh_pccard_write(void *ctx, uint32_t off, unsigned size, uint32_t val);

#endif /* MH_PCCARD_H */
