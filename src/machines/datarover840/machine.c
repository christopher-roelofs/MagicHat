#include "machines/datarover840/machine.h"

#include <limits.h>
#include "host/serial.h"

#include <stdlib.h>
#include <string.h>

void mrc_machine_rebind(machine *m);

/* Electrical edges must propagate on register accesses, not just between
 * 1024-instruction batches. Otherwise an ISR/IMR acknowledgement followed by
 * a new packet can hide the deassertion from Glacier and lose the next IRQ.
 * The shared Glacier output has the same requirement at its W1C registers.
 * Wiring: measured falling Glacier bit 2, TX39 CARDDIRPOSINT (NetBSD ICU). */
static void card_irq_update(machine *m)
{
    for (unsigned sl = 0; sl < 2; sl++)
        if (m->card_in[sl] && m->card[sl].kind == &mrc_pccard_ne2000)
            mrc_glacier_set_card_irq(&m->pcmcia[sl],
                                    mrc_ne2000_irq(&m->card[sl].nic));
        else if (m->card_in[sl] && m->card[sl].kind == &mrc_pccard_modem)
            mrc_glacier_set_card_irq(&m->pcmcia[sl], mrc_modem_irq(&m->card[sl]));
    bool asserted = mrc_glacier_irq(&m->pcmcia[0]) ||
                    mrc_glacier_irq(&m->pcmcia[1]);
    bool old = (m->soc.io_datain & 2u) != 0;
    m->soc.io_datain = (m->soc.io_datain & ~2u) | (asserted ? 2u : 0u);
    if (asserted && !old) mrc_icu_raise(&m->soc.icu, 3, 4u);
}
static uint32_t glacier_read(void *ctx, uint32_t off, unsigned size)
{
    datarover_card_port *p = ctx;
    return mrc_glacier_read(&p->owner->pcmcia[p->slot], off, size);
}
static void glacier_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    datarover_card_port *p = ctx;
    mrc_glacier_write(&p->owner->pcmcia[p->slot], off, size, val);
    card_irq_update(p->owner);
}
static uint32_t card_read(void *ctx, uint32_t off, unsigned size)
{
    datarover_card_port *p = ctx;
    uint32_t v = mrc_pccard_read(&p->owner->card_port[p->slot][p->window], off, size);
    card_irq_update(p->owner);
    return v;
}
static void card_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    datarover_card_port *p = ctx;
    mrc_pccard_write(&p->owner->card_port[p->slot][p->window], off, size, val);
    card_irq_update(p->owner);
}

static void cpu_trap(r3900 *c, const char *what, uint32_t insn)
{
    fprintf(c->log, "[cpu] %s: pc=%08X insn=%08X\n", what, c->cur_pc, insn);
}

static uint32_t rom_word(const uint8_t *rom, unsigned off)
{
    return (uint32_t)rom[off] << 24 | (uint32_t)rom[off+1] << 16 |
           (uint32_t)rom[off+2] << 8 | rom[off+3];
}

/* Recognize the documented IDT/board-5 size layout, not arbitrary constants
 * found by scanning data. This observes an already-built ROM; never edits it. */
static uint32_t rom_ram_size(const uint8_t *rom, uint32_t length)
{
    if (length < 0x1eca0 || rom_word(rom, 0) != 0x08f00007 ||
        memcmp(rom + 12, "IDT MONITOR ", 12) != 0 ||
        rom_word(rom, 0x2a8) != 0xac22c180 ||
        rom_word(rom, 0x2b4) != 0xac22c1c4 ||
        rom_word(rom, 0x1ec88) != 0x0cf09709 ||
        rom_word(rom, 0x1ec90) != 0x24030005 ||
        rom_word(rom, 0x1ec94) != 0x14430002)
        return 0;
    uint32_t a = rom_word(rom, 0x2a0), b = rom_word(rom, 0x2ac);
    uint32_t c = rom_word(rom, 0x1ec9c);
    if ((a >> 16) != 0x3c02 || (b >> 16) != 0x3c02 ||
        (c >> 16) != 0x3c04 || (a & 0xffff) != (b & 0xffff) ||
        (a & 0xffff) != (c & 0xffff))
        return 0;
    uint32_t size = (a & 0xffff) << 16;
    return size && size <= DR840_RAM_WINDOW ? size : 0;
}

bool mrc_machine_init(machine *m, const char *rom_path, uint32_t ram_size)
{
    memset(m, 0, sizeof(*m));
    if (ram_size > DR840_RAM_WINDOW) {
        fprintf(stderr, "RAM must fit the nonempty %u-byte DRAM window\n",
                DR840_RAM_WINDOW);
        return false;
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open ROM %s\n", rom_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fprintf(stderr, "ROM %s is empty\n", rom_path);
        fclose(f);
        return false;
    }
    m->rom_size = (uint32_t)len;
    m->rom = malloc(m->rom_size);
    if (!m->rom || fread(m->rom, 1, m->rom_size, f) != m->rom_size) {
        fprintf(stderr, "short read on ROM %s\n", rom_path);
        fclose(f);
        return false;
    }
    fclose(f);

    m->rom_original = malloc(m->rom_size);
    if (!m->rom_original) { free(m->rom); m->rom = NULL; return false; }
    memcpy(m->rom_original, m->rom, m->rom_size);

    if (!ram_size) {
        ram_size = rom_ram_size(m->rom, m->rom_size);
        fprintf(stderr, "RAM: %u bytes (%s)\n",
                ram_size ? ram_size : DR840_RAM_SIZE,
                ram_size ? "ROM size constants" : "unrecognized ROM layout; default");
        if (!ram_size) ram_size = DR840_RAM_SIZE;
    }
    m->ram_size = ram_size;
    m->ram = calloc(1, m->ram_size);
    if (!m->ram)
        return false;

    mrc_bus_init(&m->bus);
    mrc_cpu_init(&m->cpu, &m->bus);
    /* The TMPR3902U has no TLB; see the has_mmu comment in cpu/r3900.h for
     * the ROM evidence. */
    m->cpu.has_mmu = false;
    mrc_tx39_init(&m->soc, &m->cpu, DR840_CPU_HZ);
    mrc_dr_keyboard_init(&m->keyboard, &m->soc.mbus);
    /* The ROM's 0x40C4 boot-select read tests IOCTRL.IODIN[3]. */
    m->soc.io_ctrl |= DR840_IO_BOOT_NORMAL;
    mrc_glacier_init(&m->pcmcia[0], "pcmcia0");
    mrc_glacier_init(&m->pcmcia[1], "pcmcia1");
    mrc_unknown_init(&m->kseg3, "kseg3-dev");
    mrc_machine_rebind(m);

    mrc_bus_add_ram(&m->bus, "dram", DR840_RAM_BASE, m->ram, m->ram_size,
                    DR840_RAM_WINDOW);
    /*
     * Program storage is flash, not mask ROM. The DataRover840F flasher
     * writes it, packages install into it, and the OS image ships with a
     * patch placeholder in it: at image offset 0x296410 sits
     *
     *     lui   gp, 0x1234
     *     addiu gp, gp, 0x5678
     *     jr    ra
     *
     * — a gp-setup stub whose immediates are obvious placeholders. Magic Cap
     * rewrites both words at ROM 0x83C7D79C with the real gp and then flushes
     * the caches. Discarding those stores leaves the stub setting
     * gp = 0x12345678, so anything that calls it loads from nowhere.
     *
     * We do not model NOR program/erase command sequences; a store takes
     * effect. See docs/OPEN_QUESTIONS.md.
     *
     * The chips are also smaller than the window the chip select covers, so
     * the image mirrors through the rest — which is what lets an
     * out-of-range access produce what the board would rather than a fault.
     */
    mrc_bus_add_flash(&m->bus, "flash@3C", DR840_ROM_BASE_A, DR840_ROM_WINDOW,
                      m->rom, m->rom_size);
    mrc_bus_add_flash(&m->bus, "flash@13C", DR840_ROM_BASE_B, DR840_ROM_WINDOW,
                      m->rom, m->rom_size);
    /* At BFC00000 the ROM's first J resolves to B3C0001C, not 83C0001C:
     * MIPS J retains the current PC's high nibble. That target is the
     * existing physical 13C flash alias. The reset alias is inferred from
     * this working ROM path; exact board decode extent remains unverified. */
    mrc_bus_add_flash(&m->bus, "flash@reset", DR840_BOOT_BASE, DR840_BOOT_WINDOW,
                      m->rom, m->rom_size);
    mrc_bus_add_mmio(&m->bus, "tx39", TX39_CFG_BASE, TX39_CFG_SIZE, &m->soc,
                     mrc_tx39_read, mrc_tx39_write);
    for (unsigned sl = 0; sl < 2; sl++)
        for (unsigned w = 0; w < MRC_PCCARD_NWINDOW; w++)
            m->socket_port[sl][w] = (datarover_card_port){m, sl, w};
    mrc_bus_add_mmio(&m->bus, "pcmcia0", DR840_PCMCIA0_BASE, GLACIER_WINDOW,
                     &m->socket_port[0][0], glacier_read, glacier_write);
    mrc_bus_add_mmio(&m->bus, "pcmcia1", DR840_PCMCIA1_BASE, GLACIER_WINDOW,
                     &m->socket_port[1][0], glacier_read, glacier_write);

    mrc_bus_add_float(&m->bus, "card1a", DR840_CARD_A1_BASE, DR840_CARD_SIZE);
    mrc_bus_add_float(&m->bus, "card2a", DR840_CARD_A2_BASE, DR840_CARD_SIZE);
    mrc_bus_add_float(&m->bus, "card1b", DR840_CARD_B1_BASE, DR840_CARD_SIZE);
    mrc_bus_add_float(&m->bus, "card2b", DR840_CARD_B2_BASE, DR840_CARD_SIZE);

    mrc_bus_add_mmio(&m->bus, "kseg3-dev", DR840_KSEG3_DEV_BASE,
                     DR840_KSEG3_DEV_SIZE, &m->kseg3, mrc_unknown_read,
                     mrc_unknown_write);

    mrc_bus_enable_lookup(&m->bus, true);
    return true;
}

void mrc_machine_rebind(machine *m)
{
    mrc_bus_invalidate_lookup(&m->bus);
    m->cpu.bus     = &m->bus;
    m->cpu.log     = stderr;
    m->cpu.on_trap = cpu_trap;

    m->bus.log     = stderr;
    m->bus.pc_hint = &m->cpu.cur_pc;

    m->soc.cpu     = &m->cpu;
    m->soc.log     = stderr;
    m->soc.pc_hint = &m->cpu.cur_pc;
    m->soc.icu.soc   = &m->soc;
    m->soc.sib.soc   = &m->soc;
    m->soc.video.soc = &m->soc;
    m->soc.timer.soc = &m->soc;
    m->soc.mbus.soc  = &m->soc;
    m->soc.power.soc = &m->soc;
    for (unsigned i = 0; i < 2; i++)
        m->soc.uart[i].soc = &m->soc;

    m->pcmcia[0].name = "pcmcia0";
    m->pcmcia[1].name = "pcmcia1";
    m->pcmcia[0].log = m->pcmcia[1].log = stderr;
    m->kseg3.name = "kseg3-dev";
    m->kseg3.log  = stderr;

    /* The region table holds device context pointers, which are stable
     * across a load because the devices live inside `machine`. Only the
     * backing stores need re-pointing. */
    for (unsigned i = 0; i < m->bus.nregion; i++) {
        mrc_region *r = &m->bus.region[i];
        if (r->kind == MRC_REGION_RAM)
            r->host = m->ram;
        else if (r->kind == MRC_REGION_ROM || r->kind == MRC_REGION_FLASH)
            r->host = m->rom;
    }
}

void mrc_machine_free(machine *m)
{
    mrc_cpu_jit_report(m->jit, stderr);
    mrc_cpu_jit_free(m->jit);
    m->jit = NULL;
    mrc_cpu_decode_cache_free(m->decode_cache);
    m->decode_cache = NULL;
    for (unsigned i = 0; i < 2; i++) mrc_network_close(m->network[i]);
    for (unsigned i = 0; i < 2; i++) {
        mrc_card_image_close(&m->storage[i]);
        free(m->card_image[i]);
        free(m->card_path[i]);
        m->card_path[i] = NULL;
    }
    free(m->ram);
    free(m->rom);
    free(m->rom_original);
    m->rom_original = NULL;
    m->ram = NULL;
    m->rom = NULL;
}

void mrc_machine_reset(machine *m, uint32_t reset_pc)
{
    mrc_cpu_reset(&m->cpu, reset_pc);
    m->input_epoch_set = false;
    m->power_release_at = 0;
    m->power_close_after = 0;
}

void mrc_machine_wake(machine *m)
{
    if (!m->cpu.power_stopped) return;
    mrc_power_set_button(&m->soc.power, false);
    mrc_power_set_button(&m->soc.power, true);
    m->power_release_at = m->cpu.insn_count + DR840_CPU_HZ / 20;
    /* Host input policy: allow wake UI/housekeeping to finish before an
     * immediate window close sends OFF. This is not a hardware timer. */
    m->power_close_after = m->cpu.insn_count + (uint64_t)DR840_CPU_HZ * 5;
}

void mrc_machine_set_option(machine *m, bool down)
{
    if (down) m->soc.io_ctrl &= ~DR840_IO_BOOT_NORMAL;
    else m->soc.io_ctrl |= DR840_IO_BOOT_NORMAL;
}

bool mrc_machine_option_held(const machine *m)
{
    return !(m->soc.io_ctrl & DR840_IO_BOOT_NORMAL);
}

bool mrc_machine_power_off(machine *m)
{
    /* Do not collapse launch ON and close OFF into one guest poll, or send
     * OFF while the ROM is still ignoring buttons during wake. */
    if (!m->cpu.power_stopped && !m->cpu.halted &&
        m->power_close_after > m->cpu.insn_count)
        mrc_machine_run(m, m->power_close_after - m->cpu.insn_count, 1024);
    m->power_close_after = 0;
    m->power_release_at = 0;
    mrc_ucb_set_pen(&m->soc.sib.codec, false, 0, 0);
    mrc_machine_set_option(m, false);
    bool held = (m->soc.power.ctrl & PWRCTRL_ONBUTN) != 0;
    mrc_power_set_button(&m->soc.power, false);
    if (m->cpu.power_stopped) return true;
    if (m->cpu.halted) return false;

    /* A held F4 already requested shutdown. Finish that press instead of
     * sending another. Otherwise supply an ordinary, guest-visible press.
     * The hold and timeout are host input policy, not hardware constants. */
    if (!held) {
        mrc_power_set_button(&m->soc.power, true);
        mrc_machine_run(m, DR840_CPU_HZ / 20, 1024);
        mrc_power_set_button(&m->soc.power, false);
    }
    const uint64_t deadline = m->cpu.insn_count + (uint64_t)DR840_CPU_HZ * 30;
    while (!m->cpu.power_stopped && !m->cpu.halted &&
           m->cpu.insn_count < deadline) {
        uint64_t left = deadline - m->cpu.insn_count;
        mrc_machine_run(m, left < 1000000 ? left : 1000000, 1024);
    }
    return m->cpu.power_stopped;
}

void mrc_machine_run(machine *m, uint64_t insns, uint32_t tick_interval)
{
    if (tick_interval == 0)
        tick_interval = 1024;

    uint64_t next_shot = m->fb_watch_every;
    uint64_t done = 0;
    /* Keep one input origin across SDL's short run calls. Resetting it on
     * every frame would prevent delayed card insertion and key events. */
    if (!m->input_epoch_set) {
        m->input_epoch = m->cpu.insn_count;
        m->input_epoch_set = true;
    }
    uint64_t base = m->input_epoch;
    while (done < insns && !m->cpu.halted) {
        uint64_t chunk = insns - done;
        if (chunk > tick_interval)
            chunk = tick_interval;
        if (m->jit)
            mrc_cpu_run_jit(&m->cpu, chunk, m->jit);
        else if (m->decode_cache)
            mrc_cpu_run_decoded(&m->cpu, chunk, m->decode_cache);
        else
            mrc_cpu_run(&m->cpu, chunk);
        mrc_tx39_tick(&m->soc);
        mrc_dr_keyboard_service(&m->keyboard);
        if (m->power_release_at && m->cpu.insn_count >= m->power_release_at) {
            mrc_power_set_button(&m->soc.power, false);
            m->power_release_at = 0;
        }

        /*
         * The host's battery drains on a human timescale, so re-reading it
         * a few times a second of emulated time is ample and keeps the
         * syscall well out of the instruction loop.
         */
        if (m->track_host_battery && m->cpu.insn_count >= m->host_battery_next) {
            mrc_host_power p;
            if (mrc_host_power_read(&p) && p.has_battery)
                m->soc.sib.codec.aux[2] =
                    (uint16_t)mrc_host_power_to_adc(p.percent);
            m->host_battery_next = m->cpu.insn_count + DR840_CPU_HZ / 4;
        }

        if (m->card[0].kind || m->card[1].kind) for (unsigned sl = 0; sl < 2; sl++) {
            if (m->card[sl].kind && !m->card_in[sl] &&
                m->cpu.insn_count - base >= m->card_at[sl]) {
                m->card_in[sl] = true;
                mrc_glacier_set_present(&m->pcmcia[sl], true);
                if(m->card[sl].kind == &mrc_pccard_sram) {
                    mrc_glacier_set_memory_inputs(&m->pcmcia[sl], true, false, true);
                    /* ROM 13C346CC reads IO1 for slot 1, IO0 for slot 2.
                     * Both battery-detect inputs high mean healthy SRAM. */
                    m->soc.io_ctrl |= sl == 0 ? 2u : 1u;
                }
                fprintf(stderr, "card: slot %u closed its detect lines at "
                        "+%llu\n", sl + 1,
                        (unsigned long long)(m->cpu.insn_count - base));
            }
            if (m->card_in[sl] && m->card[sl].kind == &mrc_pccard_ne2000) {
                uint64_t cycles = m->cpu.cycle_count;
                uint64_t ns = cycles / DR840_CPU_HZ * 1000000000 +
                              cycles % DR840_CPU_HZ * 1000000000 / DR840_CPU_HZ;
                mrc_network_poll(m->network[sl], ns);
                mrc_ne2000_tick(&m->card[sl].nic, ns);
                mrc_glacier_set_ready_irq(&m->pcmcia[sl], true);
            }
            if (m->card_in[sl] && m->card[sl].kind == &mrc_pccard_modem) {
                uint64_t cycles = m->cpu.cycle_count;
                uint64_t ns = cycles / DR840_CPU_HZ * 1000000000 +
                              cycles % DR840_CPU_HZ * 1000000000 / DR840_CPU_HZ;
                mrc_modem_tick(&m->card[sl], ns);
                mrc_glacier_set_ready_irq(&m->pcmcia[sl], true);
            }
        }
        if (m->card[0].kind || m->card[1].kind)
            card_irq_update(m);

        if (m->option_i < m->option_n) {
            while (m->cpu.insn_count - base >= m->option_key[m->option_i].at) {
                mrc_machine_set_option(m, m->option_key[m->option_i].down);
                if (++m->option_i == m->option_n) break;
            }
        }
        if (m->gpio_i < m->gpio_n) {
            while (m->cpu.insn_count - base >= m->gpio[m->gpio_i].at) {
                mrc_ucb_set_gpio_in(&m->soc.sib.codec, m->gpio[m->gpio_i].level);
                fprintf(stderr, "[codec] input pins driven to %03X at +%llu\n",
                        m->gpio[m->gpio_i].level,
                        (unsigned long long)(m->cpu.insn_count - base));
                if (++m->gpio_i == m->gpio_n) break;
            }
        }
        if (m->mfio_i < m->mfio_n) {
            while (m->cpu.insn_count - base >= m->mfio_sched[m->mfio_i].at) {
                uint32_t was = m->soc.io_datain, now = m->mfio_sched[m->mfio_i].level;
                m->soc.io_datain = now;
                if (now & ~was) mrc_icu_raise(&m->soc.icu, 3, now & ~was);
                if (was & ~now) mrc_icu_raise(&m->soc.icu, 4, was & ~now);
                fprintf(stderr, "[mfio] input word driven to %08X at +%llu\n", now,
                        (unsigned long long)(m->cpu.insn_count - base));
                if (++m->mfio_i == m->mfio_n) break;
            }
        }
        if (m->io_i < m->io_n) {
            while (m->cpu.insn_count - base >= m->io_sched[m->io_i].at) {
                uint32_t was = m->soc.io_ctrl & 0x7fu, now = m->io_sched[m->io_i].level & 0x7fu;
                m->soc.io_ctrl = (m->soc.io_ctrl & ~0x7fu) | now;
                if (now & ~was) mrc_icu_raise(&m->soc.icu, 5, (now & ~was) << 7);
                if (was & ~now) mrc_icu_raise(&m->soc.icu, 5, was & ~now);
                fprintf(stderr, "[io] pins driven to %02X at +%llu\n", now,
                        (unsigned long long)(m->cpu.insn_count - base));
                if (++m->io_i == m->io_n) break;
            }
        }
        if (m->pwrint_i < m->pwrint_n) {
            while (m->cpu.insn_count - base >= m->pwrint_sched[m->pwrint_i].at) {
                bool was = (m->soc.power.ctrl & 0x40000000u) != 0;
                bool now = m->pwrint_sched[m->pwrint_i].level != 0;
                m->soc.power.ctrl = (m->soc.power.ctrl & ~0x40000000u) | (now ? 0x40000000u : 0);
                if (now && !was) mrc_icu_raise(&m->soc.icu, 5, 0x08000000u);   /* POSPWRINT */
                if (was && !now) mrc_icu_raise(&m->soc.icu, 5, 0x04000000u);   /* NEGPWRINT */
                fprintf(stderr, "[power] PWRINT pin %d at +%llu\n", now,
                        (unsigned long long)(m->cpu.insn_count - base));
                if (++m->pwrint_i == m->pwrint_n) break;
            }
        }

        if (m->power_scheduled) {
            uint64_t now = m->cpu.insn_count - base;
            if (m->power_phase == 0 && now >= m->power_at) {
                mrc_power_set_button(&m->soc.power, true); m->power_phase = 1;
                fprintf(stderr, "[power] button pressed at +%llu\n", (unsigned long long)now);
            } else if (m->power_phase == 1 && now >= m->power_at + m->tap_hold) {
                mrc_power_set_button(&m->soc.power, false); m->power_phase = 2;
                fprintf(stderr, "[power] button released at +%llu\n", (unsigned long long)now);
            }
        }

        /*
         * Scripted touch. Press, hold, release — a real tap has a duration,
         * and software that debounces or waits for a stable reading needs
         * one.
         */
        while (m->key_i < m->key_n && m->cpu.insn_count >= m->key[m->key_i].at) {
            mrc_dr_keyboard_key(&m->keyboard, m->key[m->key_i].code,
                                m->key[m->key_i].ext, m->key[m->key_i].down);
            m->key_i++;
        }
        if (m->tap_i < m->tap_n) {
            uint64_t now = m->cpu.insn_count - base;
            if (m->tap_phase == 0 && now >= m->tap[m->tap_i].at) {
                mrc_ucb_set_pen(&m->soc.sib.codec, true, m->tap[m->tap_i].x,
                                m->tap[m->tap_i].y);
                m->tap_phase = 1;
                fprintf(stderr, "[touch] press %u at raw (%u,%u), +%llu\n",
                        m->tap_i, m->tap[m->tap_i].x, m->tap[m->tap_i].y,
                        (unsigned long long)now);
            } else if (m->tap_phase == 1 &&
                       now >= m->tap[m->tap_i].at + m->tap_hold) {
                mrc_ucb_set_pen(&m->soc.sib.codec, false, 0, 0);
                m->tap_phase = 0;
                fprintf(stderr, "[touch] release %u at +%llu\n", m->tap_i,
                        (unsigned long long)now);
                if (++m->tap_i >= m->tap_n && m->tap_period) {
                    for (unsigned k = 0; k < m->tap_n; k++)
                        m->tap[k].at += m->tap_period;
                    m->tap_i = 0;
                }
            } else if (m->tap_phase == 1 && m->tap[m->tap_i].drag) {
                /* Settle at each endpoint; move the physical pen between them. */
                uint64_t elapsed = now - m->tap[m->tap_i].at;
                uint64_t settle = m->tap_hold / 4;
                uint64_t travel = m->tap_hold / 2;
                double fraction = elapsed <= settle ? 0.0 :
                    elapsed >= settle + travel ? 1.0 :
                    (double)(elapsed - settle) / (double)travel;
                unsigned i = m->tap_i;
                uint16_t x = (uint16_t)(m->tap[i].x +
                    ((int)m->tap[i].end_x - m->tap[i].x) * fraction);
                uint16_t y = (uint16_t)(m->tap[i].y +
                    ((int)m->tap[i].end_y - m->tap[i].y) * fraction);
                mrc_ucb_set_pen(&m->soc.sib.codec, true, x, y);
            }
        }

        /* A host serial port, when one is attached, feeds the same way. */
        for (unsigned uix = 0; uix < 2; uix++) {
            tx39_uart *u = &m->soc.uart[uix];
            void *lnk = mrc_uart_link(u);
            tx39_uart_timing *timing = &m->soc.uart_timing[uix];
            if (lnk && (u->ctrl1 & UART_CTRL1_ENUART) && !u->rx_full &&
                m->cpu.cycle_count >= timing->rx_next) {
                uint8_t byte;
                if (mrc_serial_read(lnk, &byte)) {
                    mrc_uart_rx_byte(u, byte);
                    timing->rx_next = m->cpu.cycle_count + mrc_uart_frame_cycles(u);
                }
            }
        }

        /* Feed the console into UART A's receiver when it has room. */
        if (m->con_active && !m->soc.uart[0].rx_full) {
            uint8_t byte;
            if (mrc_console_poll(&m->con, m->cpu.cycle_count, &byte))
                mrc_uart_rx_byte(&m->soc.uart[0], byte);
        }
        done += chunk;

        /*
         * Periodic framebuffer capture. Whether the screen is changing is
         * the difference between "still booting" and "stuck", and there is
         * no other way to tell from the outside.
         */
        if (m->fb_watch_every && done >= next_shot) {
            char path[512];
            snprintf(path, sizeof(path), "%s%04u.pgm", m->fb_watch_prefix,
                     m->fb_watch_seq++);
            mrc_machine_dump_fb(m, path);
            next_shot = done + m->fb_watch_every;
        }
    }
}

bool mrc_machine_read_fb(machine *m, uint8_t *out, unsigned *w_out,
                         unsigned *h_out)
{
    if (m->cpu.power_stopped) return false;
    uint32_t fb_pa;
    unsigned w, h, bpp;

    if (!mrc_video_geometry(&m->soc.video, &fb_pa, &w, &h, &bpp))
        return false;

    uint32_t stride = (w * bpp + 7) / 8;
    uint8_t *fb = mrc_bus_host_ptr(&m->bus, fb_pa, stride * h);
    if (!fb)
        return false;

    /*
     * On this panel a set bit turns a pixel on, i.e. dark, so the stored
     * value is ink coverage and the image is its complement. VIDEOCTRL1's
     * INVVID bit flips that in hardware.
     */
    bool invert = (m->soc.video.ctrl[0] & VID1_INVVID) != 0;
    unsigned levels = (1u << bpp) - 1;

    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = fb + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) {
            unsigned bit = x * bpp;
            unsigned v = (row[bit / 8] >> (8 - bpp - (bit % 8))) &
                         ((1u << bpp) - 1);
            if (invert)
                v = levels - v;
            out[y * w + x] = (uint8_t)(255 - v * 255 / levels);
        }
    }

    *w_out = w;
    *h_out = h;
    return true;
}

bool mrc_machine_dump_fb(machine *m, const char *path)
{
    static uint8_t gray[PANEL_SCREEN_W * PANEL_SCREEN_H];
    unsigned w, h;

    if (!mrc_machine_read_fb(m, gray, &w, &h)) {
        fprintf(stderr, "video controller is off or its framebuffer is "
                "outside RAM; nothing to dump\n");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(gray, 1, (size_t)w * h, f);
    fclose(f);

    if (!m->fb_watch_every || m->fb_watch_seq <= 1) {
        uint32_t fb_pa; unsigned bw, bh, bpp;
        mrc_video_geometry(&m->soc.video, &fb_pa, &bw, &bh, &bpp);
        fprintf(stderr, "framebuffer %ux%u @ %ubpp from %08X -> %s\n", w, h,
                bpp, fb_pa, path);
    }
    return true;
}

/*
 * Remember what went into a slot, so a state can put it back.
 *
 * The path is resolved, because the run that picks this device up again may
 * not be standing in the same directory as the run that put the card in --
 * which is the whole case this exists for. A path that cannot be resolved is
 * kept as it was given rather than dropped: it is still the best name anyone
 * has for the card.
 */
static void remember_card(machine *m, unsigned slot, mrc_card_kind kind,
                          const char *path)
{
    free(m->card_path[slot]);
    m->card_path[slot] = NULL;
    m->card_kind[slot] = kind;
    if (!path) return;
    char resolved[PATH_MAX];
    const char *keep = realpath(path, resolved) ? resolved : path;
    m->card_path[slot] = strdup(keep);
}

static void map_card(machine *m, unsigned slot)
{
    mrc_bus_invalidate_lookup(&m->bus);
    static const uint32_t base[2][MRC_PCCARD_NWINDOW] = {
        { DR840_CARD_A1_BASE, DR840_CARD_B1_BASE },
        { DR840_CARD_A2_BASE, DR840_CARD_B2_BASE },
    };
    /* Route both physical windows to the card. Memory cards retain their
     * legacy mirrored image; I/O cards distinguish the spaces themselves. */
    for (unsigned w = 0; w < MRC_PCCARD_NWINDOW; w++) {
        m->card_port[slot][w].card = &m->card[slot];
        m->card_port[slot][w].window = (mrc_pccard_window)w;
        m->card_port[slot][w].pc_hint = &m->cpu.cur_pc;
        m->card_port[slot][w].present = &m->card_in[slot];

        for (unsigned i = 0; i < m->bus.nregion; i++) {
            mrc_region *r = &m->bus.region[i];
            if (r->base != base[slot][w])
                continue;
            r->kind = MRC_REGION_MMIO;
            r->ctx = &m->socket_port[slot][w];
            r->read = card_read;
            r->write = card_write;
            r->host = NULL;
            r->host_len = 0;
            break;
        }
    }

}

bool mrc_machine_insert_card(machine *m, unsigned slot, const char *path)
{
    if (slot > 1)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "card: cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }

    free(m->card_image[slot]);
    m->card_image[slot] = malloc((size_t)n);
    if (!m->card_image[slot] ||
        fread(m->card_image[slot], 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return false;
    }
    fclose(f);

    mrc_pccard_init(&m->card[slot], slot, &mrc_pccard_memory);
    m->card[slot].image = m->card_image[slot];
    m->card[slot].image_len = (uint32_t)n;

    remember_card(m, slot, MRC_CARD_MEMORY, path);
    map_card(m, slot);
    fprintf(stderr, "card: slot %u <- %s (%ld bytes)\n", slot + 1, path, n);
    return true;
}

bool mrc_machine_insert_sram(machine *m, unsigned slot, const char *path, uint32_t create_size)
{
    if (slot > 1 || m->card[slot].kind) return false;
    if (!mrc_card_image_open(&m->storage[slot], path, create_size)) return false;
    mrc_pccard_sram_init(&m->card[slot], slot,
                        m->storage[slot].data, m->storage[slot].size);
    remember_card(m, slot, MRC_CARD_SRAM, path);
    map_card(m, slot);
    return true;
}

bool mrc_machine_eject_card(machine *m, unsigned slot)
{
    if (slot > 1 || !m->card[slot].kind) return false;
    /*
     * The departure first, while the card is still there to depart: the
     * controller raises a card-detect edge and the guest stops believing
     * whatever it had cached about the card. Then the image is flushed and
     * closed, so the file on disk is complete by the time this returns.
     */
    mrc_glacier_set_present(&m->pcmcia[slot], false);
    mrc_glacier_set_memory_inputs(&m->pcmcia[slot], false, false, false);
    m->soc.io_ctrl &= ~(slot == 0 ? 2u : 1u);
    m->card_in[slot] = false;
    if (m->network[slot]) {
        mrc_network_close(m->network[slot]);
        m->network[slot] = NULL;
    }
    mrc_card_image_close(&m->storage[slot]);
    free(m->card_image[slot]);
    m->card_image[slot] = NULL;
    m->card[slot] = (mrc_pccard){0};
    remember_card(m, slot, MRC_CARD_NONE, NULL);
    map_card(m, slot);
    fprintf(stderr, "card: slot %u opened its detect lines; the card is out\n",
            slot + 1);
    return true;
}

bool mrc_machine_insert_ne2000(machine *m, unsigned slot)
{
    if (slot > 1 || m->card[slot].kind) return false;
    mrc_pccard_init(&m->card[slot], slot, &mrc_pccard_ne2000);
    uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x84, 0x00, (uint8_t)(slot + 1) };
    mrc_ne2000_init(&m->card[slot].nic, mac);
    remember_card(m, slot, MRC_CARD_NE2000, NULL);
    /* Nothing behind it yet: a card with the cable out. mrc_machine_network
     * is what plugs the cable in, and it records what it plugged into. */
    map_card(m, slot);
    fprintf(stderr, "card: slot %u <- NE2000 (Ethernet cable disconnected)\n", slot + 1);
    return true;
}

bool mrc_machine_insert_modem(machine *m, unsigned slot, void *link)
{
    if (slot > 1 || m->card[slot].kind) return false;
    mrc_pccard_init(&m->card[slot], slot, &mrc_pccard_modem);
    mrc_modem_init(&m->card[slot]);
    m->card[slot].modem.link = link;
    remember_card(m, slot, MRC_CARD_MODEM, NULL);
    map_card(m, slot);
    fprintf(stderr, "card: slot %u <- data modem (%s)\n", slot + 1,
            link ? "far end on a pty" : "far end unplugged");
    return true;
}

static void network_receive(void *opaque, const uint8_t *frame, size_t len)
{
    mrc_ne2000_receive(opaque, frame, len);
}
bool mrc_machine_network(machine *m, unsigned slot, const char *pcap)
{
    if (slot > 1 || m->card[slot].kind != &mrc_pccard_ne2000 || m->network[slot]) return false;
    m->network[slot] = mrc_network_open(network_receive, &m->card[slot].nic, pcap);
    if (!m->network[slot]) return false;
    m->card[slot].nic.send = mrc_network_send;
    m->card[slot].nic.send_opaque = m->network[slot];
    /*
     * What the card is plugged into, so a state can plug it back in. A network
     * card with nothing behind it is a card with the cable out, which is a
     * different machine from one that can reach the world -- so "which card"
     * is not enough on its own here.
     */
    free(m->card_path[slot]);
    m->card_path[slot] = NULL;
    if (pcap) {
        size_t n = strlen(pcap) + 6;
        m->card_path[slot] = malloc(n);
        if (m->card_path[slot]) snprintf(m->card_path[slot], n, "pcap:%s", pcap);
    } else {
        m->card_path[slot] = strdup("user");
    }
    return true;
}
