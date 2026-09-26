/* Async PPP modem endpoint for Magic Cap serial clients. */
#ifndef MH_PPP_H
#define MH_PPP_H
#include <stdbool.h>
#include <stdint.h>
typedef struct mh_ppp mh_ppp;
mh_ppp *mh_ppp_open(const char *pcap);
void mh_ppp_close(mh_ppp *ppp);
void mh_ppp_write(mh_ppp *ppp, uint8_t byte);
bool mh_ppp_read(mh_ppp *ppp, uint8_t *byte);
void mh_ppp_poll(mh_ppp *ppp, uint64_t now_ns);
#endif
