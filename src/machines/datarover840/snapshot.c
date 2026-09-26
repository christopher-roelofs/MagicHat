/*
 * snapshot.c — save and restore the whole machine.
 *
 * Booting to the monitor prompt takes the better part of a minute, and
 * experiments downstream of it should not have to pay that every time.
 *
 * The state we care about is all plain data, but the structs also hold
 * pointers that wire the machine together (a device's back-pointer to the
 * SoC, the SoC's pointer to the CPU, log file handles). Those are properties
 * of this process, not of the emulated machine, so they are re-established
 * after loading rather than written out.
 */
#include "machines/datarover840/machine.h"

#include <stdlib.h>
#include <string.h>

#define SNAP_MAGIC   0x4D484154u    /* "MHAT" */
/*
 * 5: the machine. 6: adds the Magic Bus keyboard. 7: adds what was in the
 * card slots -- which card and where its image is, never the image itself.
 * All three still load.
 */
#define SNAP_VERSION 7u
#define SNAP_KEYBOARD_FROM 6u
#define SNAP_CARDS_FROM 7u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ram_size;
    uint32_t rom_size;      /* stored: flash is writable, so it is state */
    uint32_t cpu_bytes;
    uint32_t soc_bytes;
    uint32_t glacier_bytes;
    uint32_t unknown_bytes;
} snap_header;

static bool wr(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n;
}

static bool rd(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n;
}

bool mh_snapshot_ram_size(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot read %s\n", path);
        return false;
    }
    snap_header h;
    bool ok = rd(f, &h, sizeof(h));
    fclose(f);
    if (!ok || h.magic != SNAP_MAGIC || h.version < 5 || h.version > SNAP_VERSION ||
        !h.ram_size || h.ram_size > DR840_RAM_WINDOW ||
        h.cpu_bytes != sizeof(r3900) || h.soc_bytes != MH_TX39_STATE_BYTES ||
        h.glacier_bytes != sizeof(((machine *)0)->pcmcia) ||
        h.unknown_bytes != sizeof(((machine *)0)->kseg3)) {
        fprintf(stderr, "snapshot: %s has an invalid or incompatible header\n", path);
        return false;
    }
    *size = h.ram_size;
    return true;
}

bool mh_snapshot_save(machine *m, const char *path)
{
    if (m->soc.mbus_port && m->soc.mbus_port != &m->keyboard.port) {
        fprintf(stderr, "snapshot: unknown Magic Bus peripheral serialization is not implemented\n");
        return false;
    }
    /*
     * A card in a slot is not a reason to refuse.
     *
     * These two used to be, on the grounds that a writable SRAM image lives
     * in its own host file and an NE2000 has live sockets behind it, so
     * neither can be written into a state. Both of those are true and
     * neither matters, because a card is not part of the machine: it is a
     * thing that was pushed into a slot, and the controller announces
     * arrivals and departures on a card-detect edge precisely so that the
     * guest copes with either at any moment.
     *
     * So a state is taken with the slots as they will be found: empty. The
     * restore marks both absent, which the guest reads as the card having
     * been taken out while it was not looking -- a state real hardware
     * produces constantly -- and putting it back in is an insertion it
     * handles the same way it handles the first one. Its contents were never
     * at risk: an SRAM image is a shared mapping of a locked host file and
     * the writes were already in it.
     */
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot write %s\n", path);
        return false;
    }

    snap_header h = {
        .magic = SNAP_MAGIC, .version = SNAP_VERSION,
        .ram_size = m->ram_size, .rom_size = m->rom_size,
        .cpu_bytes = sizeof(m->cpu), .soc_bytes = MH_TX39_STATE_BYTES,
        .glacier_bytes = sizeof(m->pcmcia), .unknown_bytes = sizeof(m->kseg3),
    };

    bool ok = wr(f, &h, sizeof(h)) &&
              wr(f, m->ram, m->ram_size) &&
              /* Program storage is flash, and Magic Cap patches it, so its
               * contents are machine state rather than a copy of the file. */
              wr(f, m->rom, m->rom_size) &&
              wr(f, &m->cpu, sizeof(m->cpu)) &&
              wr(f, &m->soc, MH_TX39_STATE_BYTES) &&
              wr(f, m->pcmcia, sizeof(m->pcmcia)) &&
              wr(f, &m->kseg3, sizeof(m->kseg3));

    if (ok && h.version >= SNAP_KEYBOARD_FROM) {
        uint8_t keyboard[MH_DR_KEYBOARD_STATE_SIZE];
        mh_dr_keyboard_encode(&m->keyboard,keyboard);
        ok = wr(f,keyboard,sizeof(keyboard));
    }
    /*
     * What was in the slots: a name, not a copy.
     *
     * A card's contents are its own -- an SRAM image is a shared mapping of a
     * locked host file that the guest has been writing through all along, and
     * duplicating megabytes of it here would only create a second copy to
     * disagree with the first. What a state owes the next run is which card
     * was in which slot, so a device picked up again still has it.
     */
    if (ok && h.version >= SNAP_CARDS_FROM) {
        for (unsigned slot = 0; slot < 2 && ok; slot++) {
            const char *path = m->card_path[slot];
            uint32_t kind = (uint32_t)m->card_kind[slot];
            uint32_t len = path ? (uint32_t)strlen(path) : 0;
            ok = wr(f, &kind, sizeof(kind)) && wr(f, &len, sizeof(len)) &&
                 (!len || wr(f, path, len));
        }
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "snapshot: short write to %s\n", path);
        return false;
    }
    fprintf(stderr, "snapshot saved to %s at instruction %llu\n", path,
            (unsigned long long)m->cpu.insn_count);
    return true;
}

bool mh_snapshot_load(machine *m, const char *path)
{
    if (m->soc.mbus_port && m->soc.mbus_port != &m->keyboard.port) {
        fprintf(stderr, "snapshot: disconnect the unknown Magic Bus peripheral before loading\n");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot read %s\n", path);
        return false;
    }

    snap_header h;
    if (!rd(f, &h, sizeof(h)) || h.magic != SNAP_MAGIC) {
        fprintf(stderr, "snapshot: %s is not a snapshot\n", path);
        fclose(f);
        return false;
    }
    if ((h.version != 5 && h.version != SNAP_VERSION)) {
        fprintf(stderr, "snapshot: %s is version %u, this build writes %u\n",
                path, h.version, SNAP_VERSION);
        fclose(f);
        return false;
    }
    /*
     * A snapshot is only meaningful against the machine it was taken from.
     * The struct sizes catch a mismatched build, which matters because the
     * device structs are written out whole.
     */
    if (h.ram_size != m->ram_size || h.rom_size != m->rom_size ||
        h.cpu_bytes != sizeof(m->cpu) ||
        h.soc_bytes != MH_TX39_STATE_BYTES ||
        h.glacier_bytes != sizeof(m->pcmcia) ||
        h.unknown_bytes != sizeof(m->kseg3)) {
        fprintf(stderr, "snapshot: %s does not match this machine or build\n",
                path);
        fclose(f);
        return false;
    }

    bool ok = rd(f, m->ram, m->ram_size) &&
              rd(f, m->rom, m->rom_size) &&
              rd(f, &m->cpu, sizeof(m->cpu)) &&
              rd(f, &m->soc, h.soc_bytes) &&
              rd(f, m->pcmcia, sizeof(m->pcmcia)) &&
              rd(f, &m->kseg3, sizeof(m->kseg3));
    uint8_t keyboard[MH_DR_KEYBOARD_STATE_SIZE] = {1};
    if (ok && h.version >= SNAP_KEYBOARD_FROM)
        ok = rd(f,keyboard,sizeof(keyboard));
    /*
     * The slots are read but nothing is put in them here. Opening a card
     * image is the caller's business -- it is the one that knows whether the
     * command line named a different card, and a snapshot that reached out
     * and mapped files would be doing something a restore has no right to do
     * on its own.
     */
    mh_card_kind want_kind[2] = { MH_CARD_NONE, MH_CARD_NONE };
    char *want_path[2] = { NULL, NULL };
    if (ok && h.version >= SNAP_CARDS_FROM) {
        for (unsigned slot = 0; slot < 2 && ok; slot++) {
            uint32_t kind = 0, len = 0;
            ok = rd(f, &kind, sizeof(kind)) && rd(f, &len, sizeof(len));
            if (ok && len > 4096) ok = false;   /* not a path we wrote */
            if (ok && len) {
                want_path[slot] = calloc(1, len + 1);
                ok = want_path[slot] && rd(f, want_path[slot], len);
            }
            if (ok) want_kind[slot] = (mh_card_kind)kind;
        }
    }
    if (ok) {
        mh_machine_rebind(m);
        ok = mh_dr_keyboard_decode(&m->keyboard,keyboard);
    }
    fclose(f);

    if (!ok) {
        for (unsigned slot = 0; slot < 2; slot++) free(want_path[slot]);
        fprintf(stderr, "snapshot: short read from %s\n", path);
        return false;
    }

    mh_machine_rebind(m);
    for (unsigned slot = 0; slot < 2; slot++) {
        free(m->card_path[slot]);
        m->card_path[slot] = want_path[slot];
        m->card_kind[slot] = want_kind[slot];
    }

    /*
     * Diagnostic counters describe the session, not the machine. Carrying
     * them across a load makes every histogram read as the boot's traffic
     * plus a rounding error, which hides exactly what you loaded the
     * snapshot to look at.
     */
    memset(m->soc.hist_read, 0, sizeof(m->soc.hist_read));
    memset(m->soc.hist_write, 0, sizeof(m->soc.hist_write));
    memset(m->soc.hist_pc, 0, sizeof(m->soc.hist_pc));
    m->soc.unknown_reads = m->soc.unknown_writes = 0;
    m->bus.reads = m->bus.writes = 0;
    m->bus.mmio_reads = m->bus.mmio_writes = 0;
    m->bus.float_reads = m->bus.faults = 0;
    m->cpu.exc_count = 0;

    /*
     * Host-session pointers are not machine state. They ride into the file
     * only because the structs are written whole, and restoring them means
     * keeping an address that belonged to whichever process saved the
     * snapshot.
     *
     * This was not theoretical: a state saved by a run with --coverage
     * segfaulted every later run without it, inside the coverage write in
     * mh_cpu_step, because the pointer was non-NULL and pointed at freed
     * memory. The audio sink is the same bug with a worse ending, since it
     * is a function pointer that mh_sib_pump_audio would call.
     *
     * Whatever the new run wants, it configures for itself after loading.
     */
    m->cpu.coverage = NULL;
    m->cpu.coverage_words = 0;
    m->soc.sib.audio_sink = NULL;
    m->soc.sib.audio_ctx = NULL;
    /*
     * A card is host-side: the image lives in the bus window and the machine
     * owns it, so a state saved with one loads into a machine without.
     */
    for (unsigned c = 0; c < 2; c++)
        m->pcmcia[c].card_present = false;
    for (unsigned i = 0; i < 2; i++) {
        m->soc.uart[i].tx_bytes = m->soc.uart[i].rx_delivered = 0;
        m->soc.uart[i].rxhold_reads = m->soc.uart[i].ctrl1_reads = 0;
    }

    fprintf(stderr, "snapshot loaded from %s at instruction %llu, pc=%08X\n",
            path, (unsigned long long)m->cpu.insn_count, m->cpu.pc);
    return true;
}
