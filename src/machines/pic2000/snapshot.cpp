/*
 * snapshot.cpp — save and restore a whole 68k machine.
 *
 * The DataRover has had this for a long time and the 68k machines have not,
 * which is backwards: these are the ones with a battery-backed store and a
 * user who expects to come back to what they left. It is also what the rail's
 * save button needs, and what switching between devices will need, since
 * switching is nothing more than saving one machine and restoring another.
 *
 * Two kinds of state, handled differently.
 *
 * The devices are plain data with a few pointers wiring them together, so
 * they are written whole and the pointers are re-established afterwards --
 * the same approach as the DataRover's, and for the same reason. The
 * pointers are zeroed on the way out rather than written and ignored, so a
 * state file never carries this process's addresses.
 *
 * The processor state goes out field by field rather than as a copy of its
 * runtime object. That gives the snapshot a stable format independent of
 * compiler layout and keeps host pointers/caches out of the file.
 *
 * What is deliberately not saved:
 *
 *   - The ROM. It is not writable on this board -- writes to it are counted
 *     as ignored, which is how we know -- so it is a property of the image
 *     file, not of the machine. A checksum of it is stored so a state cannot
 *     be loaded into a different machine.
 *   - The block cache. Every compiled block guards the guest bytes it was
 *     built from, so a restore that changes memory under it invalidates it
 *     naturally. It is dropped anyway, because carrying it would be carrying
 *     host code addresses.
 *   - Diagnostic counters, host overrides and scheduled test input. Those
 *     describe the session that took the snapshot, not the machine.
 */
#include "machines/pic2000/board.h"
#include "machines/pic2000/cpu_state.h"

extern "C" {
#include "core/bus/bus.h"
}

#include <algorithm>
#include <cstring>

namespace {

#define SNAP_MAGIC   0x4D36384Bu    /* "M68K" */
#define SNAP_VERSION 4u

struct Header {
    uint32_t magic, version;
    uint32_t ram_len, xram_len;
    uint32_t rom_size, rom_sum;
    uint32_t cpu_bytes, board_bytes;
    uint32_t dev_bytes, sim_bytes, audio_bytes, bram_bytes;
};

/*
 * The processor's architectural state. Fixed widths and no padding worth
 * the name, so the file means the same
 * thing to any build.
 */
struct CpuState {
    uint32_t d[8], a[8];
    /*
     * All three stack pointers, and no separate active one.
     *
     * The active stack pointer is A7; the user and supervisor stack pointers
     * are kept separately. Restore the status register before A7 so the
     * active stack selection is unambiguous.
     */
    uint32_t usp, isp, msp;
    uint32_t pc, pc0;
    uint32_t vbr, sfc, dfc;
    uint32_t mbar;                 /* module base, written in CPU space */
    uint32_t flags;
    int32_t  exception;
    uint16_t sr, irc, ird;
    uint8_t  ipl, sampled_ipl, fcl, fc_source;
    uint8_t  stopped;              /* the board's own terminal-halt latch */
    uint8_t  pad[3];
    int64_t  clock;
    uint64_t insns;
};

/* The board around it: the things machine68k.cpp keeps outside any device. */
struct BoardState {
    uint64_t audio_at;
    uint64_t timer_at, pen_timer_at;
    uint64_t boot_release_at;
    uint64_t timer_fires;
    uint32_t hix_checksum_actual, hix_checksum_stored;
    uint32_t irq_now;
    uint8_t  timer_armed, pen_timer_armed, touch_down;
    uint8_t  power_down, power_key_down;
    uint8_t  hix_checksum_pending;
    uint8_t  overlay_off;          /* re-applied to the bus, not just stored */
};

/* The UART's guest-visible state, without host pointers or an attached
 * installer.  A saved package transfer is a new host session after restore,
 * but the ROM's baud, interrupt and channel setup remain machine state. */
struct DuartState {
    uint64_t tx_done_at, rx_next_at;
    uint16_t mcr;
    uint8_t ilr, ivr, acr;
    uint8_t mr1a, mr2a, csra, cra;
    uint8_t mr1b, mr2b, csrb, crb;
    uint8_t ier, isr;
    uint8_t rx[4];
    uint8_t rx_at, rx_len;
    uint8_t rx_enabled, tx_enabled, tx_busy, tx_byte;
};

/* Version 4 adds opt-in channel A; host endpoints are never serialized. */
struct DuartAState {
    uint64_t tx_done_at, rx_next_at;
    uint8_t rx[3], rx_at, rx_len, tx_byte;
    uint8_t enabled, rx_enabled, tx_enabled, tx_busy;
    uint8_t reserved[6];
};
static_assert(sizeof(DuartAState) == 32, "fixed channel-A snapshot layout");

DuartAState save_duart_a(const Mc68349Duart::ChannelA &a)
{
    DuartAState s = {};
    s.tx_done_at = a.tx_done_at; s.rx_next_at = a.rx_next_at;
    std::memcpy(s.rx, a.rx, 3);
    s.rx_at = a.rx_at; s.rx_len = a.rx_len; s.tx_byte = a.tx_byte;
    s.enabled = a.enabled; s.rx_enabled = a.rx_enabled;
    s.tx_enabled = a.tx_enabled; s.tx_busy = a.tx_busy;
    return s;
}

bool decode_duart_a(Mc68349Duart::ChannelA &a, const DuartAState &s)
{
    if (s.rx_at >= 3 || s.rx_len > 3 || s.enabled != 1 ||
        s.rx_enabled > 1 || s.tx_enabled > 1 || s.tx_busy > 1)
        return false;
    a.tx_done_at = s.tx_done_at; a.rx_next_at = s.rx_next_at;
    std::memcpy(a.rx, s.rx, 3);
    a.rx_at = s.rx_at; a.rx_len = s.rx_len; a.tx_byte = s.tx_byte;
    a.enabled = s.enabled; a.rx_enabled = s.rx_enabled;
    a.tx_enabled = s.tx_enabled; a.tx_busy = s.tx_busy;
    return true;
}

bool wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
bool rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

/* Enough to catch "this state belongs to a different ROM", which is the
 * mistake that would otherwise produce a machine that looks alive and is
 * not. Not a security property and not trying to be. */
uint32_t rom_sum(const uint8_t *rom, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) h = (h ^ rom[i]) * 16777619u;
    return h;
}

/*
 * A device struct on its way out, with the pointers taken off it.
 *
 * Writing them and ignoring them on the way back in would work just as well
 * and would put this process's addresses in a file that another process
 * reads. Taking them off costs a copy of a struct we are about to write
 * anyway.
 */
Pic2000Registers strip(const Pic2000Registers &in)
{
    Pic2000Registers out = in;
    out.name = nullptr;
    out.log = nullptr;
    out.econoram = nullptr;
    out.clock_context = nullptr;
    out.cpu_clock = nullptr;
    out.insn_src = nullptr;
    /* Per-register access counts: a session's traffic, not the machine's
     * state, and carrying them makes every later report read as this run
     * plus whatever the snapshot happened to have done. */
    std::memset(out.reads, 0, sizeof(out.reads));
    std::memset(out.writes, 0, sizeof(out.writes));
    return out;
}

Mc68349Sim strip(const Mc68349Sim &in)
{
    Mc68349Sim out = in;
    out.log = nullptr;
    out.boot_select_programmed = nullptr;
    out.board = nullptr;
    out.bus = nullptr;
    out.duart = nullptr;
    return out;
}

Pic2000Audio strip(const Pic2000Audio &in)
{
    Pic2000Audio out = in;
    out.sink = nullptr;
    out.ctx = nullptr;
    return out;
}

DuartState save_duart(const Mc68349Duart &d)
{
    DuartState s = {};
    s.tx_done_at = d.tx_done_at;
    s.rx_next_at = d.rx_next_at;
    s.mcr = d.mcr;
    s.ilr = d.ilr; s.ivr = d.ivr; s.acr = d.acr;
    s.mr1a = d.mr1a; s.mr2a = d.mr2a; s.csra = d.csra; s.cra = d.cra;
    s.mr1b = d.mr1b; s.mr2b = d.mr2b; s.csrb = d.csrb; s.crb = d.crb;
    s.ier = d.ier; s.isr = d.isr;
    std::memcpy(s.rx, d.rx, sizeof(s.rx));
    s.rx_at = (uint8_t)d.rx_at; s.rx_len = (uint8_t)d.rx_len;
    s.rx_enabled = d.rx_enabled; s.tx_enabled = d.tx_enabled;
    s.tx_busy = d.tx_busy; s.tx_byte = d.tx_byte;
    return s;
}

void load_duart(Mc68349Duart &d, const DuartState &s)
{
    d.tx_done_at = s.tx_done_at;
    d.rx_next_at = s.rx_next_at;
    d.mcr = s.mcr;
    d.ilr = s.ilr; d.ivr = s.ivr; d.acr = s.acr;
    d.mr1a = s.mr1a; d.mr2a = s.mr2a; d.csra = s.csra; d.cra = s.cra;
    d.mr1b = s.mr1b; d.mr2b = s.mr2b; d.csrb = s.csrb; d.crb = s.crb;
    d.ier = s.ier; d.isr = s.isr;
    std::memcpy(d.rx, s.rx, sizeof(d.rx));
    d.rx_at = s.rx_at % 4; d.rx_len = std::min<unsigned>(s.rx_len, 4);
    d.rx_enabled = s.rx_enabled != 0; d.tx_enabled = s.tx_enabled != 0;
    d.tx_busy = s.tx_busy != 0; d.tx_byte = s.tx_byte;
    d.dirty = true;
}

/* Version 1 did not carry the DUART.  The PIC ROM programs these values at
 * boot and leaves them in force while the Storeroom is open.  Supplying them
 * keeps the existing user-created Storeroom state usable. */
void load_legacy_pic_duart(m68k_machine *m)
{
    if (m->envoy) return;
    Mc68349Duart &d = m->duart;
    d.mcr = 0x000E; d.ilr = 6; d.ivr = 0x4F; d.acr = 0x81;
    d.mr1a = d.mr1b = 0x13;
    d.mr2a = d.mr2b = 0x07;
    d.csra = d.csrb = 0xCC;
    d.cra = 0x90; d.crb = 0x01; d.ier = 0xB3; d.isr = 0;
    d.rx_at = d.rx_len = 0;
    d.rx_enabled = true; d.tx_enabled = false; d.tx_busy = false;
    d.dirty = true;
}

/* Put a loaded struct in place without disturbing the pointers the live one
 * already holds, which is how the machine stays wired together. */
void merge(Pic2000Registers &live, const Pic2000Registers &loaded)
{
    const char *name = live.name;
    FILE *log = live.log;
    EconoRam *econoram = live.econoram;
    void *clock_context = live.clock_context;
    uint64_t (*cpu_clock)(void *) = live.cpu_clock;
    const uint64_t *insn_src = live.insn_src;
    live = loaded;
    live.name = name;
    live.log = log;
    live.econoram = econoram;
    live.clock_context = clock_context;
    live.cpu_clock = cpu_clock;
    live.insn_src = insn_src;
}

void merge(Mc68349Sim &live, const Mc68349Sim &loaded)
{
    FILE *log = live.log;
    void (*programmed)(void *) = live.boot_select_programmed;
    void *board = live.board;
    mh_bus *bus = live.bus;
    Mc68349Duart *duart = live.duart;
    live = loaded;
    live.log = log;
    live.boot_select_programmed = programmed;
    live.board = board;
    live.bus = bus;
    live.duart = duart;
}

void merge(Pic2000Audio &live, const Pic2000Audio &loaded)
{
    void (*sink)(void *, int16_t) = live.sink;
    void *ctx = live.ctx;
    live = loaded;
    live.sink = sink;
    live.ctx = ctx;
}

/* Program counter in the machine's CPU32 state. */
static uint32_t machine_pc(const m68k_machine *m)
{
    return m->core.pc;
}

CpuState save_cpu(const m68k_machine *m)
{
    const cpu_state::Saved r = cpu_state::save(m);
    CpuState s = {};
    for (int i = 0; i < 8; i++) { s.d[i] = r.d[i]; s.a[i] = r.a[i]; }
    s.usp = r.usp; s.isp = r.isp; s.msp = r.msp;
    s.pc = r.pc;   s.pc0 = r.pc0;
    s.vbr = r.vbr; s.sfc = r.sfc; s.dfc = r.dfc;
    s.sr = r.sr;   s.irc = r.irc; s.ird = r.ird;
    s.clock = r.clock;
    s.flags = r.flags; s.exception = r.exception;
    s.ipl = r.ipl; s.sampled_ipl = r.sampled_ipl;
    s.fcl = r.fcl; s.fc_source = r.fc_source;
    s.mbar = m->cpu.mbar;
    s.insns = m->cpu.insns;
    s.stopped = m->cpu.stopped;
    return s;
}

void load_cpu(m68k_machine *m, const CpuState &s)
{
    cpu_state::Saved r = {};
    for (int i = 0; i < 8; i++) { r.d[i] = s.d[i]; r.a[i] = s.a[i]; }
    r.usp = s.usp; r.isp = s.isp; r.msp = s.msp;
    r.pc = s.pc;   r.pc0 = s.pc0;
    r.vbr = s.vbr; r.sfc = s.sfc; r.dfc = s.dfc;
    r.sr = s.sr;   r.irc = s.irc; r.ird = s.ird;
    r.clock = s.clock;
    r.flags = s.flags; r.exception = s.exception;
    r.ipl = s.ipl; r.sampled_ipl = s.sampled_ipl;
    r.fcl = s.fcl; r.fc_source = s.fc_source;
    cpu_state::load(m, r);
    m->cpu.mbar = s.mbar;
    m->cpu.insns = s.insns;
    m->cpu.stopped = s.stopped != 0;
    m->cpu.stop_why = s.stopped ? "halted" : nullptr;
}

BoardState save_board(const m68k_machine *m)
{
    BoardState b = {};
    b.audio_at = m->audio_at;
    b.timer_at = m->timer_at;
    b.pen_timer_at = m->pen_timer_at;
    b.boot_release_at = m->boot_release_at;
    b.timer_fires = m->timer_fires;
    b.hix_checksum_actual = m->hix_checksum_actual;
    b.hix_checksum_stored = m->hix_checksum_stored;
    b.irq_now = m->irq_now;
    b.timer_armed = m->timer_armed;
    b.pen_timer_armed = m->pen_timer_armed;
    b.touch_down = m->touch_down;
    b.power_down = m->power_down;
    b.power_key_down = m->power_key_down;
    b.hix_checksum_pending = m->hix_checksum_pending;
    b.overlay_off = m->sim.overlay_off;
    return b;
}

void load_board(m68k_machine *m, const BoardState &b)
{
    m->audio_at = b.audio_at;
    m->timer_at = b.timer_at;
    m->pen_timer_at = b.pen_timer_at;
    m->boot_release_at = b.boot_release_at;
    m->timer_fires = b.timer_fires;
    // Integrity guards stay derived from the loaded ROM, not saved state data.
    m->irq_now = b.irq_now;
    m->timer_armed = b.timer_armed != 0;
    m->pen_timer_armed = b.pen_timer_armed != 0;
    m->touch_down = b.touch_down != 0;
    m->power_down = b.power_down != 0;
    m->power_key_down = b.power_key_down != 0;
    /* Only if this build could do it at all: a state taken on a machine
     * whose ROM was recognised must not arm an intercept on one whose
     * instructions did not match. */
    m->hix_checksum_pending = b.hix_checksum_pending && m->hix_checksum_intercept;
    /* And the engine has to be told again, since a restored machine may
     * be either side of the interception. */
    mh_m68k_set_stop_at(m, m->hix_checksum_pending ? mh_m68k_checksum_pc(m) : 0);
    m->sim.overlay_off = b.overlay_off != 0;
}

/*
 * Put the memory map back the way the saved machine had it.
 *
 * Two things move at run time and neither is stored in the bus: the boot
 * overlay, which answers the whole address space until software gives CS0 a
 * real window, and the module space, which follows MBAR. Both are restored
 * from the state that does carry them, and then the lookup cache and the
 * processor's direct page tables are dropped so nothing keeps a decode from
 * the machine that was here a moment ago.
 */
void rewire(m68k_machine *m)
{
    if (m->sim.board)
        static_cast<mh_region *>(m->sim.board)->size =
            m->sim.overlay_off ? 0 : 0x01000000u;
    uint32_t base = m->cpu.mbar & 0xFFFFF000u;
    if (m->cpu.module_region && base) m->cpu.module_region->base = base;
    mh_bus_invalidate_lookup(&m->bus);
}

}  /* namespace */

extern "C" {

bool mh_m68k_save_state(m68k_machine *m, const char *path)
{
    if (m->net_probe_enabled) {
        fprintf(m->log, "68k: cannot save with experimental NE2000 probe attached (NIC state is not serialized)\n");
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(m->log, "68k: cannot write %s\n", path);
        return false;
    }

    Header h = {
        SNAP_MAGIC, m->duart.a.enabled ? SNAP_VERSION : m->magicbus.connected ? 3u : 2u,
        m->ram_len, m->xram_len,
        PIC2000_ROM_SIZE, rom_sum(m->rom, PIC2000_ROM_SIZE),
        (uint32_t)sizeof(CpuState), (uint32_t)sizeof(BoardState),
        (uint32_t)sizeof(Pic2000Registers), (uint32_t)sizeof(Mc68349Sim),
        (uint32_t)sizeof(Pic2000Audio), (uint32_t)sizeof(EconoRam),
    };
    const CpuState cpu = save_cpu(m);
    const BoardState board = save_board(m);
    const Pic2000Registers dev21 = strip(m->dev21), dev0c = strip(m->dev0c);
    const Mc68349Sim sim = strip(m->sim);
    const DuartState duart = save_duart(m->duart);
    const DuartAState duart_a = save_duart_a(m->duart.a);
    const Pic2000Audio audio = strip(m->audio);
    uint8_t keyboard[PicMagicBus::state_size];
    m->magicbus.encode(keyboard);

    bool ok = wr(f, &h, sizeof(h)) &&
              wr(f, m->ram, m->ram_len) &&
              wr(f, m->xram, m->xram_len) &&
              wr(f, &cpu, sizeof(cpu)) &&
              wr(f, &board, sizeof(board)) &&
              wr(f, &dev21, sizeof(dev21)) &&
              wr(f, &dev0c, sizeof(dev0c)) &&
              wr(f, &sim, sizeof(sim)) &&
              wr(f, &duart, sizeof(duart)) &&
              wr(f, &audio, sizeof(audio)) &&
              wr(f, &m->battery_ram, sizeof(m->battery_ram)) &&
              (h.version < 3 || wr(f, keyboard, sizeof(keyboard))) &&
              (h.version < 4 || wr(f, &duart_a, sizeof(duart_a)));
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        fprintf(m->log, "68k: short write to %s\n", path);
        return false;
    }
    fprintf(m->log, "68k: state saved to %s at instruction %llu, pc=%08X\n",
            path, (unsigned long long)m->cpu.insns, machine_pc(m));
    return true;
}

bool mh_m68k_load_state(m68k_machine *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(m->log, "68k: cannot read %s\n", path);
        return false;
    }

    Header h;
    if (!rd(f, &h, sizeof(h)) || h.magic != SNAP_MAGIC) {
        fprintf(m->log, "68k: %s is not a state file\n", path);
        fclose(f);
        return false;
    }
    if (h.version < 1 || h.version > SNAP_VERSION) {
        fprintf(m->log, "68k: %s is version %u, this build reads 1 through %u\n",
                path, h.version, SNAP_VERSION);
        fclose(f);
        return false;
    }
    /*
     * A state is only meaningful against the machine it came from. The ROM
     * checksum catches the wrong device, the memory sizes catch a different
     * configuration of the same one, and the struct sizes catch a build whose
     * devices are laid out differently -- which matters because they are
     * written whole.
     */
    if (h.rom_size != PIC2000_ROM_SIZE ||
        h.rom_sum != rom_sum(m->rom, PIC2000_ROM_SIZE)) {
        fprintf(m->log, "68k: %s was saved from a different ROM\n", path);
        fclose(f);
        return false;
    }
    if (h.ram_len != m->ram_len || h.xram_len != m->xram_len ||
        h.cpu_bytes != sizeof(CpuState) || h.board_bytes != sizeof(BoardState) ||
        h.dev_bytes != sizeof(Pic2000Registers) ||
        h.sim_bytes != sizeof(Mc68349Sim) ||
        h.audio_bytes != sizeof(Pic2000Audio) ||
        h.bram_bytes != sizeof(EconoRam)) {
        fprintf(m->log, "68k: %s does not match this machine or build\n", path);
        fclose(f);
        return false;
    }

    /*
     * Read everything before changing anything. A short read half way
     * through would otherwise leave a machine that is part one state and
     * part another, which is worse than a machine that refused to load.
     */
    CpuState cpu;
    BoardState board;
    static Pic2000Registers dev21, dev0c;   /* 41 KB each; not on the stack */
    Mc68349Sim sim;
    DuartState duart = {};
    DuartAState duart_a = {};
    Mc68349Duart::ChannelA channel_a;
    Pic2000Audio audio;
    EconoRam battery_ram;
    PicMagicBus magicbus;
    uint8_t keyboard[PicMagicBus::state_size];
    uint8_t *ram = (uint8_t *)malloc(m->ram_len);
    uint8_t *xram = (uint8_t *)malloc(m->xram_len);
    bool ok = ram && xram &&
              rd(f, ram, m->ram_len) &&
              rd(f, xram, m->xram_len) &&
              rd(f, &cpu, sizeof(cpu)) &&
              rd(f, &board, sizeof(board)) &&
              rd(f, &dev21, sizeof(dev21)) &&
              rd(f, &dev0c, sizeof(dev0c)) &&
              rd(f, &sim, sizeof(sim)) &&
              (h.version == 1 || rd(f, &duart, sizeof(duart))) &&
              rd(f, &audio, sizeof(audio)) &&
              rd(f, &battery_ram, sizeof(battery_ram)) &&
              (h.version < 3 ||
               (rd(f, keyboard, sizeof(keyboard)) && magicbus.decode(keyboard) &&
                (!magicbus.connected || PicMagicBus::supported(m)) &&
                (h.version >= 4 || magicbus.connected))) &&
              (h.version < 4 || (rd(f, &duart_a, sizeof(duart_a)) &&
                                decode_duart_a(channel_a, duart_a)));
    fclose(f);
    if (!ok) {
        free(ram);
        free(xram);
        fprintf(m->log, "68k: short read from %s\n", path);
        return false;
    }

    memcpy(m->ram, ram, m->ram_len);
    memcpy(m->xram, xram, m->xram_len);
    free(ram);
    free(xram);
    merge(m->dev21, dev21);
    merge(m->dev0c, dev0c);
    merge(m->sim, sim);
    m->duart.detach_ppp();
    if (h.version == 1) load_legacy_pic_duart(m);
    else load_duart(m->duart, duart);
    m->duart.a = channel_a;
    merge(m->audio, audio);
    m->battery_ram = battery_ram;
    m->magicbus = magicbus;
    load_board(m, board);
    load_cpu(m, cpu);
    rewire(m);

    /*
     * The block cache goes. Every block guards the guest bytes it was built
     * from, so this would be safe without it -- but a whole new memory image
     * would invalidate nearly all of them one at a time on the way past, and
     * starting clean is both faster and easier to reason about.
     */
    if (m->blocks) {
        m68k_blocks_free(m->blocks);
        m->blocks = m68k_blocks_create();
        if (!m->blocks) {
            fprintf(m->log, "68k: no memory for the block engine after a "
                    "load; single-step interpretation will run\n");
            m->core_enabled = false;
        }
    }

    fprintf(m->log, "68k: state loaded from %s at instruction %llu, pc=%08X\n",
            path, (unsigned long long)m->cpu.insns, machine_pc(m));
    return true;
}

}  /* extern "C" */
