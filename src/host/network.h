/* Ethernet transport, independent of guest architecture and NIC model. */
#ifndef MH_NETWORK_H
#define MH_NETWORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct mh_network mh_network;
typedef void (*mh_net_receive)(void *, const uint8_t *, size_t);
mh_network *mh_network_open(mh_net_receive receive, void *opaque, const char *pcap);
void mh_network_close(mh_network *net);
bool mh_network_send(void *net, const uint8_t *frame, size_t len);
void mh_network_poll(mh_network *net, uint64_t now_ns);
#endif
