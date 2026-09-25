#include "devices/unknown_dev/unknown_dev.h"

#include <string.h>

void mrc_unknown_init(unknown_dev *d, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->log  = stderr;
}

uint32_t mrc_unknown_read(void *ctx, uint32_t off, unsigned size)
{
    unknown_dev *d = ctx;
    d->reads++;
    uint32_t v = (off / 4 < UNKNOWN_DEV_REGS) ? d->reg[off / 4] : 0;
    if (d->log_access)
        fprintf(d->log, "[%s] R%u +%03X = %08X\n", d->name, size * 8, off, v);
    return v;
}

void mrc_unknown_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    unknown_dev *d = ctx;
    d->writes++;
    if (off / 4 < UNKNOWN_DEV_REGS)
        d->reg[off / 4] = val;
    if (d->log_access)
        fprintf(d->log, "[%s] W%u +%03X = %08X\n", d->name, size * 8, off, val);
}
