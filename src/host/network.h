/* Ethernet transport, independent of guest architecture and NIC model. */
#ifndef MRC_NETWORK_H
#define MRC_NETWORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct mrc_network mrc_network;
typedef void (*mrc_net_receive)(void *, const uint8_t *, size_t);
mrc_network *mrc_network_open(mrc_net_receive receive, void *opaque, const char *pcap);
void mrc_network_close(mrc_network *net);
bool mrc_network_send(void *net, const uint8_t *frame, size_t len);
void mrc_network_poll(mrc_network *net, uint64_t now_ns);
#endif
