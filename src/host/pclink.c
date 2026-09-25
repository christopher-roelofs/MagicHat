#include "host/pclink.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transport limits, from WinPcLink. A block carries at most 256 bytes and a
 * command payload at most 1028, which is the size of the offer record. */
#define BLOCK_MAX   256u
#define PAYLOAD_MAX 1028u

/* Stream controls. 0x0e ends, 0x0f aborts, 0x10 quotes any of the three. */
#define CTRL_END   0x0eu
#define CTRL_ABORT 0x0fu
#define CTRL_QUOTE 0x10u

/*
 * CRC-32 as the transport wants it: the ordinary reflected polynomial with
 * all ones to start and no final complement. zlib's crc32 complements at the
 * end, which is why the script this replaces exclusive-ors that back out.
 */
static uint32_t crc32_of(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t c = i;
            for (unsigned k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c;
}

/* A growable byte queue, drained from the front. The offered package is the
 * only large thing that goes through one, so growth is doubling and the
 * front is an index rather than a memmove per byte. */
typedef struct {
    uint8_t *data;
    size_t   len, cap, at;
} queue;

static bool queue_reserve(queue *q, size_t extra)
{
    if (q->len + extra <= q->cap) return true;
    size_t want = q->cap ? q->cap : 4096;
    while (want < q->len + extra) want *= 2;
    uint8_t *bigger = realloc(q->data, want);
    if (!bigger) return false;
    q->data = bigger;
    q->cap = want;
    return true;
}

static bool queue_push(queue *q, const uint8_t *data, size_t len)
{
    if (!queue_reserve(q, len)) return false;
    memcpy(q->data + q->len, data, len);
    q->len += len;
    return true;
}

static bool queue_take(queue *q, uint8_t *out)
{
    if (q->at >= q->len) return false;
    *out = q->data[q->at++];
    if (q->at == q->len) q->at = q->len = 0;    /* empty: start over */
    return true;
}

static void queue_free(queue *q)
{
    free(q->data);
    *q = (queue){0};
}

struct mrc_pclink {
    /* Outgoing, already framed and ready for the wire. */
    queue out;

    /* Incoming: raw wire bytes, then the unescaped stream they carry. */
    queue wire;
    queue stream;
    bool  greeted;      /* the guest's one-off 'ChMa' has been seen */
    bool  escaped;      /* a quote byte is pending */

    /* The 68k PIC carries the same commands in UDP over async PPP. */
    bool  ppp;
    bool  ppp_direct;
    bool  ppp_escaped;
    queue ppp_frame;
    queue ppp_pending;
    bool  ppp_waiting_ack;
    bool  ppp_package_sent;
    bool  ppp_package_acked;
    uint32_t ppp_package_end;
    uint8_t ppp_prefix[12];
    uint16_t ppp_ip_id;
    uint32_t gmtp_block;
    uint32_t gmtp_sent;
    uint16_t gmtp_received;
    uint32_t gmtp_acked;
    bool     gmtp_started;

    /* The package being offered, read once at open. */
    uint8_t *package;
    uint32_t package_len;
    bool     m68k_package;
    char     name[128];

    mrc_pclink_state state;
    char             message[160];
    uint32_t         sent;      /* package bytes handed to the wire */
    bool             offered;   /* the offer has been queued */
    /*
     * How long the wire has been quiet, counted in the machine's own polls
     * rather than in host seconds -- the guest is what this is keeping pace
     * with, and it runs at whatever speed the emulator manages.
     *
     * It matters because the guest reports that it has finished taking a
     * package by answering a Ping. Nothing else says so: the last byte of the
     * transfer leaves the wire long before the guest has digested it. Without
     * a Ping to answer, a completed install sat forever looking unfinished.
     */
    unsigned         idle;
};

/* The machine offers the link a byte slot roughly every thousand
 * instructions when it has nothing to receive, so this is a fraction of a
 * second of guest time -- often enough to notice the end of a transfer, rare
 * enough that the guest is never answering pings instead of working. */
#define IDLE_PING 20000u

static void fail(mrc_pclink *l, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(l->message, sizeof(l->message), fmt, args);
    va_end(args);
    l->state = MRC_PCLINK_FAILED;
}

static uint16_t ppp_fcs(const uint8_t *p, size_t n)
{
    uint16_t fcs = 0xFFFF;
    while (n--) {
        fcs ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            fcs = (fcs & 1) ? (uint16_t)((fcs >> 1) ^ 0x8408u) : fcs >> 1;
    }
    return (uint16_t)~fcs;
}

static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 2)
        sum += (uint16_t)((uint16_t)p[i] << 8 | (i + 1 < n ? p[i + 1] : 0));
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* GMTP uses the same one's-complement checksum as IPv4, over the complete
 * GMTP PDU.  The two checksum bytes in the header are zero while it is
 * calculated. */
static uint16_t gmtp_checksum(const uint8_t *p, size_t n)
{
    return ip_checksum(p, n);
}

static bool ppp_wire_byte(queue *q, uint8_t b)
{
    static const uint8_t esc = 0x7D;
    if (b < 0x20 || b == 0x7D || b == 0x7E) {
        const uint8_t quoted = b ^ 0x20;
        return queue_push(q, &esc, 1) && queue_push(q, &quoted, 1);
    }
    return queue_push(q, &b, 1);
}

static bool ppp_queue_wire(mrc_pclink *l, const uint8_t *wire, size_t wire_len,
                           const uint8_t *data)
{
    if (data[0] == 4)
        return queue_push(&l->out, wire, wire_len);
    /* HIX acknowledges a short run of frames cumulatively; a one-frame
     * window deadlocks its greeting and package stream. PIC uses one. */
    const uint32_t window = l->ppp_direct ? 3 : 1;
    if (l->gmtp_sent - l->gmtp_acked < window) {
        const bool ok = queue_push(&l->out, wire, wire_len);
        if (ok) {
            l->gmtp_sent++;
            l->ppp_waiting_ack = true;
        }
        return ok;
    }
    uint8_t size[4] = { (uint8_t)(wire_len >> 24), (uint8_t)(wire_len >> 16),
                        (uint8_t)(wire_len >> 8), (uint8_t)wire_len };
    return queue_push(&l->ppp_pending, size, sizeof(size)) &&
           queue_push(&l->ppp_pending, wire, wire_len);
}

static bool ppp_datagram(mrc_pclink *l, const uint8_t *data, size_t len)
{
    /* HIX transmits a length-prefixed bootstrap record but its ROM receive
     * handler requires an ordinary UDP/IPv4 packet on the return path. */
    const size_t udp_payload = len;
    const size_t ip_len = 20 + 8 + udp_payload;
    if (ip_len > 0xFFFFu) return false;
    uint8_t *p = calloc(1, 4 + ip_len + 2);
    if (!p) return false;
    p[0] = 0xFF; p[1] = 0x03; p[2] = 0x00; p[3] = 0x21;
    uint8_t *ip = p + 4;
    ip[0] = 0x45;
    ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)ip_len;
    const uint16_t id = ++l->ppp_ip_id;
    ip[4] = (uint8_t)(id >> 8); ip[5] = (uint8_t)id;
    ip[6] = 0x40; ip[8] = 0x10; ip[9] = 17;
    uint8_t *udp = ip + 20;
    const uint16_t udp_len = (uint16_t)(8 + udp_payload);
    udp[4] = (uint8_t)(udp_len >> 8); udp[5] = (uint8_t)udp_len;
    memcpy(udp + 8, data, len);
    const uint16_t sum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
    const size_t body = 4 + ip_len;
    const uint16_t fcs = ppp_fcs(p, body);
    p[body] = (uint8_t)fcs; p[body + 1] = (uint8_t)(fcs >> 8);

    queue wire = {0};
    static const uint8_t flag = 0x7E;
    bool ok = queue_push(&wire, &flag, 1);
    for (size_t i = 0; ok && i < body + 2; i++) ok = ppp_wire_byte(&wire, p[i]);
    ok = ok && queue_push(&wire, &flag, 1);
    free(p);
    if (!ok) {
        queue_free(&wire);
        return false;
    }

    ok = ppp_queue_wire(l, wire.data, wire.len, data);
    queue_free(&wire);
    return ok;
}

static void ppp_acked(mrc_pclink *l, uint16_t next_block)
{
    const uint16_t advance = (uint16_t)(next_block - (uint16_t)l->gmtp_acked);
    if (!advance || advance > l->gmtp_sent - l->gmtp_acked) return;
    /* Keep the full count across wire sequence wrap for completion checks. */
    l->gmtp_acked += advance;
    if (l->ppp_package_sent && l->gmtp_acked >= l->ppp_package_end)
        l->ppp_package_acked = true;
    queue *q = &l->ppp_pending;
    const uint32_t window = l->ppp_direct ? 3 : 1;
    while (l->gmtp_sent - l->gmtp_acked < window && q->len - q->at >= 4) {
        const uint8_t *p = q->data + q->at;
        const size_t len = (size_t)p[0] << 24 | (size_t)p[1] << 16 |
                           (size_t)p[2] << 8 | p[3];
        if (len > q->len - q->at - 4 || !queue_push(&l->out, p + 4, len)) {
            fail(l, "out of memory advancing the GMTP send window");
            return;
        }
        q->at += 4 + len;
        l->gmtp_sent++;
    }
    if (q->at == q->len) q->at = q->len = 0;
    l->ppp_waiting_ack = l->gmtp_sent != l->gmtp_acked;
}

static bool emit_ppp(mrc_pclink *l, const uint8_t *data, size_t len)
{
    /* GMTPStream joins consecutive messages into a byte stream. Sending
     * complete messages lets each block be acknowledged independently. */
    bool first = true;
    while (len || first) {
        const size_t header = 12;
        const size_t capacity = 64;
        const size_t n = len < capacity ? len : capacity;
        uint8_t pdu[BLOCK_MAX];
        memset(pdu, 0, header);
        pdu[0] = 1;                         /* MSG */
        pdu[1] = 0x81;                      /* complete; request ACK */
        if (!l->gmtp_started) pdu[1] |= 0x40; /* association start */
        pdu[2] = l->ppp_prefix[2];
        pdu[3] = l->ppp_prefix[3];          /* association ID */
        pdu[4] = (uint8_t)(l->gmtp_block >> 8);
        pdu[5] = (uint8_t)l->gmtp_block++;
        pdu[8] = (uint8_t)(n >> 8);
        pdu[9] = (uint8_t)n;
        pdu[10] = l->ppp_prefix[11];        /* remote is the PIC's local */
        pdu[11] = l->ppp_prefix[10];        /* local is the PIC's remote */
        if (n) memcpy(pdu + header, data, n);
        const uint16_t sum = gmtp_checksum(pdu, header + n);
        pdu[6] = (uint8_t)(sum >> 8);
        pdu[7] = (uint8_t)sum;
        if (getenv("MRC_PCLINK_TRACE")) {
            fprintf(stderr, "[pclink] -> GMTP");
            for (size_t i = 0; i < header; i++)
                fprintf(stderr, " %02x", pdu[i]);
            fputc('\n', stderr);
        }
        if (!ppp_datagram(l, pdu, header + n)) return false;
        data += n;
        len -= n;
        first = false;
        l->gmtp_started = true;
    }
    return true;
}

/*
 * Quote the stream bytes and cut the result into independently checked
 * blocks. Everything that goes out goes through here.
 */
static bool emit(mrc_pclink *l, const uint8_t *data, size_t len)
{
    if (l->ppp) return emit_ppp(l, data, len);
    uint8_t quoted[BLOCK_MAX];
    size_t at = 0, i = 0;
    while (i < len || at) {
        /* Fill a block, never splitting a quote from the byte it quotes. */
        while (i < len && at + 2 <= BLOCK_MAX) {
            uint8_t b = data[i++];
            if (b == CTRL_END || b == CTRL_ABORT || b == CTRL_QUOTE)
                quoted[at++] = CTRL_QUOTE;
            quoted[at++] = b;
        }
        uint8_t header[2] = { (uint8_t)(at >> 8), (uint8_t)at };
        uint32_t c = crc32_of(quoted, at);
        uint8_t trailer[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16),
                               (uint8_t)(c >> 8), (uint8_t)c };
        if (!queue_push(&l->out, header, 2) ||
            !queue_push(&l->out, quoted, at) ||
            !queue_push(&l->out, trailer, 4)) {
            fail(l, "out of memory building the transfer");
            return false;
        }
        at = 0;
        if (i >= len) break;
    }
    return true;
}

/* A command: four-byte tag, big-endian length, payload. */
static bool command(mrc_pclink *l, const char tag[4], const uint8_t *payload,
                    uint32_t len)
{
    if (getenv("MRC_PCLINK_TRACE"))
        fprintf(stderr, "[pclink] -> %c%c%c%c (%u bytes)\n",
                tag[0], tag[1], tag[2], tag[3], len);
    uint8_t head[8];
    memcpy(head, tag, 4);
    head[4] = (uint8_t)(len >> 24); head[5] = (uint8_t)(len >> 16);
    head[6] = (uint8_t)(len >> 8);  head[7] = (uint8_t)len;
    uint8_t *buf = malloc(8u + len);
    if (!buf) { fail(l, "out of memory building a command"); return false; }
    memcpy(buf, head, 8);
    if (len) memcpy(buf + 8, payload, len);
    bool ok = emit(l, buf, 8u + len);
    free(buf);
    return ok;
}

/* Compiler CLUS files need the same envelope that the desktop link writes.
 * ClassName's OctetString field is typed, so it carries its length and bytes
 * without an object tag. FrozenPackage has one defined class with two fields:
 * the package cluster and a nil changes cluster. */
static bool wrap_68k_package(mrc_pclink *l)
{
    static const uint8_t object[] = {
        1, 0,                 /* Wireline version */
        117, 0xC7, 62, 13,   /* defined Object; ClassName */
        'F','r','o','z','e','n','P','a','c','k','a','g','e',
        1, 2, 22             /* defined class count, fields, Buffer */
    };
    uint8_t length[5];
    unsigned count = 0;
    const uint32_t n = l->package_len;
    if (n < 0xC0) length[count++] = (uint8_t)n;
    else if (n < 0x1000) {
        length[count++] = (uint8_t)(0xC0 | (n >> 8));
        length[count++] = (uint8_t)n;
    } else {
        const unsigned bytes = n < 0x10000 ? 2 : n < 0x1000000 ? 3 : 4;
        length[count++] = (uint8_t)(0xD0 + bytes - 2);
        for (unsigned i = bytes; i > 0; i--)
            length[count++] = (uint8_t)(n >> ((i - 1) * 8));
    }
    const size_t prefix = 32 + sizeof(object) + count;
    uint8_t *wrapped = calloc(1, prefix + n + 1);
    if (!wrapped) { fail(l, "out of memory wrapping the 68k package"); return false; }
    memcpy(wrapped, "MPkg\0\0\0\1", 8);
    memcpy(wrapped + 32, object, sizeof(object));
    memcpy(wrapped + 32 + sizeof(object), length, count);
    memcpy(wrapped + prefix, l->package, n);
    wrapped[prefix + n] = 18; /* nil changes cluster */
    free(l->package);
    l->package = wrapped;
    l->package_len = (uint32_t)(prefix + n + 1);
    return true;
}

static bool offer_package(mrc_pclink *l)
{
    if (l->ppp) {
        if (l->m68k_package && !wrap_68k_package(l)) return false;
        uint8_t info[4 + sizeof(l->name)] = {0};
        info[0] = (uint8_t)(l->package_len >> 24);
        info[1] = (uint8_t)(l->package_len >> 16);
        info[2] = (uint8_t)(l->package_len >> 8);
        info[3] = (uint8_t)l->package_len;
        const size_t chars = strlen(l->name);
        memcpy(info + 4, l->name, chars + 1);
        if (!command(l, "SPkg", info, (uint32_t)(5 + chars))) return false;
        l->state = MRC_PCLINK_SENDING;
        snprintf(l->message, sizeof(l->message), "offering %s", l->name);
        return true;
    }
    /* MIPS offer: the fixed 1028-byte WinPcLink record with a UTF-16 name. */
    uint8_t info[PAYLOAD_MAX];
    memset(info, 0, sizeof(info));
    uint32_t words[8] = { l->package_len, l->package_len, 0, 0, 0,
                          0x80000000u, 0, 0 };
    size_t chars = strlen(l->name);
    if (chars > 498) chars = 498;          /* 996 bytes of UTF-16 */
    words[7] = (uint32_t)chars;
    for (unsigned i = 0; i < 8; i++) {
        info[i * 4 + 0] = (uint8_t)(words[i] >> 24);
        info[i * 4 + 1] = (uint8_t)(words[i] >> 16);
        info[i * 4 + 2] = (uint8_t)(words[i] >> 8);
        info[i * 4 + 3] = (uint8_t)words[i];
    }
    /* The name is plain ASCII widened; anything else in a filename is not
     * something the guest's own name field can carry either. */
    for (size_t i = 0; i < chars; i++) {
        info[32 + i * 2] = 0;
        info[32 + i * 2 + 1] = (uint8_t)l->name[i];
    }
    if (!command(l, "SPkg", info, PAYLOAD_MAX)) return false;

    /* Then the package itself as a raw stream, with the four-byte zero
     * trailer WinPcLink appends at 0x402b30. */
    static const uint8_t zero[4] = {0};
    if (!emit(l, l->package, l->package_len)) return false;
    if (!emit(l, zero, sizeof(zero))) return false;
    l->state = MRC_PCLINK_SENDING;
    snprintf(l->message, sizeof(l->message), "sending %s", l->name);
    return true;
}

/* Everything the guest can say that we act on. Notifications it sends and
 * WinPcLink deliberately ignores are consumed and dropped. */
static void handle(mrc_pclink *l, const char tag[4], const uint8_t *payload,
                   uint32_t len)
{
    (void)payload;
    /* MRC_PCLINK_TRACE prints the conversation. A transfer that stalls looks
     * identical to one that is merely slow, and the only way to tell them
     * apart is to see which side spoke last. */
    if (getenv("MRC_PCLINK_TRACE"))
        fprintf(stderr, "[pclink] <- %c%c%c%c (%u bytes)\n",
                tag[0], tag[1], tag[2], tag[3], len);
    (void)len;
    if (!memcmp(tag, "Cnct", 4)) {
        /*
         * Two replies, not one. A single Cntd starts the guest processing
         * commands but leaves its second wait to time out twenty seconds
         * later, which closes the UART in the middle of a large transfer --
         * the transfer ceiling this project spent a while chasing.
         */
        /* The 68k PC Link reader uses a zero body length as its retry
         * sentinel.  Its Cntd handler ignores the body, but needs one byte
         * present to dispatch the command.  The MIPS link accepts the
         * original empty command. */
        if (l->ppp) {
            static const uint8_t zero = 0;
            if (l->offered) return;
            command(l, "Cntd", &zero, 1);
            command(l, "Cntd", &zero, 1);
        } else if (!l->offered) {
            command(l, "Cntd", NULL, 0);
            command(l, "Cntd", NULL, 0);
        }
        if (!l->offered) {
            l->state = MRC_PCLINK_CONNECTED;
            snprintf(l->message, sizeof(l->message), "linked");
            l->offered = true;
            offer_package(l);
        }
        return;
    }
    if (l->ppp && !memcmp(tag, "Send", 4)) {
        if (l->ppp_package_sent) return;
        l->ppp_package_sent = true;
        for (uint32_t at = 0; at < l->package_len;) {
            uint32_t n = l->package_len - at;
            if (n > 512) n = 512;
            if (!command(l, "SBuf", l->package + at, n)) return;
            at += n;
        }
        static const uint8_t zero = 0;
        if (!command(l, "SndX", &zero, 1)) return;
        l->ppp_package_end = l->gmtp_block;
        snprintf(l->message, sizeof(l->message), "sending %s", l->name);
        return;
    }
    if (!memcmp(tag, "Ping", 4)) { command(l, "Pong", NULL, 0); return; }
    if (!memcmp(tag, "Pong", 4)) {
        /* It answers again once it has finished taking the package. */
        if (l->state == MRC_PCLINK_SENDING &&
            (l->ppp ? l->ppp_package_acked : l->out.at >= l->out.len)) {
            l->state = MRC_PCLINK_DONE;
            snprintf(l->message, sizeof(l->message),
                     l->ppp ? "%s transfer complete; check the guest for installation errors"
                            : "%s is in the guest's Storeroom", l->name);
        }
        return;
    }
    if (!memcmp(tag, "GBye", 4)) {
        if (l->state != MRC_PCLINK_DONE)
            fail(l, "the guest hung up before taking the package");
        return;
    }
    if (!memcmp(tag, "Abrt", 4)) {
        if (l->state != MRC_PCLINK_DONE) fail(l, "the guest stopped the transfer");
        return;
    }
    /* APkg, Free, Flsh, Baud and anything else: consumed, no action. */
}

/* Pull whole commands out of the unescaped stream. */
static void drain_stream(mrc_pclink *l)
{
    for (;;) {
        size_t have = l->stream.len - l->stream.at;
        if (have < 8) return;
        const uint8_t *p = l->stream.data + l->stream.at;
        uint32_t len = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 |
                       (uint32_t)p[6] << 8 | p[7];
        if (len > PAYLOAD_MAX) {
            fail(l, "the guest sent a %u-byte command payload", len);
            return;
        }
        if (have < 8u + len) return;
        char tag[4];
        memcpy(tag, p, 4);
        handle(l, tag, p + 8, len);
        l->stream.at += 8u + len;
        if (l->stream.at == l->stream.len) l->stream.at = l->stream.len = 0;
        if (l->state == MRC_PCLINK_FAILED) return;
    }
}

static void ppp_frame_received(mrc_pclink *l)
{
    uint8_t *p = l->ppp_frame.data;
    const size_t n = l->ppp_frame.len;
    if (getenv("MRC_PCLINK_TRACE"))
        fprintf(stderr, "[pclink] <- PPP frame (%zu bytes)\n", n);
    if (n < 4 + 8 + 8 + 2) return;
    const uint16_t got_fcs = (uint16_t)(p[n - 2] | (uint16_t)p[n - 1] << 8);
    if (ppp_fcs(p, n - 2) != got_fcs) {
        if (getenv("MRC_PCLINK_TRACE"))
            fprintf(stderr, "[pclink] PPP frame has a bad FCS\n");
        return;
    }
    if (memcmp(p, "\xFF\x03\x00\x21", 4)) return;
    uint8_t *ip = p + 4;
    /* PIC carries GMTP in UDP/IPv4. HIX's GMTPOverPPPLink puts a four-byte
     * packet length and its 24-byte link header before the same GMTP PDU.
     * The total 28-byte prefix is confirmed by HIX MC19 C2's Cnct frame. */
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint16_t ip_id = 0;
    const size_t ihl = (size_t)(ip[0] & 15) * 4;
    const size_t ip_len = (size_t)ip[2] << 8 | ip[3];
    if ((ip[0] >> 4) == 4 && ihl >= 20 && ip[9] == 17 &&
        4 + ip_len + 2 <= n && ihl + 8 <= ip_len) {
        uint8_t *udp = ip + ihl;
        const size_t udp_len = (size_t)udp[4] << 8 | udp[5];
        if (udp_len < 8 + 8 || ihl + udp_len > ip_len) return;
        payload = udp + 8;
        payload_len = udp_len - 8;
        ip_id = (uint16_t)((uint16_t)ip[4] << 8 | ip[5]);
    } else {
        /* HIX's serial link prepends an exact byte count and 24 bytes of
         * link/IP metadata. The shared GMTP PDU then starts at byte 28. */
        const size_t packet_len = n - 4 - 2;
        if (packet_len < 28 + 8 ||
            ((uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 |
             (uint32_t)ip[2] << 8 | ip[3]) != packet_len)
            return;
        payload = ip + 28;
        payload_len = packet_len - 28;
        l->ppp_direct = true;
    }
    if (getenv("MRC_PCLINK_TRACE")) {
        fprintf(stderr, "[pclink] <- GMTP");
        for (size_t i = 0; i < payload_len; i++)
            fprintf(stderr, " %02x", payload[i]);
        fputc('\n', stderr);
    }
    /* HIX piggybacks control and MSG PDUs in one UDP datagram. Each PDU has
     * its own checksum; a MSG may also hold several PC Link commands. */
    for (size_t at = 0; at + 8 <= payload_len;) {
        uint8_t *pdu = payload + at;
        const size_t remaining = payload_len - at;
        size_t header = 8, size = 8;
        if (pdu[0] == 1) {
            header = (pdu[1] & 0x80) ? 12u : 16u;
            if (remaining < header) return;
            size = header + ((size_t)pdu[8] << 8 | pdu[9]);
        } else if (pdu[0] != 4 && pdu[0] != 8) {
            return;
        }
        if (size > remaining || gmtp_checksum(pdu, size) != 0) return;
        at += size;
        if (pdu[0] == 4) {
            if (pdu[2] == l->ppp_prefix[2] && pdu[3] == l->ppp_prefix[3])
                ppp_acked(l, (uint16_t)((uint16_t)pdu[4] << 8 | pdu[5]));
            continue;
        }
        if (pdu[0] != 1) continue; /* NAK */
        memcpy(l->ppp_prefix, pdu, sizeof(l->ppp_prefix));
        l->ppp_ip_id = ip_id;
        const uint16_t block = (uint16_t)((uint16_t)pdu[4] << 8 | pdu[5]);
        const bool fresh = block == l->gmtp_received;
        if (fresh) l->gmtp_received++;
        uint8_t ack[8] = {4, 0, pdu[2], pdu[3],
            (uint8_t)(l->gmtp_received >> 8), (uint8_t)l->gmtp_received, 0, 0};
        const uint16_t check = gmtp_checksum(ack, sizeof(ack));
        ack[6] = (uint8_t)(check >> 8); ack[7] = (uint8_t)check;
        if (!ppp_datagram(l, ack, sizeof(ack))) {
            fail(l, "out of memory acknowledging GMTP");
            return;
        }
        if (!fresh) continue;
        if (!queue_push(&l->stream, pdu + header, size - header)) {
            fail(l, "out of memory decoding GMTP stream");
            return;
        }
        l->greeted = true;
        drain_stream(l);
        if (l->state == MRC_PCLINK_FAILED) return;
    }
}

static void ppp_feed(mrc_pclink *l, uint8_t byte)
{
    if (byte == 0x7E) {
        if (l->ppp_frame.len) ppp_frame_received(l);
        l->ppp_frame.at = l->ppp_frame.len = 0;
        l->ppp_escaped = false;
        return;
    }
    if (l->ppp_escaped) {
        byte ^= 0x20;
        l->ppp_escaped = false;
    } else if (byte == 0x7D) {
        l->ppp_escaped = true;
        return;
    }
    if (!queue_push(&l->ppp_frame, &byte, 1))
        fail(l, "out of memory receiving a PPP frame");
}

void mrc_pclink_from_guest(mrc_pclink *l, uint8_t byte)
{
    if (!l || l->state == MRC_PCLINK_FAILED) return;
    if (l->ppp || (!l->greeted && byte == 0x7E)) {
        l->ppp = true;
        ppp_feed(l, byte);
        return;
    }
    if (!queue_push(&l->wire, &byte, 1)) {
        fail(l, "out of memory receiving from the guest");
        return;
    }
    /* The greeting arrives once, ahead of any block. */
    if (!l->greeted) {
        size_t have = l->wire.len - l->wire.at;
        const uint8_t *p = l->wire.data + l->wire.at;
        for (size_t i = 0; i + 4 <= have; i++) {
            if (memcmp(p + i, "ChMa", 4)) continue;
            l->wire.at += i + 4;
            l->greeted = true;
            break;
        }
        if (!l->greeted) {
            /* Keep only what could still be the start of it. */
            if (have > 3) l->wire.at += have - 3;
            return;
        }
    }

    for (;;) {
        size_t have = l->wire.len - l->wire.at;
        if (have < 2) break;
        const uint8_t *p = l->wire.data + l->wire.at;
        unsigned n = (unsigned)p[0] << 8 | p[1];
        if (n < 1 || n > BLOCK_MAX) {
            fail(l, "the guest sent a %u-byte transport block", n);
            return;
        }
        if (have < n + 6u) break;
        uint32_t want = (uint32_t)p[2 + n] << 24 | (uint32_t)p[3 + n] << 16 |
                        (uint32_t)p[4 + n] << 8 | p[5 + n];
        uint32_t got = crc32_of(p + 2, n);
        if (want != got) {
            fail(l, "a block arrived corrupted (CRC %08x, expected %08x)",
                 got, want);
            return;
        }
        for (unsigned i = 0; i < n; i++) {
            uint8_t b = p[2 + i];
            if (l->escaped) { l->escaped = false; }
            else if (b == CTRL_QUOTE) { l->escaped = true; continue; }
            else if (b == CTRL_END || b == CTRL_ABORT) continue;
            if (!queue_push(&l->stream, &b, 1)) {
                fail(l, "out of memory decoding the guest's stream");
                return;
            }
        }
        l->wire.at += n + 6u;
        if (l->wire.at == l->wire.len) l->wire.at = l->wire.len = 0;
        drain_stream(l);
        if (l->state == MRC_PCLINK_FAILED) return;
    }
}

bool mrc_pclink_to_guest(mrc_pclink *l, uint8_t *byte)
{
    if (!l) return false;
    if (queue_take(&l->out, byte)) {
        l->idle = 0;
        if (l->state == MRC_PCLINK_SENDING && l->sent < l->package_len)
            l->sent++;
        return true;
    }
    if (l->ppp && l->ppp_waiting_ack) return false;
    /* Nothing to send. Ask the guest how it is getting on, now and then. */
    if ((l->state == MRC_PCLINK_CONNECTED || l->state == MRC_PCLINK_SENDING) &&
        ++l->idle >= IDLE_PING) {
        l->idle = 0;
        static const uint8_t zero = 0;
        command(l, "Ping", l->ppp ? &zero : NULL, l->ppp ? 1 : 0);
        return queue_take(&l->out, byte);
    }
    return false;
}

mrc_pclink *mrc_pclink_open(const char *package_path)
{
    if (!package_path) return NULL;
    FILE *f = fopen(package_path, "rb");
    if (!f) {
        fprintf(stderr, "pclink: cannot read %s\n", package_path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || size > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "pclink: %s is empty or improbably large\n", package_path);
        fclose(f);
        return NULL;
    }
    mrc_pclink *l = calloc(1, sizeof(*l));
    if (!l) { fclose(f); return NULL; }
    l->package = malloc((size_t)size);
    if (!l->package || fread(l->package, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "pclink: cannot read %s\n", package_path);
        fclose(f);
        mrc_pclink_close(l);
        return NULL;
    }
    fclose(f);
    l->package_len = (uint32_t)size;

    /* MCap distribution files contain a length-prefixed name followed by
     * an already serialized MPkg stream. Strip only that outer envelope;
     * in particular, its word at offset 8 is NOT the on-disk file size.
     * Leave the stream's metadata and FrozenPackage encoding untouched. */
    if (l->package_len >= 4 && !memcmp(l->package, "MCap", 4)) {
        if (l->package_len < 16 || memcmp(l->package + 4, "\0\0\0\0", 4))
            goto invalid_package;
        uint32_t name_len = (uint32_t)l->package[12] << 24 |
            (uint32_t)l->package[13] << 16 |
            (uint32_t)l->package[14] << 8 | l->package[15];
        if (name_len > l->package_len - 16)
            goto invalid_package;
        uint32_t offset = 16 + name_len;
        if (l->package_len - offset < 34 ||
            memcmp(l->package + offset, "MPkg\0\0\0\1", 8))
            goto invalid_package;
        l->package_len -= offset;
        memmove(l->package, l->package + offset, l->package_len);
    }

    /*
     * Is this a package at all?
     *
     * MIPS packages use the SALTCOD envelope; the older 68k packages use a
     * length-prefixed flat CLUS data fork. The extension is a habit of
     * whoever published it, not a format. So the container is what decides,
     * and a file that is not one is refused here rather than after minutes
     * of a guest patiently receiving nonsense.
     */
    static const uint8_t signature[8] = { 0x00, 'S', 'A', 'L', 'T', 'C', 'O', 'D' };
    bool mips_package = l->package_len >= sizeof(signature) &&
        !memcmp(l->package, signature, sizeof(signature));
    /* Magic Cap 1.x's 68k linker emits a flat CLUS data fork.  Its first
     * sixteen bytes are reserved and the first longword is the file length;
     * unlike the later MIPS SALTCOD envelope it has no ASCII signature. */
    bool m68k_package = l->package_len >= 20 &&
        !memcmp(l->package, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) &&
        ((uint32_t)l->package[16] << 24 | (uint32_t)l->package[17] << 16 |
         (uint32_t)l->package[18] << 8 | l->package[19]) == l->package_len;
    bool stream_package = l->package_len >= 34 &&
        !memcmp(l->package, "MPkg\0\0\0\1", 8);
    l->m68k_package = m68k_package;
    if (!mips_package && !m68k_package && !stream_package)
        goto invalid_package;

    /* The name the guest shows: the filename without its directory or its
     * extension, which is what WinPcLink offers too. */
    const char *base = strrchr(package_path, '/');
    base = base ? base + 1 : package_path;
    snprintf(l->name, sizeof(l->name), "%s", base);
    char *dot = strrchr(l->name, '.');
    if (dot && dot != l->name) *dot = 0;

    l->state = MRC_PCLINK_WAITING;
    snprintf(l->message, sizeof(l->message),
             "waiting for the guest to link; open the Storeroom and tap the "
             "computer");
    return l;

invalid_package:
    fprintf(stderr, "pclink: %s is not a Magic Cap package\n", package_path);
    mrc_pclink_close(l);
    return NULL;
}

void mrc_pclink_close(mrc_pclink *l)
{
    if (!l) return;
    queue_free(&l->out);
    queue_free(&l->wire);
    queue_free(&l->stream);
    queue_free(&l->ppp_frame);
    queue_free(&l->ppp_pending);
    free(l->package);
    free(l);
}

mrc_pclink_state mrc_pclink_state_of(const mrc_pclink *l)
{
    return l ? l->state : MRC_PCLINK_FAILED;
}

const char *mrc_pclink_message(const mrc_pclink *l)
{
    return l && l->message[0] ? l->message : "no transfer";
}

uint32_t mrc_pclink_sent(const mrc_pclink *l) { return l ? l->sent : 0; }
uint32_t mrc_pclink_total(const mrc_pclink *l) { return l ? l->package_len : 0; }
