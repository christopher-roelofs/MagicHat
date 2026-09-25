/* Async PPP modem endpoint for Magic Cap serial clients. */
#ifndef MRC_PPP_H
#define MRC_PPP_H
#include <stdbool.h>
#include <stdint.h>
typedef struct mrc_ppp mrc_ppp;
mrc_ppp *mrc_ppp_open(const char *pcap);
void mrc_ppp_close(mrc_ppp *ppp);
void mrc_ppp_write(mrc_ppp *ppp, uint8_t byte);
bool mrc_ppp_read(mrc_ppp *ppp, uint8_t *byte);
void mrc_ppp_poll(mrc_ppp *ppp, uint64_t now_ns);
#endif
