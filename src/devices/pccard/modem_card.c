/*
 * modem_card.c -- a PC Card data modem: a 16550 behind a Hayes responder.
 *
 * A prototype, to find out whether the DataRover's ROM will use a generic
 * modem card for its own PPP ("PC Card modem" is in its string table), so
 * that the MiSTer core can offer Internet access with no guest package and
 * nothing on the host but MiSTer's own serial PPP mode.
 *
 * The CIS is a real card's: linux-firmware cis/MT5634ZLX.cis, a Multi-Tech
 * 56K data/fax card -- a serial-port function (FUNCID 2) with its COR at
 * attribute 0xFF80 and four configurations at the PC's COM ranges. Attribute
 * bytes are at even addresses in window A, as the NE2000's are; the UART is
 * assumed to be in window A at its I/O address, as the NE2000's registers
 * are at +0x300. Every access is logged until the log budget runs out, so
 * what the ROM actually does is visible whichever of these guesses is wrong.
 *
 * The responder: commands echo (unless ATE0), each AT line answers OK (or 0
 * with ATV0), ATD answers CONNECT and goes on line, ATH hangs up, and on
 * line the bytes go to the far end -- a pty, when one is attached -- and
 * "+++" after a second's quiet returns to commands.
 */
#include "devices/pccard/pccard.h"
#include "host/serial.h"
#include <ctype.h>
#include <string.h>

static const uint8_t cis[] = {
    0x01, 0x01, 0xff,
    0x15, 0x22, 0x04, 0x01, 'M','u','l','t','i','T','e','c','h', 0x00,
          'P','C','M','C','I','A',' ','5','6','K',' ','D','a','t','a','F','a','x', 0x00, 0x00, 0x00, 0xff,
    0x20, 0x04, 0x00, 0x02, 0x01, 0x00,
    0x21, 0x02, 0x02, 0x00,
    0x1a, 0x05, 0x01, 0x27, 0x80, 0xff, 0x67,
    0x1b, 0x0f, 0xcf, 0x41, 0x8b, 0x01, 0x55, 0x01, 0x55, 0x01, 0x55, 0xaa, 0x60, 0xf8, 0x03, 0x07, 0x28,
    0x1b, 0x08, 0x97, 0x01, 0x08, 0xaa, 0x60, 0xf8, 0x02, 0x07,
    0x1b, 0x08, 0x9f, 0x01, 0x08, 0xaa, 0x60, 0xe8, 0x03, 0x07,
    0x1b, 0x08, 0xa7, 0x01, 0x08, 0xaa, 0x60, 0xe8, 0x02, 0x07,
    0x14, 0x00,
    0xff,
};
#define COR_ADDR 0xff80u
#define BYTE_NS  86806u                /* a 10-bit frame at 115200 */

static void rxq(mh_modem *m, const char *s)
{
    for (; *s; s++) {
        unsigned next = (m->rx_head + 1) % sizeof(m->rx);
        if (next == m->rx_tail) return;
        m->rx[m->rx_head] = (uint8_t)*s;
        m->rx_head = next;
    }
}
static void result(mh_modem *m, const char *word, const char *code)
{
    char buf[64];
    if (m->verbose) snprintf(buf, sizeof buf, "\r\n%s\r\n", word);
    else snprintf(buf, sizeof buf, "%s\r", code);
    rxq(m, buf);
}

static uint16_t io_base(const mh_pccard *c)
{
    switch (c->config & 0x3f) {
    case 0x0f: return 0x3f8;
    case 0x17: return 0x2f8;
    case 0x1f: return 0x3e8;
    case 0x27: return 0x2e8;
    default:   return 0;
    }
}

static void command(mh_pccard *c)
{
    mh_modem *m = &c->modem;
    m->line[m->line_len] = 0;
    char up[sizeof m->line];
    for (unsigned i = 0; i <= m->line_len; i++) up[i] = (char)toupper((unsigned char)m->line[i]);
    fprintf(c->log, "modem: command \"%s\"\n", m->line);
    m->line_len = 0;
    if (strncmp(up, "AT", 2)) return;
    for (const char *p = up + 2; *p; ) {
        char k = *p++;
        if (k == ' ') continue;
        if (k == 'D') {
            fprintf(c->log, "modem: dialling \"%s\"; CONNECT\n", p);
            result(m, "CONNECT 115200", "1");
            m->online = true;
            m->plus = 0;
            return;
        }
        if (k == 'E') { m->echo = *p != '0'; if (isdigit((unsigned char)*p)) p++; continue; }
        if (k == 'V') { m->verbose = *p != '0'; if (isdigit((unsigned char)*p)) p++; continue; }
        if (k == 'H') { m->online = false; while (isdigit((unsigned char)*p)) p++; continue; }
        if (k == 'O') { if (m->connected_once) { result(m, "CONNECT 115200", "1"); m->online = true; return; } continue; }
        if (k == 'Z') { m->echo = true; m->verbose = true; while (isdigit((unsigned char)*p)) p++; continue; }
        if (k == 'I') { while (isdigit((unsigned char)*p)) p++; rxq(m, "\r\nMiSTer DataRover modem\r\n"); continue; }
        /* &x, \x, %x, Sn=v, Xn, Wn, Ln, Mn, Qn...: accepted and ignored. */
        if (k == '&' || k == '\\' || k == '%' || k == '+') { if (*p) p++; }
        if (k == 'S') { while (*p && *p != '=' && *p != '?') p++; if (*p) p++; }
        while (isdigit((unsigned char)*p)) p++;
    }
    result(m, "OK", "0");
}

static void thr(mh_pccard *c, uint8_t b, uint64_t now)
{
    mh_modem *m = &c->modem;
    m->tx_bytes++;
    if (m->online) {
        if (m->log_data) {
            m->log_data--;
            fprintf(c->log, "modem: data -> %02X%s\n", b, b == 0x7e ? "  (PPP flag)" : "");
        }
        m->connected_once = true;
        /* "+++" with a second's quiet either side returns to commands. */
        if (b == '+' && (m->plus || now - m->last_tx_ns > 1000000000ull)) m->plus++;
        else m->plus = 0;
        m->last_tx_ns = now;
        if (m->link) mh_serial_write(m->link, b);
        return;
    }
    m->last_tx_ns = now;
    if (m->echo) { char e[2] = { (char)b, 0 }; rxq(m, e); }
    if (b == '\r') command(c);
    else if (b == 8 || b == 127) { if (m->line_len) m->line_len--; }
    else if (b != '\n' && m->line_len + 1 < sizeof m->line) m->line[m->line_len++] = (char)b;
}

static bool rx_ready(const mh_modem *m, uint64_t now)
{
    return m->rx_head != m->rx_tail && now >= m->next_rx_ns;
}

bool mh_modem_irq(const mh_pccard *c)
{
    const mh_modem *m = &c->modem;
    if (!(m->mcr & 8)) return false;              /* OUT2 gates the interrupt */
    return ((m->ier & 1) && rx_ready(m, m->now_ns)) ||
           ((m->ier & 2) && m->thre_pending) ||
           ((m->ier & 8) && m->msr_delta);
}

void mh_modem_tick(mh_pccard *c, uint64_t now_ns)
{
    mh_modem *m = &c->modem;
    m->now_ns = now_ns;
    uint8_t dcd = m->online ? 0x80 : 0;
    if (dcd != m->dcd) { m->dcd = dcd; m->msr_delta |= 0x08; }
    if (m->online && m->plus >= 3 && now_ns - m->last_tx_ns > 1000000000ull) {
        m->online = false; m->plus = 0;
        result(m, "OK", "0");
    }
    uint8_t b;
    while (m->online && m->link && ((m->rx_head + 1) % sizeof(m->rx)) != m->rx_tail &&
           mh_serial_read(m->link, &b)) {
        m->rx[m->rx_head] = b;
        m->rx_head = (m->rx_head + 1) % sizeof(m->rx);
    }
}

static uint8_t uart_read(mh_pccard *c, unsigned r)
{
    mh_modem *m = &c->modem;
    uint64_t now = m->now_ns;
    switch (r) {
    case 0:
        if (m->lcr & 0x80) return m->dll;
        if (rx_ready(m, now)) {
            uint8_t b = m->rx[m->rx_tail];
            m->rx_tail = (m->rx_tail + 1) % sizeof(m->rx);
            m->next_rx_ns = now + BYTE_NS;
            m->rx_bytes++;
            return b;
        }
        return 0;
    case 1: return (m->lcr & 0x80) ? m->dlm : m->ier;
    case 2: {
        uint8_t fifo = (m->fcr & 1) ? 0xc0 : 0;
        if ((m->ier & 1) && rx_ready(m, now)) return fifo | 0x04;
        if ((m->ier & 2) && m->thre_pending) { m->thre_pending = false; return fifo | 0x02; }
        if ((m->ier & 8) && m->msr_delta) return fifo | 0x00;
        return fifo | 0x01;
    }
    case 3: return m->lcr;
    case 4: return m->mcr;
    case 5: return (rx_ready(m, now) ? 1 : 0) | 0x60;
    case 6: {
        uint8_t v = 0x30 | m->dcd | m->msr_delta;  /* CTS, DSR, DCD and deltas */
        m->msr_delta = 0;
        return v;
    }
    default: return m->scr;
    }
}

static void uart_write(mh_pccard *c, unsigned r, uint8_t v)
{
    mh_modem *m = &c->modem;
    switch (r) {
    case 0:
        if (m->lcr & 0x80) { m->dll = v; return; }
        thr(c, v, m->now_ns);
        m->thre_pending = true;               /* sent at once */
        return;
    case 1:
        if (m->lcr & 0x80) { m->dlm = v; return; }
        if ((v & 2) && !(m->ier & 2)) m->thre_pending = true;
        m->ier = v & 0x0f;
        return;
    case 2: m->fcr = v; return;
    case 3: m->lcr = v; return;
    case 4: m->mcr = v; return;
    case 7: m->scr = v; return;
    default: return;
    }
}

static bool read_card(mh_pccard *c, mh_pccard_window w, uint32_t off, unsigned size, uint32_t *out)
{
    uint16_t base = io_base(c);
    if (w != MH_PCCARD_WINDOW_A) return false;
    if (base && off >= base && off < base + 8u && size == 1) {
        *out = uart_read(c, off - base);
        if (c->modem.log_io) { c->modem.log_io--; fprintf(c->log, "modem: R +%X = %02X\n", off - base, *out); }
        return true;
    }
    if (off == COR_ADDR && size == 1) { *out = c->config; return true; }
    if (off > COR_ADDR && off < COR_ADDR + 16 && size == 1) { *out = 0; return true; }
    if (size == 1 && !(off & 1) && off / 2 < sizeof cis) { *out = cis[off / 2]; return true; }
    if (c->modem.log_io) { c->modem.log_io--; fprintf(c->log, "modem: unmapped R%u +%X\n", size * 8, off); }
    return false;
}

static bool write_card(mh_pccard *c, mh_pccard_window w, uint32_t off, unsigned size, uint32_t val)
{
    uint16_t base = io_base(c);
    if (w != MH_PCCARD_WINDOW_A) return false;
    if (base && off >= base && off < base + 8u && size == 1) {
        if (c->modem.log_io) { c->modem.log_io--; fprintf(c->log, "modem: W +%X = %02X\n", off - base, val & 0xff); }
        uart_write(c, off - base, (uint8_t)val);
        return true;
    }
    if (off == COR_ADDR && size == 1) {
        c->config = (uint8_t)val;
        fprintf(c->log, "modem: COR = %02X (UART at %03X)\n", c->config, io_base(c));
        return true;
    }
    if (off > COR_ADDR && off < COR_ADDR + 16 && size == 1) return true;
    if (c->modem.log_io) { c->modem.log_io--; fprintf(c->log, "modem: unmapped W%u +%X = %X\n", size * 8, off, val); }
    return false;
}

void mh_modem_init(mh_pccard *c)
{
    memset(&c->modem, 0, sizeof c->modem);
    c->modem.echo = true;
    c->modem.verbose = true;
    c->modem.log_io = 400;
    c->modem.log_data = 64;
}

const mh_pccard_kind mh_pccard_modem = {
    .name = "modem", .read = read_card, .write = write_card
};
