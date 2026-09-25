#include "frontend/sdl/shell.h"
#include "host/pclink.h"
#include "host/state.h"

#include <unistd.h>
/*
 * mcap — a low-level emulator for the General Magic DataRover 840.
 *
 * Ground rule, enforced by review rather than by code: this program contains
 * no high-level emulation of the Magic Cap OS. There are no patches keyed on
 * ROM addresses, no synthesised call frames, no forced dispatches. Every
 * deviation from hardware is an explicit command-line option, is listed by
 * --show-deviations, and is printed at startup when active.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machines/datarover840/machine.h"
#include "util/portable.h"
#include "frontend/sdl/datarover_gui.h"
#include "host/serial.h"
#include "host/wav.h"

static void usage(FILE *f)
{
    fprintf(f,
"usage: mcap --rom <image> [options]\n"
"\n"
"  --rom <path>          ROM image (MagicCap-USA.image)\n"
"  --temporary           run without writing the device's state\n"
"  --fresh               start from the ROM, ignoring any saved state\n"
"  --headless            do not open a window (implied when there is no\n"
"                        display; scripts and measurements want this)\n"
"  --no-audio            silence the window\n"
"  --no-host-battery     report a healthy battery instead of this machine's\n"
"                        (already the case headless, so runs are repeatable)\n"
"  --lcd                 panel structure: dot grid and STN ghosting\n"
"  --tint <colour>       backlight colour: green|amber|grey|none\n"
"  --smooth              filter the panel when scaling (default: sharp)\n"
"  --integer             scale by whole pixels only (letterboxes more)\n"
"  --touch-debug         draw a cross where the emulator puts the pen\n"
"                        (in the window: F2 saves the screen, F3 the machine)\n"
"  --keyboard            attach Magic Bus keyboard (default in GUI guest mode)\n"
"  --no-keyboard         disconnect keyboard; use SDL typing for UART console\n"
"  --console             bridge the host terminal to UART A (the IDT monitor)\n"
"  --input <text>        feed this text to UART A first (use \\n for newline)\n"
"  --input-after <n>     deliver no input before cycle n\n"
"  --ram <mb>            exposed DRAM, 1..60 MiB (auto: state/ROM, fallback 4)\n"
"  --monitor             boot to the IDT monitor instead of Magic Cap\n"
"  --reset-pc <hex>      reset vector (default 0xBFC00000, architectural)\n"
"  -n <count>            instructions to run (default 1000000)\n"
"  --cpu-engine <name>   auto (default: jit where available), interpreter,\n"
"                        blocks, or jit (native x86-64/AArch64)\n"
"\n"
"diagnostics (observation only; none of these change behaviour):\n"
"  --trace               log every instruction\n"
"  --trace-state F       write binary architectural state per retired instruction\n"
"  --trace-state-count N how many records --trace-state writes (default 1000000)\n"
"  --trace-bus F         write binary record of every data-bus access\n"
"  --log-exceptions <n>  report the first n exceptions and where they came from\n"
"  --log-mmio            log every MMIO access\n"
"  --log-unmapped        log unmapped accesses and ROM writes\n"
"  --log-unknown         log accesses to TX39 registers we do not decode\n"
"  --histogram           print TX39 register traffic at exit\n"
"  --log-codec <n>       log the first n UCB1100 codec transactions\n"
"  --watch-pc <a,b,...>  count executions of these ROM addresses\n"
"  --watch-log <n>       log the first n watchpoint hits with registers\n"
"  --watch-after <n>     start watch logging only after instruction n\n"
"  --watch-write <hex>   log stores to this word (pc, value)\n"
"  --coverage <path>     record which instructions executed; scripts/covdiff\n"
"                        compares two such files to show what an input reached\n"
"  --audio-wav <path>    capture the sound DMA stream to a WAV file\n"
"  --dump-fb <path>      write the framebuffer as PGM at exit\n"
"  --fb-watch <prefix> <n>  also write <prefix>NNNN.pgm every n instructions\n"
"\n"
"input:\n"
"  --tap <x> <y> <at>    touch the panel at raw ADC (x,y) at instruction <at>\n"
"                        (10-bit counts, 0..1023; the OS applies its own\n"
"                        calibration, so these are not pixels)\n"
"  --taps <list>         a sequence of taps: \"x,y,at;x,y,at;...\"\n"
"  --tap-px <x> <y> <at> touch at a SCREEN PIXEL instead of a raw count\n"
"  --taps-px <list>      a sequence of pixel taps, same syntax as --taps\n"

"  --aux-adc <a,b,c,d>   UCB1100 auxiliary ADC channels AD0..AD3 (0..1023),\n"
"                        which carry board voltages such as battery level\n"
"  --mfio-in <hex>       drive the MFIO input word (pin 13 stays the pen)\n"
"  --codec-gpio <list>   drive the codec's ten input pins: \"hex,at;hex,at\"\n"
"  --mfio-at <list>      drive the MFIO input word over time, with its edge\n"
"                        interrupts: \"hex,at;hex,at\"\n"
"  --option-key <list>   press/release Option: \"1,at;0,at\" (instruction counts)\n"
"                        (an instruction each, so software sees the edge)\n"
"  --pcmcia-fill <hex>   fill both Glacier register files with a 16-bit value\n"
"  --serial <a|b>        expose a UART as a host serial port (a pty)\n"
"  --sram1/--sram2 <path> writable SRAM image; creates a blank 2 MiB card if absent\n"
"  --card1 <path>        put a PC Card in slot 1 (its memory image)\n"
"  --power-at <n>      press the power button at slot n (duration --tap-hold)\n"
"  --net <none|user>    userspace NAT; defaults to NE2000 in slot 1\n"
"  --net-pcap <path>    capture Ethernet traffic with --net user\n"
"  --ne2000 <1|2>        NE2000 PC Card (disconnected unless --net user)\n"
"  --modem <1|2>         a data modem PC Card; --modem-pty puts its far end on a pty\n"
"  --log-card <n>        log the first n accesses to a card, with the site\n"
"  --card-at <n>         insert the cards at instruction n rather than at\n"
"                        reset, so the OS sees the detect lines close\n"
"  --card2 <path>        the same for slot 2\n"
"  --drag-px <list>      one drag: x1,y1,x2,y2,at; duration is --tap-hold\n"
"  --tap-hold <n>        instructions to hold each touch (default 2000000)\n"
"  --tap-every <n>       repeat the whole sequence every n instructions\n"
"  --dump-ram <path>     write DRAM at exit\n"
"\n"
"snapshots (booting to the monitor prompt costs ~800M instructions):\n"
"  --install <path>      send a package to the guest over its own serial\n"
"                        link, with no second program involved\n"
"  --save-state <path>   write the whole machine at exit\n"
"  --load-state <path>   start from a saved machine instead of resetting\n"
"\n"
"deviations from hardware (each one weakens the result; see docs/ACCURACY.md):\n"
"  --force-mmu           route kuseg and kseg2/3 through the TLB (this part has none)\n"
"  --show-deviations     list them and exit\n");
}

static void show_deviations(void)
{
    printf(
"Deviations available, and what each one costs you:\n"
"\n"
"  --force-mmu\n"
"      Routes kuseg and kseg2/kseg3 through the TLB. The TMPR3902U has no\n"
"      TLB, so this is wrong for this machine; it exists only so the\n"
"      evidence for that can be re-tested. With it on, the ROM faults at\n"
"      0x83C00568 storing to kseg3 0xFF000010 a few hundred instructions\n"
"      after reset, having never programmed a single TLB entry.\n"
"\n"
"Not offered, deliberately:\n"
"  - patching or skipping instructions at named ROM addresses\n"
"  - calling ROM functions directly with synthesised arguments\n"
"  - injecting method dispatches into the Telescript VM\n"
"  - forcing interrupt or status bits that no device model produced\n"
"\n"
"Those are what made the previous emulator's results impossible to trust.\n"
"If the ROM will not do something, the bug is in a device model.\n");
}

static uint32_t parse_u32(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 0);
}

int mrc_datarover_main(int argc, char **argv)
{
    const char *rom_path = NULL;
    bool temporary = false, fresh = false;
    bool decoded_cpu = false, jit_cpu = false, auto_cpu = true;
    int keyboard = -1; /* auto: GUI guest keyboard, monitor UART */
    const char *dump_fb = NULL, *dump_ram = NULL;
    const char *save_state = NULL, *load_state = NULL, *install = NULL;
    const char *fb_watch = NULL;
    const char *audio_wav = NULL;
    uint64_t fb_watch_every = 0;
    uint64_t tap_hold = 2000000, tap_every = 0;
    const char *taps = NULL, *drag = NULL, *keys = NULL, *load_ram = NULL;
    char tap_one[64] = "";
    bool taps_in_pixels = false;
    const char *aux_adc = NULL;
    bool host_battery = false, host_battery_set = false;
    uint32_t mfio_in = 0;
    const char *gpios = NULL;
    const char *mfio_at = NULL;
    const char *io_at = NULL;
    const char *pwrint_at = NULL;
    const char *option_keys = NULL;
    bool mfio_in_set = false;
    uint32_t pcmcia_fill = 0;
    unsigned log_card = 0;
    uint64_t card_at = 0;
    bool pcmcia_fill_set = false;
    uint32_t ram_bytes = 0;
    const char *ram_arg = NULL;
    const char *card_path[2] = { NULL, NULL };
    const char *sram_path[2] = { NULL, NULL };
    int ne2000_slot = -1;
    int modem_slot = -1;
    bool modem_pty = false;
    static mrc_serial modem_link;
    bool net_user = false;
    bool power_scheduled = false;
    uint64_t power_at = 0;
    const char *net_pcap = NULL;
    int link_uart = -1;
    uint32_t reset_pc = 0xBFC00000u;
    bool reset_pc_given = false;
    bool boot_monitor = false;
    uint64_t insns = 1000000;
    bool insns_set = false;   /* -n given? the GUI runs free without it */
    bool trace = false, log_mmio = false, log_unmapped = false;
    const char *trace_state = NULL, *trace_bus = NULL;
    uint64_t trace_state_count = 1000000;
    uint32_t trace_after_pc = 0; uint64_t trace_after_hit = 1, trace_after_n = 100;
    bool trace_bus_dev = false;
    const char *trace_irq = NULL;
    const char *replay_dev = NULL, *replay_irq = NULL, *replay_take = NULL;
    uint64_t trace_from = 0;
    uint32_t log_exceptions = 0;
    bool use_console = false;
    bool use_gui = true, gui_set = false;
    const char *input = NULL;
    uint64_t input_after = 0;
    bool log_unknown = false, force_mmu = false, histogram = false;
    uint32_t log_codec = 0;
    const char *watch_pc = NULL;
    const char *coverage = NULL;
    uint32_t watch_log = 0;
    uint64_t watch_after = 0;
    uint32_t watch_write = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(stderr), exit(2), ""))
        if (!strcmp(a, "--keyboard")) keyboard = 1;
        else if (!strcmp(a, "--no-keyboard")) keyboard = 0;
        else if (!strcmp(a, "--temporary")) temporary = true;
        else if (!strcmp(a, "--fresh")) fresh = true;
        else if (!strcmp(a, "--rom"))                rom_path = NEXT();
        else if (!strcmp(a, "--ram"))           ram_arg = NEXT();
        else if (!strcmp(a, "--monitor"))     boot_monitor = true;
        else if (!strcmp(a, "--reset-pc"))    { reset_pc = parse_u32(NEXT());
                                                reset_pc_given = true; }
        else if (!strcmp(a, "--audio"))         mrc_gui_audio = true;
        else if (!strcmp(a, "--no-audio"))      mrc_gui_audio = false;
        else if (!strcmp(a, "--headless"))    { use_gui = false; gui_set = true; }
        else if (!strcmp(a, "--no-host-battery")) { host_battery = false;
                                                    host_battery_set = true; }
        else if (!strcmp(a, "--smooth"))        mrc_gui_smooth = true;
        else if (!strcmp(a, "--integer"))       mrc_gui_integer = true;
        else if (!strcmp(a, "--touch-debug"))   mrc_gui_touch_debug = true;
        else if (!strcmp(a, "--serial")) {
            const char *w = NEXT();
            if (!strcmp(w, "a"))      link_uart = 0;
            else if (!strcmp(w, "b")) link_uart = 1;
            else { fprintf(stderr, "--serial: a|b\n"); return 2; }
        }
        else if (!strcmp(a, "--card-at"))       card_at = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--log-card"))      log_card = (unsigned)strtoul(NEXT(), NULL, 0);
        else if (!strcmp(a, "--sram1")) sram_path[0] = NEXT();
        else if (!strcmp(a, "--sram2")) sram_path[1] = NEXT();
        else if (!strcmp(a, "--card1"))         card_path[0] = NEXT();
        else if (!strcmp(a, "--card2"))         card_path[1] = NEXT();
        else if (!strcmp(a, "--power-at")) {
            power_at = strtoull(NEXT(), NULL, 0); power_scheduled = true;
        }
        else if (!strcmp(a, "--net")) {
            const char *mode = NEXT();
            if (!strcmp(mode, "user")) net_user = true;
            else if (!strcmp(mode, "none")) net_user = false;
            else { fprintf(stderr, "--net: none|user\n"); return 2; }
        }
        else if (!strcmp(a, "--net-pcap")) net_pcap = NEXT();
        else if (!strcmp(a, "--modem")) {
            const char *slot = NEXT();
            modem_slot = !strcmp(slot, "2") ? 1 : 0;
        }
        else if (!strcmp(a, "--modem-pty"))      modem_pty = true;
        else if (!strcmp(a, "--ne2000")) {
            const char *slot = NEXT();
            if (!strcmp(slot, "1")) ne2000_slot = 0;
            else if (!strcmp(slot, "2")) ne2000_slot = 1;
            else { fprintf(stderr, "--ne2000: 1|2\n"); return 2; }
        }
        else if (!strcmp(a, "--lcd"))           mrc_gui_lcd = true;
        else if (!strcmp(a, "--tint")) {
            int tint = mrc_tint_by_name(NEXT());
            if (tint < 0) { fprintf(stderr, "--tint: green|amber|grey|none\n"); return 2; }
            mrc_gui_tint = tint;
        }
        else if (!strcmp(a, "-n"))              { insns = strtoull(NEXT(), NULL, 0); insns_set = true; }
        else if (!strcmp(a, "--gui"))         { use_gui = true; gui_set = true; }
        else if (!strcmp(a, "--console"))       use_console = true;
        else if (!strcmp(a, "--input"))         input = NEXT();
        else if (!strcmp(a, "--input-after"))   input_after = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--trace"))         trace = true;
        else if (!strcmp(a, "--trace-after")) { const char *t = NEXT();
            trace_after_pc = parse_u32(t); const char *cm = strchr(t, ',');
            if (cm) { trace_after_hit = strtoull(cm + 1, NULL, 0); cm = strchr(cm + 1, ',');
                      if (cm) trace_after_n = strtoull(cm + 1, NULL, 0); } }
        else if (!strcmp(a, "--trace-state"))   trace_state = NEXT();
        else if (!strcmp(a, "--trace-bus"))     trace_bus = NEXT();
        else if (!strcmp(a, "--trace-bus-dev")) { trace_bus = NEXT(); trace_bus_dev = true; }
        else if (!strcmp(a, "--trace-irq"))     trace_irq = NEXT();
        else if (!strcmp(a, "--replay-dev"))    replay_dev = NEXT();
        else if (!strcmp(a, "--replay-irq"))    replay_irq = NEXT();
        else if (!strcmp(a, "--replay-take"))   replay_take = NEXT();
        else if (!strcmp(a, "--trace-from"))  { const char *t = NEXT();
            trace_from = strtoull(t, NULL, 0); const char *cm = strchr(t, ',');
            if (cm) trace_after_n = strtoull(cm + 1, NULL, 0); }
        else if (!strcmp(a, "--trace-state-count")) trace_state_count = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--log-exceptions")) log_exceptions = parse_u32(NEXT());
        else if (!strcmp(a, "--log-mmio"))      log_mmio = true;
        else if (!strcmp(a, "--log-unmapped"))  log_unmapped = true;
        else if (!strcmp(a, "--log-unknown"))   log_unknown = true;
        else if (!strcmp(a, "--histogram"))     histogram = true;
        else if (!strcmp(a, "--log-codec"))     log_codec = parse_u32(NEXT());
        else if (!strcmp(a, "--watch-pc"))      watch_pc = NEXT();
        else if (!strcmp(a, "--coverage"))      coverage = NEXT();
        else if (!strcmp(a, "--watch-log"))     watch_log = parse_u32(NEXT());
        else if (!strcmp(a, "--watch-after"))   watch_after = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--watch-write"))   watch_write = parse_u32(NEXT());
        else if (!strcmp(a, "--audio-wav"))     audio_wav = NEXT();
        else if (!strcmp(a, "--dump-fb"))       dump_fb = NEXT();
        else if (!strcmp(a, "--tap"))         { const char *x = NEXT();
                                                const char *y = NEXT();
                                                const char *t = NEXT();
                                                snprintf(tap_one, sizeof(tap_one),
                                                         "%s,%s,%s", x, y, t);
                                                taps = tap_one; }
        else if (!strcmp(a, "--taps"))          taps = NEXT();
        else if (!strcmp(a, "--keys"))          keys = NEXT();
        else if (!strcmp(a, "--aux-adc"))       aux_adc = NEXT();
        else if (!strcmp(a, "--host-battery")) { host_battery = true;
                                                host_battery_set = true; }
        else if (!strcmp(a, "--codec-gpio"))    gpios = NEXT();
        else if (!strcmp(a, "--mfio-at"))       mfio_at = NEXT();
        else if (!strcmp(a, "--io-at"))         io_at = NEXT();
        else if (!strcmp(a, "--pwrint-at"))     pwrint_at = NEXT();
        else if (!strcmp(a, "--option-key"))    option_keys = NEXT();
        else if (!strcmp(a, "--mfio-in"))     { mfio_in = parse_u32(NEXT());
                                                mfio_in_set = true; }
        else if (!strcmp(a, "--pcmcia-fill")) { pcmcia_fill = parse_u32(NEXT());
                                                pcmcia_fill_set = true; }
        else if (!strcmp(a, "--tap-px"))      { const char *x = NEXT();
                                                const char *y = NEXT();
                                                const char *t = NEXT();
                                                snprintf(tap_one, sizeof(tap_one),
                                                         "%s,%s,%s", x, y, t);
                                                taps = tap_one;
                                                taps_in_pixels = true; }
        else if (!strcmp(a, "--taps-px"))     { taps = NEXT();
                                                taps_in_pixels = true; }
        else if (!strcmp(a, "--drag-px"))       drag = NEXT();
        else if (!strcmp(a, "--tap-hold"))      tap_hold = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--tap-every"))     tap_every = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--fb-watch"))    { fb_watch = NEXT();
                                                fb_watch_every = strtoull(NEXT(), NULL, 0); }
        else if (!strcmp(a, "--dump-ram"))      dump_ram = NEXT();
        else if (!strcmp(a, "--load-ram"))      load_ram = NEXT();
        else if (!strcmp(a, "--install"))       install = NEXT();
        else if (!strcmp(a, "--save-state"))    save_state = NEXT();
        else if (!strcmp(a, "--load-state"))    load_state = NEXT();
        else if (!strcmp(a, "--force-mmu"))     force_mmu = true;
        else if (!strcmp(a, "--cpu-engine")) {
            const char *engine = NEXT();
            if (strcmp(engine, "interpreter") && strcmp(engine, "blocks") &&
                strcmp(engine, "jit") && strcmp(engine, "auto")) {
                fprintf(stderr, "unknown CPU engine: %s\n", engine);
                return 1;
            }
            decoded_cpu = !strcmp(engine, "blocks");
            jit_cpu = !strcmp(engine, "jit");
            /* "auto" asks for native execution where the host and its
             * sandbox allow it, and accepts the interpreter where they do
             * not. "jit" is a requirement and fails instead. */
            auto_cpu = !strcmp(engine, "auto");
        }
        else if (!strcmp(a, "--show-deviations")) { show_deviations(); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        }
        #undef NEXT
    }

    if (!rom_path) {
        usage(stderr);
        return 2;
    }

    if (ram_arg) {
        char *end;
        unsigned long value = strtoul(ram_arg, &end, 0);
        if (!*ram_arg || *end || ram_arg[0] == '-' || !value ||
            value > DR840_RAM_WINDOW / (1024u * 1024u)) {
            fprintf(stderr, "--ram must be 1..%u MiB; it only sets exposed RAM\n",
                    DR840_RAM_WINDOW / (1024u * 1024u));
            return 2;
        }
        ram_bytes = (uint32_t)value * 1024u * 1024u;
    }

    /*
     * Where this device keeps what it was doing.
     *
     * A device resumes by default, the way a real one does: it was not off,
     * it was put down. --fresh starts it from its ROM instead, --temporary
     * runs it without keeping anything, and --load-state, --reset-pc and
     * --boot-monitor each say to begin somewhere else.
     */
    char device_state[4096];
    /*
     * A caller that named a state is managing it, so the device's own is left
     * alone: --load-state and --save-state are how the tooling drives
     * experiments, and a batch run that wrote a whole machine beside the ROM
     * every time would be a surprise nobody asked for. --temporary says the
     * same thing for a run that named nothing, and --reset-pc and
     * --boot-monitor both say to begin somewhere other than where it stopped.
     */
    bool have_device_state = !temporary && !load_state && !save_state &&
        !reset_pc_given && !boot_monitor &&
        mrc_state_path_for(rom_path, device_state, sizeof(device_state));
    if (have_device_state) mrc_state_set_path(device_state);
    /*
     * Whether the state about to be loaded is the device's own rather than
     * one somebody named. The two fail differently: a state a person asked
     * for by name is the point of the run, and a state a device happens to
     * have is a convenience that the ROM beside it can do without.
     */
    bool state_is_the_device_s = false;
    if (have_device_state && !fresh) {
        FILE *probe = fopen(device_state, "rb");
        if (probe) {
            fclose(probe);
            load_state = device_state;
            state_is_the_device_s = true;
        }
    }

    if (!ram_arg && load_state) {
        /* Unreadable here means unreadable below as well, so a device's own
         * stale state drops out now and the machine is built at its default
         * size rather than at one read out of a file that will not load. */
        if (!mrc_snapshot_ram_size(load_state, &ram_bytes)) {
            if (!state_is_the_device_s) return 1;
            fprintf(stderr, "snapshot: starting \"%s\" from its ROM instead\n",
                    rom_path);
            load_state = NULL;
        }
    }
    if (ram_bytes)
        fprintf(stderr, "RAM: %u bytes (%s)\n", ram_bytes,
                ram_arg ? "--ram" : "saved state");
    machine m;
    if (!mrc_machine_init(&m, rom_path, ram_bytes))
        return 1;

    /* Accept \n escapes in --input so a command line can carry a newline. */
    char *input_cooked = NULL;
    if (input) {
        input_cooked = malloc(strlen(input) + 1);
        char *o = input_cooked;
        for (const char *p = input; *p; p++) {
            if (p[0] == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
            else if (p[0] == '\\' && p[1] == 'r') { *o++ = '\r'; p++; }
            else *o++ = *p;
        }
        *o = 0;
    }
    if (use_console || input_cooked) {
        mrc_console_init(&m.con, use_console, input_cooked, input_after);
        m.con_active = true;
    }

    /*
     * A device whose own state will not load still starts.
     *
     * It is stale rather than precious: a build that lays its devices out
     * differently cannot read what the last one wrote, and that is a reason
     * to boot the ROM, not a reason to refuse. Refusing took the whole
     * session down -- the window is opened above this and closes when this
     * returns, so choosing a device with an unreadable state made the
     * emulator vanish rather than start the machine that was asked for.
     *
     * A state named on the command line is a different matter: it is the
     * point of the run, and quietly booting something else instead would be
     * a worse answer than stopping.
     */
    if (load_state && !mrc_snapshot_load(&m, load_state)) {
        if (!state_is_the_device_s) {
            mrc_machine_free(&m); free(input_cooked); return 1;
        }
        fprintf(stderr, "snapshot: starting \"%s\" from its ROM instead\n",
                rom_path);
        load_state = NULL;
    }
    if (!load_state) {
        if (boot_monitor)
            m.soc.io_ctrl &= ~DR840_IO_BOOT_NORMAL;
        mrc_machine_reset(&m, reset_pc);
    }

    /*
     * After the snapshot, not before: a snapshot restores the whole
     * controller, so a card put in first is taken straight back out again.
     */
    static mrc_serial link;
    static mrc_pclink *installer;
    if (install) {
        /*
         * The emulator is the computer. No pty, no second program: the far
         * end of the guest's serial port is a few hundred lines away in this
         * process, and the guest cannot tell the difference.
         */
        installer = mrc_pclink_open(install);
        if (!installer) return 1;
        mrc_serial_attach(&link, installer);
        mrc_uart_set_link(&m.soc.uart[link_uart < 0 ? 0 : link_uart], &link);
        fprintf(stderr, "install: %s\n", mrc_pclink_message(installer));
    } else if (link_uart >= 0) {
        if (!mrc_serial_open_pty(&link))
            return 1;
        mrc_uart_set_link(&m.soc.uart[link_uart], &link);
    }

    if (net_pcap && !net_user) {
        fprintf(stderr, "--net-pcap requires --net user\n"); return 2;
    }
    if (net_user && ne2000_slot < 0) ne2000_slot = 0;
    /*
     * What the state remembered, for any slot the command line did not speak
     * for. A card is expected to still be in the device when you pick it up
     * again -- that is the whole point of remembering it -- but a run that
     * names a card is making a decision about that slot, and it wins.
     *
     * The image is reopened as it is now, not as it was: the contents belong
     * to the card, and they have been in its own file all along.
     */
    bool recalled[2] = { false, false };
    for (unsigned c = 0; c < 2; c++) {
        bool named = sram_path[c] || card_path[c] || (int)c == ne2000_slot;
        if (named || m.card_kind[c] == MRC_CARD_NONE) continue;
        const char *was = m.card_path[c];
        recalled[c] = true;
        switch (m.card_kind[c]) {
        case MRC_CARD_SRAM:   sram_path[c] = was; break;
        case MRC_CARD_MEMORY: card_path[c] = was; break;
        case MRC_CARD_NE2000:
            ne2000_slot = (int)c;
            /* And what it was plugged into, if anything. */
            if (was && !strcmp(was, "user")) net_user = true;
            else if (was && !strncmp(was, "pcap:", 5)) {
                net_user = true;
                net_pcap = was + 5;
            }
            break;
        default: break;
        }
        if (m.card_kind[c] == MRC_CARD_NE2000)
            fprintf(stderr, "card: slot %u had a network card%s%s when this "
                    "device was put down\n", c + 1, was ? " on " : " with "
                    "nothing behind it", was ? was : "");
        else if (was)
            fprintf(stderr, "card: slot %u had %s when this device was put "
                    "down\n", c + 1, was);
    }
    for (unsigned c = 0; c < 2; c++) {
        if ((sram_path[c] && card_path[c]) || ((int)c == ne2000_slot && (card_path[c] || sram_path[c]))) {
            fprintf(stderr, "slot %u has more than one card configured\n", c + 1);
            return 2;
        }
        /*
         * A card the state remembered may not be there any more: the image was
         * moved, renamed or deleted between sessions. That is a card that is
         * not in the slot, not a reason to refuse to start the device. The
         * machine runs, the slot stays empty, and the guest is simply never
         * told a card arrived.
         *
         * A card named on the command line is a different thing, because this
         * run asked for it by name, and failing to produce it is still fatal.
         * The same line divides creating from not creating: --sram1 makes a
         * blank card when the file is absent, which is how you get one in the
         * first place, but a remembered card must already exist. Recreating
         * an image the user deleted would put a blank card into a device that
         * believed it had a full one.
         */
        if (sram_path[c] && recalled[c] && access(sram_path[c], R_OK | W_OK)) {
            fprintf(stderr, "card: slot %u had %s, which is not there any "
                    "more; the slot is empty\n", c + 1, sram_path[c]);
            sram_path[c] = NULL;
        }
        if (card_path[c] && recalled[c] && access(card_path[c], R_OK)) {
            fprintf(stderr, "card: slot %u had %s, which is not there any "
                    "more; the slot is empty\n", c + 1, card_path[c]);
            card_path[c] = NULL;
        }
        if (sram_path[c] &&
            !mrc_machine_insert_sram(&m, c, sram_path[c], 2u*1024*1024)) {
            if (!recalled[c]) return 1;
            fprintf(stderr, "card: slot %u could not take %s; the slot is "
                    "empty\n", c + 1, sram_path[c]);
        }
        if (card_path[c] && !mrc_machine_insert_card(&m, c, card_path[c])) {
            if (!recalled[c]) return 1;
            fprintf(stderr, "card: slot %u could not take %s; the slot is "
                    "empty\n", c + 1, card_path[c]);
        }
        if ((int)c == ne2000_slot && !mrc_machine_insert_ne2000(&m, c)) return 1;
        if ((int)c == modem_slot) {
            void *far = NULL;
            if (modem_pty) {
                if (!mrc_serial_open_pty(&modem_link)) return 1;
                fprintf(stderr, "modem: far end at %s\n", modem_link.path);
                far = &modem_link;
            }
            if (!mrc_machine_insert_modem(&m, c, far)) return 1;
        }
        if (net_user && (int)c == ne2000_slot &&
            !mrc_machine_network(&m, c, net_pcap)) {
            /* The card is in; only what it plugs into is missing, which is a
             * network card with the cable out rather than a failure to run. */
            if (!recalled[c]) return 1;
            fprintf(stderr, "card: slot %u could not reach the network it had; "
                    "the cable is out\n", c + 1);
        }
        m.card[c].log_first = log_card;
        m.pcmcia[c].log_unknown = log_card != 0;
        m.card_at[c] = card_at;
    }


    /*
     * Diagnostic settings are applied after any snapshot load, because the
     * load restores the CPU and SoC structs wholesale and would otherwise
     * overwrite them with whatever was in force when the state was saved.
     */
    if (trace_irq) mrc_cpu_trace_irq(trace_irq);
    if (replay_dev) mrc_cpu_replay(replay_dev, replay_irq);
    if (replay_take) mrc_cpu_replay_takes(replay_take);
    m.cpu.trace          = trace;
    m.cpu.trace_after_pc = trace_after_pc; m.cpu.trace_after_hit = trace_after_hit;
    m.cpu.trace_after_n  = trace_after_n;
    m.cpu.trace_from     = trace_from;
    if (trace_state)
        mrc_cpu_trace_state(trace_state, trace_state_count);
    if (trace_bus) {
        mrc_cpu_trace_bus_devices_only(trace_bus_dev);
        mrc_cpu_trace_bus(trace_bus, trace_state_count);
    }
    if (jit_cpu || auto_cpu) {
        m.jit = mrc_cpu_jit_create();
        if (!m.jit && jit_cpu) {
            fprintf(stderr, "MIPS JIT requires x86-64 or AArch64 Linux and executable memory support\n");
            mrc_machine_free(&m);
            return 1;
        }
        fprintf(stderr, "cpu: %s\n", m.jit ? "native engine"
                : "interpreter (no executable memory for the native engine)");
    }
    if (decoded_cpu) {
        m.decode_cache = mrc_cpu_decode_cache_create();
        if (!m.decode_cache) {
            fprintf(stderr, "cannot allocate MIPS decode cache\n");
            mrc_machine_free(&m);
            return 1;
        }
    }
    m.cpu.log_exceptions = log_exceptions;
    if (force_mmu)
        m.cpu.has_mmu = true;
    m.bus.log_mmio       = log_mmio;
    m.bus.log_unmapped   = log_unmapped;
    m.soc.log_unknown    = log_unknown;
    m.soc.sib.log_codec  = log_codec;
    m.cpu.watch_log      = watch_log;
    m.cpu.watch_after    = m.cpu.insn_count + watch_after;
    mrc_cpu_watch_write(watch_write);
    if (coverage) {
        m.cpu.coverage_words = 512u * 1024u * 1024u / 4u;
        m.cpu.coverage = calloc(1, m.cpu.coverage_words / 8);
        if (!m.cpu.coverage) {
            fprintf(stderr, "cannot allocate coverage bitmap\n");
            return 1;
        }
    }
    if (watch_pc) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", watch_pc);
        for (char *tok = strtok(buf, ","); tok && m.cpu.watch_n < R3900_MAX_WATCH;
             tok = strtok(NULL, ","))
            m.cpu.watch_pc[m.cpu.watch_n++] = parse_u32(tok);
    }
    if (aux_adc) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", aux_adc);
        unsigned i = 0;
        for (char *tok = strtok(buf, ","); tok && i < 4; tok = strtok(NULL, ","))
            m.soc.sib.codec.aux[i++] = (uint16_t)(parse_u32(tok) & 0x3FF);
        fprintf(stderr, "aux ADC: AD0=%u AD1=%u AD2=%u AD3=%u\n",
                m.soc.sib.codec.aux[0], m.soc.sib.codec.aux[1],
                m.soc.sib.codec.aux[2], m.soc.sib.codec.aux[3]);
    }

    /*
     * A window is the point of the program, so it is the default. Two things
     * override that without being asked for: no display to open it on, and
     * the diagnostics, which exist to be read from a terminal or a file.
     */
    if (use_gui && !gui_set && !mrc_gui_available()) {
        use_gui = false;
        fprintf(stderr, "no display; running headless\n");
    }

    if ((keyboard == 1 || (keyboard < 0 && use_gui && !boot_monitor)) && !m.soc.mbus_port)
        mrc_mbus_connect(&m.soc.mbus,&m.keyboard.port);
    if (keyboard == 0 && m.soc.mbus_port == &m.keyboard.port)
        mrc_mbus_connect(&m.soc.mbus,NULL);
    if (m.soc.mbus_port == &m.keyboard.port) {
        mrc_dr_keyboard_release(&m.keyboard); /* host keys from previous session */
        fprintf(stderr,"Keyboard: Magic Bus AT accessory (reconstructed profile; SDL physical keys)\n");
    }

    /*
     * The host's battery is a nicety for someone watching a window and a
     * hazard for anything that compares framebuffers, because it makes the
     * same command produce different pixels on a different machine -- or on
     * the same one an hour later. So it follows the window: on when there is
     * one, off when there is not, and --host-battery / --no-host-battery to
     * say so explicitly either way.
     */
    if (!host_battery_set)
        host_battery = use_gui;

    if (host_battery) {
        mrc_host_power hp;
        if (mrc_host_power_read(&hp) && hp.has_battery) {
            m.track_host_battery = true;
            m.soc.sib.codec.aux[2] = (uint16_t)mrc_host_power_to_adc(hp.percent);
            fprintf(stderr, "host battery: %d%%%s (via %s) -> AD2 %u\n",
                    hp.percent, hp.on_ac ? ", on AC" : "",
                    mrc_host_power_source(), m.soc.sib.codec.aux[2]);
            /*
             * The OS samples the battery about every eight seconds and only
             * repaints its title-bar gauge when it does. Starting from a
             * snapshot, the gauge on screen is whatever was painted when that
             * snapshot was taken, so it can read full for several seconds
             * before catching up. Say so, because otherwise it looks broken.
             */
            fprintf(stderr, "  (the title-bar gauge repaints on the OS's own "
                    "sampling interval — allow ~10s)\n");
        } else {
            fprintf(stderr, "host battery: none found (via %s); "
                    "leaving the emulated battery at its default\n",
                    mrc_host_power_source());
        }
    }

    if (mfio_in_set) {
        m.soc.io_datain = mfio_in;
        fprintf(stderr, "MFIO input word: %08X\n", mfio_in);
    }
    if (pcmcia_fill_set) {
        for (unsigned sl = 0; sl < 2; sl++)
            for (unsigned r = 0; r < GLACIER_NREG / 2; r++)
                m.pcmcia[sl].reg[r] = (uint16_t)pcmcia_fill;
        fprintf(stderr, "Glacier register files filled with %04X\n",
                pcmcia_fill & 0xFFFF);
    }

    mrc_wav *wav = NULL;
    if (audio_wav) {
        wav = mrc_wav_open(audio_wav, mrc_sib_output_rate_hz(&m.soc.sib));
        if (!wav)
            return 1;
        mrc_sib_set_audio_sink(&m.soc.sib, mrc_wav_sample, wav);
    }

    if (option_keys) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", option_keys);
        for (char *tok = strtok(buf, ";"); tok && m.option_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned down;
            unsigned long long at;
            if (sscanf(tok, "%u,%llu", &down, &at) != 2 || down > 1) {
                fprintf(stderr, "bad --option-key entry: %s\n", tok);
                return 2;
            }
            m.option_key[m.option_n].down = down != 0;
            m.option_key[m.option_n++].at = at;
        }
    }
    if (gpios) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", gpios);
        for (char *tok = strtok(buf, ";"); tok && m.gpio_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned long long lvl = 0, at = 0;
            if (sscanf(tok, "%llx,%llu", &lvl, &at) != 2) {
                fprintf(stderr, "bad --codec-gpio entry: %s\n", tok);
                return 2;
            }
            m.gpio[m.gpio_n].level = (uint16_t)lvl;
            m.gpio[m.gpio_n].at = at;
            m.gpio_n++;
        }
    }
    if (mfio_at) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", mfio_at);
        for (char *tok = strtok(buf, ";"); tok && m.mfio_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned long long lvl = 0, at = 0;
            if (sscanf(tok, "%llx,%llu", &lvl, &at) != 2) {
                fprintf(stderr, "bad --mfio-at entry: %s\n", tok);
                return 2;
            }
            m.mfio_sched[m.mfio_n].level = (uint32_t)lvl;
            m.mfio_sched[m.mfio_n].at = at;
            m.mfio_n++;
        }
    }
    if (pwrint_at) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", pwrint_at);
        for (char *tok = strtok(buf, ";"); tok && m.pwrint_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned long long lvl = 0, at = 0;
            if (sscanf(tok, "%llx,%llu", &lvl, &at) != 2) {
                fprintf(stderr, "bad --pwrint-at entry: %s\n", tok);
                return 2;
            }
            m.pwrint_sched[m.pwrint_n].level = (uint32_t)lvl;
            m.pwrint_sched[m.pwrint_n].at = at;
            m.pwrint_n++;
        }
    }
    if (io_at) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", io_at);
        for (char *tok = strtok(buf, ";"); tok && m.io_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned long long lvl = 0, at = 0;
            if (sscanf(tok, "%llx,%llu", &lvl, &at) != 2) {
                fprintf(stderr, "bad --io-at entry: %s\n", tok);
                return 2;
            }
            m.io_sched[m.io_n].level = (uint32_t)lvl;
            m.io_sched[m.io_n].at = at;
            m.io_n++;
        }
    }
    if (drag && (taps || tap_hold < 4)) {
        fprintf(stderr, "--drag-px requires --tap-hold >= 4 and cannot accompany taps\n");
        return 2;
    }
    if (drag) {
        unsigned x, y, ex, ey; unsigned long long at; char extra;
        if (sscanf(drag, "%u,%u,%u,%u,%llu%c", &x, &y, &ex, &ey, &at, &extra) != 5 ||
            x >= 480 || ex >= 480 || y >= 320 || ey >= 320) {
            fprintf(stderr, "bad --drag-px entry: %s\n", drag);
            return 2;
        }
        mrc_panel_px_to_raw(x, y, &m.tap[0].x, &m.tap[0].y);
        mrc_panel_px_to_raw(ex, ey, &m.tap[0].end_x, &m.tap[0].end_y);
        m.tap[0].at = at;
        m.tap[0].drag = true;
        m.tap_n = 1;
    }
    m.power_scheduled = power_scheduled;
    m.power_at = power_at;
    m.tap_hold           = tap_hold;
    m.tap_period         = tap_every;
    if (taps) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", taps);
        for (char *tok = strtok(buf, ";"); tok && m.tap_n < MRC_MAX_TAPS;
             tok = strtok(NULL, ";")) {
            unsigned x, y; unsigned long long at;
            if (sscanf(tok, "%u,%u,%llu", &x, &y, &at) != 3) {
                fprintf(stderr, "bad --taps entry: %s\n", tok);
                return 2;
            }
            if (taps_in_pixels)
                mrc_panel_px_to_raw(x, y, &m.tap[m.tap_n].x, &m.tap[m.tap_n].y);
            else {
                m.tap[m.tap_n].x = (uint16_t)x;
                m.tap[m.tap_n].y = (uint16_t)y;
            }
            m.tap[m.tap_n].at = at;
            m.tap_n++;
        }
    }
    if (keys) {
        /* "code,ext,down,at;..." -- the code in hex, as the harness takes it. */
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", keys);
        for (char *tok = strtok(buf, ";"); tok && m.key_n < 64; tok = strtok(NULL, ";")) {
            unsigned code, ext, down; unsigned long long at;
            if (sscanf(tok, "%x,%u,%u,%llu", &code, &ext, &down, &at) != 4) {
                fprintf(stderr, "bad --keys entry: %s\n", tok);
                return 2;
            }
            m.key[m.key_n].code = (uint8_t)code; m.key[m.key_n].ext = ext != 0;
            m.key[m.key_n].down = down != 0; m.key[m.key_n].at = at; m.key_n++;
        }
    }
    if (load_ram) {
        /* Retained memory: the RAM as some earlier run left it, under a
         * reset -- what a device with its standby power intact does. */
        FILE *f = fopen(load_ram, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", load_ram); return 2; }
        size_t got = fread(m.ram, 1, m.ram_size, f);
        fclose(f);
        fprintf(stderr, "RAM <- %s (%zu bytes)\n", load_ram, got);
        m.soc.power.warm = true;
    }
    m.fb_watch_prefix    = fb_watch;
    m.fb_watch_every     = fb_watch ? fb_watch_every : 0;

    fprintf(stderr, "mcap — DataRover 840 (TMPR3902U @ %u.%03u MHz)\n",
            DR840_CPU_HZ / 1000000, (DR840_CPU_HZ / 1000) % 1000);
    fprintf(stderr, "ROM  : %s (%u bytes)\n", rom_path, m.rom_size);
    mrc_bus_print_map(&m.bus, stderr);
    if (load_state) fprintf(stderr, "Resumed from %s (no reset)\n", load_state);
    else fprintf(stderr, "Reset PC: %08X%s\n", reset_pc,
                 reset_pc_given ? "" : " (architectural default)");
    fprintf(stderr, "MMU: %s\n", m.cpu.has_mmu
            ? "TLB enabled (DEVIATION: this part has none)"
            : "none — kuseg and kseg2/3 map straight through");
    fprintf(stderr, "---\n");

    int gui_result = 0;
    if (use_gui) {
        /*
         * A window with no -n should keep running until it is closed.
         * The headless default of a million instructions is about two
         * frames, which looked exactly like an instant crash.
         */
        if (!mrc_shell_current()) {
            fprintf(stderr, "no window was opened for this machine\n");
            gui_result = 1;
        } else {
            gui_result = mrc_gui_run(&m, insns_set ? insns : 0,
                                     mrc_shell_current());
        }
    } else {
        mrc_machine_run(&m, insns, 1024);
    }

    fprintf(stderr, "\n---\n");
    mrc_cpu_dump(&m.cpu, stderr);
    fprintf(stderr,
            "bus: %" PRIu64 " reads, %" PRIu64 " writes, %" PRIu64 " MMIO reads, "
            "%" PRIu64 " MMIO writes, %" PRIu64 " floating reads, "
            "%" PRIu64 " flash writes, %" PRIu64 " faults\n",
            m.bus.reads, m.bus.writes, m.bus.mmio_reads, m.bus.mmio_writes,
            m.bus.float_reads, m.bus.flash_writes, m.bus.faults);
    for (unsigned i = 0; i < 2; i++) {
        const tx39_uart *u = &m.soc.uart[i];
        fprintf(stderr, "uart%c: %" PRIu64 " bytes out, %" PRIu64 " bytes in, "
                "%" PRIu64 " rxhold reads, %" PRIu64 " ctrl1 reads\n",
                'A' + i, u->tx_bytes, u->rx_delivered, u->rxhold_reads,
                u->ctrl1_reads);
    }
    fprintf(stderr, "tx39: %" PRIu64 " undecoded reads, %" PRIu64 " undecoded writes\n",
            m.soc.unknown_reads, m.soc.unknown_writes);
    for (unsigned sl = 0; sl < 2; sl++) {
        mrc_pccard_report(&m.card[sl]);
        if (log_card) {
            fprintf(stderr, "glacier%u registers:", sl + 1);
            for (unsigned r = 0; r < GLACIER_NREG / 2; r++)
                fprintf(stderr, " %02X=%04X", r * 2, m.pcmcia[sl].reg[r]);
            fprintf(stderr, "\n");
        }
        if (m.pcmcia[sl].unknown_reads || m.pcmcia[sl].unknown_writes)
            fprintf(stderr, "glacier%u: %" PRIu64 " undecoded reads, %" PRIu64 " undecoded writes\n",
                    sl + 1, m.pcmcia[sl].unknown_reads, m.pcmcia[sl].unknown_writes);
    }
    {
        static const char *const ipname[8] = {
            "SW0", "SW1", "IP2", "IP3", "IP4", "IP5", "IP6", "IP7" };
        char drv[64] = "", ena[64] = "";
        for (unsigned i = 0; i < 8; i++) {
            if (m.cpu.irq_ever_asserted & (1u << i))
                { strcat(drv, ipname[i]); strcat(drv, " "); }
            if (m.cpu.im_ever_enabled & (1u << i))
                { strcat(ena, ipname[i]); strcat(ena, " "); }
        }
        fprintf(stderr, "irq: devices drove [%s], software enabled [%s], "
                "%" PRIu64 " taken\n",
                drv[0] ? drv : "none", ena[0] ? ena : "none", m.cpu.irqs_taken);
    }
    fprintf(stderr, "cpu: %" PRIu64 " exceptions; TLB %s used by this ROM\n",
            m.cpu.exc_count, m.cpu.tlb_used ? "WAS" : "was NOT");

    if (coverage && m.cpu.coverage) {
        FILE *cf = fopen(coverage, "wb");
        if (cf) {
            fwrite(m.cpu.coverage, 1, m.cpu.coverage_words / 8, cf);
            fclose(cf);
            uint64_t n = 0;
            for (uint32_t i = 0; i < m.cpu.coverage_words / 8; i++)
                n += (unsigned)MRC_POPCOUNT(m.cpu.coverage[i]);
            fprintf(stderr, "coverage: %" PRIu64 " distinct instructions -> %s\n",
                    n, coverage);
        }
    }

    bool save_ok = !save_state || mrc_snapshot_save(&m, save_state);
    /*
     * Putting the device down saves it, with nothing to press and nothing to
     * confirm: the next launch picks up here.
     *
     * Best effort, and deliberately not part of the exit status. A machine
     * with a writable SRAM card or a network card in it cannot be snapshotted
     * -- mrc_snapshot_save says which -- and a run like that used to keep its
     * retained memory instead. Failing the process over it would break those
     * runs for the sake of a save they never asked for.
     */
    if (have_device_state && !gui_result) mrc_snapshot_save(&m, device_state);
    for (unsigned i = 0; i < 2; i++)
        if (!mrc_card_image_flush(&m.storage[i])) save_ok = false;

    for (unsigned i = 0; i < m.cpu.watch_n; i++)
        fprintf(stderr, "watch %08X: %" PRIu64 " hits\n", m.cpu.watch_pc[i],
                m.cpu.watch_hits[i]);

    if (wav) {
        fprintf(stderr, "audio: %llu samples captured -> %s\n",
                (unsigned long long)mrc_wav_samples(wav), audio_wav);
        mrc_wav_close(wav);
    }

    if (histogram) {
        mrc_tx39_print_histogram(&m.soc, stderr, 16);
        mrc_sib_print_codec_traffic(&m.soc.sib, stderr);
    }

    if (dump_fb)
        mrc_machine_dump_fb(&m, dump_fb);
    if (dump_ram) {
        FILE *f = fopen(dump_ram, "wb");
        if (f) {
            fwrite(m.ram, 1, m.ram_size, f);
            fclose(f);
            fprintf(stderr, "DRAM -> %s (%u bytes)\n", dump_ram, m.ram_size);
        }
    }

    if (m.con_active)
        mrc_console_shutdown(&m.con);
    if (installer) {
        fprintf(stderr, "install: %s (%u of %u bytes sent)\n",
                mrc_pclink_message(installer), mrc_pclink_sent(installer),
                mrc_pclink_total(installer));
        if (mrc_pclink_state_of(installer) != MRC_PCLINK_DONE) save_ok = false;
        mrc_pclink_close(installer);
    }
    free(input_cooked);
    if (gui_result) save_ok = false;
    mrc_machine_free(&m);
    return save_ok ? 0 : 1;
}
