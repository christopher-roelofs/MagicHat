#include "host/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MRC_HAVE_SLIRP
#include <libslirp.h>
#include <poll.h>
#include <errno.h>
typedef struct net_timer {
    struct net_timer *next;
    SlirpTimerCb cb;
    void *opaque;
    int64_t expires;
} net_timer;
struct mrc_network {
    Slirp *slirp;
    mrc_net_receive receive;
    void *opaque;
    uint64_t now, next_poll, tx, rx;
    net_timer *timers;
    struct pollfd *fds;
    unsigned nfds, capacity;
    bool poll_error;
    FILE *pcap;
};
static void capture(mrc_network *n, const void *frame, size_t len)
{
    if (!n->pcap) return;
    uint32_t h[] = {n->now / 1000000000, n->now / 1000 % 1000000, len, len};
    if (fwrite(h, sizeof(h), 1, n->pcap) != 1 || fwrite(frame, len, 1, n->pcap) != 1 ||
        fflush(n->pcap) != 0) {
        fprintf(stderr, "network: packet capture write failed\n");
        fclose(n->pcap); n->pcap = NULL;
    }
}
static ssize_t receive_packet(const void *buf, size_t len, void *opaque)
{
    mrc_network *n = opaque;
    capture(n, buf, len); n->rx++;
    n->receive(n->opaque, buf, len);
    return (ssize_t)len;
}
static void guest_error(const char *msg, void *opaque) { fprintf(stderr, "slirp: %s\n", msg); }
static int64_t clock_ns(void *opaque) { return ((mrc_network *)opaque)->now; }
static void *timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
    mrc_network *n = opaque;
    net_timer *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    *t = (net_timer){ .next = n->timers, .cb = cb, .opaque = cb_opaque, .expires = -1 };
    n->timers = t; return t;
}
static void timer_free(void *timer, void *opaque)
{
    mrc_network *n = opaque;
    net_timer **p = &n->timers;
    while (*p && *p != timer) p = &(*p)->next;
    if (*p) { net_timer *t = *p; *p = t->next; free(t); }
}
static void timer_mod(void *timer, int64_t expires, void *opaque)
{ ((net_timer *)timer)->expires = expires; }
static void fd_notify(int fd, void *opaque) { (void)fd; (void)opaque; }
static void notify(void *opaque) { ((mrc_network *)opaque)->next_poll = 0; }
static const SlirpCb callbacks = {
    .send_packet = receive_packet, .guest_error = guest_error,
    .clock_get_ns = clock_ns, .timer_new = timer_new, .timer_free = timer_free,
    .timer_mod = timer_mod, .register_poll_fd = fd_notify,
    .unregister_poll_fd = fd_notify, .notify = notify,
};
mrc_network *mrc_network_open(mrc_net_receive receive, void *opaque, const char *pcap)
{
    mrc_network *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->receive = receive; n->opaque = opaque;
    SlirpConfig cfg = { .version = 1, .in_enabled = true };
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    n->slirp = slirp_new(&cfg, &callbacks, n);
    if (!n->slirp) { mrc_network_close(n); return NULL; }
    if (pcap) {
        n->pcap = fopen(pcap, "wb");
        /* Native-endian pcap header; magic tells readers the byte order. */
        uint32_t magic = 0xa1b2c3d4, fields[] = {0, 0, 65535, 1};
        uint16_t version[] = {2, 4};
        if (!n->pcap || fwrite(&magic, 4, 1, n->pcap) != 1 ||
            fwrite(version, 4, 1, n->pcap) != 1 || fwrite(fields, 16, 1, n->pcap) != 1) {
            fprintf(stderr, "network: cannot create capture %s\n", pcap);
            mrc_network_close(n); return NULL;
        }
    }
    fprintf(stderr, "network: libslirp %s NAT, guest 10.0.2.15/24, gateway/host 10.0.2.2, DNS 10.0.2.3\n", slirp_version_string());
    return n;
}
void mrc_network_close(mrc_network *n)
{
    if (!n) return;
    if (n->slirp) slirp_cleanup(n->slirp);
    while (n->timers) timer_free(n->timers, n);
    if (n->pcap) fclose(n->pcap);
    fprintf(stderr, "network: %llu frames from guest, %llu to guest\n",
            (unsigned long long)n->tx, (unsigned long long)n->rx);
    free(n->fds); free(n);
}
/* Proxy ARP's reply mechanism is described in RFC 1027 section 2.1.
 * The tested Magic Cap provider ARPs for off-subnet destinations. Our virtual router answers those requests with its MAC;
 * libslirp handles the resulting IP frames. Local-subnet ARP is left to
 * libslirp, so the guest's duplicate-address probe cannot receive a false
 * claim that its own address is occupied. */
static void proxy_arp(mrc_network *n, const uint8_t *f, size_t len)
{
    if (len < 42 || f[12] != 8 || f[13] != 6 || f[14] || f[15] != 1 ||
        f[16] != 8 || f[17] || f[18] != 6 || f[19] != 4 || f[20] || f[21] != 1)
        return;
    const uint8_t *target = f + 38;
    if (!memcmp(target, f + 28, 4) || !memcmp(f + 28, "\0\0\0\0", 4) ||
        !target[0] || target[0] == 127 || target[0] >= 224 ||
        (target[0] == 10 && target[1] == 0 && target[2] == 2)) return;
    const uint8_t router[6] = {0x52,0x55,10,0,2,2};
    uint8_t reply[60] = {0};
    memcpy(reply, f + 22, 6); memcpy(reply + 6, router, 6);
    memcpy(reply + 12, f + 12, 10); reply[21] = 2;
    memcpy(reply + 22, router, 6); memcpy(reply + 28, target, 4);
    memcpy(reply + 32, f + 22, 6); memcpy(reply + 38, f + 28, 4);
    receive_packet(reply, sizeof(reply), n);
}
bool mrc_network_send(void *opaque, const uint8_t *frame, size_t len)
{
    mrc_network *n = opaque;
    if (!n || len < 14 || len > 1518) return false;
    capture(n, frame, len); n->tx++;
    slirp_input(n->slirp, frame, (int)len);
    proxy_arp(n, frame, len);
    return true;
}
static int add_poll(int fd, int events, void *opaque)
{
    mrc_network *n = opaque;
    if (n->nfds == n->capacity) {
        unsigned cap = n->capacity ? n->capacity * 2 : 16;
        struct pollfd *fds = realloc(n->fds, cap * sizeof(*fds));
        if (!fds) { n->poll_error = true; return -1; }
        n->fds = fds; n->capacity = cap;
    }
    short flags = 0;
    if (events & SLIRP_POLL_IN) flags |= POLLIN;
    if (events & SLIRP_POLL_OUT) flags |= POLLOUT;
    if (events & SLIRP_POLL_PRI) flags |= POLLPRI;
    n->fds[n->nfds] = (struct pollfd){ .fd = fd, .events = flags };
    return n->nfds++;
}
static int get_events(int idx, void *opaque)
{
    mrc_network *n = opaque;
    if (idx < 0 || (unsigned)idx >= n->nfds) return 0;
    short flags = n->fds[idx].revents;
    int events = 0;
    if (flags & POLLIN) events |= SLIRP_POLL_IN;
    if (flags & POLLOUT) events |= SLIRP_POLL_OUT;
    if (flags & POLLPRI) events |= SLIRP_POLL_PRI;
    if (flags & (POLLERR | POLLNVAL)) events |= SLIRP_POLL_ERR;
    if (flags & POLLHUP) events |= SLIRP_POLL_HUP;
    return events;
}
void mrc_network_poll(mrc_network *n, uint64_t now_ns)
{
    if (!n) return;
    n->now = now_ns;
    if (n->now < n->next_poll) return;
    n->next_poll = n->now + 1000000; /* at most 1 ms of guest time */
    /* Callbacks may remove timers, so restart after each expiration. */
    for (;;) {
        net_timer *t = n->timers;
        while (t && (t->expires < 0 || (uint64_t)t->expires > n->now / 1000000)) t = t->next;
        if (!t) break;
        t->expires = -1; t->cb(t->opaque);
    }
    uint32_t timeout = 0;
    n->nfds = 0; n->poll_error = false;
    slirp_pollfds_fill(n->slirp, &timeout, add_poll, n);
    int result = poll(n->fds, n->nfds, 0); /* never block emulation or SDL */
    slirp_pollfds_poll(n->slirp, n->poll_error || result < 0, get_events, n);
}
#else
struct mrc_network { int unused; };
mrc_network *mrc_network_open(mrc_net_receive receive, void *opaque, const char *pcap)
{ fprintf(stderr, "network: rebuild with libslirp development files for --net user\n"); return NULL; }
void mrc_network_close(mrc_network *n) { (void)n; }
bool mrc_network_send(void *n, const uint8_t *f, size_t len) { return false; }
void mrc_network_poll(mrc_network *n, uint64_t now_ns) { (void)n; (void)now_ns; }
#endif
