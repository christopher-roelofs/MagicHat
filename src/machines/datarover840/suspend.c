/* Retained standby context: the hardware state that survives a suspend with
 * RAM held up.
 * Only hardware fields are encoded: no host pointers, diagnostics settings,
 * sockets, or automatic full snapshot. Bump VERSION when field semantics or
 * ordering change. This encoding is build/host-layout checked, not portable.
 */
#include "machines/datarover840/machine.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VERSION 2u

typedef struct {
    uint32_t magic, version, endian, hardware_size;
    uint32_t ram_size, rom_size, patches, reserved;
    uint64_t firmware;
    uint32_t card_type[2], card_size[2];
    uint64_t card_hash[2];
} header;

static uint64_t hash(const uint8_t *p, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (n--) h = (h ^ *p++) * UINT64_C(1099511628211);
    return h;
}
static unsigned card_type(const mh_pccard *c)
{
    if (c->kind == &mh_pccard_memory) return 1;
    if (c->kind == &mh_pccard_sram) return 2;
    if (c->kind == &mh_pccard_ne2000) return 3;
    return 0;
}

/* Explicit field list deliberately excludes every process pointer. NULL is
 * a sizing pass. Decode is first performed into a temporary board copy. */
static size_t hardware(machine *m, uint8_t *data, bool load)
{
    size_t at = 0;
#define BYTES(field, count) do { size_t n = (count); if (data) { \
    if (load) memcpy(&(field), data + at, n); \
    else memcpy(data + at, &(field), n); } at += n; } while (0)
#define F(field) BYTES(m->field, sizeof(m->field))
    F(cpu.r); F(cpu.hi); F(cpu.lo); F(cpu.pc); F(cpu.next_pc);
    F(cpu.cur_pc); F(cpu.branch_pending); F(cpu.in_delay); F(cpu.cp0);
    F(cpu.tlb); F(cpu.irq_lines); F(cpu.insn_count); F(cpu.cycle_count);
    F(cpu.halted); F(cpu.exc_refill); F(cpu.power_stopped); F(cpu.has_mmu);
    F(soc.icu.status); F(soc.icu.enable);
    for (unsigned i = 0; i < 2; i++) {
        F(soc.uart[i].ctrl1); F(soc.uart[i].ctrl2);
        F(soc.uart[i].dmactrl1); F(soc.uart[i].dmactrl2);
        F(soc.uart[i].dmacnt); F(soc.uart[i].rx); F(soc.uart[i].rx_full);
    }
    F(soc.uart_timing);
    F(soc.sib.size); F(soc.sib.ctrl); F(soc.sib.dmactrl);
    F(soc.sib.sndrxstart); F(soc.sib.sndtxstart);
    F(soc.sib.telrxstart); F(soc.sib.teltxstart);
    F(soc.sib.sndhold); F(soc.sib.telhold);
    F(soc.sib.sf0ctrl); F(soc.sib.sf1ctrl);
    F(soc.sib.sf0stat); F(soc.sib.sf1stat); F(soc.sib.codec);
    F(soc.sib.snd_read_off); F(soc.sib.snd_cycle_ref);
    F(soc.sib.snd_running); F(soc.sib.irq_seen); F(soc.audio);
    F(soc.video.ctrl);
    F(soc.timer.control); F(soc.timer.periodic);
    F(soc.timer.alarm_hi); F(soc.timer.alarm_lo); F(soc.timer.rtc);
    F(soc.timer.rtc_cycle_ref); F(soc.timer.periodic_reload); F(soc.timer.last_period);
    F(soc.mbus.reg); F(soc.power.ctrl); F(soc.power.stptimer_armed);
    F(soc.power.stptimer_deadline);
    F(soc.memconfig); F(soc.io_ctrl); F(soc.io_dataout); F(soc.io_datadir);
    F(soc.io_datain); F(soc.io_datasel); F(soc.io_powerdwn); F(soc.io_mfiopowerdwn);
    F(soc.clockctrl); F(soc.ext); F(soc.spictrl); F(soc.spihold);
    F(soc.irctrl1); F(soc.irctrl2); F(soc.irtxhold); F(soc.cpu_hz);
    F(kseg3.reg);
    for (unsigned i = 0; i < 2; i++) {
        F(pcmcia[i].reg); F(pcmcia[i].card_present); F(card_in[i]);
        F(card[i].config);
        /* NE2000 prefix contains its registers, packet RAM and transmit
         * timing, but stops before host send callback/opaque pointers. */
        BYTES(m->card[i].nic, offsetof(ne2000, send));
    }
#undef F
#undef BYTES
    return at;
}

bool mh_suspend_save(machine *m, uint8_t **data, uint32_t *size)
{
    *data = NULL; *size = 0;
    /* A running/failed session is a RAM checkpoint, not a suspended device. */
    if (!m->cpu.power_stopped || m->cpu.halted) return true;
    header h = { .magic = 0x44525350, .version = m->soc.mbus_port ? VERSION : 1, .endian = 0x01020304,
        .hardware_size = (uint32_t)hardware(m, NULL, false),
        .ram_size = m->ram_size, .rom_size = m->rom_size,
        .firmware = hash(m->rom_original, m->rom_size) };
    for (uint32_t i = 0; i < m->rom_size; i++)
        if (m->rom[i] != m->rom_original[i]) h.patches++;
    for (unsigned i = 0; i < 2; i++) {
        h.card_type[i] = card_type(&m->card[i]);
        h.card_size[i] = m->card[i].image_len;
        h.card_hash[i] = hash(m->card[i].image, m->card[i].image_len);
    }
    if (m->soc.mbus_port && m->soc.mbus_port != &m->keyboard.port) return false;
    size_t keyboard_size = h.version == 2 ? MH_DR_KEYBOARD_STATE_SIZE : 0;
    size_t total = sizeof(h) + h.hardware_size + (size_t)h.patches * 5 + keyboard_size;
    if (total > 64u * 1024 * 1024) return false;
    uint8_t *p = malloc(total);
    if (!p) return false;
    memcpy(p, &h, sizeof(h));
    hardware(m, p + sizeof(h), false);
    size_t at = sizeof(h) + h.hardware_size;
    /* Preserve writes that the guest actually made to our writable firmware
     * model. Never synthesize patches or modify the original ROM file. */
    for (uint32_t i = 0; i < m->rom_size; i++) {
        if (m->rom[i] == m->rom_original[i]) continue;
        memcpy(p + at, &i, 4); p[at + 4] = m->rom[i]; at += 5;
    }
    if (keyboard_size) mh_dr_keyboard_encode(&m->keyboard,p+at);
    *data = p; *size = (uint32_t)total;
    return true;
}

bool mh_suspend_load(machine *m, const uint8_t *data, uint32_t size)
{
    header h;
    if (size < sizeof(h)) goto invalid;
    memcpy(&h, data, sizeof(h));
    if (h.magic != 0x44525350 || (h.version != 1 && h.version != VERSION) || h.endian != 0x01020304 ||
        h.reserved || h.hardware_size != hardware(m, NULL, false) ||
        h.ram_size != m->ram_size || h.rom_size != m->rom_size ||
        h.firmware != hash(m->rom_original, m->rom_size) ||
        sizeof(h) + (uint64_t)h.hardware_size + (uint64_t)h.patches * 5 +
        (h.version == 2 ? MH_DR_KEYBOARD_STATE_SIZE : 0) != size)
        goto invalid;
    size_t start = sizeof(h) + h.hardware_size;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < h.patches; i++) {
        uint32_t offset; memcpy(&offset, data + start + (size_t)i * 5, 4);
        if (offset >= m->rom_size || (i && offset <= previous)) goto invalid;
        previous = offset;
    }
    machine *copy = malloc(sizeof(*copy));
    if (!copy) return false;
    *copy = *m;
    hardware(copy, (uint8_t *)data + sizeof(h), true);
    if (!copy->cpu.power_stopped || copy->cpu.halted ||
        copy->soc.cpu_hz != DR840_CPU_HZ ||
        (copy->soc.power.ctrl & (PWRCTRL_PWRCS | PWRCTRL_VCCON)) ||
        copy->soc.audio.cursor >= UCB_AUDIO_TAPS ||
        copy->card[0].nic.tx_len > 1518 || copy->card[1].nic.tx_len > 1518) {
        free(copy); goto invalid;
    }
    uint8_t empty_keyboard[MH_DR_KEYBOARD_STATE_SIZE] = {1};
    const uint8_t *keyboard_data = h.version == 2
        ? data + start + (size_t)h.patches * 5 : empty_keyboard;
    copy->soc.mbus.soc = &copy->soc;
    mh_dr_keyboard_init(&copy->keyboard,&copy->soc.mbus);
    bool keyboard_ok = mh_dr_keyboard_decode(&copy->keyboard,keyboard_data);
    free(copy);
    if (!keyboard_ok) goto invalid;
    /* Validation is complete before modifying live CPU/devices or firmware. */
    for (unsigned i = 0; i < 2; i++) {
        if (h.card_type[i] > 3) goto invalid;
    }
    mh_pccard configured[2] = { m->card[0], m->card[1] };
    hardware(m, (uint8_t *)data + sizeof(h), true);
    for (uint32_t i = 0; i < h.patches; i++) {
        uint32_t offset; memcpy(&offset, data + start + (size_t)i * 5, 4);
        m->rom[offset] = data[start + (size_t)i * 5 + 4];
    }
    for (unsigned i = 0; i < 2; i++) {
        if (h.card_type[i] != card_type(&configured[i]) ||
            h.card_size[i] != configured[i].image_len ||
            h.card_hash[i] != hash(configured[i].image, configured[i].image_len)) {
            /* Cards can be removed/replaced while the device is off. The
             * controller generates removal, then the normal insertion path
             * presents this session's configured card. */
            m->card[i] = configured[i];
            mh_glacier_set_present(&m->pcmcia[i], false);
            m->card_in[i] = false;
        }
    }
    mh_dr_keyboard_decode(&m->keyboard,keyboard_data);
    m->input_epoch_set = false;
    fprintf(stderr, "suspend: restored suspended CPU at %08X (%u retained firmware bytes)\n",
            m->cpu.pc, h.patches);
    return true;
invalid:
    fprintf(stderr, "suspend: context is incompatible with this firmware/build or invalid; "
                    "use --reset-pc 0xbfc00000 for explicit RAM-only recovery\n");
    return false;
}
