#include "machines/datarover840/machine.h"
#include "util/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void card_irq_edges(machine *m)
{
    bool ok;
    for (unsigned sl = 0; sl < 2; sl++) {
        uint32_t card = sl ? DR840_CARD_A2_BASE : DR840_CARD_A1_BASE;
        uint32_t ctrl = sl ? DR840_PCMCIA1_BASE : DR840_PCMCIA0_BASE;
        CHECK(mrc_machine_insert_ne2000(m, sl));
        m->card_in[sl] = true;
        mrc_glacier_set_present(&m->pcmcia[sl], true);
#define WRITE(addr, size, value) do { mrc_bus_write(&m->bus, addr, size, value, &ok); CHECK(ok); } while (0)
        WRITE(card + 0x3f8, 1, 0x60);
        WRITE(ctrl + GLACIER_FALL_PENDING, 2, 0xffff);
        WRITE(ctrl + 0x14, 2, 4);
        WRITE(card + 0x30f, 1, 0x40); /* remote-DMA completion interrupt */
        for (unsigned repeat = 0; repeat < 3; repeat++) {
            /* No board tick between these accesses. A second completion
             * must produce another edge even within one CPU batch. */
            WRITE(card + 0x300, 1, 0x0a); /* zero-length remote read */
            CHECK(mrc_ne2000_irq(&m->card[sl].nic));
            CHECK(m->pcmcia[sl].reg[GLACIER_FALL_PENDING / 2] & 4);
            CHECK(m->soc.io_datain & 2);
            CHECK(m->soc.icu.status[2] & 4);
            mrc_icu_write(&m->soc.icu, 0x108, 4);
            WRITE(ctrl + GLACIER_FALL_PENDING, 2, 4);
            CHECK(!(m->soc.io_datain & 2));
            WRITE(card + 0x307, 1, 0x40);
            CHECK(!mrc_ne2000_irq(&m->card[sl].nic));
            CHECK(m->pcmcia[sl].reg[GLACIER_STATUS / 2] & 4);
            CHECK(!(m->soc.icu.status[2] & 4));
        }
        /* Mask changes and reset-port reads also change the physical line. */
        WRITE(card + 0x30f, 1, 0);
        WRITE(card + 0x300, 1, 0x0a);
        CHECK(!(m->soc.icu.status[2] & 4));
        WRITE(card + 0x30f, 1, 0x40);
        CHECK(m->soc.icu.status[2] & 4);
        (void)mrc_bus_read(&m->bus, card + 0x31f, 1, &ok);
        CHECK(ok && (m->pcmcia[sl].reg[GLACIER_STATUS / 2] & 4));
        WRITE(ctrl + GLACIER_FALL_PENDING, 2, 4);
        mrc_icu_write(&m->soc.icu, 0x108, 4);
        CHECK(!(m->soc.io_datain & 2));
#undef WRITE
    }
    /* The sockets share one wire: acknowledging one controller must not
     * lower it while the other still has an enabled pending event. */
    mrc_bus_write(&m->bus, DR840_CARD_A1_BASE + 0x30f, 1, 0x40, &ok);
    mrc_bus_write(&m->bus, DR840_CARD_A2_BASE + 0x30f, 1, 0x40, &ok);
    mrc_bus_write(&m->bus, DR840_CARD_A1_BASE + 0x300, 1, 0x0a, &ok);
    CHECK(m->soc.io_datain & 2);
    mrc_icu_write(&m->soc.icu, 0x108, 4);
    mrc_bus_write(&m->bus, DR840_CARD_A2_BASE + 0x300, 1, 0x0a, &ok);
    CHECK(!(m->soc.icu.status[2] & 4)); /* shared wire was already high */
    mrc_bus_write(&m->bus, DR840_PCMCIA0_BASE + GLACIER_FALL_PENDING, 2, 4, &ok);
    CHECK(m->soc.io_datain & 2);
    mrc_bus_write(&m->bus, DR840_PCMCIA1_BASE + GLACIER_FALL_PENDING, 2, 4, &ok);
    CHECK(!(m->soc.io_datain & 2));
    mrc_bus_write(&m->bus, DR840_CARD_A1_BASE + 0x307, 1, 0x40, &ok);
    mrc_bus_write(&m->bus, DR840_CARD_A1_BASE + 0x300, 1, 0x0a, &ok);
    CHECK(m->soc.icu.status[2] & 4);
}

/*
 * A card goes in and comes out, and the machine remembers which one it was.
 *
 * The contents of a card are its own -- an SRAM image is a shared mapping of
 * a host file the guest writes through -- so what a state owes the next run
 * is the card's name, not a copy of it. And a removal has to be an event:
 * the controller raises the same card-detect edge for a departure as for an
 * arrival, because that is what stops the guest believing whatever it cached
 * about a card that is no longer there.
 */
static void card_slots(machine *m)
{
    char image[512];
    snprintf(image, sizeof(image), "%s/mrc-card-test-XXXXXX", mrc_temp_dir());
    int fd = mkstemp(image);
    CHECK(fd >= 0);
    close(fd);
    unlink(image);            /* insert_sram creates it at the size we ask */

    /* The earlier checks left a network card in each slot. Taking them out is
     * the first thing eject has ever been asked to do. */
    for (unsigned slot = 0; slot < 2; slot++)
        if (m->card[slot].kind) CHECK(mrc_machine_eject_card(m, slot));

    CHECK(mrc_machine_insert_sram(m, 0, image, 65536));
    CHECK(m->card_kind[0] == MRC_CARD_SRAM);
    /* Resolved, because the run that picks this device up again need not be
     * standing where the run that put the card in was. */
    CHECK(m->card_path[0] && (m->card_path[0][0] == '/' || m->card_path[0][1] == ':'));
    CHECK(m->card[0].kind != NULL);

    /* In, then out. Each is an edge, and they are different edges. */
    m->card_in[0] = true;
    mrc_glacier_set_present(&m->pcmcia[0], true);
    m->pcmcia[0].reg[GLACIER_RISE_PENDING / 2] = 0;
    CHECK(mrc_machine_eject_card(m, 0));
    CHECK(m->pcmcia[0].reg[GLACIER_RISE_PENDING / 2] & GLACIER_ST_CD_MASK);
    CHECK(!m->card_in[0] && !m->card[0].kind);
    CHECK(m->card_kind[0] == MRC_CARD_NONE && !m->card_path[0]);
    CHECK(!mrc_machine_eject_card(m, 0));      /* nothing to take out */

    /* What a state carries across: the name, and nothing of the card. */
    CHECK(mrc_machine_insert_sram(m, 1, image, 65536));
    char statefile[512];
    snprintf(statefile, sizeof(statefile), "%s/mrc-card-state-XXXXXX", mrc_temp_dir());
    fd = mkstemp(statefile);
    CHECK(fd >= 0);
    close(fd);
    CHECK(mrc_snapshot_save(m, statefile));
    CHECK(mrc_machine_eject_card(m, 1));
    CHECK(m->card_kind[1] == MRC_CARD_NONE);
    CHECK(mrc_snapshot_load(m, statefile));
    CHECK(m->card_kind[1] == MRC_CARD_SRAM);
    CHECK(m->card_path[1] && !strcmp(m->card_path[1], m->card_path[1]));
    /* Reported, not reopened: the slot is still empty and the caller decides,
     * because it is the one that knows whether this run named another card. */
    CHECK(!m->card[1].kind && !m->card_in[1]);
    CHECK(!m->pcmcia[1].card_present);

    unlink(statefile);
    unlink(image);
}

int main(void)
{
    /* A tiny ROM reproduces the real image's reset J and boot-pin read.
     * No ROM redistribution or host-injected monitor command is needed. */
    const uint32_t code[] = {
        0x08f00007, 0, 0, 0, 0, 0, 0,
        0x3c02b0c0, /* lui v0,b0c0 */
        0x8c420180, /* lw v0,IOCTRL(v0) */
        0,          /* load delay */
        0x000210c2, /* srl v0,3 */
        0x38420001, /* xori v0,1 */
        0x30420001, /* andi v0,1: zero means normal boot */
        0x03e00008, 0 /* jr ra; nop */
    };
    char path[512];
    snprintf(path, sizeof(path), "%s/mrc-boot-test-XXXXXX", mrc_temp_dir());
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    CHECK(f);
    for (unsigned i = 0; i < sizeof(code)/sizeof(code[0]); i++) {
        uint8_t bytes[] = {code[i] >> 24, code[i] >> 16, code[i] >> 8, code[i]};
        CHECK(fwrite(bytes, 1, 4, f) == 4);
    }
    CHECK(fclose(f) == 0);
    machine *m = calloc(1, sizeof(*m));
    CHECK(m && mrc_machine_init(m, path, DR840_RAM_SIZE));
    unlink(path);
    CHECK(mrc_tx39_read(&m->soc, TX39_IOCTRL, 4) & DR840_IO_BOOT_NORMAL);
    mrc_tx39_write(&m->soc, TX39_IOCTRL, 4, 0x12340000);
    CHECK(mrc_tx39_read(&m->soc, TX39_IOCTRL, 4) == 0x12340008);
    mrc_tx39_write(&m->soc, TX39_IOCTRL, 4, 0xffffffff);
    CHECK((mrc_tx39_read(&m->soc, TX39_IOCTRL, 4) & IOCTRL_IODIN_MASK) == 8);

    for (unsigned monitor = 0; monitor < 2; monitor++) {
        mrc_machine_set_option(m, monitor != 0);
        uint32_t inputs = m->soc.io_ctrl & IOCTRL_IODIN_MASK;
        mrc_tx39_write(&m->soc, TX39_IOCTRL, 4, ~m->soc.io_ctrl);
        CHECK((m->soc.io_ctrl & IOCTRL_IODIN_MASK) == inputs);
        mrc_machine_reset(m, 0xbfc00000);
        m->cpu.r[31] = 0x80001000;
        mrc_cpu_step(&m->cpu);
        mrc_cpu_step(&m->cpu);
        CHECK(m->cpu.pc == 0xb3c0001c); /* J retains B, not 8 */
        for (unsigned i = 0; i < 8; i++) mrc_cpu_step(&m->cpu);
        CHECK(m->cpu.pc == 0x80001000);
        CHECK(m->cpu.r[2] == monitor);
        CHECK(m->cpu.exc_count == 0 && m->bus.faults == 0);
    }
    mrc_machine_set_option(m, false);
    CHECK(m->soc.io_ctrl & DR840_IO_BOOT_NORMAL);
    bool ok;
    /* All aliases share backing storage; no copied or patched boot stub. */
    mrc_bus_write(&m->bus, DR840_ROM_BASE_B + 8, 4, 0x12345678, &ok);
    CHECK(ok);
    CHECK(mrc_bus_read(&m->bus, DR840_BOOT_BASE + 8, 4, &ok) == 0x12345678 && ok);
    CHECK(mrc_bus_read(&m->bus, DR840_ROM_BASE_A + 8, 4, &ok) == 0x12345678 && ok);
    /* Physical button edges latch once; software cannot write the input. */
    mrc_power_set_button(&m->soc.power, true);
    CHECK(mrc_tx39_read(&m->soc, TX39_POWERCTRL, 4) & PWRCTRL_ONBUTN);
    CHECK(m->soc.power.ctrl & PWRCTRL_PWRCS);
    CHECK(m->soc.icu.status[4] & INT5_POSONBUTNINT);
    mrc_tx39_write(&m->soc, TX39_POWERCTRL, 4, 0);
    CHECK(mrc_tx39_read(&m->soc, TX39_POWERCTRL, 4) & PWRCTRL_ONBUTN);
    mrc_icu_write(&m->soc.icu, 0x110, INT5_POSONBUTNINT);
    mrc_power_set_button(&m->soc.power, true);
    CHECK(!(m->soc.icu.status[4] & INT5_POSONBUTNINT));
    mrc_power_set_button(&m->soc.power, false);
    CHECK(m->soc.icu.status[4] & INT5_NEGONBUTNINT);
    mrc_tx39_write(&m->soc, TX39_POWERCTRL, 4, PWRCTRL_ONBUTN);
    CHECK(!(mrc_tx39_read(&m->soc, TX39_POWERCTRL, 4) & PWRCTRL_ONBUTN));
    CHECK(m->cpu.power_stopped && !m->cpu.halted);
    uint32_t sleep_pc = m->cpu.pc;
    uint64_t sleep_slots = m->cpu.insn_count;
    uint64_t sleep_reads = m->bus.reads;
    m->ram[0x2000] = 0x5a;
    mrc_cpu_run(&m->cpu, 1000);
    CHECK(m->cpu.pc == sleep_pc && m->bus.reads == sleep_reads);
    CHECK(m->cpu.insn_count == sleep_slots + 1000);
    uint8_t frame[PANEL_SCREEN_W * PANEL_SCREEN_H]; unsigned w, h;
    CHECK(!mrc_machine_read_fb(m, frame, &w, &h));
    /* Closing an already-off GUI must not wake it or execute guest code. */
    CHECK(mrc_machine_power_off(m));
    CHECK(m->cpu.power_stopped && m->cpu.pc == sleep_pc);
    CHECK(m->cpu.insn_count == sleep_slots + 1000);
    CHECK(m->ram[0x2000] == 0x5a);
    mrc_power_set_button(&m->soc.power, true);
    CHECK(!m->cpu.power_stopped && (m->soc.power.ctrl & PWRCTRL_PWRCS));
    CHECK(m->cpu.pc == sleep_pc && m->ram[0x2000] == 0x5a);
    mrc_power_set_button(&m->soc.power, false);
    /* Wake resumes the instruction stream, not the reset vector. */
    mrc_cpu_step(&m->cpu);
    CHECK(m->cpu.pc == sleep_pc + 4);
    /* SDL advances in short calls. Scheduled input must keep its origin. */
    m->option_key[0].down = true;
    m->option_key[0].at = 9;
    m->option_key[1].down = false;
    m->option_key[1].at = 17;
    m->option_n = 2;
    mrc_machine_run(m, 8, 8);
    CHECK(m->soc.io_ctrl & DR840_IO_BOOT_NORMAL);
    mrc_machine_run(m, 8, 8);
    CHECK(!(m->soc.io_ctrl & DR840_IO_BOOT_NORMAL));
    mrc_machine_run(m, 8, 8);
    CHECK(m->soc.io_ctrl & DR840_IO_BOOT_NORMAL);
    card_irq_edges(m);
    /* Standby retains chip state and guest-written firmware bytes, while
     * host callbacks and diagnostic choices belong to the new session. */
    mrc_tx39_write(&m->soc, TX39_POWERCTRL, 4, 3);
    mrc_tx39_write(&m->soc, TX39_POWERCTRL, 4, 0);
    m->card[0].nic.ram[17] = 0xab;
    m->soc.timer.rtc = 1234567;
    uint8_t *context = NULL; uint32_t context_size = 0;
    uint32_t retained_pc = m->cpu.pc;
    CHECK(mrc_suspend_save(m, &context, &context_size) && context_size);
    mrc_machine_reset(m, 0xbfc00000);
    m->rom[8] = 0;
    m->card[0].nic.ram[17] = 0;
    m->card[0].nic.send_opaque = m;
    m->soc.timer.rtc = 0;
    m->cpu.trace = true;
    CHECK(!mrc_suspend_load(m, context, context_size - 1));
    CHECK(m->cpu.pc == 0xbfc00000 && m->rom[8] == 0);
    CHECK(mrc_suspend_load(m, context, context_size));
    CHECK(m->cpu.pc == retained_pc && m->cpu.power_stopped);
    CHECK(m->rom[8] == 0x12 && m->card[0].nic.ram[17] == 0xab);
    CHECK(m->soc.timer.rtc == 1234567 && m->cpu.trace);
    CHECK(m->card[0].nic.send_opaque == m);
    CHECK(m->cpu.bus == &m->bus && m->soc.power.soc == &m->soc);
    free(context);
    card_slots(m);
    mrc_machine_free(m);
    free(m);
    puts("DataRover reset aliases, power inputs and PC Card interrupt edges passed");
    return 0;
}
