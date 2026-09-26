#include "host/ppp.h"
#include "host/network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PPP_FRAME_MAX 4096
#define PPP_TX_CAP 65536
#define LCP 0xc021
#define IPCP 0x8021
#define IPV4 0x0021

struct mh_ppp {
    mh_network *net;
    bool command_mode, echo, verbose, escaped, frame_started;
    bool lcp_sent, lcp_local, lcp_peer, ipcp_sent, ipcp_local, ipcp_peer;
    bool bad_frame_reported;
    uint8_t line[256];
    unsigned line_len;
    uint8_t frame[PPP_FRAME_MAX];
    size_t frame_len;
    uint8_t tx[PPP_TX_CAP];
    size_t tx_read, tx_len;
    uint8_t lcp_id, ipcp_id, request_id;
    uint16_t lcp_mru;
    uint32_t lcp_accm;
    uint8_t request[64];
    size_t request_len;
    uint32_t ip_peer, ip_guest, dns;
    uint64_t guest_ip_packets, host_ip_packets;
};

static const char *proto_name(uint16_t proto)
{ return proto == LCP ? "LCP" : proto == IPCP ? "IPCP" : "PPP"; }

static uint16_t fcs_update(uint16_t fcs, uint8_t byte)
{
    fcs ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        fcs = (fcs >> 1) ^ (fcs & 1 ? 0x8408 : 0);
    return fcs;
}

static uint16_t be16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t ipv4(unsigned a, unsigned b, unsigned c, unsigned d)
{ return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d; }

static bool tx_push(mh_ppp *p, uint8_t b)
{
    if (p->tx_len == PPP_TX_CAP) return false;
    p->tx[(p->tx_read + p->tx_len++) % PPP_TX_CAP] = b;
    return true;
}

static void serial_text(mh_ppp *p, const char *s)
{
    if (p->verbose) tx_push(p, '\r'), tx_push(p, '\n');
    while (*s) tx_push(p, (uint8_t)*s++);
    if (p->verbose) tx_push(p, '\r'), tx_push(p, '\n');
    else tx_push(p, '\r');
}

static void send_frame(mh_ppp *p, uint16_t protocol,
                       const uint8_t *payload, size_t length)
{
    if (length + 6 > PPP_FRAME_MAX) return;
    uint8_t raw[PPP_FRAME_MAX];
    size_t n = 0;
    raw[n++] = 0xff; raw[n++] = 0x03;
    raw[n++] = (uint8_t)(protocol >> 8); raw[n++] = (uint8_t)protocol;
    if (length) { memcpy(raw + n, payload, length); n += length; }
    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < n; ++i) fcs = fcs_update(fcs, raw[i]);
    fcs ^= 0xffff;
    raw[n++] = (uint8_t)fcs; raw[n++] = (uint8_t)(fcs >> 8);
    /* Reserve worst case (every octet escaped) so the ring never contains
     * a truncated frame if its consumer is temporarily behind. */
    if (PPP_TX_CAP - p->tx_len < n * 2 + 2) return;
    tx_push(p, 0x7e);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = raw[i];
        if (b < 0x20 || b == 0x7d || b == 0x7e) {
            tx_push(p, 0x7d); tx_push(p, b ^ 0x20);
        } else tx_push(p, b);
    }
    tx_push(p, 0x7e);
}

static void send_control(mh_ppp *p, uint16_t protocol, uint8_t code,
                         uint8_t id, const uint8_t *data, size_t len)
{
    if (len > 250) return;
    uint8_t packet[254];
    packet[0] = code; packet[1] = id;
    put16(packet + 2, (uint16_t)(len + 4));
    if (len) memcpy(packet + 4, data, len);
    if (getenv("MH_PPP_TRACE"))
        fprintf(stderr, "ppp: host -> %s code=%u id=%u length=%zu\n",
                proto_name(protocol), code, id, len + 4);
    send_frame(p, protocol, packet, len + 4);
}

static void send_configure(mh_ppp *p, uint16_t protocol)
{
    uint8_t id;
    uint8_t options[16];
    size_t len;
    if (protocol == LCP) {
        id = ++p->lcp_id;
        options[0] = 1; options[1] = 4;
        put16(options + 2, p->lcp_mru);
        options[4] = 2; options[5] = 6;
        options[6] = (uint8_t)(p->lcp_accm >> 24);
        options[7] = (uint8_t)(p->lcp_accm >> 16);
        options[8] = (uint8_t)(p->lcp_accm >> 8);
        options[9] = (uint8_t)p->lcp_accm;
        len = 10;
        p->lcp_sent = true;
    } else {
        id = ++p->ipcp_id;
        options[0] = 3; options[1] = 6;
        options[2] = (uint8_t)(p->ip_peer >> 24);
        options[3] = (uint8_t)(p->ip_peer >> 16);
        options[4] = (uint8_t)(p->ip_peer >> 8);
        options[5] = (uint8_t)p->ip_peer;
        options[6] = 129; options[7] = 6;
        options[8] = (uint8_t)(p->dns >> 24);
        options[9] = (uint8_t)(p->dns >> 16);
        options[10] = (uint8_t)(p->dns >> 8);
        options[11] = (uint8_t)p->dns;
        len = 12;
        p->ipcp_sent = true;
    }
    p->request_id = id;
    memcpy(p->request, options, len);
    p->request_len = len;
    send_control(p, protocol, 1, id, options, len);
}

static bool is_ip(const uint8_t *packet, size_t len, uint32_t *dst)
{
    if (len < 20 || (packet[0] >> 4) != 4) return false;
    size_t header = (packet[0] & 15) * 4;
    if (header < 20 || header > len || be16(packet + 2) > len ||
        be16(packet + 2) < header) return false;
    *dst = (uint32_t)packet[16] << 24 | (uint32_t)packet[17] << 16 |
           (uint32_t)packet[18] << 8 | packet[19];
    return true;
}

static void slirp_receive(void *opaque, const uint8_t *frame, size_t len)
{
    mh_ppp *p = opaque;
    if (len < 34 || frame[12] != 8 || frame[13] != 0) return;
    uint32_t dst;
    if (!is_ip(frame + 14, len - 14, &dst) || dst != p->ip_guest) return;
    ++p->host_ip_packets;
    if (getenv("MH_PPP_TRACE"))
        fprintf(stderr, "ppp: slirp -> guest IPv4 (%u bytes)\n", be16(frame + 16));
    send_frame(p, IPV4, frame + 14, be16(frame + 16));
}

static void send_network_packet(mh_ppp *p, const uint8_t *ip, size_t len)
{
    uint32_t dst;
    if (!p->net || len > 1500 || !is_ip(ip, len, &dst)) return;
    const uint16_t iplen = be16(ip + 2);
    if (iplen < 20 || iplen > len) return;
    uint8_t frame[1514];
    static const uint8_t gateway[6] = {0x52,0x55,10,0,2,2};
    static const uint8_t guest[6] = {0x52,0x55,10,0,2,15};
    memcpy(frame, gateway, 6); memcpy(frame + 6, guest, 6);
    frame[12] = 8; frame[13] = 0;
    memcpy(frame + 14, ip, iplen);
    if (mh_network_send(p->net, frame, 14 + iplen)) {
        ++p->guest_ip_packets;
        if (getenv("MH_PPP_TRACE"))
            fprintf(stderr, "ppp: guest -> slirp IPv4 (%u bytes)\n", iplen);
    }
}

static void configure_request(mh_ppp *p, uint16_t proto,
                              uint8_t id, const uint8_t *opt, size_t len)
{
    uint8_t reject[PPP_FRAME_MAX], nak[PPP_FRAME_MAX];
    size_t reject_len = 0, nak_len = 0;
    bool address_seen = false;
    for (size_t at = 0; at < len;) {
        if (len - at < 2 || opt[at + 1] < 2 || opt[at + 1] > len - at) {
            send_control(p, proto, 4, id, opt, len);
            return;
        }
        size_t n = opt[at + 1];
        uint8_t type = opt[at];
        if (proto == LCP) {
            /* Refuse peer authentication and options not implemented here. */
            if (type == 3 || (type != 1 && type != 2 && type != 5 &&
                              type != 7 && type != 8)) {
                memcpy(reject + reject_len, opt + at, n); reject_len += n;
            }
        } else {
            if (type == 3 && n == 6) {
                address_seen = true;
                uint32_t requested = (uint32_t)opt[at+2] << 24 |
                    (uint32_t)opt[at+3] << 16 | (uint32_t)opt[at+4] << 8 | opt[at+5];
                if (requested != p->ip_guest) {
                    uint8_t v[6] = {3,6,(uint8_t)(p->ip_guest>>24),
                        (uint8_t)(p->ip_guest>>16),(uint8_t)(p->ip_guest>>8),(uint8_t)p->ip_guest};
                    memcpy(nak + nak_len, v, 6); nak_len += 6;
                }
            } else if ((type == 129 || type == 131) && n == 6) {
                uint32_t requested = (uint32_t)opt[at+2] << 24 |
                    (uint32_t)opt[at+3] << 16 | (uint32_t)opt[at+4] << 8 | opt[at+5];
                if (requested != p->dns) {
                    uint8_t v[6] = {type,6,(uint8_t)(p->dns>>24),
                        (uint8_t)(p->dns>>16),(uint8_t)(p->dns>>8),(uint8_t)p->dns};
                    memcpy(nak + nak_len, v, 6); nak_len += 6;
                }
            } else {
                memcpy(reject + reject_len, opt + at, n); reject_len += n;
            }
        }
        at += n;
    }
    if (proto == IPCP && !address_seen) {
        uint8_t v[6] = {3,6,(uint8_t)(p->ip_guest>>24),
            (uint8_t)(p->ip_guest>>16),(uint8_t)(p->ip_guest>>8),(uint8_t)p->ip_guest};
        memcpy(nak + nak_len, v, sizeof(v)); nak_len += sizeof(v);
    }
    if (reject_len) send_control(p, proto, 4, id, reject, reject_len);
    else if (nak_len) send_control(p, proto, 3, id, nak, nak_len);
    else {
        send_control(p, proto, 2, id, opt, len);
        if (proto == LCP) {
            p->lcp_peer = true;
            if (!p->lcp_sent) send_configure(p, LCP);
            if (p->lcp_local && !p->ipcp_sent) send_configure(p, IPCP);
        } else {
            p->ipcp_peer = true;
        }
    }
}

static void control_packet(mh_ppp *p, uint16_t proto,
                           const uint8_t *d, size_t n)
{
    if (n < 4 || be16(d + 2) < 4 || be16(d + 2) > n) return;
    size_t len = be16(d + 2) - 4;
    uint8_t id = d[1];
    if (getenv("MH_PPP_TRACE"))
        fprintf(stderr, "ppp: guest -> %s code=%u id=%u length=%zu\n",
                proto_name(proto), d[0], id, len + 4);
    switch (d[0]) {
    case 1:
        configure_request(p, proto, id, d + 4, len);
        break;
    case 2:
        if (proto == LCP && p->lcp_sent && id == p->request_id &&
            len == p->request_len && !memcmp(d + 4, p->request, len)) {
            p->lcp_local = true;
            if (p->lcp_peer && !p->ipcp_sent) send_configure(p, IPCP);
        } else if (proto == IPCP && p->ipcp_sent && id == p->request_id &&
                   len == p->request_len && !memcmp(d + 4, p->request, len)) {
            p->ipcp_local = true;
        }
        break;
    case 3: /* Configure-Nak: retry IPCP with the peer's proposed address. */
        if (id == p->request_id) {
            if (proto == LCP && p->lcp_sent) {
                for (size_t at = 0; at + 2 <= len;) {
                    const uint8_t *o = d + 4 + at;
                    size_t olen = o[1];
                    if (olen < 2 || olen > len - at) break;
                    if (o[0] == 1 && o[1] == 4) p->lcp_mru = be16(o + 2);
                    if (o[0] == 2 && o[1] == 6)
                        p->lcp_accm = (uint32_t)o[2] << 24 | (uint32_t)o[3] << 16 |
                                      (uint32_t)o[4] << 8 | o[5];
                    at += olen;
                }
                p->lcp_sent = false;
                send_configure(p, LCP);
            } else if (proto == IPCP && p->ipcp_sent) {
                for (size_t at = 0; at + 2 <= len;) {
                    const uint8_t *o = d + 4 + at;
                    size_t olen = o[1];
                    if (olen < 2 || olen > len - at) break;
                    uint32_t value;
                    if (olen == 6 && (o[0] == 3 || o[0] == 129 || o[0] == 131)) {
                        value = (uint32_t)o[2] << 24 | (uint32_t)o[3] << 16 |
                                (uint32_t)o[4] << 8 | o[5];
                        if (o[0] == 3) p->ip_peer = value;
                        else p->dns = value;
                    }
                    at += olen;
                }
                p->ipcp_sent = false;
                send_configure(p, IPCP);
            }
        }
        break;
    case 5: /* Terminate-Request */
        send_control(p, proto, 6, id, d + 4, len);
        if (proto == LCP) {
            p->lcp_sent = p->lcp_local = p->lcp_peer = false;
            p->ipcp_sent = p->ipcp_local = p->ipcp_peer = false;
            p->command_mode = true;
            serial_text(p, p->verbose ? "NO CARRIER" : "3");
        } else if (proto == IPCP) {
            p->ipcp_sent = p->ipcp_local = p->ipcp_peer = false;
        }
        break;
    case 9: /* Echo-Request */
        send_control(p, LCP, 10, id, d + 4, len);
        break;
    }
}

static void receive_frame(mh_ppp *p)
{
    if (p->frame_len < 6) return;
    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < p->frame_len; ++i) fcs = fcs_update(fcs, p->frame[i]);
    if (fcs != 0xf0b8) return;
    size_t n = p->frame_len - 2, at = 0;
    if (n >= 2 && p->frame[0] == 0xff && p->frame[1] == 3) at = 2;
    if (at >= n) return;
    uint16_t proto;
    if (p->frame[at] & 1) proto = p->frame[at++];
    else if (at + 1 < n) { proto = be16(p->frame + at); at += 2; }
    else return;
    if (proto == LCP || proto == IPCP)
        control_packet(p, proto, p->frame + at, n - at);
    else if (proto == IPV4 && p->lcp_local && p->lcp_peer &&
             p->ipcp_local && p->ipcp_peer)
        send_network_packet(p, p->frame + at, n - at);
}

static void start_dial(mh_ppp *p)
{
    p->lcp_sent = p->lcp_local = p->lcp_peer = false;
    p->ipcp_sent = p->ipcp_local = p->ipcp_peer = false;
    p->request_len = 0;
    p->command_mode = false;
    serial_text(p, p->verbose ? "CONNECT" : "1");
    if (!p->lcp_sent) send_configure(p, LCP);
}

static void at_command(mh_ppp *p)
{
    uint8_t *s = p->line;
    size_t n = p->line_len;
    p->line_len = 0;
    if (n < 2 || (s[0] != 'A' && s[0] != 'a') || (s[1] != 'T' && s[1] != 't')) return;
    for (size_t i = 2; i < n; ++i) {
        uint8_t c = s[i];
        if ((c == '&' || c == '\\' || c == '%') && i + 1 < n) { ++i; while (i + 1 < n && s[i+1] >= '0' && s[i+1] <= '9') ++i; }
        else if ((c == 'E' || c == 'V') && i + 1 < n) {
            if (c == 'E') p->echo = s[i+1] != '0';
            else p->verbose = s[i+1] != '0';
            ++i;
        } else if (c == 'D' || c == 'd') {
            if (memchr(s + i + 1, ';', n - i - 1)) { serial_text(p, "ERROR"); return; }
            start_dial(p); return;
        }
    }
    serial_text(p, p->verbose ? "OK" : "0");
}

mh_ppp *mh_ppp_open(const char *pcap)
{
    mh_ppp *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->command_mode = true; p->echo = true; p->verbose = true;
    p->ip_peer = ipv4(10,0,2,2); p->ip_guest = ipv4(10,0,2,15);
    p->dns = ipv4(10,0,2,3);
    p->lcp_mru = 1500;
    p->net = mh_network_open(slirp_receive, p, pcap);
    if (!p->net) { free(p); return NULL; }
    fprintf(stderr, "ppp: virtual ISP ready; guest .15, gateway .2, DNS .3\n");
    return p;
}

void mh_ppp_close(mh_ppp *p)
{
    if (!p) return;
    fprintf(stderr, "ppp: closing; %llu IPv4 packets from guest, %llu to guest\n",
            (unsigned long long)p->guest_ip_packets,
            (unsigned long long)p->host_ip_packets);
    mh_network_close(p->net);
    free(p);
}

void mh_ppp_write(mh_ppp *p, uint8_t b)
{
    if (!p) return;
    if (p->command_mode) {
        if (p->echo) tx_push(p, b);
        if (b == '\r' || b == '\n') {
            if (p->line_len) at_command(p);
        } else if (b == 8 || b == 127) {
            if (p->line_len) --p->line_len;
        } else if (p->line_len < sizeof(p->line)) p->line[p->line_len++] = b;
        else p->line_len = 0;
        return;
    }
    if (b == 0x7e) {
        if (p->escaped) { p->escaped = false; p->frame_len = 0; }
        else if (p->frame_started && p->frame_len) receive_frame(p);
        p->frame_started = true; p->frame_len = 0;
        return;
    }
    if (!p->frame_started) return;
    if (p->escaped) { b ^= 0x20; p->escaped = false; }
    else if (b == 0x7d) { p->escaped = true; return; }
    if (p->frame_len < sizeof(p->frame)) p->frame[p->frame_len++] = b;
    else p->frame_len = 0;
}

bool mh_ppp_read(mh_ppp *p, uint8_t *b)
{
    if (!p || !p->tx_len) return false;
    *b = p->tx[p->tx_read];
    p->tx_read = (p->tx_read + 1) % PPP_TX_CAP;
    --p->tx_len;
    return true;
}

void mh_ppp_poll(mh_ppp *p, uint64_t now_ns)
{
    if (p) mh_network_poll(p->net, now_ns);
}
