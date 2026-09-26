#include "machines/envoy/board.h"
#include "soc/mc68349/sim.h"
#include "machines/pic2000/registers.h"
#include "machines/pic2000/audio.h"
#include "machines/pic2000/machine68k.h"
#include "host/ppp.h"

extern "C" {
#include "core/bus/bus.h"
#include "devices/pccard/pccard.h"
#include "rom/identify.h"
}

#include <cstdlib>
#include <cstring>
extern "C" {
#include "cpu/m68k/core/m68k.h"
#include "cpu/m68k/core/m68k_block.h"
#include "cpu/m68k/core/m68k_emit.h"
#include "cpu/m68k/core/m68k_emit_policy.h"
}
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <string>

#include "machines/pic2000/board.h"
#include "machines/pic2000/cpu_state.h"


extern "C" {

/*
 * Where the HIX-300 compares its ROM checksum against the stored one.
 *
 * One instruction in a whole run, and until the engine could be told to
 * stop in front of an address, finding it meant checking every program
 * counter on every interpreter step.
 */
static const uint32_t HIX_CHECKSUM_PC = MH_M68K_HIX_CHECKSUM_PC;

/*
 * Ask the engine to stop in front of an address, or stop asking.
 *
 * The block cache goes with it: a block built while nothing was being
 * watched for covers whatever instructions follow each other, and one of
 * them may be the address now being watched for.
 */
void mh_m68k_set_stop_at(m68k_machine *m, uint32_t pc)
{
    if (m->stop_at == pc) return;
    m->stop_at = pc;
    if (m->blocks) m68k_blocks_flush(m->blocks);
}

/* The core's bus, defined with the rest of the engine further down. */
static uint32_t core_read(void *ctx, uint32_t addr, unsigned size);
static void core_write(void *ctx, uint32_t addr, unsigned size, uint32_t value);
/* Named where the run loop notices one; defined with the rest of the
 * engine, below. */
static void core_report_exceptions(m68k_machine *m);

static inline uint32_t cpu_pc(const m68k_machine *m)
{
    return m->core.pc;
}
static inline uint32_t cpu_a(const m68k_machine *m, unsigned r)
{
    return m->core.a[r];
}
static inline uint32_t cpu_d(const m68k_machine *m, unsigned r)
{
    return m->core.d[r];
}
static inline uint16_t cpu_sr(const m68k_machine *m)
{
    return m68k_get_sr(&m->core);
}
static inline void cpu_set_d(m68k_machine *m, unsigned r, uint32_t v)
{
    m->core.d[r] = v;
}
/*
 * Whether the part has retired an LPSTOP and is waiting for an interrupt.
 * Not a halt: the devices go on running, and this is where an idle machine
 * spends nearly all of its time.
 */
static inline bool cpu_part_stopped(const m68k_machine *m)
{
    return m->core.stopped;
}
static inline uint32_t cpu_vbr(const m68k_machine *m)
{
    return m->core.vbr;
}

bool mh_m68k_is_envoy(const m68k_machine *m)
{
    return m->envoy;
}

bool mh_m68k_is_hix(const m68k_machine *m) { return m->hix; }

m68k_machine *mh_m68k_new(const char *rom_path, unsigned ram_mb, FILE *log)
{
    auto *m = new m68k_machine();
    m->log = log ? log : stderr;
    m->sim.log = m->log;
    m->sim.bus = &m->bus;
    const char *idle_env = std::getenv("MH_68K_IDLE_FAST");
    m->idle_fast_enabled = !idle_env || std::strcmp(idle_env, "0");
    const char *quiet_env = std::getenv("MH_68K_QUIET_FAST");
    m->quiet_fast_enabled = !quiet_env || std::strcmp(quiet_env, "0");
    if (!ram_mb || ram_mb > 16) {
        fprintf(m->log, "68k: RAM must be between 1 and 16 MB\n");
        delete m;
        return nullptr;
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(m->log, "68k: cannot open %s\n", rom_path);
        delete m;
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n != PIC2000_ROM_SIZE) {
        fprintf(m->log, "68k: expected a 4 MiB ROM, got %ld bytes\n", n);
        fclose(f);
        delete m;
        return nullptr;
    }
    m->rom = (uint8_t *)malloc((size_t)n);
    if (!m->rom || fread(m->rom, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); mh_m68k_free(m); return nullptr;
    }
    fclose(f);
    if (mh_rom_identify(m->rom, (size_t)n) == MH_ROM_HIX300) {
        m->hix = true;
        // Default HIX model requested by the user; the inferred divider
        // fits known rate codes but still lacks a hardware specification.
        m->audio.experimental_divider = true;
        fprintf(m->log, "68k: HIX-300 audio: inferred divider enabled by default (unverified hardware timing)\n");
        // HIX MC19 C2's startup byte sum; diagnose without changing the ROM
        // or the guest's own integrity-check result.
        uint32_t actual = 0;
        for (size_t i = 0xC8E; i < 0x2AF08E; ++i) actual += m->rom[i];
        const uint32_t stored = (uint32_t)m->rom[0x4C] << 24 |
            (uint32_t)m->rom[0x4D] << 16 | (uint32_t)m->rom[0x4E] << 8 |
            m->rom[0x4F];
        if (actual != stored) {
            fprintf(m->log, "68k: WARNING: HIX-300 ROM checksum mismatch: "
                    "stored %08X, calculated %08X (bytes [00000C8E,002AF08E)); "
                    "ROM unchanged\n",
                    stored, actual);
            static const uint8_t check_code[] = {
                0x4e,0xba,0xf9,0x52,0x50,0x4f,0xb0,0xac,0x00,0x40,
                0x67,0x06,0x70,0x16,0x60,0x00,0x00,0x82,0x48,0x78,0x00,0xac
            };
            m->hix_checksum_intercept =
                !memcmp(m->rom + 0xA7C, check_code, sizeof(check_code));
            m->hix_checksum_pending = m->hix_checksum_intercept;
            if (m->hix_checksum_pending)
                mh_m68k_set_stop_at(m, HIX_CHECKSUM_PC);
            m->hix_checksum_actual = actual;
            m->hix_checksum_stored = stored;
            fprintf(m->log, m->hix_checksum_intercept
                ? "68k: DEVIATION: HIX-300 checksum-result interception enabled; ROM bytes remain unchanged\n"
                : "68k: HIX-300 checksum interception refused: unrecognized check instructions\n");
        }
    }
    const uint32_t reset_pc = (uint32_t)m->rom[4] << 24 |
        (uint32_t)m->rom[5] << 16 | (uint32_t)m->rom[6] << 8 | m->rom[7];
    // All four available images use an aligned 4 MiB ROM window. Envoy's
    // vector is 02400216; the Sony images use 0E000xxx.
    const uint32_t rom_base = reset_pc & ~(PIC2000_ROM_SIZE - 1u);

    mh_bus_init(&m->bus);
    m->bus.log = m->log;
    m->bus.log_unmapped = false;   /* we report unknowns ourselves, with a PC */
    m->duart.insns = &m->cpu.insns;
    m->duart.clock_hz = PIC2000_CPU_HZ;
    m->sim.duart = &m->duart;

    /*
     * The boot overlay, and it is not a hack.
     *
     * On a 68300-family part the boot chip select answers the *whole*
     * address space out of reset, precisely so the CPU can fetch its vector
     * pair from address 0 before anything has been configured. That is why
     * this image begins with a stack pointer and a reset PC: at that moment
     * address 0 is the ROM. Software then programs the chip selects to give
     * ROM its real window and put DRAM underneath, which is exactly what the
     * reset path here does at 0x3C000040 onwards. The SIM callback retires
     * the low ROM mapping when CS0 is programmed; power-on restores it.
     */
    mh_bus_add_rom(&m->bus, "rom", rom_base, PIC2000_ROM_SIZE,
                    m->rom, (uint32_t)n);
    if (rom_base == ENVOY_ROM_BASE) {
        mh_bus_add_rom(&m->bus, "envoy-rom-alias", ENVOY_ROM_ALIAS, PIC2000_ROM_SIZE,
                       m->rom, (uint32_t)n);
        fprintf(m->log,"68k: experimental Envoy ROM alias at 00400000 (CS0 boot configuration)\n");
    }
    mh_region *ovl = mh_bus_add_rom(&m->bus, "bootovl", 0x00000000u,
                                      0x01000000u, m->rom, (uint32_t)n);
    m->sim.board = ovl;
    m->sim.boot_select_programmed = [](void *region) {
        mh_region *r = static_cast<mh_region *>(region);
        r->size = 0;
    };

    /*
     * RAM, and this is a guess rather than a measurement.
     *
     * The reset vector puts the stack at 0x00100000, so there is memory
     * below that and the board answers at address zero. How much, and
     * whether it is contiguous, is exactly what the ROM's own sizing loop
     * will tell us once it runs -- so this is deliberately generous and
     * deliberately marked. The window is larger than the array so it mirrors,
     * which is what a sizing loop needs to see.
     */
    /*
     * DRAM at zero, and extra RAM at 0x04000000.
     *
     * Both are measured rather than assumed now. The reset path loads its
     * stack from *(0x0E000212) = 0x00080000 and the warm-boot base from
     * *(0x0E0000A0) = 0x00200000, so low memory has to be RAM and has to
     * reach at least 2 MB. And chip select 2 names 0x04000000, where the ROM
     * looks for the longword 'XRAM' -- 0x5852414D -- when choosing its stack.
     * That test alone does not decide persistent heap recovery: the ROM also
     * accepts a valid low-RAM restart block without the XRAM signature.
     *
     * The sizes are still guesses. Nothing has yet been seen that sizes
     * either bank, and the ROM's own sizing loop is the thing to watch for.
     */
    m->ram_len = ram_mb * 1024u * 1024u;
    m->ram = (uint8_t *)calloc(1, m->ram_len);
    if (!m->ram) { mh_m68k_free(m); return nullptr; }
    mh_bus_add_ram(&m->bus, "dram", 0x00000000u, m->ram, m->ram_len,
                    0x01000000u);

    m->xram_len = ram_mb * 1024u * 1024u;
    m->xram = (uint8_t *)calloc(1, m->xram_len);
    if (!m->xram) { mh_m68k_free(m); return nullptr; }
    m->xram_region = mh_bus_add_ram(&m->bus, "xram", 0x04000000u,
                                     m->xram, m->xram_len, 0x01000000u);

    /*
     * The System Integration Module. Modelled as a logger first and a device
     * second: what it is asked to do is the memory map, and reading that off
     * the ROM's own writes is more reliable than reading a datasheet we do
     * not have for a board nobody documented.
     */
    /*
     * The module space. Its base is whatever MBAR says, and MBAR is written
     * in CPU space in the reset path -- so this initial address is only where
     * it sits until the ROM says otherwise, and the ROM says 0x3C000000 a few
     * hundred instructions in. Registering it here at all is so that an
     * access before that write is decoded rather than lost.
     */
    mh_region *modreg =
        mh_bus_add_mmio(&m->bus, "modules", PIC2000_SIM_BASE, PIC2000_SIM_SIZE,
                         &m->sim, mc68349_sim_read, mc68349_sim_write);
    m->cpu.module_region = modreg;

    /*
     * Two devices the boot code uses and we cannot yet name.
     *
     * 0x21000000 is the larger of them and is plainly a register file rather
     * than memory: the ROM loads it as a base and then works at fixed
     * offsets -- 0xB8, 0xBA, 0xC2, 0xD0 -- and polls a status word at 0xEE
     * while writing a control word at 0xF0. The 'BS' and 'TF' written to
     * 0xFE look like a signature or an unlock rather than a memory test.
     *
     * 0x0C000000 is named by chip select 3, and the boot code sets single
     * bits in it (0x100, 0x200) after seeing bits in that status word, each
     * followed by a spin of 0x1E000 -- which is what powering something up
     * and waiting for it looks like.
     */
    /*
     * A slot the board decodes and that need not have anything in it. The
     * ROM looks at 0x08000000 for the longwords 'TEST' and 'test' and, if it
     * finds them, takes a stack from just past them and jumps in -- a debug
     * or development image. Finding nothing is the ordinary case, and a
     * region that floats high says "decoded, empty" where an undecoded fault
     * would wrongly say "the board does not answer here".
     */
    m->testimg_region = mh_bus_add_float(&m->bus, "testimg",
                                          0x08000000u, 0x01000000u);

    m->dev21.name = "dev21"; m->dev21.base = 0x21000000u; m->dev21.log = m->log;
    m->dev21.insn_src = &m->cpu.insns;
    m->dev21.power_control = true;
    m->dev21.counter_off = 0xD4u;
    m->dev21.compare_off = 0xD8u;
    /* Group 1's pending register is a longword at +0xB0; groups 2 and 3 read
     * longwords at +0xBA and +0xB8, so they share +0xBA and reach +0xBD. */
    for (int i = 0; i < 1024; i++) m->dev21.adc_chan[i] = -1;
    /* Fixed simulated batteries, not measured voltages. ReadAtoDChannel
     * samples channels 2 and 4 for the main and backup battery servers.
     * The ROM's battery percentage routine (0x0E07A2AA) compares the
     * averaged samples directly: full main = 805, full backup = 487.
     * Use fixed samples above those thresholds, within the 10-bit range.
     * Explicit --adc / --adc-chan inputs still override these defaults.
     * Keep these separate from the mux-dependent pen pressure readings. */
    m->dev21.adc_chan[2] = 832;
    // HIX backup thresholds at ROM 2579D6 are 613/549/512/510/128.
    // Its CalculateLevel (0E052610) confirms 510..613; PIC's healthy
    // sample 512 means almost empty on this board.
    m->dev21.adc_chan[4] = m->hix ? 640 : 512;
    fprintf(m->log, "68k: simulated main/backup battery ADC channels 2/4 = "
                    "832/%d (fixed readings, no voltage/discharge model)\n",
                    m->dev21.adc_chan[4]);
    m->dev21.adc_off = 0xE4u;
    /*
     * Bit 6 only. The battery poll at 0x0E07A1DC waits for (pending & 0x50),
     * so either bit satisfies it, but the pen path names bit 6 specifically:
     * it clears bit 6, enables bit 6 and starts a conversion (0x0E08B81E,
     * 0x0E08B824, 0x0E08B82A), and the handler that entry 24 points at
     * clears bit 6 first thing (0x0E08B854). Nothing in the ROM says the
     * converter sets bit 4, so setting it would be inventing an interrupt.
     */
    m->dev21.adc_done_bits = 0x40u;
    m->dev21.w1c_lo[0] = 0xB0u; m->dev21.w1c_hi[0] = 0xB4u;
    m->dev21.w1c_lo[1] = 0xB8u; m->dev21.w1c_hi[1] = 0xBEu;
    mh_m68k_set_cpi(m, PIC2000_DEFAULT_CPI);
    m->dev0c.name = "dev0c"; m->dev0c.base = 0x0C000000u; m->dev0c.log = m->log;
    /* Only established for PIC-2000. Other 68k ROMs using this diagnostic
     * harness need their own board wiring established independently. */
    m->dev0c.magicbus_empty_input =
        mh_rom_identify(m->rom, (size_t)n) == MH_ROM_PIC2000;
    m->envoy = mh_rom_identify(m->rom, (size_t)n) == MH_ROM_ENVOY;
    static const char mc31[]="1,0.31,MOTO,1,";
    m->envoy_mc31 = m->envoy &&
        std::search(m->rom,m->rom+n,mc31,mc31+sizeof(mc31)-1)!=m->rom+n;
    if(m->envoy_mc31)
        fprintf(m->log,"68k: Envoy mc31 keyboard remains unverified and disabled; "
                       "see docs/68K_KEYBOARD.md\n");
    if(m->envoy_mc31) {
        uint32_t actual=0;
        for(size_t i=0x1130;i<0x2e0f28;++i) actual+=m->rom[i];
        const uint32_t stored=uint32_t(m->rom[0x4c])<<24 |
            uint32_t(m->rom[0x4d])<<16 | uint32_t(m->rom[0x4e])<<8 | m->rom[0x4f];
        // Recognize the checksum call, comparison, assertion and range metadata.
        static const uint8_t check_code[]={
            0x4e,0xba,0xf8,0x86,0x50,0x4f,0xb0,0xab,0x00,0x40,
            0x67,0x0a,0x48,0x7a,0x02,0x8e,0x21,0xdf,0x00,0x10,0x4a,0xfa
        };
        static const uint8_t range_start[]={0,0,0x11,0x30};
        static const uint8_t range_size[]={0,0x2d,0xfe,0x04};
        m->hix_checksum_actual=actual; m->hix_checksum_stored=stored;
        if(actual!=stored) {
            fprintf(m->log,"68k: WARNING: Envoy mc31 ROM checksum mismatch: "
                "stored %08X, calculated %08X; ROM unchanged\n",stored,actual);
            m->hix_checksum_intercept=
                !memcmp(m->rom+0xe82,check_code,sizeof(check_code)) &&
                !memcmp(m->rom+0x34,range_start,sizeof(range_start)) &&
                !memcmp(m->rom+0x1124,range_size,sizeof(range_size));
            m->hix_checksum_pending=m->hix_checksum_intercept;
            if(m->hix_checksum_pending) mh_m68k_set_stop_at(m,mh_m68k_checksum_pc(m));
            fprintf(m->log,m->hix_checksum_intercept ?
                "68k: DEVIATION: Envoy mc31 checksum-result interception enabled; ROM bytes remain unchanged\n" :
                "68k: Envoy mc31 checksum interception refused: unrecognized check instructions or range\n");
        }
    }
    m->dev21.adapter_input = m->envoy;
    if (m->envoy) {
        /* Envoy 00463B8A samples byte E7 bit 2. Discovery at 004622B6
         * takes the high-input path through stop/arm at 00461CA2 and
         * returns zero devices; 00462D28 independently gates discovery.
         * The disconnected polarity matches PIC, but the pin does not. */
        m->dev21.magicbus_empty_input = true;
        m->dev21.magicbus_empty_offset = 0xE7;
    }
    m->audio.sample_ready_irq = m->envoy;
    // HIX uses this pin for a reset/presence identity protocol, not EconoRAM.
    // HIX 0E059DAE sends Read ROM (33) then checks eight identity bytes.
    // A latched zero input falsely supplied presence and an all-zero ID
    // with a valid CRC. Model the passive wire while the identity chip is
    // unimplemented, not a fabricated serial number or presence response.
    m->dev21.serial_pullup = m->hix;
    if (m->hix)
        fprintf(m->log, "68k: HIX serial identity device unimplemented; "
                        "passive pull-up only (no presence or serial ID)\n");
    if (m->envoy) {
        m->dev21.econoram = &m->battery_ram;
        m->dev21.clock_context = m;
        /*
         * The processor's cycle count, which only this machine reads: its
         * battery RAM is a one-wire part whose data line is a function of
         * elapsed cycles. Both cores keep one now, and the answer comes
         * from whichever is holding the registers -- during a stretch on
         * our own core that is ours, which is what stops the clock
         * standing still for the length of a block.
         */
        m->dev21.cpu_clock = [](void *p) -> uint64_t {
            auto *m = static_cast<m68k_machine *>(p);
            return m->core.cycles;
        };
        m->dev21.clock_hz = PIC2000_CPU_HZ;
    }
    if (mh_rom_identify(m->rom, (size_t)n) == MH_ROM_HIX300) {
        /* HIX reset programs CS3=1E0000F1, and its speaker startup at
         * 0E077D82 writes the auxiliary output latch at 1E000000.
         * Keep this a reported register file until its wiring is known. */
        m->dev0c.name = "hix-aux";
        m->dev0c.base = 0x1E000000u;
    }
    mh_bus_add_mmio(&m->bus, "dev21", 0x21000000u, 0x1000u,
                     m, pic2000_dev21_read, pic2000_dev21_write);
    mh_bus_add_mmio(&m->bus, m->dev0c.name, m->dev0c.base, 0x1000u,
                     m, pic2000_dev0c_read, pic2000_dev0c_write);

    m->cpu.bus = &m->bus;
    m->cpu.log = m->log;
    {
        m68k_bus core_bus = { core_read, core_write, m, nullptr, nullptr };
        m68k_init(&m->core, &core_bus);
        m->core.hand_back = true;
        /* Only the Envoy reads the cycle count, and only a core that is
         * asked to keep one pays the add per instruction that keeping it
         * costs -- in emitted code as well as in the interpreter. */
        m->core.count_cycles = m->envoy;
    }
    m->cpu.pc_src = &m->core.pc;
    m68k_reset(&m->core);
    /* All board regions are installed before enabling the shared page cache.
     * CS0 and MBAR mutations invalidate it when their geometry changes. */
    const char *cache_env = std::getenv("MH_68K_BUS_CACHE");
    const bool cache_enabled = !cache_env || std::strcmp(cache_env, "0");
    mh_bus_enable_lookup(&m->bus, cache_enabled);
    const char *direct_env = std::getenv("MH_68K_DIRECT");
    m->cpu.direct_enabled = !(direct_env && !strcmp(direct_env, "0"));
    fprintf(m->log, "68k: bus page cache %s\n", cache_enabled ? "enabled" : "disabled");

    fprintf(m->log, "68k: %s (%ld bytes) at %08X, %u MB DRAM at 0\n",
            rom_path, n, rom_base, ram_mb);
    mh_bus_print_map(&m->bus, m->log);
    fprintf(m->log, "68k: reset PC=%08X SP=%08X\n",
            cpu_pc(m), cpu_a(m, 7));
    return m;
}

bool mh_m68k_insert_sram(m68k_machine *m, unsigned slot, const char *path,
                          uint32_t create_size)
{
    static const uint32_t common_base[2] = {0x04000000u, 0x08000000u};
    static const uint32_t attr_base[2]   = {0x24000000u, 0x2C000000u};
    if (!m || slot > 1 || !path || m->card[slot].kind)
        return false;
    if (!mh_card_image_open(&m->card_storage[slot], path, create_size))
        return false;

    mh_pccard_sram_init(&m->card[slot], slot, m->card_storage[slot].data,
                         m->card_storage[slot].size);
    m->card[slot].log = m->log;
    m->card_path[slot] = strdup(path);
    if (!m->card_path[slot]) {
        mh_card_image_close(&m->card_storage[slot]);
        m->card[slot] = {};
        return false;
    }
    m->card_present[slot] = true;

    /* The card's common-memory decode replaces the board's empty/test RAM
     * decode at these chip-select addresses. Keep the host backing stores
     * allocated so existing snapshot formats remain valid, but retire their
     * bus regions before adding the card decode. */
    if (slot == 0 && m->xram_region) m->xram_region->size = 0;
    if (slot == 1 && m->testimg_region) m->testimg_region->size = 0;

    const uint32_t base[2] = {attr_base[slot], common_base[slot]};
    for (unsigned w = 0; w < MH_PCCARD_NWINDOW; w++) {
        mh_pccard_port &p = m->card_port[slot][w];
        p.card = &m->card[slot];
        p.window = (mh_pccard_window)w;
        p.pc_hint = &m->core.pc;
        p.present = &m->card_present[slot];
        if (!mh_bus_add_mmio(&m->bus, w ? "68k-card-common" : "68k-card-attr",
                              base[w], 0x04000000u, &p,
                              mh_pccard_read, mh_pccard_write)) {
            fprintf(m->log, "68k: card slot %u could not be mapped\n", slot + 1);
            mh_card_image_close(&m->card_storage[slot]);
            free(m->card_path[slot]);
            m->card_path[slot] = nullptr;
            m->card[slot] = {};
            m->card_present[slot] = false;
            return false;
        }
    }

    /* The ROM's CardSlot status methods use bits 6/7 for detect, bits 8/9
     * for ready, and two bits per slot at 12..15 for card-battery state.
     * A seated SRAM card is powered, ready, unlocked, and reports a healthy
     * battery. The lock-switch inputs are left low (unlocked); the ROM's
     * write-protect tuple bit remains clear because this card is writable. */
    m->dev21.reg[0xEE / 2] |= (uint16_t)(slot ? 0xC280u : 0x3140u);
    mh_bus_invalidate_lookup(&m->bus);
    fprintf(m->log, "68k: SRAM card slot %u <- %s (%u bytes)\n", slot + 1,
            path, m->card_storage[slot].size);
    return true;
}

bool mh_m68k_eject_card(m68k_machine *m, unsigned slot)
{
    if (!m || slot > 1 || !m->card[slot].kind)
        return false;
    m->card_present[slot] = false;
    m->dev21.reg[0xEE / 2] &= (uint16_t)~(slot ? 0xC280u : 0x3140u);
    mh_card_image_close(&m->card_storage[slot]);
    free(m->card_path[slot]);
    m->card_path[slot] = nullptr;
    m->card[slot] = {};
    /* A removal is an edge event, unlike the level-valued detect/ready
     * inputs. BA bit 12/13 is the ROM's slot-1/slot-2 status event source;
     * the normal IPL6 dispatcher will deliver it if C2 enables it. */
    m->card_event[slot] = true;
    m->dev21.reg[0xBA / 2] |= (uint16_t)(slot ? 0x2000u : 0x1000u);
    m->dev21.irq_dirty = true;
    /* Restore the board decode underneath the now-empty socket. The card
     * MMIO region remains in the table, but its presence gate returns open
     * bus values and the earlier board region wins again. */
    if (slot == 0 && m->xram_region) m->xram_region->size = 0x01000000u;
    if (slot == 1 && m->testimg_region) m->testimg_region->size = 0x01000000u;
    mh_bus_invalidate_lookup(&m->bus);
    fprintf(m->log, "68k: SRAM card ejected from slot %u\n", slot + 1);
    return true;
}

bool mh_m68k_card_present(const m68k_machine *m, unsigned slot)
{
    return m && slot < 2 && m->card[slot].kind && m->card_present[slot];
}

const char *mh_m68k_card_path(const m68k_machine *m, unsigned slot)
{
    return m && slot < 2 && m->card[slot].kind && m->card_present[slot]
         ? m->card_path[slot] : nullptr;
}

void mh_m68k_free(m68k_machine *m)
{
    if (!m) return;
    mh_network_close(m->net_probe_link);
    mh_serial_close(&m->duart.link_a);
    mh_serial_close(&m->duart.link);
    mh_ppp_close(m->duart.ppp);
    for (unsigned slot = 0; slot < 2; slot++)
    {
        mh_card_image_close(&m->card_storage[slot]);
        free(m->card_path[slot]);
    }
    if (m->blocks) m68k_blocks_free(m->blocks);
    free(m->rom);
    free(m->ram);
    free(m->xram);
    delete m;
}

/*
 * DEVIATION. Raise an interrupt line that no modelled device drove.
 *
 * This exists to answer one question and should not become a fixture: is the
 * machine waiting for an interrupt, or stuck for some other reason? Those
 * look identical from outside, and the answer decides whether modelling
 * dev21's interrupt behaviour is the next piece of work or a distraction.
 *
 * What is known without it: the OS has built a real vector table -- VBR is 0
 * and the handlers are installed -- and it gives levels 5 and 6 handlers of
 * their own while sharing one catch-all for 1 to 4 and 7. Both of those
 * handlers service dev21 at 0x21000000, IPL5 around +0xB0 and IPL6 reading
 * +0xB8 first. So the hardware that interrupts this machine is dev21, and an
 * interrupt asserted from nowhere is a test, not a model of it.
 */
void mh_m68k_force_irq(m68k_machine *m, unsigned level, uint64_t at)
{
    m->cpu.force_irq_level = level;
    m->cpu.force_irq_at = at;
}

/*
 * DEVIATION. Make one register of an unnamed device read back a value.
 *
 * Both probes answer zero to everything, which is the honest default -- we do
 * not know what is behind them -- but a zero is also indistinguishable from a
 * device reporting "nothing here", and the boot code branches on exactly that
 * distinction. Seeding a register asks whether a gate matters before any
 * effort goes into modelling what is behind it. Nothing here is a model: the
 * value comes from the command line, and the run says so in its output.
 */
bool mh_m68k_probe_preset(m68k_machine *m, const char *dev, uint32_t off,
                           uint16_t val)
{
    Pic2000Registers *p = !strcmp(dev, "dev21") ? &m->dev21
             : !strcmp(dev, "dev0c") ? &m->dev0c : nullptr;
    if (!p || off >= 0x1000 || (off & 1)) return false;
    p->reg[off / 2] = val;
    p->irq_dirty = p->audio_dirty = true;
    p->preset = true;
    fprintf(m->log, "68k: DEVIATION: %s +%03X preset to %04X; no modelled "
            "device produced it\n", dev, off & ~1u, val);
    return true;
}

/*
 * How fast the counter runs against our instruction count.
 *
 * There is no cycle timing here at all -- no instruction costs what it costs
 * on the part -- so an average is the only thing available to turn retired
 * instructions into elapsed time. That makes the counter's rate against the
 * emulated CPU approximate. The counter frequency (128 Hz) follows the
 * ROM's millisecond conversion.
 * The system clock (16.777216 MHz) follows SYNCR=CF83 assuming the
 * documented 32.768 kHz reference; board clock wiring remains unverified.
 *
 * CPI remains an approximation, exposed through --cpi until instruction
 * cycle accounting replaces it. Prior work-queue measurements used the
 * erroneous 128 kHz counter and do not establish a physical CPI value.
 */
/*
 * DEVIATION. Give the converter a reading.
 *
 * The protocol at +0xE4 is the ROM's; the number is not. Nothing here senses
 * anything, so a value supplied on the command line is a stated input and the
 * run says so. Used to find out what the guest does with a reading, which is
 * the only way to tell "the sensor is unmodelled" from "the sensor is
 * unmodelled AND that is why this subsystem stopped".
 */
void mh_m68k_set_adc(m68k_machine *m, int value)
{
    m->battery_override = true;
    m->dev21.adc_value = value;
    m->dev21.adc_chan[2] = value;
    m->dev21.adc_chan[4] = value;
}

static void set_adc_pair(m68k_machine *m, unsigned mux, unsigned chan,
                           int value, bool report)
{
    mux &= 0x7F;
    chan &= 0x3FF;
    for (unsigned k = 0; k < m->dev21.adc_pairs; k++) {
        auto &e = m->dev21.adc_pair[k];
        if (e.mux == (int)mux && e.chan == (int)chan) {
            e.value = value;
            if (report) fprintf(m->log, "68k: DEVIATION: dev21 +0E4 returns %d for mux "
                    "%02X channel %u; no modelled sensor produced it\n",
                    value, mux, chan);
            return;
        }
    }
    if (m->dev21.adc_pairs >= 16) return;
    auto &e = m->dev21.adc_pair[m->dev21.adc_pairs++];
    e.mux = (int)mux; e.chan = (int)chan; e.value = value;
    if (report) fprintf(m->log, "68k: DEVIATION: dev21 +0E4 returns %d for mux %02X "
            "channel %u; no modelled sensor produced it\n", value,
            mux & 0x7F, chan & 0x3FF);
}

bool mh_m68k_load_battery_ram(m68k_machine *m, const uint8_t *data, uint32_t size)
{
    if (!size) return true;
    if (!m->dev21.econoram || size != 36 || std::memcmp(data, "ECR1", 4))
        return false;
    std::memcpy(m->battery_ram.bytes, data + 4, 32);
    return true;
}

bool mh_m68k_save_battery_ram(m68k_machine *m, uint8_t **data, uint32_t *size)
{
    if (!m->dev21.econoram) return true;
    auto *out = static_cast<uint8_t *>(std::malloc(36));
    if (!out) return false;
    std::memcpy(out, "ECR1", 4);
    std::memcpy(out + 4, m->battery_ram.bytes, 32);
    std::free(*data); *data = out; *size = 36;
    return true;
}

bool mh_m68k_host_battery(m68k_machine *m, int percent)
{
    if (m->envoy || m->battery_override || percent < 0 || percent > 100)
        return false;
    /* PIC CalculateLevel at 0E07A2AA uses EmptyThreshold/FullThreshold.
     * The main-battery threshold sequence at ROM 26B42A is
     * 805,756,689,679,660. Host percentage maps linearly from 689 to 805;
     * this is a presentation mapping, not a measured discharge curve.
     * HIX 0E052638 observes EmptyThreshold=745 and FullThreshold=808
     * for its default main battery. Envoy retains a separate pack model. */
    const int empty = m->hix ? 745 : 689;
    const int full = m->hix ? 808 : 805;
    m->dev21.adc_chan[2] = empty + ((full - empty) * percent + 50) / 100;
    return true;
}

void mh_m68k_set_adc_pair(m68k_machine *m, unsigned mux, unsigned chan, int value)
{
    if ((chan & 0x3ff) == 2) m->battery_override = true;
    set_adc_pair(m, mux, chan, value, true);
}

void mh_m68k_set_adc_chan(m68k_machine *m, unsigned chan, int value)
{
    if ((chan & 0x3ff) == 2) m->battery_override = true;
    m->dev21.adc_chan[chan & 0x3FF] = value;
    fprintf(m->log, "68k: DEVIATION: dev21 +0E4 returns %d for channel %u; "
            "no modelled sensor produced it\n", value, chan & 0x3FF);
}

static uint32_t core_read(void *ctx, uint32_t addr, unsigned size);
static void core_write(void *ctx, uint32_t addr, unsigned size, uint32_t value);

bool mh_m68k_set_engine(m68k_machine *m, const char *name)
{
    /* Read for every engine, so that a trace taken one way can be
     * compared line for line against a trace taken the other. */
    if (const char *t = std::getenv("MH_68K_CORE_TRACE"))
        m->core_trace = strtoull(t, nullptr, 0);
    if (!name || !strcmp(name, "interpreter")) {
        /* The single-step interpreter is the same project-owned core as the
         * block engine, with decoding and dispatch performed for each step. */
        m->core_enabled = false;
        return true;
    }
    if (strcmp(name, "blocks") && strcmp(name, "jit") && strcmp(name, "auto"))
        return false;
    /* `auto` uses cached/native blocks where available. The core maintains
     * the per-instruction cycle count required by the Envoy as well. */
    if (!m->blocks) {
        m->blocks = m68k_blocks_create();
        if (!m->blocks) {
            fprintf(m->log, "68k: no memory for the block engine\n");
            return false;
        }
    }
    m->core_enabled = true;
    fprintf(m->log, "68k: block engine selected; native code is %s on this "
            "host\n", m68k_emit_supported() ? "available" : "unavailable");
    return true;
}

void mh_m68k_engine_report(const m68k_machine *m)
{
    if (!m->blocks || !m->core_calls) return;
    m68k_block_stats st;
    m68k_blocks_report(m->blocks, &st);
    fprintf(m->log, "68k: block engine ran %llu instructions over %llu "
            "block-engine calls (%.1f instructions each, %llu empty); %s\n",
            (unsigned long long)m->core_ran,
            (unsigned long long)m->core_calls,
            m->core_calls ? (double)m->core_ran / (double)m->core_calls : 0.0,
            (unsigned long long)m->core_empty,
            m->core_enabled ? "still enabled" : "disabled during the run");
    fprintf(m->log, "68k:   %llu interrupts raised, %llu taken by this "
            "engine, %llu refused by the mask; %llu stretches too short for "
            "block execution, %llu of those with an interrupt waiting\n",
            (unsigned long long)m->irq_raises,
            (unsigned long long)m->core_interrupts,
            (unsigned long long)m->core_irq_refused,
            (unsigned long long)m->core_skipped_short,
            (unsigned long long)m->core_skipped_irq);
    /*
     * Behind a switch, not because it is expensive but because these
     * counts legitimately differ between a run that took the idle fast
     * path and one that did not, and tests/host/test_m68k_cli requires
     * those two runs to say the same things on stderr.
     */
    if (!std::getenv("MH_68K_ENGINE_STATS")) return;
    fprintf(m->log, "68k:   %llu fetches from an odd address, which this "
            "part faults on\n", (unsigned long long)m->core_odd);
    fprintf(m->log, "68k:   quiet path refused: %llu stopped, %llu hix "
            "checksum, %llu power, %llu off, %llu diagnostics\n",
            (unsigned long long)m->noquiet_stopped,
            (unsigned long long)m->noquiet_hix,
            (unsigned long long)m->noquiet_power,
            (unsigned long long)m->noquiet_off,
            (unsigned long long)m->noquiet_diag);
    if (m->core.exception_count)
        fprintf(m->log, "68k:   it raised %llu exceptions\n",
                (unsigned long long)m->core.exception_count);
    fprintf(m->log, "68k:   %llu blocks built for %llu entries, %.1f "
            "instructions each, %.1f%% reached from the block before, "
            "%llu stale, %llu evicted, %llu sent to the interpreter\n",
            (unsigned long long)st.built, (unsigned long long)st.entered,
            st.entered ? (double)st.instructions / (double)st.entered : 0.0,
            st.entered ? 100.0 * (double)st.linked / (double)st.entered : 0.0,
            (unsigned long long)st.stale, (unsigned long long)st.evicted,
            (unsigned long long)st.refused);
    m68k_refusal worst[6];
    unsigned kinds = m68k_blocks_refusals(&st, worst, 6);
    if (kinds) {
        fprintf(m->log, "68k:   interpreter fallback for:");
        for (unsigned i = 0; i < kinds; i++)
            fprintf(m->log, " %s x%llu", worst[i].name,
                    (unsigned long long)worst[i].count);
        fprintf(m->log, "\n");
    }
    fprintf(m->log, "68k:   %llu blocks emitted, %llu linked to a successor "
            "(%llu relinked), %llu instructions run by emitted code; of the "
            "instructions in those blocks %.1f%% are translated rather than "
            "called (--insn-heat weights that by what ran); %llu arena "
            "resets\n",
            (unsigned long long)st.compiled,
            (unsigned long long)st.chained,
            (unsigned long long)st.relinked,
            (unsigned long long)st.ran_native,
            st.emitted_insns ? 100.0 * (double)st.translated /
                               (double)st.emitted_insns : 0.0,
            (unsigned long long)st.arena_resets);
}

void mh_m68k_set_cpi(m68k_machine *m, unsigned cpi)
{
    m->audio_at = 0;
    if (!cpi) cpi = 1;
    uint64_t num = (uint64_t)PIC2000_COUNTER_HZ * cpi;
    uint64_t den = PIC2000_CPU_HZ;
    uint64_t a = num, b = den;
    while (b) { uint64_t t = a % b; a = b; b = t; }
    num /= a;
    den /= a;
    m->dev21.counter_num = num;
    m->dev21.counter_den = den;
    m->cpi = cpi;
}

/*
 * The timer half of dev21: a compare, a pending bit and an interrupt line.
 *
 * Which bit is not a guess. The service routine at 0x0E0889FE sets the group
 * up -- handler table at 0x0510, pending at 0x210000B0, enable four bytes
 * later, and d4 starting at 30 -- and the loop at 0x0E0889C4 tests bit d4
 * while walking the table eight bytes at a time and counting d4 down. So the
 * pending bit of table entry k is 30 - k. Entry 17 is 0x0E08CDDC, which takes
 * the time, walks the expired timers and reprograms the compare, so the timer
 * is pending bit 13.
 *
 * The cross-check is that the enable mask the OS ends up writing, 0x010C3002,
 * is bits 1, 12, 13, 18, 19 and 24, and those map to entries 29, 18, 17, 12,
 * 11 and 6 -- every one of which has a handler, and no entry without one is
 * enabled. A wrong mapping would not line up like that.
 *
 * The handler acknowledges by writing the remaining mask back over the
 * pending register (0x0E0889F6), which is why a plain store is the right
 * model for it and why the line has to be re-evaluated after every write.
 */
#define DEV21_PENDING   0xB0u
#define DEV21_ENABLE    0xB4u
#define DEV21_TIMER_BIT 13u
#define DEV21_CONV_BIT   6u    /* converter complete; see adc_done_bits */
#define DEV21_PEN_BIT    1u    /* pen down; handler 0x0E08B75C */
#define DEV21_TX_BIT    10u    /* +0x90 transmitter ready */
#define DEV21_PEN_TIMER_BIT 14u /* +0xDC/+0xDE pen sampling countdown */
#define DEV21_CARD1_BIT 12u    /* +BA: slot-1 status event */
#define DEV21_CARD2_BIT 13u    /* +BA: slot-2 status event */
/* Only sources this emulator actually produces may drive the line. The rest
 * of the pending register holds whatever the guest last wrote. */
#define DEV21_DRIVEN ((7u << 18) | (1u << DEV21_TIMER_BIT) | (1u << DEV21_CONV_BIT) | \
                      (1u << DEV21_PEN_BIT) | (1u << DEV21_TX_BIT) | \
                      (1u << DEV21_PEN_TIMER_BIT))

static void timer_arm(m68k_machine *m)
{
    const uint32_t now = m->dev21.counter_now();
    const uint32_t cmp = m->dev21.reg32(m->dev21.compare_off);
    const int32_t delta = (int32_t)(cmp - now);
    m->timer_armed = true;
    /* A deadline already past is due now, which is what the hardware would
     * do with a compare it has already gone by. */
    m->timer_at = delta <= 0 ? m->cpu.insns
        : m->cpu.insns + (uint64_t)delta * m->dev21.counter_den /
                         m->dev21.counter_num;
}

static void dev21_irq(m68k_machine *m)
{
    if (!m->dev21.irq_dirty) return;
    m->dev21.irq_dirty = false;
    const uint32_t live = m->dev21.reg32(DEV21_PENDING) &
                          m->dev21.reg32(DEV21_ENABLE) &
                          (DEV21_DRIVEN |
                           (PicMagicBus::supported(m) ? 0x300u : 0) |
                           (m->magicbus.connected ? 0x800u : 0));
    /* Only sources we model drive the line. The other pending bits hold
     * whatever the OS last wrote and nothing in here produces them, so
     * asserting on those would be inventing interrupts. */
    /* IPL6's 0E088E80 dispatcher walks B8/BA masked by C0/C2.
     * Slot 2's registered status handlers acknowledge BA bits 12/13. */
    const uint32_t card_events = (m->card_event[0] ? (1u << DEV21_CARD1_BIT) : 0) |
                                 (m->card_event[1] ? (1u << DEV21_CARD2_BIT) : 0);
    const uint32_t power = m->dev21.reg32(0xB8) &
                           m->dev21.reg32(0xC0) &
                           (0x000C0000u | (m->envoy ? 0x00300000u : 0) |
                            (m->net_probe_enabled ? 0x00003000u : 0) |
                            card_events);
    const unsigned accessory = m->envoy ?
        (m->dev21.reg[0xbc/2] & m->dev21.reg[0xc4/2] & 0x30u) : 0;
    m->dev21_irq_now = power ? 6u : (live || accessory) ? 5u : 0u;
    unsigned dma = 0;
    if (m->magicbus.connected) {
        const unsigned csr=m->sim.reg[0x7aa/2]>>8;
        const unsigned ccr=m->sim.reg[0x7a8/2];
        if (((csr&0x40) && (ccr&0x4000)) ||
            ((csr&0x38) && (ccr&0x2000)))
            dma=(m->sim.reg[0x7a4/2]>>8)&7;
    }
    const unsigned want = std::max({m->dev21_irq_now, m->serial_irq_now, dma});
    m->cpu.irq_vector = dma && dma==want ? m->sim.reg[0x7a4/2]&255 :
        m->serial_irq_now && m->serial_irq_now==want ? m->duart.ivr :
        (want ? 24+want : 0);
    if (want != m->irq_now) {
        if (want) m->irq_raises++;
        m->irq_now = want;
    }
}

static void duart_tick(m68k_machine *m)
{
    if (!m->duart.active()) return;
    m->duart.tick();
    m->serial_irq_now = m->duart.irq() ? m->duart.ilr : 0;
    m->dev21.irq_dirty = true;
    dev21_irq(m);
}

/*
 * DEVIATION. Assert pen-down, which no modelled digitizer produced.
 *
 * The pen is IPL5 group 1 bit 1: its handler at 0x0E08B75C clears bits 0 and
 * 1, masks them, and hands off to a sampler that configures the converter and
 * enables the conversion-complete interrupt. Asserting the bit is how to find
 * out what the driver asks for next -- which mux values, in what order --
 * without having to guess the digitizer's protocol first.
 *
 * With --trace-conv every conversion the driver starts is printed with the
 * control and mux words in force, which is the measurement this exists for.
 */
void mh_m68k_touch(m68k_machine *m, uint64_t at, uint64_t len, bool trace)
{
    /* A measured, internally consistent point near the middle of the panel.
     * Explicit --adc-chan pairs are parsed after --touch and replace these. */
    mh_m68k_set_adc_pair(m, 0x35, 64, 500 << 6);
    mh_m68k_set_adc_pair(m, 0x55,  0, 300 << 6);
    mh_m68k_set_adc_pair(m, 0x55, 64, 310 << 6);
    mh_m68k_set_adc_pair(m, 0x66,  0, 320 << 6);
    mh_m68k_set_adc_pair(m, 0x69,  0, 300 << 6);
    mh_m68k_set_adc_pair(m, 0x69, 64, 310 << 6);
    m->touches.push_back({at, len ? at + len : 0, 300, 500, false});
    m->dev21.trace_conv = trace;
    fprintf(m->log, "68k: DEVIATION: asserting pen-down (IPL5 bit 1) at +%llu"
            "%s; no modelled digitizer drove it\n", (unsigned long long)at,
            len ? ", releasing after the window" : " and never releasing");
}

bool mh_m68k_tap(m68k_machine *m, uint64_t at, uint64_t len,
                  unsigned x, unsigned y, bool trace)
{
    if (x >= 480 || y >= 320 || !len) return false;
    m->touches.push_back({at, at + len, x, y, true});
    m->dev21.trace_conv = trace;
    fprintf(m->log, "68k: DEVIATION: scheduling screen tap (%u,%u) at +%llu "
            "for %llu instructions\n", x, y, (unsigned long long)at,
            (unsigned long long)len);
    return true;
}

static void touch_point(m68k_machine *m, int x, int y)
{
    set_adc_pair(m, 0x35, 64, y << 6, false);
    set_adc_pair(m, 0x55,  0, x << 6, false);
    set_adc_pair(m, 0x55, 64, (x + 10) << 6, false);
    set_adc_pair(m, 0x66,  0, (x + 20) << 6, false);
    set_adc_pair(m, 0x69,  0, x << 6, false);
    set_adc_pair(m, 0x69, 64, (x + 10) << 6, false);
}

static unsigned ram_be16(const m68k_machine *m, unsigned off)
{
    return (unsigned)m->ram[off] << 8 | m->ram[off + 1];
}

static void touch_screen_point(m68k_machine *m, unsigned x, unsigned y)
{
    /* The ROM's transform at 0x0E08BDA8 uses four signed longs, whose low
     * words hold the raw panel endpoints. Calibration replaces them, so a
     * later screen-coordinate tap must use the current values rather than
     * the factory defaults captured at process startup. */
    unsigned x0 = ram_be16(m, 0xA96), y0 = ram_be16(m, 0xA9A);
    unsigned x1 = ram_be16(m, 0xA9E), y1 = ram_be16(m, 0xAA2);
    /* Calibration uses these globals as scratch storage between targets.
     * Values outside the ADC's ten-bit range are not endpoint pairs yet. */
    if (x1 <= x0 || x0 > 1023 || x1 > 1023) { x0 = 138; x1 = 965; }
    if (y1 <= y0 || y0 > 1023 || y1 > 1023) { y0 = 175; y1 = 895; }
    const int raw_x = (int)(x0 + (uint64_t)x * (x1 - x0) / 479);
    const int raw_y = (int)(y0 + (uint64_t)y * (y1 - y0) / 319);
    touch_point(m, raw_x, raw_y);
    m->dev21.pen_second_690 = raw_y << 6;
    if (!m->touch_down || getenv("MH_TOUCH_TRACE"))
        fprintf(m->log, "68k: DEVIATION: screen tap (%u,%u) uses current raw "
            "mapping (%d,%d)\n", x, y, raw_x, raw_y);
}

static void touch_press_now(m68k_machine *m, unsigned x, unsigned y)
{
    touch_screen_point(m, x, y);
    if (m->touch_down)
        return;
    m->touch_down = true;
    /* D1 bit 1 is HardwareOptionKey, not pen contact. Pen contact is
     * conveyed by the digitizer edge and pressure conversions. */
    m->dev21.set_bit32(DEV21_PENDING, DEV21_PEN_BIT);
}

static void touch_release_now(m68k_machine *m)
{
    if (!m->touch_down)
        return;
    m->touch_down = false;
    /* The digitizer interrupt is an edge notification, not a
     * pen-down-only source.  The tracking UI has already consumed
     * the live points when the level falls, but it needs this edge
     * to run the release action.  Without it buttons visibly press
     * and unpress while their command is never sent. */
    m->dev21.set_bit32(DEV21_PENDING, DEV21_PEN_BIT);
    /* Four pressure samples at or below 15 mean pen-up to the ROM
     * (0x0E08BBBA..0x0E08BBDA). */
    m->dev21.adc_chan[0] = 0;
    m->dev21.adc_chan[64] = 0;
    for (unsigned k = 0; k < m->dev21.adc_pairs; k++)
        if (m->dev21.adc_pair[k].mux == 0x35 ||
            m->dev21.adc_pair[k].mux == 0x55 ||
            m->dev21.adc_pair[k].mux == 0x66 ||
            m->dev21.adc_pair[k].mux == 0x69)
            m->dev21.adc_pair[k].value = 0;
}

uint64_t mh_m68k_elapsed_ns(const m68k_machine *m)
{
    // insns is the legacy execution-slot counter, including LPSTOP waits.
    return mh_time_ns(m->cpu.insns * m->cpi, PIC2000_CPU_HZ);
}

uint64_t mh_m68k_insns(const m68k_machine *m)
{
    return m->cpu.insns;
}

void mh_m68k_lcd(const m68k_machine *m, uint8_t *pixels)
{
    if (!pixels)
        return;
    if (m->dev21.power_off || !(m->dev21.reg[0x40 / 2] & 0x10)) {
        // Envoy 0046A3FE/0046A412 set/clear LCD enable bit 4. Retained
        // framebuffer bytes are not visible before the guest enables it.
        /* Match the frontend's black powered-off display convention. */
        memset(pixels, 3, PIC2000_SCREEN_W * PIC2000_SCREEN_H);
        return;
    }
    /* LCD base is in four-byte units: PIC writes 0A00 for RAM 2800;
     * HIX 0E05D8D4 clears RAM 8000 and writes 2000 at dev21+48.
     * Keep the pre-configuration default for diagnostic fixtures. */
    const uint32_t lcd_base = m->dev21.reg[0x48 / 2]
        ? (uint32_t)m->dev21.reg[0x48 / 2] << 2 : PIC2000_FB_ADDR;
    const uint8_t *src = m->ram + lcd_base;
    for (unsigned i = 0; i < PIC2000_FB_SIZE; i++) {
        const uint8_t value = src[i];
        pixels[i * 4 + 0] = (uint8_t)((value >> 6) & 3);
        pixels[i * 4 + 1] = (uint8_t)((value >> 4) & 3);
        pixels[i * 4 + 2] = (uint8_t)((value >> 2) & 3);
        pixels[i * 4 + 3] = (uint8_t)(value & 3);
    }
}

bool mh_m68k_set_pen(m68k_machine *m, bool down, unsigned x, unsigned y)
{
    if (down && (x >= PIC2000_SCREEN_W || y >= PIC2000_SCREEN_H))
        return false;
    if (down)
        touch_press_now(m, x, y);
    else
        touch_release_now(m);
    return true;
}

bool mh_m68k_host_adapter(m68k_machine *m, bool attached)
{
    if (!m->envoy || m->adapter_override) return false;
    // ACAdapterAttached at 00466816 samples D1 bit 6. IRQ6 fallback
    // entries for B8 long bits 21/20 at 0046C5EA/0046C5FE set
    // deferred flags 2/3 and schedule 00466324, which reads
    // the physical level and delivers ACAdapterStatusChanged.
    if (m->dev21.adapter_attached == attached) return true;
    m->dev21.adapter_attached = attached;
    m->dev21.set_bit32(0xB8, attached ? 21 : 20);
    dev21_irq(m);
    fprintf(m->log, "68k: Envoy AC adapter %s\n", attached ? "attached" : "removed");
    return true;
}

bool mh_m68k_set_adapter(m68k_machine *m, bool attached)
{
    const bool previous = m->adapter_override;
    m->adapter_override = false;
    const bool ok = mh_m68k_host_adapter(m, attached);
    m->adapter_override = ok || previous;
    return ok;
}

void mh_m68k_power_button(m68k_machine *m, bool down)
{
    if (m->hix) {
        // HIX 0E054D4E requests shutdown when the input becomes false;
        // 0E054CCC arms its falling edge. Model a latched ON/OFF input,
        // toggled by each host key press, rather than a momentary button.
        const bool pressed = down && !m->power_key_down;
        m->power_key_down = down;
        if (!pressed) return;
        down = m->dev21.power_off || !m->power_down;
    }
    if (m->power_down == down && !(down && m->dev21.power_off)) return;
    m->power_down = down;
    if (down && m->dev21.power_off) {
        m->dev21.power_off = false;
        m->dev21.audio_dirty = true;
        m->dev21.reg[0x40 / 2] &= (uint16_t)~0x10u;
        m->dev21.reg[0xB8 / 2] = 0;
        m->irq_now = 0;
        m->dev21_irq_now = m->serial_irq_now = 0;
        m->cpu.irq_vector = 0;
        /* Reset fetches its vectors through CS0's boot overlay. The previous
         * session retired it when it programmed CS0, so restore that decode
         * before resetting the CPU, without touching the RAM underneath. */
        static_cast<mh_region *>(m->sim.board)->size = 0x01000000u;
        mh_bus_invalidate_lookup(&m->bus);
        m->sim.overlay_off = false;
        /* Whichever core reads the reset vectors holds the registers
          * afterwards; the engine picks them up again at the first
          * stretch either way. */
        m->cpu.pc_src = &m->core.pc;
        m68k_reset(&m->core);
        /* It will check its ROM again on the way up. */
        m->hix_checksum_pending = m->hix_checksum_intercept;
        mh_m68k_set_stop_at(m, m->hix_checksum_pending ? mh_m68k_checksum_pc(m) : 0);
        fprintf(m->log, "68k: power on; CPU reset with retained RAM\n");
    }
    /* PIC ROM 0E07FC2E reads the active-high button at dev21+D1 bit 2.
     * IPL6's fallback table dispatches B8 word bits 3/2 to 0E088CB8/CCC;
     * 0E07FAF8 delivers these as button down/up to object 8705A019.
     * 0E07FBC2 acknowledges both edges and re-enables them. */
    if (down) m->dev21.reg[0xD0 / 2] |= 4u;
    else m->dev21.reg[0xD0 / 2] &= (uint16_t)~4u;
    m->dev21.set_bit32(0xB8, down ? 19 : 18);
    dev21_irq(m);
}

void mh_m68k_attach_pclink(m68k_machine *m, struct mh_pclink *link)
{
    m->duart.attach(link);
}

bool mh_m68k_open_serial_a(m68k_machine *m)
{
    if (!m || !m->duart.open_a()) return false;
    fprintf(m->log, "68k: experimental DUART A at %s; modem power/carrier and PPP backend unimplemented\n",
            m->duart.link_a.path);
    return true;
}

bool mh_m68k_open_ppp(m68k_machine *m, const char *pcap)
{
    if (!m || m->duart.ppp || m->duart.link_a.fd >= 0) return false;
    mh_ppp *endpoint = mh_ppp_open(pcap);
    if (!endpoint) return false;
    m->duart.attach_ppp(endpoint);
    fprintf(m->log, "68k: experimental channel-A virtual ISP attached; PPP is available after Hayes dialing\n");
    return true;
}

/* This address is an emulator-only probe aperture on CS3. The ROM's
 * CardSlotAstro objects declare separate slot windows (slot 2 I/O is
 * 0x30000000). Slot-2 windows and a diagnostic insertion event are modeled,
 * but OS card-server binding is still unresolved. The guest probe uses byte
 * I/O, avoiding an invented word lane. */
static constexpr uint32_t NET_PROBE_BASE = 0x0C010000u;
static constexpr uint32_t NET_SLOT2_ATTRIBUTE_BASE = 0x2C000000u;
static constexpr uint32_t NET_SLOT2_IO_BASE = 0x30000000u;

static uint32_t net_slot2_attribute_read(void *ctx, uint32_t off, unsigned size)
{
    auto *m = (m68k_machine *)ctx;
    uint8_t value = 0;
    bool decoded = size == 1 &&
        mh_ne2000_magic_attribute_byte(off, m->net_slot2_config, &value);
    if (std::getenv("MH_68K_NET_TRACE") && m->net_slot2_attr_logged++ < 128)
        fprintf(m->log, "68k: slot-2 attribute R%u +%08X = %02X (%s, pc=%08X)\n",
                size * 8, off, decoded ? value : 0xff,
                decoded ? "CIS" : "open", m->cpu.at_pc());
    if (decoded)
        return value;
    return size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1u;
}

static void net_slot2_attribute_write(void *ctx, uint32_t off,
                                      unsigned size, uint32_t value)
{
    auto *m = (m68k_machine *)ctx;
    if (off != 0x3f8 || size != 1) return;
    m->net_slot2_config = (uint8_t)value;
    if (value & 0x80) (void)mh_ne2000_read(&m->net_probe_nic, 0x1f, 1);
    if (std::getenv("MH_68K_NET_TRACE"))
        fprintf(m->log, "68k: slot-2 NE2000 COR <- %02X\n", value & 255);
}

static uint32_t net_probe_read(void *ctx, uint32_t off, unsigned size)
{
    auto *m = (m68k_machine *)ctx;
    if (size != 1) return 0xFFFFFFFFu;
    uint16_t addr = m->net_probe_nic.rsar;
    uint32_t value = mh_ne2000_read(&m->net_probe_nic, off, size);
    if (off == 0x10 && addr >= 0x4800 && addr < 0x4838 &&
        std::getenv("MH_68K_NET_TRACE"))
        fprintf(m->log, "68k: NIC DMA[%04X] -> %02X\n", addr, value & 255);
    return value;
}

static void net_probe_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    auto *m = (m68k_machine *)ctx;
    if (off == 0x10 && size == 1 && m->net_probe_logged < 80 &&
        std::getenv("MH_68K_NET_TRACE")) {
        fprintf(m->log, "68k: NIC DMA[%04X] <- %02X (CR=%02X count=%u)\n",
                m->net_probe_nic.rsar, val & 255, m->net_probe_nic.cr,
                m->net_probe_nic.rbcr);
        m->net_probe_logged++;
    }
    if (size == 1) mh_ne2000_write(&m->net_probe_nic, off, size, val);
}

static void net_probe_receive(void *ctx, const uint8_t *frame, size_t len)
{
    auto *m = (m68k_machine *)ctx;
    unsigned at = m->net_probe_nic.curr;
    bool accepted = mh_ne2000_receive(&m->net_probe_nic, frame, len);
    if (std::getenv("MH_68K_NET_TRACE"))
        fprintf(m->log, "68k: NIC RX len=%zu accepted=%u page=%02X next=%02X BNRY=%02X ISR=%02X\n",
                len, accepted, at, m->net_probe_nic.curr,
                m->net_probe_nic.bnry, m->net_probe_nic.isr);
}

bool mh_m68k_open_ne2000_probe(m68k_machine *m, const char *pcap)
{
    if (!m || m->envoy || m->hix || m->net_probe_enabled) return false;
    unsigned regions_before = m->bus.nregion;
    static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x68, 0x00, 0x01};
    mh_ne2000_init(&m->net_probe_nic, mac);
    m->net_slot2_config = 0;
    m->net_probe_link = mh_network_open(net_probe_receive, m, pcap);
    if (!m->net_probe_link) return false;
    /* Experimental card-space mapping. The dev21 slot-2 presence input is
     * asserted while attached; common memory and IRQ remain unmodeled. The
     * original diagnostic aperture stays available. */
    bool mapped = mh_bus_add_mmio(&m->bus, "68k-ne2000-probe", NET_PROBE_BASE,
                                   0x20u, m, net_probe_read, net_probe_write) &&
                  mh_bus_add_mmio(&m->bus, "68k-ne2000-slot2-attribute",
                                   NET_SLOT2_ATTRIBUTE_BASE, 0x04000000u, m,
                                   net_slot2_attribute_read,
                                   net_slot2_attribute_write) &&
                  mh_bus_add_mmio(&m->bus, "68k-ne2000-slot2-io",
                                   NET_SLOT2_IO_BASE, 0x20u, m,
                                   net_probe_read, net_probe_write);
    if (!mapped) {
        m->bus.nregion = regions_before;
        mh_bus_invalidate_lookup(&m->bus);
        mh_network_close(m->net_probe_link);
        m->net_probe_link = nullptr;
        return false;
    }
    m->net_probe_nic.send = mh_network_send;
    m->net_probe_nic.send_opaque = m->net_probe_link;
    m->net_probe_enabled = true;
    /* An insertion edge is deliberately not synthesized yet: BA=2000
     * reaches the ROM's slot-2 handler but the current CIS then raises its
     * incompatible-card dialog, blocking the working native probe. The
     * edge remains reproducible with --probe-preset dev21:BA=2000. */
    m->dev21.irq_dirty = true;
    dev21_irq(m);
    m->idle_fast_enabled = false;
    m->quiet_fast_enabled = false;
    fprintf(m->log, "68k: experimental NE2000 byte I/O at %08X; slot-2 attribute %08X and I/O %08X, dev21+EE slot-2 present, MAC 02:00:00:68:00:01\n",
            NET_PROBE_BASE, NET_SLOT2_ATTRIBUTE_BASE, NET_SLOT2_IO_BASE);
    return true;
}

/*
 * The hardware option key, which these machines have as a level the OS
 * samples rather than an edge it is told about. Magic Cap reads it at
 * dev21+D1 bit 1 -- the byte the power button's own input also lives in,
 * two bits along.
 *
 * Sampled, not edge-driven, means the state has to be in place before the
 * touch it modifies. That is why the interface offers it as something held
 * rather than something pressed: on a touchscreen there is no way to hold a
 * modifier and tap at the same time with one finger.
 */
void mh_m68k_set_option(m68k_machine *m, bool down)
{
    if (down) m->dev21.reg[0xD0 / 2] |= 2u;
    else m->dev21.reg[0xD0 / 2] &= (uint16_t)~2u;
}

bool mh_m68k_option_held(const m68k_machine *m)
{
    return (m->dev21.reg[0xD0 / 2] & 2u) != 0;
}

void mh_m68k_start(m68k_machine *m)
{
    /* Launching a device is a press of its ON button, including on a warm
     * boot. A CPU reset without this input can legitimately return to off. */
    mh_m68k_power_button(m, true);
    if (m->hix) {
        mh_m68k_power_button(m, false); // release host key; switch stays ON
        fprintf(m->log, "68k: HIX-300 power switch ON (ROM-inferred latching input)\n");
    } else m->boot_release_at = m->cpu.insns + 1000000;
}

bool mh_m68k_powered_off(const m68k_machine *m)
{
    return m->dev21.power_off;
}

bool mh_m68k_power_off(m68k_machine *m)
{
    if (m->dev21.power_off) return true;
    mh_m68k_set_pen(m, false, 0, 0);
    mh_m68k_power_button(m, false);
    mh_m68k_power_button(m, true);
    mh_m68k_run(m, 1000000);
    mh_m68k_power_button(m, false);
    for (unsigned i = 0; i < 160 && !m->dev21.power_off && !m->cpu.stopped; i++)
        mh_m68k_run(m, 1000000);
    return m->dev21.power_off;
}

void mh_m68k_schedule_power(m68k_machine *m, uint64_t at, uint64_t hold)
{
    m->power_scheduled = true;
    m->power_at = at;
    m->power_release_at = at + hold;
}

static inline void audio_tick_if_due(m68k_machine *m)
{
    if (MH_LIKELY(m->cpu.insns < m->audio_at && !m->dev21.audio_dirty)) return;
    m->dev21.audio_dirty = false;
    m->audio.tick(m->cpu.insns * m->cpi, PIC2000_CPU_HZ, m->dev21, m->bus);
    // Exact next output-sample boundary. Register changes still tick on the
    // instruction that writes them, preserving start/stop and DMA timing.
    const uint64_t cycles = (PIC2000_CPU_HZ - m->audio.phase +
        Pic2000Audio::output_rate - 1) / Pic2000Audio::output_rate;
    m->audio_at = m->cpu.insns + (cycles + m->cpi - 1) / m->cpi;
}

static uint64_t next_idle_deadline(const m68k_machine *m)
{
    const uint64_t now = m->cpu.insns;
    uint64_t next = UINT64_MAX;
    auto consider = [&](uint64_t at) {
        if (at > now && at < next) next = at;
    };
    consider(m->audio_at);
    if (m->timer_armed) consider(m->timer_at);
    if (m->pen_timer_armed) consider(m->pen_timer_at);
    if (m->magicbus.connected && m->dev21.tx_busy) consider(m->dev21.tx_done_at);
    if (m->power_scheduled) {
        consider(m->power_at);
        consider(m->power_release_at);
    }
    if (m->boot_release_at) consider(m->boot_release_at);
    if (m->touch_next < m->touches.size()) {
        consider(m->touches[m->touch_next].at);
        consider(m->touches[m->touch_next].release_at);
    }
    return next;
}

/*
 * Whether the asserted interrupt level is one the processor would take.
 * Level seven is not maskable; anything else has to be above the mask.
 */
static bool interrupt_waiting(const m68k_machine *m)
{
    return m->irq_now &&
           (m->irq_now == 7u || m->irq_now > ((cpu_sr(m) >> 8) & 7u));
}

static bool idle_fast_path_allowed(const m68k_machine *m)
{
    if (!m->idle_fast_enabled) return false;
    const Cpu &c = m->cpu;
    /*
     * A stopped core wakes on the interrupt line itself, at the very next
     * instruction, and not at any deadline this loop knows about. Jumping
     * the clock forward would swallow that wake: the host can raise the
     * power button, or a device can raise a pending bit, at a moment no
     * deadline names. An asserted line the mask refuses is the ordinary
     * idle case and still takes the fast path, because nothing will come
     * of it until the guest lowers the mask, which it cannot do while
     * stopped without an interrupt first.
     */
    if (interrupt_waiting(m)) return false;
    return cpu_part_stopped(m) && !m->duart.active() &&
           !c.trace_left && !c.trace_after_armed &&
           !c.sample_every && !c.force_irq_level && c.watch.empty() &&
           !c.cover && !m->bus.log_mmio && !m->bus.log_unmapped;
}

static void post_slot(m68k_machine *m)
{
    if (m->net_probe_enabled && !(m->cpu.insns & 1023u)) {
        const uint64_t ns = mh_m68k_elapsed_ns(m);
        mh_network_poll(m->net_probe_link, ns);
        mh_ne2000_tick(&m->net_probe_nic, ns);
    }
    if (m->magicbus.connected) m->magicbus.service(m);
    duart_tick(m);
    if (MH_UNLIKELY(m->dev21.compare_dirty)) {
        m->dev21.compare_dirty = false;
        /*
         * Writing a new compare is how this device is serviced, and so
         * how its pending bit goes away. The dispatcher never clears it:
         * it clears each bit it is about to service out of its working
         * copy precisely so the writeback leaves the device alone.
         */
        m->dev21.reg[DEV21_PENDING / 2 + 1] &=
            (uint16_t)~(uint16_t)(1u << DEV21_TIMER_BIT);
        m->dev21.irq_dirty = true;
        timer_arm(m);
    }
    if (m->touch_down && m->touch_next < m->touches.size() &&
        m->touches[m->touch_next].release_at &&
        m->cpu.insns >= m->touches[m->touch_next].release_at) {
        touch_release_now(m);
        m->touch_next++;
    }
    if (!m->touch_down && m->touch_next < m->touches.size() &&
        m->cpu.insns >= m->touches[m->touch_next].at) {
        auto &touch = m->touches[m->touch_next];
        if (touch.screen_point) touch_press_now(m, touch.x, touch.y);
        else {
            m->touch_down = true;
            m->dev21.set_bit32(DEV21_PENDING, DEV21_PEN_BIT);
        }
    }
    if (MH_UNLIKELY(m->dev21.adc_dirty)) {
        m->dev21.adc_dirty = false;
        /*
         * A conversion takes time, and completing it instantly is not a
         * harmless simplification. The driver clears the completion bit
         * and then polls for it (0x0E08B906, 0x0E08B914), so a completion
         * delivered inside the same instruction as the start can be
         * cleared by a step that was meant to precede it, and the poll
         * then waits forever -- which it did, 39.7 million reads of the
         * pending register in one run.
         *
         * How long a real conversion takes is not known. This is one
         * settling delay's worth of instructions, enough to land after
         * any of the driver's own clear-then-poll sequences.
         */
        m->dev21.adc_busy = true;
        m->dev21.adc_done_at = m->cpu.insns + 64;
    }
    if (m->dev21.adc_busy && m->cpu.insns >= m->dev21.adc_done_at) {
        m->dev21.adc_busy = false;
        m->dev21.reg[DEV21_PENDING / 2 + 1] |=
            (uint16_t)m->dev21.adc_done_bits;
        m->dev21.irq_dirty = true;
    }
    if (MH_UNLIKELY(m->dev21.tx_dirty)) {
        m->dev21.tx_dirty = false;
        m->dev21.tx_busy = true;
        m->dev21.tx_done_at = m->cpu.insns + 64;
    }
    if (m->dev21.tx_busy && m->cpu.insns >= m->dev21.tx_done_at) {
        m->dev21.tx_busy = false;
        m->dev21.set_bit32(DEV21_PENDING, DEV21_TX_BIT);
    }
    if (MH_UNLIKELY(m->dev21.pen_timer_dirty)) {
        m->dev21.pen_timer_dirty = false;
        const uint16_t enable = m->dev21.reg[0xDE / 2];
        const uint16_t count = m->dev21.reg[0xDC / 2];
        m->pen_timer_armed = enable != 0;
        if (m->pen_timer_armed) {
            /* The ROM rounds 10/20 ms requests into 8 ms countdown
             * units at 0x0E08B710. Convert independently of the slower
             * free-running counter to preserve sub-tick durations. */
            const uint64_t ms = (uint64_t)(count ? count : 1) * 8u;
            m->pen_timer_at = m->cpu.insns +
                (ms * PIC2000_CPU_HZ + 1000u * m->cpi - 1) /
                (1000u * m->cpi);
        }
    }
    if (MH_UNLIKELY(m->pen_timer_armed && m->cpu.insns >= m->pen_timer_at)) {
        m->pen_timer_armed = false;
        m->dev21.set_bit32(DEV21_PENDING, DEV21_PEN_TIMER_BIT);
    }
    if (MH_UNLIKELY(m->timer_armed && m->cpu.insns >= m->timer_at)) {
        m->timer_armed = false;
        m->timer_fires++;
        m->dev21.set_bit32(DEV21_PENDING, DEV21_TIMER_BIT);
    }
    audio_tick_if_due(m);
    dev21_irq(m);
}

/*
 * The slot before the earliest thing the full loop body would act on:
 * deadlines that fire at the start of a slot (boot release, power presses)
 * are taken as they are, deadlines checked after the instruction (ADC,
 * transmit, pen timer, timer, audio, scripted touches) one slot earlier.
 * Everything else the body reacts to is a flag a device write sets during
 * the instruction, and the quiet loop tests those after each one.
 */
static uint64_t next_run_limit(const m68k_machine *m, uint64_t end)
{
    uint64_t next = end;
    auto pre = [&](uint64_t at) { if (at < next) next = at; };
    auto post = [&](uint64_t at) { if (at && at - 1 < next) next = at - 1; };
    if (m->boot_release_at) pre(m->boot_release_at);
    post(m->audio_at);
    if (m->timer_armed) post(m->timer_at);
    if (m->pen_timer_armed) post(m->pen_timer_at);
    if (m->dev21.adc_busy) post(m->dev21.adc_done_at);
    if (m->dev21.tx_busy) post(m->dev21.tx_done_at);
    if (m->touch_next < m->touches.size()) {
        post(m->touches[m->touch_next].at);
        if (m->touch_down) post(m->touches[m->touch_next].release_at);
    }
    return next;
}

/*
 * DEVIATION. Substitute the result of the HIX-300's ROM checksum.
 *
 * Only the result, and only for the one call that is recognised: the
 * comparison and branch that follow execute normally, so flags and
 * prefetch take the path a machine with a matching ROM would take.
 * Checked against the registers rather than the address alone, because an
 * address is reached for more than one reason.
 */
static void checksum_substitute(m68k_machine *m)
{
    if (cpu_pc(m) != mh_m68k_checksum_pc(m) ||
        cpu_a(m, m->envoy_mc31?3:4) != (m->envoy_mc31?0x0040000Cu:0x0E00000Cu) ||
        cpu_d(m, 0) != m->hix_checksum_actual)
        return;
    fprintf(m->log, "68k: DEVIATION: intercepted %s checksum result at "
            "%08X (%08X -> %08X)\n", m->envoy_mc31?"Envoy mc31":"HIX-300",
            mh_m68k_checksum_pc(m), m->hix_checksum_actual,
            m->hix_checksum_stored);
    cpu_set_d(m, 0, m->hix_checksum_stored);
    m->hix_checksum_pending = false;
    mh_m68k_set_stop_at(m, 0);
}

static bool quiet_path_allowed(m68k_machine *m)
{
    if (!m->quiet_fast_enabled) return false;
    const Cpu &c = m->cpu;
    /* Counted by reason: which of these keeps the run in the diagnostic
     * body is the difference between the reference executing half a
     * million instructions and nine million. */
    if (cpu_part_stopped(m))        { m->noquiet_stopped++; return false; }
    if (m->power_scheduled)         { m->noquiet_power++;   return false; }
    if (m->dev21.power_off)         { m->noquiet_off++;     return false; }
    if (c.trace_left || c.trace_after_armed || c.sample_every ||
        c.force_irq_level || !c.watch.empty() || c.cover || c.insnheat) {
        m->noquiet_diag++; return false;
    }
    return true;
}

/*
 * Anything a device write set that the run loop has to act on now.
 * The same set the quiet loop tests after every instruction: this is that
 * test, asked from inside the bus so another engine can be stopped at the
 * instruction that caused it rather than at the end of its block.
 */
static inline bool device_wants_attention(const m68k_machine *m)
{
    return m->dev21.compare_dirty || m->dev21.adc_dirty || m->dev21.tx_dirty ||
           m->dev21.pen_timer_dirty || m->dev21.audio_dirty ||
           m->dev21.irq_dirty || m->dev21.power_off || m->duart.active();
}

static uint32_t core_read(void *ctx, uint32_t addr, unsigned size)
{
    auto *m = static_cast<m68k_machine *>(ctx);
    uint32_t v = MH_UNLIKELY(m->core.fc == mh::M68kBus::FC_CPU_SPACE)
                     ? m->cpu.cpu_space_read(addr, size)
                     : m->cpu.rd(addr, size);
    /* A read can change a device as surely as a write: several of these
     * registers clear a status bit when they are looked at. */
    if (MH_UNLIKELY(device_wants_attention(m))) m->core.yield = true;
    return v;
}

static void core_write(void *ctx, uint32_t addr, unsigned size, uint32_t value)
{
    auto *m = static_cast<m68k_machine *>(ctx);
    if (MH_UNLIKELY(m->core.fc == mh::M68kBus::FC_CPU_SPACE))
        m->cpu.cpu_space_write(addr, size, value);
    else
        m->cpu.wr(addr, size, value);
    if (m->magicbus.connected) {
        m->magicbus.service(m);
        if (addr >= (m->cpu.mbar & 0xfffff000u)+0x7a0 &&
            addr < (m->cpu.mbar & 0xfffff000u)+0x7b8)
            m->dev21.irq_dirty=true;
    }
    if (MH_UNLIKELY(device_wants_attention(m))) m->core.yield = true;
}

/*
 * An odd program counter. The block engine stops in front of one rather
 * than faulting inside a block, because the fault is recorded against the
 * instruction that made the jump rather than the address it jumped to,
 * and by then the block has moved on. This core builds the CPU32
 * twenty-four byte format C frame itself; it used to hand the whole thing
 * to the reference, which was the last exception it could not raise.
 */
static void core_odd_fetch(m68k_machine *m)
{
    m->core_odd++;
    m68k_address_error(&m->core, m->core.pc, m->core.prev_pc);
}

/*
 * The shortest stretch worth submitting to the block engine.
 *
 * This was 64, to keep block dispatch from costing more than the
 * engine saves, and to stop a hot loop with one unimplemented instruction
 * in it paying that exchange on every iteration to run three instructions.
 * The second reason has gone: the engine refuses nothing now, so there is
 * no instruction for such a loop to be built around. The first turned out
 * not to be true at 64 either -- on a PIC-2000 resuming into the UI,
 * 300 million instructions:
 *
 *     64   292,112,564 on this engine   7,098,845 stretches skipped
 *      4   299,188,873                     22,536
 *      1   299,211,409                          0
 *
 * with the whole of RAM identical at every setting and the wall clock
 * inside its own noise -- 9.80 s against 9.58 s at best of three, the
 * 11% more block-engine calls buying seven million instructions.
 *
 * One, in the end, and not four. Counting interpreter fallbacks says the
 * difference is not the 0.01% of instructions it looks like: at four the
 * quiet path still leaves 22,536 slots to the interpreter, and at one it
 * leaves none. That is the whole point of the exercise, so the
 * give-up guard below has been given its own constant rather than being
 * derived from this one, which is what made four look necessary.
 *
 * MH_68K_MIN_RUN overrides it, which is how the table above was made.
 */
static uint64_t CORE_MIN_RUN = []{
    if (const char *e = std::getenv("MH_68K_MIN_RUN")) {
        unsigned long v = std::strtoul(e, nullptr, 0);
        if (v) return (uint64_t)v;
    }
    return (uint64_t)1;
}();

/*
 * The average below which block execution is not worth doing at all.
 *
 * This used to be CORE_MIN_RUN/2, which tied it to the gate above and so
 * quietly retired itself as that gate came down. It is its own number now,
 * because the two say different things: the gate is how short a stretch is
 * worth submitting, and this is how badly the engine has to be doing before
 * the machine stops submitting work at all.
 */
static const double CORE_GIVE_UP_AVERAGE = 2.0;

static uint64_t run_on_core(m68k_machine *m, uint64_t budget)
{
    m->cpu.direct_prepare();
    m->core.bus.read_pages =
        m->cpu.direct_enabled ? m->cpu.direct_rd.data() : nullptr;
    m->core.bus.write_pages =
        m->cpu.direct_enabled ? m->cpu.direct_wr.data() : nullptr;
    m->core.exception = 0;
    m->core.yield = false;
    m->core.stop_pc = m->stop_at;
    /*
     * The machine's instruction count, shared with the block engine.
     *
     * dev21's counter register is the instruction count scaled, and the
     * guest polls it. Leaving the count frozen for the length of a block
     * run stops that clock dead for a few hundred instructions at a
     * time, and the guest notices: the first version of this diverged from
     * the reference inside a second of emulated time.
     *
     * One less than the machine's count, because this core counts an
     * instruction as it starts one and the loop below counts it after. The
     * two then agree about what the count is during the instruction, which
     * is the only moment anything can read it.
     */
    m->core.insn_count = m->cpu.insns ? m->cpu.insns - 1 : 0;
    m->dev21.insn_src = &m->core.insn_count;
    uint64_t did;
    if (MH_UNLIKELY(m->core_trace)) {
        did = 0;
        while (did < budget && m->core_trace) {
            uint32_t was = m->core.pc, sp = m->core.a[7];
            uint64_t one = m68k_run_blocks(&m->core, 1, m->blocks);
            if (!one) break;      /* block yielded; caller handles this slot */
            fprintf(m->log, "[68k] %08X sp=%08X\n", was, sp);
            m->core_trace--;
            did += one;
            if (m->core.yield || m->core.exception) break;
        }
    } else {
        /*
         * Run, and take an interrupt whenever one comes due, rather than
         * handing the processor back for it. m68k_interrupt applies the
         * architectural rule -- a level above the mask, or seven -- and
         * builds the exception frame in the project-owned CPU32 core.
         */
        did = 0;
        for (;;) {
            /*
             * Before running, not after: an interrupt that is already due
             * has to be taken in front of the next instruction, or the
             * stretch runs past the point where the part would have
             * vectored. m68k_interrupt refuses a level the mask covers, so
             * a line that stays asserted is taken once and not again.
             */
            if (m->irq_now) {
                m->core_irq_seen++;
                if (m68k_interrupt(&m->core, m->irq_now, m->cpu.irq_vector)) {
                    m->core_interrupts++;
                    m->core.stopped = false;   /* and it ends LPSTOP's wait */
                } else {
                    m->core_irq_refused++;
                }
            }
            if (did >= budget) break;
            did += m68k_run_blocks(&m->core, budget - did, m->blocks);
            if (m->core.yield || m->core.exception || (m->core.pc & 1)) break;
            if (!m->irq_now) break;
        }
    }
    m->dev21.insn_src = &m->cpu.insns;
    m->core.yield = false;
    if (MH_UNLIKELY(m->core.exception_count != m->core_exceptions_seen))
        core_report_exceptions(m);
    m->core_calls++;
    m->core_ran += did;
    if (!did) m->core_empty++;
    /*
     * Give up on the engine if it is not earning its keep. Handing over
     * for a handful of instructions at a time is slower than not handing
     * over at all, and a guest can walk into code where that is all this
     * engine can do. Measuring it and stopping is better than a guess
     * about which guests those are.
     */
    if (MH_UNLIKELY(m->core_calls == 4096)) {
        double each = (double)m->core_ran / (double)m->core_calls;
        if (each < CORE_GIVE_UP_AVERAGE) {
            m->core_enabled = false;
            fprintf(m->log, "68k: block engine disabled; it averaged %.1f "
                    "instructions per block-engine call over %llu calls, "
                    "which costs more in dispatch than it saves\n", each,
                    (unsigned long long)m->core_calls);
        }
    }
    return did;
}

/*
 * Name exceptions as they happen. Vector 11 is genuinely ambiguous: it is
 * both the coprocessor opcodes this ROM emulates in software and a jump to
 * an address nothing decodes, because a floating bus reads as FFFF and
 * FFFF is a line-F opcode. Print the opcode and address so a lost program
 * counter cannot be mistaken for arithmetic.
 */
static void core_report_exceptions(m68k_machine *m)
{
    uint64_t raised = m->core.exception_count;
    if (MH_LIKELY(raised == m->core_exceptions_seen)) return;
    uint64_t missed = raised - m->core_exceptions_seen - 1;
    m->core_exceptions_seen = raised;

    const unsigned vector = m->core.last_vector;
    const uint32_t at = m->core.last_vector_pc;
    /* Traps are how the guest calls its operating system; naming every
     * one is noise. The frame below is still worth seeing for the first
     * few, so this only skips the line, not the dump. */
    const bool routine = vector == M68K_VEC_TRAP0 || vector >= 32;
    bool name_it = !routine;
    if (name_it && m->cpu.exception_logs++ >= 32) {
        m->cpu.suppressed_exceptions++;
        name_it = false;
    }
    m->cpu.suppressed_exceptions += missed;
    if (name_it) {

    const char *name = "exception";
    switch (vector) {
    case M68K_VEC_BUS_ERROR:      name = "bus error"; break;
    case M68K_VEC_ADDRESS_ERROR:  name = "address error"; break;
    case M68K_VEC_ILLEGAL:        name = "illegal instruction"; break;
    case M68K_VEC_DIVIDE_BY_ZERO: name = "divide by zero"; break;
    case M68K_VEC_CHK:            name = "CHK out of bounds"; break;
    case M68K_VEC_TRAPV:          name = "TRAPV"; break;
    case M68K_VEC_PRIVILEGE:      name = "privilege violation"; break;
    case M68K_VEC_TRACE:          name = "trace"; break;
    case M68K_VEC_LINE_A:         name = "line A"; break;
    case M68K_VEC_LINE_F:         name = "line F"; break;
    default: break;
    }
    if (vector == M68K_VEC_ADDRESS_ERROR)
        /* Both addresses, because this part records both: the fetch went
         * to the first and the instruction that sent it there is the
         * second, which is the one a handler can do anything with. */
        fprintf(m->log, "[68k] %s at pc=%08X, from %08X (vector %u)\n",
                name, m->core.exception_data, at, vector);
    else
        fprintf(m->log, "[68k] %s at pc=%08X (vector %u)\n", name, at, vector);

    bool ok = true;
    const uint16_t word = (uint16_t)mh_bus_read(&m->bus, at, 2, &ok);
    if (vector == M68K_VEC_LINE_F)
        fprintf(m->log, "       fetched opcode %04X at %08X%s\n", word, at,
                ok ? "" : ", which nothing decodes -- a wild jump, not the "
                          "software FPU");
    else if (vector == M68K_VEC_ILLEGAL && word == 0x4AFA) {
        /*
         * BGND is how this ROM asserts. On a production machine, with no
         * debugger attached, the part takes an illegal instruction
         * instead -- so this is right for the right reason. An assertion
         * firing is the ROM saying our model is wrong somewhere, and it
         * names the place, so the registers it is asserting about are
         * worth having.
         */
        fprintf(m->log, "       BGND at %08X after %llu instructions: the "
                "ROM's own assertion fired\n", at,
                (unsigned long long)m->cpu.insns);
        for (unsigned r = 0; r < 8; r++)
            fprintf(m->log, "         d%u=%08X%s", r, cpu_d(m, r),
                    r == 3 || r == 7 ? "\n" : "");
        for (unsigned r = 0; r < 8; r++)
            fprintf(m->log, "         a%u=%08X%s", r, cpu_a(m, r),
                    r == 3 || r == 7 ? "\n" : "");
    } else if (vector == M68K_VEC_ILLEGAL)
        fprintf(m->log, "       opcode %04X at %08X\n", word, at);
    }

    /*
     * And the frame itself.
     *
     * This ROM emulates the 68881 in software, and its line-F handler
     * reads the faulting instruction back out of the frame -- "movea.l
     * $46(a7),a0" then "moves.l (a0),a1" at 0x0E1230B8. That only works if
     * the layout is what the handler expects, which is the one place a
     * 68020 model standing in for a CPU32 is most likely to differ, so the
     * first few are printed rather than reasoned about. The same budget
     * the reference used, and the same counter, so a run that took
     * exceptions on both cores still shows four.
     */
    if (m->cpu.frames_shown >= 4) return;
    m->cpu.frames_shown++;
    fprintf(m->log, "[68k] vector %u -> %08X, frame at %08X:", vector,
            m->core.last_vector_to, m->core.last_vector_sp);
    for (unsigned k = 0; k < 12; k += 2) {
        bool word_ok = true;
        uint32_t w = mh_bus_read(&m->bus, m->core.last_vector_sp + k, 2,
                                  &word_ok);
        fprintf(m->log, " %04X", word_ok ? w : 0xFFFF);
    }
    fprintf(m->log, "\n");
}

/*
 * One instruction in the project-owned CPU32 interpreter.
 *
 * The slow body below is where the machine ends up whenever anything is
 * being traced, watched or counted -- and also for the one slot that ends
 * every quiet stretch, which on an idle machine is nearly all of what
 * runs. Interrupts are checked before the instruction; taking one is this
 * slot's work rather than an addition to it.
 */
static void core_step_one(m68k_machine *m)
{
    m->cpu.direct_prepare();
    m->core.bus.read_pages =
        m->cpu.direct_enabled ? m->cpu.direct_rd.data() : nullptr;
    m->core.bus.write_pages =
        m->cpu.direct_enabled ? m->cpu.direct_wr.data() : nullptr;
    m->core.exception = 0;
    m->core.yield = false;
    if (MH_UNLIKELY(m->irq_now != 0)) {
        m->core_irq_seen++;
        if (m68k_interrupt(&m->core, m->irq_now, m->cpu.irq_vector)) {
            m->core_interrupts++;
            m->core.stopped = false;   /* and it ends LPSTOP's wait */
            return;
        }
        m->core_irq_refused++;
    }
    /* Stopped: the slot passes and the devices run, but nothing is
     * fetched. This is the idle loop, and it is most of a run. */
    if (MH_UNLIKELY(m->core.stopped)) return;
    /* Arriving on an odd address faults in this slot, and so does landing
     * on one: the part checks the next fetch address as part of the
     * instruction that produced it, not as the following instruction. */
    if (MH_UNLIKELY(m->core.pc & 1)) { core_odd_fetch(m); return; }
    m68k_step(&m->core);
    if (MH_UNLIKELY(m->core.pc & 1)) core_odd_fetch(m);
    m->core.yield = false;
    if (MH_UNLIKELY(m->core.exception_count != m->core_exceptions_seen))
        core_report_exceptions(m);
}

uint64_t mh_m68k_run(m68k_machine *m, uint64_t insns)
{
    // Host keyboard edges can arrive while the CPU is in LPSTOP.
    if (m->magicbus.connected) dev21_irq(m);
    /*
     * Counted here at the instruction boundary, so every execution engine
     * advances the same machine instruction count.
     */
    uint64_t n = 0;
    while (n < insns && MH_LIKELY(!m->cpu.stopped)) {
        /* LPSTOP does not retire instructions. Advance the slot clock in one
         * jump, stopping one slot before the next modeled event so the normal
         * loop below performs the wakeup and all side effects in order. */
        if (MH_UNLIKELY(idle_fast_path_allowed(m))) {
            const uint64_t now = m->cpu.insns;
            const uint64_t end = now + (insns - n);
            const uint64_t deadline = next_idle_deadline(m);
            const uint64_t limit = deadline < end ? deadline : end;
            if (limit > now + 1) {
                const uint64_t skip = limit - now - 1;
                m->cpu.insns += skip;
                n += skip;
                continue;
            }
        }
        /* Running core, nothing to observe: execute straight through to the
         * slot before the next deadline, handling device-written flags the
         * moment they appear, exactly as the full body below would. */
        if (MH_LIKELY(quiet_path_allowed(m))) {
            uint64_t limit = next_run_limit(m, m->cpu.insns + (insns - n));
            while (m->cpu.insns < limit && MH_LIKELY(!m->cpu.stopped)) {
                /*
                 * Hand a stretch to the block engine when there is a
                 * stretch to run as a block. Interrupts are handled at
                 * block boundaries so the block engine cannot run past
                 * one that becomes due.
                 */
                if (MH_UNLIKELY(m->core_enabled) &&
                    limit - m->cpu.insns < CORE_MIN_RUN) {
                    m->core_skipped_short++;
                    if (interrupt_waiting(m)) m->core_skipped_irq++;
                }
                if (MH_UNLIKELY(m->core_enabled) &&
                    limit - m->cpu.insns >= CORE_MIN_RUN &&
                    !(cpu_pc(m) & 1)) {
                    /* Deliver pending interrupts at each block boundary. */
                    uint64_t did = run_on_core(m, limit - m->cpu.insns);
                    if (did) {
                        n += did;
                        m->cpu.insns += did;
                        if (MH_UNLIKELY(m->core.pc & 1)) {
                            core_odd_fetch(m);
                            /* Named now rather than at the end of the next
                             * stretch: by then its handler has run and the
                             * frame it built is under a different stack. */
                            core_report_exceptions(m);
                        }
                        if (MH_UNLIKELY(device_wants_attention(m))) {
                            post_slot(m);
                            if (m->dev21.power_off) break;
                            limit = next_run_limit(m, m->cpu.insns + (insns - n));
                        }
                        if (n >= insns || MH_UNLIKELY(cpu_part_stopped(m)))
                            break;
                        continue;
                    }
                    /* The interpreter below handles a slot the block
                     * engine declined or could not start. */
                }
                if (MH_UNLIKELY(m->core_trace)) {
                    fprintf(m->log, "[68k] %08X sp=%08X\n", cpu_pc(m),
                            cpu_a(m, 7));
                    m->core_trace--;
                }
                /*
                 * The engine returns here with the program counter on the
                 * watched address, having retired nothing. The slow path
                 * below performs the watched instruction itself.
                 */
                if (MH_UNLIKELY(m->hix_checksum_pending)) {
                    checksum_substitute(m);
                    if (!m->hix_checksum_pending) continue;
                }
                core_step_one(m);
                n++;
                m->cpu.insns++;
                if (MH_UNLIKELY(m->dev21.compare_dirty || m->dev21.adc_dirty ||
                                 m->dev21.tx_dirty || m->dev21.pen_timer_dirty ||
                                 m->dev21.audio_dirty || m->dev21.irq_dirty ||
                                 m->dev21.power_off)) {
                    post_slot(m);
                    if (m->dev21.power_off) break;
                    limit = next_run_limit(m, m->cpu.insns + (insns - n));
                }
                if (MH_UNLIKELY(cpu_part_stopped(m))) break;
            }
            if (n >= insns || m->cpu.stopped) break;
        }
        if (MH_UNLIKELY(m->boot_release_at && m->cpu.insns >= m->boot_release_at)) {
            m->boot_release_at = 0;
            mh_m68k_power_button(m, false);
        }
        if (MH_UNLIKELY(m->power_scheduled && m->cpu.insns >= m->power_at)) {
            mh_m68k_power_button(m, m->cpu.insns < m->power_release_at);
            if (m->cpu.insns >= m->power_release_at)
                m->power_scheduled = false;
        }
        if (MH_UNLIKELY(m->dev21.power_off)) {
            n++;
            m->cpu.insns++;
            audio_tick_if_due(m);
            continue;
        }
        if (MH_UNLIKELY(m->hix_checksum_pending)) checksum_substitute(m);
        if (m->cpu.trace_after_armed &&
            cpu_pc(m) == m->cpu.trace_after_pc &&
            ++m->cpu.trace_after_seen == m->cpu.trace_after_hit) {
            m->cpu.trace_after_armed = false;
            m->cpu.trace_left = m->cpu.trace_after_count;
        }
        if (m->cpu.trace_left) {
            m->cpu.trace_left--;
            fprintf(m->log, "[68k] %08X\n", cpu_pc(m));
        }
        /* The same line from the slow body, so that a trace taken with the
         * block engine on covers every instruction the machine runs and not
         * only the ones one of the two loops ran. */
        if (MH_UNLIKELY(m->core_trace)) {
            m->core_trace--;
            fprintf(m->log, "[68k] %08X sp=%08X\n", cpu_pc(m), cpu_a(m, 7));
        }
        if (m->cpu.force_irq_level && !m->cpu.force_irq_done &&
            n >= m->cpu.force_irq_at) {
            m->cpu.force_irq_done = true;
            fprintf(m->log, "[68k] DEVIATION: asserting IPL%u at +%llu; no "
                    "modelled device drove it\n", m->cpu.force_irq_level,
                    (unsigned long long)n);
        }
        {
            uint32_t pc = cpu_pc(m);
            auto w = m->cpu.watch.find(pc);
            if (w != m->cpu.watch.end()) {
                w->second++;
                if (m->cpu.watch_log) {
                    m->cpu.watch_log--;
                    fprintf(m->log, "[watch] the %u instructions before "
                            "%08X:\n        ", m->cpu.ring_full ? Cpu::RING
                            : m->cpu.ring_at, pc);
                    unsigned n = m->cpu.ring_full ? Cpu::RING : m->cpu.ring_at;
                    for (unsigned k = 0; k < n; k++) {
                        unsigned idx = (m->cpu.ring_at + Cpu::RING - n + k)
                                       % Cpu::RING;
                        fprintf(m->log, "%08X%s", m->cpu.ring[idx],
                                (k % 8 == 7) ? "\n        " : " ");
                    }
                    fprintf(m->log, "\n");
                    fprintf(m->log, "[watch] %08X #%llu at +%llu\n", pc,
                            (unsigned long long)w->second,
                            (unsigned long long)m->cpu.insns);
                    for (int r = 0; r < 8; r++)
                        fprintf(m->log, "         d%d=%08X%s", r,
                                cpu_d(m, (unsigned)r),
                                r == 3 || r == 7 ? "\n" : "");
                    for (int r = 0; r < 8; r++)
                        fprintf(m->log, "         a%d=%08X%s", r,
                                cpu_a(m, (unsigned)r),
                                r == 3 || r == 7 ? "\n" : "");
                    /* The stack itself, since the value that goes wrong is
                     * popped from it and the suspect is its layout. */
                    uint32_t sp = cpu_a(m, 7);
                    fprintf(m->log, "         stack at %08X:", sp);
                    for (unsigned k = 0; k < 8; k++) {
                        bool ok = true;
                        uint32_t v = mh_bus_read(&m->bus, sp + k * 4, 4, &ok);
                        fprintf(m->log, " %08X", ok ? v : 0);
                    }
                    fprintf(m->log, "\n");
                }
            }
        }
        if (m->cpu.cover) m->cpu.blocks.insert(cpu_pc(m) & ~0xFFu);
        const uint32_t current_pc = cpu_pc(m);
        if (!m->cpu.watch.empty()) {
            m->cpu.ring[m->cpu.ring_at] = current_pc;
            if (++m->cpu.ring_at == Cpu::RING) {
                m->cpu.ring_at = 0;
                m->cpu.ring_full = true;
            }
        }
        m->cpu.prev_pc = current_pc;
        if (m->cpu.sample_every && n >= m->cpu.sample_next) {
            m->cpu.pc_pages[m->cpu.prev_pc & ~0xFFFFu]++;
            m->cpu.sample_next = n + m->cpu.sample_every;
        }
        if (MH_UNLIKELY(m->cpu.insnheat))
            cpu_state::count_insn_heat(m, current_pc);
        core_step_one(m);
        n++;
        m->cpu.insns++;

        post_slot(m);
    }
    return n;
}

bool mh_m68k_stopped(const m68k_machine *m) { return m->cpu.stopped; }
void mh_m68k_audio_divider(m68k_machine *m, bool enabled)
{
    m->audio_at = 0;
    m->audio.experimental_divider = enabled;
    if (enabled) fprintf(m->log, "68k: DEVIATION: experimental audio divider; unknown rates inferred from modulo-16 counter hypothesis\n");
}

void mh_m68k_audio_approx(m68k_machine *m, bool enabled)
{
    m->audio.approximate_output = enabled;
    m->audio.filter1 = m->audio.filter2 = 0;
}
unsigned mh_m68k_audio_rate(const m68k_machine *) { return Pic2000Audio::output_rate; }
void mh_m68k_audio_sink(m68k_machine *m, void (*sink)(void *, int16_t), void *ctx)
{ m->audio.sink = sink; m->audio.ctx = ctx; }
void mh_m68k_trace(m68k_machine *m, uint64_t n) { m->cpu.trace_left = n; }
void mh_m68k_trace_after(m68k_machine *m, uint32_t pc, uint64_t n)
{
    mh_m68k_trace_after_hit(m, pc, 1, n);
}
void mh_m68k_trace_after_hit(m68k_machine *m, uint32_t pc, uint64_t hit,
                              uint64_t n)
{
    m->cpu.trace_after_pc = pc;
    m->cpu.trace_after_count = n;
    m->cpu.trace_after_hit = hit;
    m->cpu.trace_after_seen = 0;
    m->cpu.trace_after_armed = hit != 0 && n != 0;
}
void mh_m68k_sample(m68k_machine *m, uint64_t every)
{
    m->cpu.sample_every = every;
}

void mh_m68k_heat(m68k_machine *m, bool on) { m->cpu.heat = on; }
void mh_m68k_readheat(m68k_machine *m, bool on) { m->cpu.readheat = on; }
void mh_m68k_insnheat(m68k_machine *m, bool on) { m->cpu.insnheat = on; }
void mh_m68k_watch_write(m68k_machine *m, uint32_t lo, uint32_t hi)
{
    m->cpu.watch_write_lo = lo;
    m->cpu.watch_write_hi = hi;
}
void mh_m68k_watch_read(m68k_machine *m, uint32_t addr)
{
    m->cpu.watch_read = addr;
}
void mh_m68k_cover(m68k_machine *m, const char *path)
{
    m->cpu.cover = true;
    m->cover_path = path;
}

/*
 * Dump memory at exit.
 *
 * Low RAM is where this operating system keeps the parts of itself that
 * matter most -- the vector table, the dispatch table at a5, and trampolines
 * that are built at run time and jumped through. None of that is in the ROM
 * image, so it cannot be read statically; it has to be taken from a machine
 * that has run.
 */
void mh_m68k_dump(m68k_machine *m, uint32_t addr, uint32_t len,
                   const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(m->log, "68k: cannot write %s\n", path); return; }
    for (uint32_t offset = 0; offset < len; offset++) {
        bool ok = true;
        uint32_t v = mh_bus_read(&m->bus, addr + offset, 1, &ok);
        fputc(ok ? (int)(v & 0xFF) : 0xFF, f);
    }
    fclose(f);
    fprintf(m->log, "68k: dumped %u bytes from %08X to %s\n", len, addr, path);
}
void mh_m68k_watch(m68k_machine *m, uint32_t pc) { m->cpu.watch[pc] = 0; }
void mh_m68k_watch_log(m68k_machine *m, unsigned n) { m->cpu.watch_log = n; }
void mh_m68k_log_unknown(m68k_machine *m, bool on)
{
    m->cpu.log_unknown = on;
    /*
     * A write to ROM is not an undecoded access -- the board decodes it fine
     * and the hardware ignores it -- but it is the same kind of news: software
     * writing somewhere it cannot have meant to. The bus counts those
     * separately, and without this they are invisible to a run that reports
     * zero undecoded accesses while faulting two million times.
     */
    m->bus.log_unmapped = on;
}

void mh_m68k_report(const m68k_machine *m)
{
    fprintf(m->log, "68k: power %s%s\n", m->dev21.power_off ? "off" : "on",
            m->envoy ? (m->dev21.adapter_attached ? ", AC attached" : ", AC disconnected") : "");
    if (m->dev21.econoram)
        fprintf(m->log, "battery EconoRAM: %llu transactions, %llu data bits written\n",
                (unsigned long long)m->battery_ram.transactions,
                (unsigned long long)m->battery_ram.written_bits);
    for (unsigned slot = 0; slot < 2; slot++)
        if (m->card[slot].kind)
            mh_pccard_report(&m->card[slot]);
    const Cpu &c = m->cpu;
    fprintf(m->log, "\n---\n");
    /*
     * Where the vector table ended up.
     *
     * The table in ROM points all sixty-four vectors at one catch-all at
     * 0x0E089114, so it is a placeholder: the operating system is expected to
     * build a real table and move VBR to it. Whether it has done so, and the
     * handlers it installed, is what says which interrupts this machine is
     * actually prepared to take -- and nothing can be delivered until we know.
     */
    {
        uint32_t vbr = cpu_vbr(m);
        fprintf(m->log, "68k: VBR = %08X%s\n", vbr,
                vbr == 0x0E0000FA ? " (still the ROM's placeholder table)"
                                  : " (relocated)");
        static const struct { int v; const char *n; } want[] = {
            {2,"bus error"}, {3,"address error"}, {4,"illegal"},
            {11,"line F"}, {24,"spurious"}, {25,"IPL1"}, {26,"IPL2"},
            {27,"IPL3"}, {28,"IPL4"}, {29,"IPL5"}, {30,"IPL6"}, {31,"IPL7"},
        };
        for (auto &w : want) {
            bool ok = true;
            uint32_t h = mh_bus_read(const_cast<mh_bus *>(&m->bus),
                                      vbr + w.v * 4, 4, &ok);
            if (ok)
                fprintf(m->log, "       %-14s vector %2d -> %08X\n",
                        w.n, w.v, h);
        }
    }
    fprintf(m->log, "68k: %llu instructions, pc=%08X sp=%08X%s%s\n",
            (unsigned long long)c.insns, cpu_pc(m), cpu_a(m, 7),
            c.stopped ? ", stopped: " : "", c.stopped ? c.stop_why : "");
    fprintf(m->log, "bus: %llu reads, %llu writes, %llu ignored writes to "
            "ROM or unmapped space\n",
            (unsigned long long)m->bus.reads,
            (unsigned long long)m->bus.writes,
            (unsigned long long)m->bus.faults);
    if (c.mbar)
        fprintf(m->log, "68k: MBAR %08X, so the module space is at %08X\n",
                c.mbar, c.mbar & 0xFFFFF000u);
    if (c.cpu_space_other)
        fprintf(m->log, "68k: %llu CPU-space accesses to addresses we do not "
                "model\n", (unsigned long long)c.cpu_space_other);
    /*
     * The --insn-heat reports, aggregated through our own decoder. They
     * used to go through the reference core's instruction metadata, which
     * is why this diagnostic needed it in the build; the decoder answers
     * the same three questions -- what, through what, at what width.
     */
    if (m->cpu.insnheat) {
        struct Row { uint64_t count; unsigned opcodes; };
        std::map<std::string, Row> by_form;
        uint64_t total = 0;
        for (const auto &e : m->cpu.insn_heat_counts) {
            /*
             * Decoded from the opcode word alone, with filler where the
             * extension words were: what those hold changes the operands
             * and not the form, and the form is all this prints.
             */
            uint8_t code[MH_JIT_M68K_MAX_BYTES];
            code[0] = (uint8_t)(e.first >> 8);
            code[1] = (uint8_t)e.first;
            for (unsigned k = 2; k < sizeof code; k++) code[k] = (uint8_t)(k * 7);
            m68k_insn insn;
            char key[64];
            if (!m68k_decode(code, sizeof code, 0x1000, &insn))
                snprintf(key, sizeof key, "%-8s %-9s %c", "?", "?", '-');
            else
                snprintf(key, sizeof key, "%-8s %-9s %c",
                         m68k_op_name(insn.op),
                         m68k_mode_name(insn.src.mode == M68K_NOEA
                                            ? insn.dst.mode : insn.src.mode),
                         insn.size == 1 ? 'b' : insn.size == 2 ? 'w' :
                         insn.size == 4 ? 'l' : '-');
            by_form[key].count += e.second;
            by_form[key].opcodes++;
            total += e.second;
        }
        std::vector<std::pair<std::string, Row>> rows(by_form.begin(), by_form.end());
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.second.count > b.second.count; });
        fprintf(m->log, "68k: %llu instructions retired in %zu distinct opcodes,"
                " %zu instruction/mode/size forms\n",
                (unsigned long long)total, m->cpu.insn_heat_counts.size(),
                rows.size());
        uint64_t running = 0;
        for (size_t k = 0; k < rows.size(); k++) {
            running += rows[k].second.count;
            fprintf(m->log, "  %-22s %11llu  %5.2f%%  cum %5.1f%%  (%u opcodes)\n",
                    rows[k].first.c_str(),
                    (unsigned long long)rows[k].second.count,
                    total ? 100.0 * (double)rows[k].second.count / (double)total : 0.0,
                    total ? 100.0 * (double)running / (double)total : 0.0,
                    rows[k].second.opcodes);
            if (k == 39 && rows.size() > 40) {
                fprintf(m->log, "  ... %zu further forms, %5.2f%% together\n",
                        rows.size() - 40,
                        total ? 100.0 * (double)(total - running) / (double)total : 0.0);
                break;
            }
        }
    }
    if (m->cpu.insnheat) {
        /*
         * What this machine runs that the core of our own does not yet.
         * Weighted by how often it runs, because the order to implement
         * things in is the order the guest asks for them, and a long tail
         * of instructions that appear once is not the same obstacle as one
         * that appears in every other loop.
         */
        std::map<std::string, uint64_t> missing;
        uint64_t total = 0, unimplemented = 0;
        for (const auto &e : m->cpu.insn_heat_counts) {
            uint8_t code[24];
            code[0] = (uint8_t)(e.first >> 8);
            code[1] = (uint8_t)e.first;
            for (unsigned k = 2; k < sizeof code; k++) code[k] = (uint8_t)(k * 7);
            m68k_insn insn;
            total += e.second;
            if (!m68k_decode(code, sizeof code, 0x1000, &insn)) continue;
            if (m68k_implemented(insn.op)) continue;
            missing[m68k_op_name(insn.op)] += e.second;
            unimplemented += e.second;
        }
        /*
         * And the same question for the emitter, which is a different
         * one: the core runs everything now, but generated code calls the
         * interpreter for whatever it cannot translate, and that call is
         * the single largest cost left in a block. Measured, emitting a
         * block and then calling the interpreter for every instruction in
         * it is *slower* than not emitting at all -- 6.54 s against 5.67
         * on a PIC-2000 boot -- so the fraction translated is what makes
         * the whole arrangement pay. This is the worklist for raising it,
         * in the order the guest asks for them.
         */
        {
            std::map<std::string, uint64_t> declined;
            uint64_t ran = 0, called = 0;
            for (const auto &e : m->cpu.insn_heat_counts) {
                uint8_t code[MH_JIT_M68K_MAX_BYTES];
                code[0] = (uint8_t)(e.first >> 8);
                code[1] = (uint8_t)e.first;
                for (unsigned k = 2; k < sizeof code; k++)
                    code[k] = (uint8_t)(k * 7);
                m68k_insn insn;
                ran += e.second;
                if (!m68k_decode(code, sizeof code, 0x1000, &insn)) continue;
                if (m68k_translatable(&insn) || m68k_inline_memory(&insn))
                    continue;
                char key[48];
                snprintf(key, sizeof key, "%-8s %-9s", m68k_op_name(insn.op),
                         m68k_mode_name(insn.src.mode == M68K_NOEA
                                            ? insn.dst.mode : insn.src.mode));
                declined[key] += e.second;
                called += e.second;
            }
            fprintf(m->log, "68k: the emitter translates %.1f%% of what ran; "
                    "the rest is an interpreter call from inside a block\n",
                    ran ? 100.0 * (double)(ran - called) / (double)ran : 0.0);
            std::vector<std::pair<std::string, uint64_t>> rows(declined.begin(),
                                                              declined.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
                return a.second > b.second; });
            unsigned shown = 0;
            for (const auto &r : rows) {
                if (shown++ == 16) break;
                fprintf(m->log, "  called: %-20s %11llu  %5.2f%%\n",
                        r.first.c_str(), (unsigned long long)r.second,
                        ran ? 100.0 * (double)r.second / (double)ran : 0.0);
            }
        }
        fprintf(m->log, "68k: the new core covers %.3f%% of what ran; %zu "
                "instructions are missing\n",
                total ? 100.0 * (double)(total - unimplemented) / (double)total : 0.0,
                missing.size());
        std::vector<std::pair<std::string, uint64_t>> rows(missing.begin(), missing.end());
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.second > b.second; });
        for (const auto &r : rows)
            fprintf(m->log, "  missing: %-10s %llu\n", r.first.c_str(),
                    (unsigned long long)r.second);
    }
    if (m->cpu.insnheat && !m->cpu.after_divide_counts.empty()) {
        uint64_t total = 0, reads_zero = 0;
        std::map<std::string, uint64_t> by_name;
        for (const auto &e : m->cpu.after_divide_counts) {
            uint8_t code[MH_JIT_M68K_MAX_BYTES];
            code[0] = (uint8_t)(e.first >> 8);
            code[1] = (uint8_t)e.first;
            for (unsigned k = 2; k < sizeof code; k++) code[k] = (uint8_t)(k * 7);
            m68k_insn insn;
            const char *name =
                m68k_decode(code, sizeof code, 0x1000, &insn)
                    ? m68k_op_name(insn.op) : "?";
            by_name[name] += e.second;
            total += e.second;
            /* The conditions that consult the zero flag, by name: equality
             * either way, and the four orderings that fold it in. */
            static const char *const zero_users[] = {
                "BEQ", "BNE", "DBEQ", "DBNE", "SEQ", "SNE", "TRAPEQ", "TRAPNE",
                "BLE", "BGT", "BLS", "BHI", "DBLE", "DBGT", "DBLS", "DBHI",
                "SLE", "SGT", "SLS", "SHI",
            };
            for (const char *z : zero_users)
                if (!strcmp(name, z)) { reads_zero += e.second; break; }
        }
        fprintf(m->log, "68k: %llu instructions ran straight after a divide; "
                "%llu of them read the zero flag (%.2f%%)\n",
                (unsigned long long)total, (unsigned long long)reads_zero,
                total ? 100.0 * (double)reads_zero / (double)total : 0.0);
        unsigned shown = 0;
        std::vector<std::pair<std::string, uint64_t>> rows(by_name.begin(), by_name.end());
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.second > b.second; });
        for (const auto &r : rows) {
            if (shown++ == 8) break;
            fprintf(m->log, "  after divide: %-10s %llu\n", r.first.c_str(),
                    (unsigned long long)r.second);
        }
    }
    if (std::getenv("MH_68K_DIRECT_DEBUG"))
        fprintf(m->log, "68k: direct memory path: %llu hits, %llu misses\n",
                (unsigned long long)m->cpu.direct_hits,
                (unsigned long long)m->cpu.direct_misses);
    fprintf(m->log, "68k: %llu undecoded reads, %llu undecoded writes, "
            "%llu writes ignored by ROM\n",
            (unsigned long long)c.unknown_reads,
            (unsigned long long)c.unknown_writes,
            (unsigned long long)c.ignored_writes);
    if (!c.ignored_at.empty()) {
        fprintf(m->log, "     writes that hit ROM, by 1M region:\n");
        unsigned shown = 0;
        for (auto &kv : c.ignored_at) {
            if (shown++ >= 8) { fprintf(m->log, "       ...\n"); break; }
            fprintf(m->log, "       %08X  %llu\n", kv.first,
                    (unsigned long long)kv.second);
        }
    }
    if (!c.pc_pages.empty()) {
        uint64_t total = 0;
        for (auto &kv : c.pc_pages) total += kv.second;
        fprintf(m->log, "68k: where the time went, %llu samples over "
                "%zu 64K pages\n", (unsigned long long)total,
                c.pc_pages.size());
        std::vector<std::pair<uint64_t, uint32_t>> byhits;
        for (auto &kv : c.pc_pages) byhits.push_back({kv.second, kv.first});
        std::sort(byhits.rbegin(), byhits.rend());
        unsigned shown = 0;
        for (auto &h : byhits) {
            if (shown++ >= 10) { fprintf(m->log, "       ...\n"); break; }
            fprintf(m->log, "       %08X  %6.2f%%\n", h.second,
                    100.0 * (double)h.first / (double)total);
        }
    }
    /*
     * The interrupt handler tables the OS built.
     *
     * dev21 is an interrupt controller: the service routine at 0x0E0889C4
     * reads a 32-bit pending register, masks it with an enable register,
     * walks the set bits and calls a handler per bit out of a table in low
     * RAM, then writes the remainder back to acknowledge. The tables are
     * eight bytes per entry -- a routine and an argument -- and which
     * entries the OS has filled in is what names the interrupt sources this
     * machine actually uses.
     */
    {
        /*
         * `top` is the bit the dispatcher starts at, and it counts down as
         * it walks the table upward, so entry k carries pending bit top - k.
         * Printing both, with the enable mask beside them, is what turned an
         * index into a wire: the enabled bits and the filled entries agree
         * exactly, which is the check that the mapping is right.
         */
        static const struct {
            uint32_t at; unsigned n, top, pend, en; const char *who;
        } tbl[] = {
            { 0x0510, 31, 30, 0xB0, 0xB4, "IPL5" },
            { 0x0608,  8,  7, 0xBA, 0xC2, "IPL5" },
            { 0x0648, 28, 27, 0xB8, 0xC0, "IPL6" },
        };
        for (auto &t : tbl) {
            unsigned filled = 0;
            const uint32_t en = m->dev21.reg32(t.en);
            fprintf(m->log, "68k: handler table at %04X (%s, pending "
                    "210000%02X, enable 210000%02X = %08X)\n",
                    t.at, t.who, t.pend, t.en, en);
            for (unsigned k = 0; k < t.n; k++) {
                bool ok = true;
                uint32_t fn = mh_bus_read(const_cast<mh_bus *>(&m->bus),
                                           t.at + k * 8, 4, &ok);
                uint32_t arg = mh_bus_read(const_cast<mh_bus *>(&m->bus),
                                            t.at + k * 8 + 4, 4, &ok);
                const unsigned bit = t.top - k;
                if (ok && fn) {
                    filled++;
                    fprintf(m->log, "       entry %2u, bit %2u -> %08X "
                            "(arg %08X)%s%s\n", k, bit, fn, arg,
                            (en >> bit) & 1 ? " enabled" : "",
                            fn == 0x0E08CDDCu ? "  <- timer" : "");
                }
            }
            if (!filled) fprintf(m->log, "       (empty)\n");
        }
    }
    if (m->cover_path) {
        FILE *f = fopen(m->cover_path, "w");
        if (f) {
            for (uint32_t b : c.blocks) fprintf(f, "%08X\n", b);
            fclose(f);
            fprintf(m->log, "68k: %zu distinct 256-byte blocks executed -> %s\n",
                    c.blocks.size(), m->cover_path);
        }
    }
    if (c.watch_write_hi) {
        if (c.watch_write_pcs.empty()) {
            fprintf(m->log, "68k: nothing wrote %08X..%08X in this run\n",
                    c.watch_write_lo, c.watch_write_hi - 1);
        } else {
            fprintf(m->log, "68k: code writing %08X..%08X\n",
                    c.watch_write_lo, c.watch_write_hi - 1);
            for (auto &kv : c.watch_write_pcs)
                fprintf(m->log, "       pc %08X  %llu times\n", kv.first,
                        (unsigned long long)kv.second);
        }
    }
    if (!c.watch_read_pcs.empty()) {
        std::vector<std::pair<uint64_t, uint32_t>> hot;
        for (auto &kv : c.watch_read_pcs) hot.push_back({kv.second, kv.first});
        std::sort(hot.rbegin(), hot.rend());
        fprintf(m->log, "68k: code reading %08X\n", c.watch_read);
        for (unsigned k = 0; k < 10 && k < hot.size(); k++)
            fprintf(m->log, "       pc %08X  %llu times\n", hot[k].second,
                    (unsigned long long)hot[k].first);
    }
    if (!c.read_addrs.empty()) {
        std::vector<std::pair<uint64_t, uint32_t>> hot;
        for (auto &kv : c.read_addrs) hot.push_back({kv.second, kv.first});
        std::sort(hot.rbegin(), hot.rend());
        fprintf(m->log, "68k: most-read RAM addresses\n");
        for (unsigned k = 0; k < 12 && k < hot.size(); k++)
            fprintf(m->log, "       %08X  %llu reads\n", hot[k].second,
                    (unsigned long long)hot[k].first);
    }
    if (!c.write_pages.empty()) {
        uint64_t total = 0;
        for (auto &kv : c.write_pages) total += kv.second;
        std::vector<std::pair<uint64_t, uint32_t>> hot;
        for (auto &kv : c.write_pages) hot.push_back({kv.second, kv.first});
        std::sort(hot.rbegin(), hot.rend());
        fprintf(m->log, "68k: bytes written, %llu total over %zu 4K pages\n",
                (unsigned long long)total, c.write_pages.size());
        unsigned shown = 0;
        for (auto &h : hot) {
            if (shown++ >= 14) { fprintf(m->log, "       ...\n"); break; }
            fprintf(m->log, "       %08X  %6.2f%%  %llu bytes\n", h.second,
                    100.0 * (double)h.first / (double)total,
                    (unsigned long long)h.first);
        }
    }
    if (!c.watch.empty()) {
        fprintf(m->log, "68k: watchpoints\n");
        for (auto &kv : c.watch)
            fprintf(m->log, "       %08X  %llu\n", kv.first,
                    (unsigned long long)kv.second);
    }
    if (c.asserts)
        fprintf(m->log, "68k: %llu of the ROM's own assertions fired (BGND)\n",
                (unsigned long long)c.asserts);
    if (m->timer_fires)
        fprintf(m->log, "68k: dev21 timer compare fired %llu times "
                "(pending bit %u, IPL5)\n",
                (unsigned long long)m->timer_fires, DEV21_TIMER_BIT);
    /* Merge the CPU32 vector counters into the machine's diagnostic map. */
    {
        Cpu &mutable_cpu = const_cast<Cpu &>(c);
        for (unsigned v = 0; v < 256; v++)
            if (m->core.vector_count[v])
                mutable_cpu.vec_count[(int)v] += m->core.vector_count[v];
    }
    if (!c.vec_count.empty()) {
        if (c.suppressed_exceptions)
            fprintf(m->log, "68k: %llu additional exception messages suppressed\n",
                    (unsigned long long)c.suppressed_exceptions);
        fprintf(m->log, "68k: exceptions taken, by vector\n");
        for (auto &kv : c.vec_count) {
            const char *what = "";
            switch (kv.first) {
            case 2:  what = "  bus error"; break;
            case 3:  what = "  address error"; break;
            case 4:  what = "  illegal instruction"; break;
            case 8:  what = "  privilege violation"; break;
            case 10: what = "  line A"; break;
            case 11: what = "  line F (a coprocessor opcode the ROM emulates"
                            " -- or a wild jump; see the lines above)"; break;
            default: break;
            }
            fprintf(m->log, "  %3d  %8llu%s\n", kv.first,
                    (unsigned long long)kv.second, what);
        }
    }
    mc68349_sim_report_chip_selects(&m->sim, m->log);
    pic2000_probe_report(&m->dev21, m->log);
    pic2000_probe_report(&m->dev0c, m->log);
    if (!c.unknown_at.empty()) {
        fprintf(m->log, "     undecoded addresses, by 4K page:\n");
        unsigned shown = 0;
        for (auto &kv : c.unknown_at) {
            if (shown++ >= 16) { fprintf(m->log, "     ...\n"); break; }
            fprintf(m->log, "       %08X  %llu\n", kv.first,
                    (unsigned long long)kv.second);
        }
    }
}

} /* extern "C" */

void mh_m68k_retained_regions(m68k_machine *m, mh_m68k_region r[2])
{
    r[0] = {m->ram, m->ram_len};
    r[1] = {m->xram, m->xram_len};
}
