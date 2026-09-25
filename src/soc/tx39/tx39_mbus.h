#ifndef MRC_TX39_MBUS_H
#define MRC_TX39_MBUS_H
#include <stdbool.h>
#include <stdint.h>
/* Board-owned connection to a physical peripheral. Never stored in snapshots.
 * Ingress APIs represent words/commands actually received over the wire. */
typedef struct {
    void *ctx;
    void (*command)(void *ctx, uint16_t word);
    void (*transmit)(void *ctx, uint32_t word, unsigned bits);
    bool input_high;
    bool rx_complete;
    uint32_t rx_bytes;
} tx39_mbus_port;
#endif
