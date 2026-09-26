/*
 * power.h — the host's battery, reported to the emulated one.
 *
 * Host discovery only. Each board maps this information into its own
 * battery circuitry and chooses whether host or simulated power is used.
 */
#ifndef MH_POWER_H
#define MH_POWER_H

#include <stdbool.h>

typedef struct {
    bool has_battery;   /* false on a desktop, or when we cannot tell */
    bool on_ac;
    int  percent;       /* 0..100; -1 when unknown */
} mh_host_power;

/* Returns false when the host's power state cannot be determined at all. */
bool mh_host_power_read(mh_host_power *out);

/* Name of the source actually being used, for the startup banner. */
const char *mh_host_power_source(void);

#endif
