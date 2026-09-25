#include "machines/datarover840/keyboard.h"
#include "soc/tx39/tx39.h"
#include <string.h>

/* Stock ROM command/address encoding, observed at 13C28500..13C2852C.
 * Literal wire values, never fetched from guest ROM at runtime. */
static const uint16_t codes[] = {
    0,0xc018,0xc024,0xc028,0xc030,0xc03c,0xc044,0xc048,0xc050,
    0xc000,0xc00c,0xc014,0xc05c,0xc060,0xc06c,0xc074,0xc078,
    0xc084,0xc088,0xc090,0xc09c,0xd0e0,0xd0ec,0xd0a4,0xd0a8,
    0xd0b0,0xd0bc,0xd0c4,0xd0c8,0xd0d0,0xd0dc,0xd0f4,0xd0f8
};
static const uint16_t addresses[] = {0xc00,0xa00,0x804,0x600,0x404,0x204,0,0xe04};

static void release_keys(mrc_dr_keyboard *k);

static void request_line(mrc_dr_keyboard *k)
{
    if (k->assigned)
        mrc_mbus_input(k->controller, k->keys.count && !k->notified);
}

static void command(void *ctx, uint16_t word)
{
    mrc_dr_keyboard *k = ctx;
    for (unsigned a = 0; a < 8; ++a) {
        for (unsigned op = 1; op < 33; ++op) {
            if ((codes[op] ^ addresses[a]) != word) continue;
            if (op == 31 && a == 7) {
                k->assigned = k->notified = false;
                k->selection = k->pending_read = k->write_size = 0;
                mrc_mbus_input(k->controller, true);
                return;
            }
            if (op == 24 && a == 0) { k->assigned = true; return; }
            if (!k->assigned || (a != 0 && a != 7)) return;
            if (op == 21 && a == 0) {
                /* Single peripheral: no next device in the discovery chain. */
                mrc_mbus_input(k->controller, false);
                return;
            }
            if (op == 28) { request_line(k); return; }
            if (a == 7) { k->unknown_commands++; return; }
            switch (op) {
            case 1: case 2: k->pending_read = op; return;
            case 5: k->selection = 5; k->write_size = 0; return;
            case 12: case 13: k->selection = op; return;
            case 32: return; /* Host transfer termination. */
            default: k->unknown_commands++; return;
            }
        }
    }
    k->unknown_commands++;
}

static void transmit(void *ctx, uint32_t word, unsigned bits)
{
    mrc_dr_keyboard *k = ctx;
    if (k->selection != 5 || bits != 32 || k->write_size > 4) {
        k->rejected_writes++;
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        k->write_data[k->write_size++] = (uint8_t)(word >> (24 - 8*i));
    if (k->write_size == 8) {
        if (!mrc_mb_keyboard_write(&k->keys, k->write_data, 8)) k->rejected_writes++;
        k->selection = 0;
        if (!k->keys.count) k->notified = false;
        request_line(k);
    }
}

static void put32(uint8_t *data, unsigned at, uint32_t v)
{
    for (int i = 3; i >= 0; --i) { data[at+i] = (uint8_t)v; v >>= 8; }
}

static unsigned information(uint8_t *data)
{
    /* Constructed emulated accessory profile, not a physical keyboard dump.
     * Format/checksum verified by stock PeripheralInfo at 13C29284. */
    put32(data,4,MRC_MBKEY_ID);
    put32(data,0xc,256000); put32(data,0x14,256000); put32(data,0x1c,256000);
    put32(data,0x24,16); put32(data,0x28,16);
    put32(data,0x2c,10000000); put32(data,0x30,10000000);
    data[0x40] = 16;
    const char *names[] = {"Emulated AT keyboard", "Keyboard", "magicrecomp"};
    unsigned at = 0x50;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned n = (unsigned)strlen(names[i]);
        data[at++] = (uint8_t)n; memcpy(data+at,names[i],n); at += n;
    }
    unsigned size = (at + 1 + 2 + 3) & ~3u;
    data[2] = (uint8_t)((size-4) >> 8); data[3] = (uint8_t)(size-4);
    unsigned sum = 1;
    for (unsigned i = 0; i < size-2; ++i) sum += data[i];
    data[size-2] = (uint8_t)(sum >> 8); data[size-1] = (uint8_t)sum;
    return size;
}

void mrc_dr_keyboard_init(mrc_dr_keyboard *k, tx39_mbus *controller)
{
    memset(k,0,sizeof(*k));
    k->controller = controller;
    k->port.ctx = k; k->port.command = command; k->port.transmit = transmit;
    /* Low before enumeration means a newly connected device. */
    k->port.input_high = false;
}

bool mrc_dr_keyboard_key(mrc_dr_keyboard *k, uint8_t code, bool extended, bool down)
{
    if (!mrc_mb_keyboard_key(&k->keys, code, extended, down)) return false;
    request_line(k);
    return true;
}

void mrc_dr_keyboard_service(mrc_dr_keyboard *k)
{
    tx39_mbus *m = k->controller;
    if (m->soc->mbus_port != &k->port) return;
    release_keys(k);
    if (!k->pending_read || (m->reg[0] & 9) != 9)
        return;
    uint8_t data[256] = {0};
    unsigned size;
    /* Prepare without consuming scan bytes until controller accepts them. */
    mrc_mb_keyboard after = k->keys;
    bool scan_read = false, request = k->pending_read == 1;
    if (request) {
        /* Request header byte 1=0E: stock PeripheralRequest at 13C27B20.
         * Remaining request bytes are unused by that driver. */
        data[1] = k->keys.count ? 0x0e : 0;
        size = 4;
    } else if (k->selection == 12) {
        put32(data,0,MRC_MBKEY_ID); size = 4;
    } else if (k->selection == 13) {
        size = information(data);
    } else if (k->selection == 0) {
        size = (unsigned)mrc_mb_keyboard_read(&after,data,16);
        scan_read = true;
    } else {
        k->receive_errors++; k->pending_read = 0; return;
    }
    unsigned rounded = (size+3) & ~3u;
    unsigned capacity = (m->reg[0] & 0x10000)
        ? (m->reg[(TX39_MBUSDMALENGTH-TX39_MBUS_FIRST)/4] & 0xffffc) + 4
        : ((m->reg[0] & 2) ? 4 : 2);
    /* This profile uses whole long words. Reject unsupported short buffers
     * instead of repeatedly overwriting a non-DMA receive register. */
    if (!(m->reg[0] & 2) || rounded > capacity) {
        k->receive_errors++; k->pending_read = 0; return;
    }
    for (unsigned at = 0; at < rounded; at += 4) {
        uint32_t word = 0;
        for (unsigned i = 0; i < 4; ++i) word = (word << 8) | data[at+i];
        if (!mrc_mbus_receive_word(m,word)) {
            k->receive_errors++; k->pending_read = 0; return;
        }
    }
    /* Peripheral end command inferred from ROM encoding; verified by fresh
     * boot through discovery, request queue, and DispatchATKeys. */
    if (!mrc_mbus_receive_command(m,0xdcf8)) {
        k->receive_errors++; k->pending_read = 0; return;
    }
    k->pending_read = 0;
    if (request) k->notified = true;
    else {
        k->selection = 0;
        if (scan_read) { k->keys = after; k->notified = false; }
    }
    request_line(k);
}

bool mrc_dr_keyboard_host_key(mrc_dr_keyboard *k, unsigned usage, bool down, bool repeat)
{
    uint8_t code; bool ext;
    if (!mrc_mb_keyboard_usage(usage,&code,&ext)) return false;
    uint8_t mask = (uint8_t)(1u << (usage & 7)); unsigned at = usage / 8;
    if (!down) {
        if (k->held[at] & mask) {
            k->held[at] &= (uint8_t)~mask;
            k->release[at] |= mask;
            release_keys(k);
        }
        return true;
    }
    /* Never put a new make ahead of a pending break for the same key. */
    if (k->release[at] & mask) return false;
    if ((k->held[at] & mask) && !repeat) return true;
    if (repeat && !(k->held[at] & mask)) return true;
    if (!mrc_dr_keyboard_key(k,code,ext,true)) return false;
    k->held[at] |= mask;
    return true;
}

void mrc_dr_keyboard_release(mrc_dr_keyboard *k)
{
    for (unsigned i=0;i<32;++i) { k->release[i] |= k->held[i]; k->held[i]=0; }
}

static void release_keys(mrc_dr_keyboard *k)
{
    for (unsigned usage=0;usage<256;++usage) {
        unsigned at=usage/8; uint8_t mask=(uint8_t)(1u<<(usage&7));
        if (!(k->release[at]&mask)) continue;
        uint8_t code; bool ext;
        if (mrc_mb_keyboard_usage(usage,&code,&ext) &&
            MRC_MBKEY_CAPACITY-k->keys.count < (ext?3u:2u)) break;
        if (!mrc_mb_keyboard_usage(usage,&code,&ext) ||
            mrc_dr_keyboard_key(k,code,ext,false)) k->release[at]&=(uint8_t)~mask;
        else break; /* Queue full: retry next service, never drop a release. */
    }
}

void mrc_dr_keyboard_encode(const mrc_dr_keyboard *k, uint8_t *d)
{
    memset(d,0,MRC_DR_KEYBOARD_STATE_SIZE);
    d[0]=1;
    d[1]=(k->controller->soc->mbus_port==&k->port);
    d[2]=k->assigned; d[3]=k->notified; d[4]=k->selection;
    d[5]=k->pending_read; d[6]=k->write_size;
    d[7]=k->port.input_high; d[8]=k->port.rx_complete;
    d[9]=k->keys.leds; d[10]=k->keys.repeat;
    d[12]=(uint8_t)(k->keys.count>>8); d[13]=(uint8_t)k->keys.count;
    put32(d,16,k->port.rx_bytes);
    for (unsigned i=0;i<k->keys.count;++i)
        d[24+i]=k->keys.queue[(k->keys.head+i)%MRC_MBKEY_CAPACITY];
    memcpy(d+280,k->write_data,8);
    memcpy(d+288,k->held,32); memcpy(d+320,k->release,32);
}

bool mrc_dr_keyboard_decode(mrc_dr_keyboard *k, const uint8_t *d)
{
    unsigned count=((unsigned)d[12]<<8)|d[13];
    uint32_t rx=((uint32_t)d[16]<<24)|((uint32_t)d[17]<<16)|((uint32_t)d[18]<<8)|d[19];
    if (d[0]!=1 || d[1]>1 || d[2]>1 || d[3]>1 ||
        (d[4]!=0 && d[4]!=5 && d[4]!=12 && d[4]!=13) || d[5]>2 ||
        d[6]>8 || (d[6]&3) || d[7]>1 || d[8]>1 || d[9]>7 || d[10]>127 ||
        count>MRC_MBKEY_CAPACITY || rx>0x100000 || (rx&3)) return false;
    for (unsigned u=0;u<256;++u) {
        uint8_t code; bool ext;
        if (((d[288+u/8]|d[320+u/8])&(1u<<(u&7))) &&
            !mrc_mb_keyboard_usage(u,&code,&ext)) return false;
    }
    tx39_mbus *controller=k->controller;
    mrc_dr_keyboard_init(k,controller);
    k->assigned=d[2]; k->notified=d[3]; k->selection=d[4];
    k->pending_read=d[5]; k->write_size=d[6];
    k->port.input_high=d[7]; k->port.rx_complete=d[8]; k->port.rx_bytes=rx;
    k->keys.leds=d[9]; k->keys.repeat=d[10]; k->keys.count=count;
    memcpy(k->keys.queue,d+24,count); memcpy(k->write_data,d+280,8);
    memcpy(k->held,d+288,32); memcpy(k->release,d+320,32);
    controller->soc->mbus_port=d[1]?&k->port:NULL;
    return true;
}
