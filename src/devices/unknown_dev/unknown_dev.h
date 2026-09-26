/*
 * unknown_dev.h — a register file for a chip we have not identified yet.
 *
 * Using this is a statement that we know a device exists at an address and
 * do not yet know what it does. It stores what is written and returns it,
 * which is the least-wrong behaviour available, and it counts and can log
 * every access so the accesses themselves become evidence.
 *
 * It is deliberately not a silent catch-all: --verify reports how much
 * traffic each one absorbed, so an unidentified device cannot quietly become
 * load-bearing.
 */
#ifndef MH_UNKNOWN_DEV_H
#define MH_UNKNOWN_DEV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define UNKNOWN_DEV_REGS 64

typedef struct {
    const char *name;
    uint32_t    reg[UNKNOWN_DEV_REGS];
    FILE       *log;
    bool        log_access;
    uint64_t    reads, writes;
} unknown_dev;

void     mh_unknown_init(unknown_dev *d, const char *name);
uint32_t mh_unknown_read(void *ctx, uint32_t off, unsigned size);
void     mh_unknown_write(void *ctx, uint32_t off, unsigned size, uint32_t val);

#endif /* MH_UNKNOWN_DEV_H */
