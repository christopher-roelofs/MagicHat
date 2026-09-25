/*
 * The Magic Cap 1.x machines, starting with the Sony PIC-2000.
 *
 * Deliberately a separate translation unit from the DataRover's CLI rather
 * than a mode of it. The two share a bus and a method and nothing else: a
 * different CPU, a different board, and a version of the operating system
 * four years apart. Keeping them apart means work here cannot destabilise a
 * DataRover that boots.
 *
 * They are the same *program*, though. This used to be its own binary as
 * well, and that second entry point silently missed everything the launcher
 * grew -- devices, the chooser, switching -- because it never went through
 * it. One emulator, reached as `mcap --device pic2000` when a ROM cannot
 * identify itself.
 */
#include "machines/pic2000/machine68k.h"
#include "frontend/sdl/gui68k.h"
#include "frontend/sdl/shell.h"
#include "frontend/sdl/look.h"
#include "host/state.h"
#include "host/pclink.h"
#include "host/wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf(
"usage: mcap --rom <image> [options]   (68k machines)\n"
"\n"
"  --rom <path>          a Magic Cap 68k ROM (PIC-2000, PIC-1000, HIX-300,\n"
"                        Envoy). PIC-2000 and experimental Envoy support.\n"
"  --ram <mb>            DRAM size in MB (default 4; not yet measured)\n"
"  --sram1 <path>        attach writable SRAM card in 68k slot 1 (creates\n"
"                        a blank 2 MiB image if absent)\n"
"  --sram2 <path>        attach writable SRAM card in 68k slot 2\n"
"  --gui                 open the interactive PIC-2000 display (no limit\n"
"                        unless -n is also supplied)\n"
"  --temporary           run without writing the device's state\n"
"  --fresh               start from the ROM, ignoring any saved state\n"
"  --keyboard            attach a Magic Bus keyboard (GUI default)\n"
"  --no-keyboard         disconnect the Magic Bus keyboard\n"
"  --load-state <path>   restore a whole machine saved earlier; without it\n"
"                        the device resumes from its own .state if it has one\n"
"  --save-state <path>   write the whole machine out when the run ends; the\n"
"                        device's own .state is written anyway\n"
"  --ac-adapter on|off    Envoy adapter connection; overrides host AC status\n"
"  --power-at <n>         press power at slot n (hold 1000000 slots)\n"
"  --headless            run without a window (fixed simulated battery readings)\n"
"  --no-host-battery     disable host battery/adapter polling in the GUI\n"
"  --wav <path>          capture PIC speaker output (headless)\n"
"  --dump-fb <path>      write the 480x320 guest framebuffer as a PGM\n"
"  --install <path>      offer a package over the guest's PC Link\n"
"                        headless: stop after transfer and 1s guest settling;\n"
"                        -n is the maximum instruction budget\n"
"  --serial a           experimental channel-A PTY (not Internet support)\n"
"  --net user           connect channel A to experimental PPP/libslirp NAT\n"
"  --net ne2000         experimental PIC-2000 NE2000 driver probe on CS3\n"
"  --net-pcap <path>    capture PPP or NE2000 Ethernet traffic\n"
"  --audio/--no-audio    enable/disable SDL speaker output (default on)\n"
"  -n <count>            instructions to run (default 1000000)\n"
"  --trace <n>           log the first n instructions\n"
"  --trace-after <pc,n>  log n instructions starting when pc is reached\n"
"  --trace-after <pc,hit,n>  start on a particular execution of pc\n"
"  --cpi <n>             approximate CPU cycles per retired instruction\n"
"  --lcd                 panel structure: dot grid and STN ghosting\n"
"  --tint <colour>       backlight colour: green|amber|grey|none\n"
"  --smooth              filter the scaled panel instead of doubling pixels\n"
"  --integer             scale the panel by whole pixels only\n"
"  --cpu-engine <name>   auto/jit (default), blocks, or interpreter for\n"
"                        the project-owned CPU32 core\n"
"  --quiet-unknown       do not print undecoded accesses as they happen\n"
"  --sample <n>          sample the PC every n instructions; the spread over\n"
"                        64K pages tells progress from a busy loop\n"
"\n"
"  --write-heat          report bytes written per 4K page at exit\n"
"  --coverage <path>     record which 256-byte blocks executed\n"
"  --read-heat           report the most-read RAM addresses at exit\n"
"  --insn-heat           report which instructions were executed, at exit\n"
"  --watch-read <addr>   report which instructions read that address\n"
"  --watch-write <a,b>   report which instructions write in [a, b)\n"
"  --dump <addr,len,path>  write memory to a file at exit\n"
"  --watch-pc <a,b,...>  count executions of these addresses\n"
"  --watch-log <n>       print registers at the first n watchpoint hits\n"
"\n"
"  --no-pic-audio-approx disable approximate PIC volume/filter response\n"
"deviations from hardware (these weaken any result that uses them):\n"
"  --experimental-audio-divider  test inferred rates for unknown audio codes\n"
"  --force-irq <lvl,at>  assert an interrupt level that no device drove, to\n"
"                        test whether the machine is waiting for one\n"
"  --adc <n>             make the converter at dev21 +0xE4 return n for every\n"
"                        channel; nothing here senses anything\n"
"  --touch <at,len>      inject a center-area pen press at instruction at and\n"
"                        release it after len instructions\n"
"  --tap <at,len,x,y>    inject a tap at a 480x320 screen coordinate; may be\n"
"                        repeated to drive calibration\n"
"  --trace-conv          log the pen driver's converter starts and reads\n"
"  --adc-chan <m:c=v,...>  override a mux/channel reading used by --touch\n"
"  --probe-preset <d:o=v>  make a register of an unnamed device read back a\n"
"                        value (dev21:EE=00C0), to test whether a gate the\n"
"                        boot code branches on matters. A deviation, not a\n"
"                        model: the run announces it.\n");
}

int mrc_pic2000_main(int argc, char **argv)
{
    struct tap_option {
        unsigned long long at, len;
        unsigned x, y;
    };
    const char *rom = NULL;
    const char *sram_path[2] = {NULL, NULL};
    const char *engine = "jit";
    const char *wav_path = NULL;
    const char *dump_fb = NULL;
    bool temporary = false, fresh = false, audio_approx = true, audio_divider = false;
    unsigned ram_mb = 4;
    unsigned long long insns = 1000000, trace = 0, sample = 0, irq_at = 0;
    unsigned long long trace_after_count = 0, trace_after_hit = 1;
    unsigned long trace_after_pc = 0;
    unsigned irq_level = 0, watch_log = 0, cpi = 0;
    int adc = -1, adapter = -1, keyboard = -1;
    unsigned long long touch_at = 0, touch_len = 0;
    struct tap_option taps[16];
    unsigned tap_count = 0;
    bool trace_conv = false;
    const char *adcmux = NULL;
    const char *watch = NULL;
    bool heat = false, readheat = false, insnheat = false;
    uint32_t wread = 0;
    const char *wwrite = NULL;
    const char *dump = NULL, *cover = NULL, *preset = NULL;
    const char *load_state = NULL, *save_state = NULL;
    const char *install_path = NULL;
    bool serial_a = false;
    bool ppp_network = false;
    bool ne2000_probe = false;
    const char *net_pcap = NULL;
    bool quiet = false, gui = false, insns_set = false;
    bool power_scheduled = false;
    uint64_t power_at = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--temporary")) temporary = true;
        else if (!strcmp(a, "--fresh")) fresh = true;
        else if (!strcmp(a, "--keyboard")) keyboard = 1;
        else if (!strcmp(a, "--no-keyboard")) keyboard = 0;
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(a, "--save-state") && i + 1 < argc) save_state = argv[++i];
        else if (!strcmp(a, "--install") && i + 1 < argc) install_path = argv[++i];
        else if (!strcmp(a, "--serial") && i + 1 < argc) {
            if (strcmp(argv[++i], "a")) {
                fprintf(stderr, "68k: --serial supports only experimental channel a\n");
                return 2;
            }
            serial_a = true;
        }
        else if (!strcmp(a, "--net") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "user")) ppp_network = true;
            else if (!strcmp(value, "ne2000")) ne2000_probe = true;
            else {
                fprintf(stderr, "68k: --net supports user or ne2000\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--net-pcap") && i + 1 < argc) net_pcap = argv[++i];
        else if (!strcmp(a, "--ac-adapter") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "on")) adapter = 1;
            else if (!strcmp(value, "off")) adapter = 0;
            else { fprintf(stderr, "--ac-adapter expects on or off\n"); return 2; }
        }
        else if (!strcmp(a, "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--audio-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--dump-fb") && i + 1 < argc) dump_fb = argv[++i];
        else if (!strcmp(a, "--audio")) mrc_gui68k_set_audio(true);
        else if (!strcmp(a, "--pic-audio-approx")) audio_approx = true;
        else if (!strcmp(a, "--experimental-audio-divider")) audio_divider = true;
        else if (!strcmp(a, "--no-pic-audio-approx")) audio_approx = false;
        else if (!strcmp(a, "--no-audio")) mrc_gui68k_set_audio(false);
        else if (!strcmp(a, "--no-host-battery")) mrc_gui68k_set_host_battery(false);
        else if (!strcmp(a, "--power-at") && i + 1 < argc) {
            power_at = strtoull(argv[++i], NULL, 0); power_scheduled = true;
        }
        else if (!strcmp(a, "--rom") && i + 1 < argc)          rom = argv[++i];
        else if (!strcmp(a, "--sram1") && i + 1 < argc)       sram_path[0] = argv[++i];
        else if (!strcmp(a, "--sram2") && i + 1 < argc)       sram_path[1] = argv[++i];
        else if (!strcmp(a, "--ram") && i + 1 < argc)     ram_mb = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "-n") && i + 1 < argc) {
            insns = strtoull(argv[++i], NULL, 0); insns_set = true;
        }
        else if (!strcmp(a, "--gui"))                     gui = true;
        else if (!strcmp(a, "--headless"))                gui = false;
        else if (!strcmp(a, "--trace") && i + 1 < argc)   trace = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--trace-after") && i + 1 < argc) {
            const char *v = argv[++i]; trace_after_pc = strtoul(v, NULL, 0);
            const char *c = strchr(v, ',');
            char *end = NULL;
            if (c) trace_after_count = strtoull(c + 1, &end, 0);
            if (end && *end == ',') {
                trace_after_hit = trace_after_count;
                trace_after_count = strtoull(end + 1, &end, 0);
            }
            if (!c || !trace_after_pc || !trace_after_hit ||
                !trace_after_count || !end || *end) {
                fprintf(stderr, "bad --trace-after (want pc,count or pc,hit,count)\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--force-irq") && i + 1 < argc) {
            const char *v = argv[++i]; irq_level = (unsigned)strtoul(v, NULL, 0);
            const char *c = strchr(v, ','); irq_at = c ? strtoull(c + 1, NULL, 0) : 0;
        }
        else if (!strcmp(a, "--cpi") && i + 1 < argc)      cpi = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--adc") && i + 1 < argc)      adc = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--touch") && i + 1 < argc) {
            const char *v = argv[++i]; touch_at = strtoull(v, NULL, 0);
            const char *c = strchr(v, ','); touch_len = c ? strtoull(c + 1, NULL, 0) : 0;
        }
        else if (!strcmp(a, "--tap") && i + 1 < argc) {
            unsigned long long at = 0, len = 0; unsigned x = 0, y = 0;
            if (tap_count == 16 ||
                sscanf(argv[++i], "%llu,%llu,%u,%u", &at, &len, &x, &y) != 4 ||
                !at || !len || x >= 480 || y >= 320) {
                fprintf(stderr, "bad --tap (want at,len,x,y within 480x320)\n");
                return 2;
            }
            taps[tap_count++] = (struct tap_option){at, len, x, y};
        }
        else if (!strcmp(a, "--trace-conv"))               trace_conv = true;
        else if (!strcmp(a, "--adc-chan") && i + 1 < argc) adcmux = argv[++i];
        else if (!strcmp(a, "--probe-preset") && i + 1 < argc) preset = argv[++i];
        else if (!strcmp(a, "--read-heat"))                 readheat = true;
        else if (!strcmp(a, "--insn-heat"))                 insnheat = true;
        else if (!strcmp(a, "--watch-write") && i + 1 < argc) wwrite = argv[++i];
        else if (!strcmp(a, "--watch-read") && i + 1 < argc) wread = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--coverage") && i + 1 < argc)  cover = argv[++i];
        else if (!strcmp(a, "--dump") && i + 1 < argc)      dump = argv[++i];
        else if (!strcmp(a, "--write-heat"))                heat = true;
        else if (!strcmp(a, "--watch-pc") && i + 1 < argc)  watch = argv[++i];
        else if (!strcmp(a, "--watch-log") && i + 1 < argc)  watch_log = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--sample") && i + 1 < argc) sample = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--quiet-unknown"))           quiet = true;
        else if (!strcmp(a, "--lcd"))      mrc_gui_lcd = true;
        else if (!strcmp(a, "--smooth"))   mrc_gui_smooth = true;
        else if (!strcmp(a, "--integer"))  mrc_gui_integer = true;
        else if (!strcmp(a, "--tint") && i + 1 < argc) {
            int tint = mrc_tint_by_name(argv[++i]);
            if (tint < 0) {
                fprintf(stderr, "--tint: green|amber|grey|none\n");
                return 2;
            }
            mrc_gui_tint = tint;
        }
        else if (!strcmp(a, "--cpu-engine") && i + 1 < argc) engine = argv[++i];
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(); return 2; }
    }
    if (!rom) { usage(); return 2; }
    if (ppp_network && ne2000_probe) {
        fprintf(stderr, "68k: select one --net backend\n");
        return 2;
    }
    if (serial_a && ppp_network) {
        fprintf(stderr, "68k: --serial a and --net user select the same channel-A port\n");
        return 2;
    }
    if (ne2000_probe && sram_path[1]) {
        fprintf(stderr, "68k: --net ne2000 uses slot 2; cannot combine it with --sram2\n");
        return 2;
    }

    m68k_machine *m = mrc_m68k_new(rom, ram_mb, stderr);
    if (!m) return 1;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (sram_path[slot] && !mrc_m68k_insert_sram(m, slot, sram_path[slot],
                                                     2u * 1024u * 1024u)) {
            fprintf(stderr, "68k: could not attach --sram%u %s\n", slot + 1,
                    sram_path[slot]);
            mrc_m68k_free(m);
            return 1;
        }
    }
    if (adapter >= 0 && !mrc_m68k_set_adapter(m, adapter != 0)) {
        fprintf(stderr, "--ac-adapter is currently supported for Envoy only\n");
        mrc_m68k_free(m); return 2;
    }
    if (trace) mrc_m68k_trace(m, trace);
    if (trace_after_count)
        mrc_m68k_trace_after_hit(m, (uint32_t)trace_after_pc,
                                 trace_after_hit, trace_after_count);
    if (!mrc_m68k_set_engine(m, engine)) {
        /* Invalid engine names have not changed machine state. */
        fprintf(stderr, "no %s engine for the 68k machines\n", engine);
        mrc_m68k_free(m); return 2;
    }
    if (cpi) mrc_m68k_set_cpi(m, cpi);
    if (adc >= 0) mrc_m68k_set_adc(m, adc);
    if (adcmux) {
        /* chan=value,chan=value -- channel decimal, value decimal or 0x. */
        char buf[256]; snprintf(buf, sizeof(buf), "%s", adcmux);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            unsigned mx = 0, ch = 0; int v = 0;
            if (sscanf(t, "%x:%u=%i", &mx, &ch, &v) == 3)
                mrc_m68k_set_adc_pair(m, mx, ch, v);
            else if (sscanf(t, "%u=%i", &ch, &v) == 2)
                mrc_m68k_set_adc_chan(m, ch, v);
            else { fprintf(stderr, "bad --adc-chan '%s'\n", t); return 2; }
        }
    }
    if (audio_divider) mrc_m68k_audio_divider(m, true);
    mrc_m68k_audio_approx(m, audio_approx);
    if (audio_approx)
        fprintf(stderr, "68k audio: volume curve and output filter are unverified approximations\n");
    if (quiet) mrc_m68k_log_unknown(m, false);
    if (sample) mrc_m68k_sample(m, sample);
    if (irq_level) mrc_m68k_force_irq(m, irq_level, irq_at);
    if (watch) {
        char buf[256]; snprintf(buf, sizeof(buf), "%s", watch);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
            mrc_m68k_watch(m, (uint32_t)strtoul(t, NULL, 0));
    }
    if (watch_log) mrc_m68k_watch_log(m, watch_log);
    if (heat) mrc_m68k_heat(m, true);
    if (cover) mrc_m68k_cover(m, cover);
    if (readheat) mrc_m68k_readheat(m, true);
    if (insnheat) mrc_m68k_insnheat(m, true);
    if (wread) mrc_m68k_watch_read(m, wread);
    if (wwrite) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(wwrite, "%lx,%lx", &lo, &hi) == 2)
            mrc_m68k_watch_write(m, (uint32_t)lo, (uint32_t)hi);
        else fprintf(stderr, "bad --watch-write (want lo,hi)\n");
    }

    /*
     * Where this device keeps what it was doing.
     *
     * A device resumes by default, the way a real one does: it was not off,
     * it was put down. --fresh starts it from its ROM instead, and
     * --temporary runs it without keeping anything. --load-state names a
     * different state and takes precedence over the device's own.
     */
    char device_state[4096];
    /*
     * A caller that named a state is managing it, so the device's own is
     * left alone: --load-state and --save-state are how the tooling drives
     * experiments, and a batch run that wrote 8 MB beside the ROM every time
     * would be a surprise nobody asked for. --temporary says the same thing
     * for a run that named nothing.
     */
    bool have_device_state = !temporary && !load_state && !save_state &&
        mrc_state_path_for(rom, device_state, sizeof(device_state));
    if (have_device_state) mrc_state_set_path(device_state);
    /* An explicit output state is also a live UI save destination.  Without
     * registering it here the CLI would save on exit, but the 68k storage
     * button would be hidden during a GUI run. */
    if (save_state) mrc_state_set_path(save_state);
    /* A GUI session restored from a state should save back to that state when
     * the storage button is pressed. An explicit --save-state still wins. */
    else if (load_state) mrc_state_set_path(load_state);
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

    uint64_t before = mrc_m68k_insns(m);
    /*
     * A restored machine is already on, and must not be started.
     *
     * Launching a device is a press of its ON button; doing that to a machine
     * that is already running is a second press, which drives the button
     * input, raises its interrupt and re-arms the boot hold. Two runs that
     * should have been identical ended up at different program counters five
     * million instructions later, on an otherwise byte-exact state.
     *
     * The load itself goes here rather than beside the other options so that
     * nothing after it touches the machine, and after the engine is chosen so
     * that whichever one is running has its block cache dropped. It comes
     * after the battery RAM for the same reason: a state carries the retained
     * store as it was.
     */
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
    if (load_state && !mrc_m68k_load_state(m, load_state)) {
        if (!state_is_the_device_s) { mrc_m68k_free(m); return 1; }
        fprintf(stderr, "68k: starting \"%s\" from its ROM instead\n", rom);
        load_state = NULL;
    }
    if (load_state) before = mrc_m68k_insns(m);
    else mrc_m68k_start(m);
    if (preset) {
        /* Apply after restore so a diagnostic input can vary between runs
         * from the same state. dev:off=value; offset/value are hex. */
        char dev[16] = {0};
        unsigned off = 0, val = 0;
        int end = 0;
        if (sscanf(preset, "%15[^:]:%4x=%4x%n", dev, &off, &val, &end) != 3 ||
            preset[end] || val > 0xFFFF ||
            !mrc_m68k_probe_preset(m, dev, off, (uint16_t)val)) {
            fprintf(stderr, "bad --probe-preset '%s'; want dev21:EE=00C0\n",
                    preset);
            mrc_m68k_free(m);
            return 2;
        }
    }
    if (serial_a && !mrc_m68k_open_serial_a(m)) {
        mrc_m68k_free(m);
        return 1;
    }
    if (net_pcap && !ppp_network && !ne2000_probe) {
        fprintf(stderr, "68k: --net-pcap requires --net user or ne2000\n");
        mrc_m68k_free(m);
        return 2;
    }
    if (ppp_network && !mrc_m68k_open_ppp(m, net_pcap)) {
        fprintf(stderr, "68k: could not start PPP/libslirp virtual ISP\n");
        mrc_m68k_free(m);
        return 1;
    }
    if (ne2000_probe && !mrc_m68k_open_ne2000_probe(m, net_pcap)) {
        fprintf(stderr, "68k: could not start PIC-2000 NE2000/libslirp probe\n");
        mrc_m68k_free(m);
        return 1;
    }
    if (keyboard >= 0 || gui) {
        bool supported = mrc_m68k_keyboard_connect(m, keyboard != 0);
        if (keyboard == 1 && !supported) {
            fprintf(stderr, "68k: Magic Bus keyboard is unsupported for this ROM; "
                            "supported: PIC-2000, Envoy 1.0/pt4, HIX-300\n");
            mrc_m68k_free(m); return 2;
        }
    }
    mrc_m68k_keyboard_release(m);
    /* Touch times are offsets from the machine state that actually runs.
     * Scheduling before a restore left every event behind the restored
     * instruction counter, so --tap appeared to work only from reset. */
    if (touch_at) mrc_m68k_touch(m, before + touch_at, touch_len, trace_conv);
    for (unsigned i = 0; i < tap_count; i++)
        if (!mrc_m68k_tap(m, before + taps[i].at, taps[i].len,
                          taps[i].x, taps[i].y,
                          trace_conv)) {
            mrc_m68k_free(m); return 2;
        }
    mrc_pclink *install_link = NULL;
    if (install_path) {
        mrc_runtime view = mrc_pic2000_runtime(m);
        install_link = view.ops->install ? view.ops->install(view.board, install_path) : NULL;
        if (!install_link) {
            fprintf(stderr, "68k: could not start package transfer\n");
            mrc_m68k_free(m); return 1;
        }
        fprintf(stderr, "68k: PC Link waiting for the guest\n");
    }
    if (power_scheduled)
        mrc_m68k_schedule_power(m, before + power_at, 1000000);
    int gui_result = 0;
    if (wav_path && gui) {
        fprintf(stderr, "PIC --wav currently requires --headless\n");
        mrc_m68k_free(m); return 2;
    }
    mrc_wav *wav = wav_path ? mrc_wav_open(wav_path, mrc_m68k_audio_rate(m)) : NULL;
    if (wav_path && !wav) { mrc_m68k_free(m); return 1; }
    if (wav) mrc_m68k_audio_sink(m, mrc_wav_sample, wav);
    if (gui) {
        if (!mrc_gui68k_available()) gui_result = 1;
        else if (!mrc_shell_current()) {
            fprintf(stderr, "68k: no window was opened for this machine\n");
            gui_result = 1;
        }
        else gui_result = mrc_gui68k_run(m, insns_set ? insns : 0,
                                         mrc_shell_current());
    } else if (install_link) {
        /* A scripted transfer has an observable end. The instruction budget
         * is a timeout, not idle time to burn after receiving the final Pong.
         * Give the ROM one emulated second to finish its asynchronous work
         * before the normal framebuffer/state output below. */
        uint64_t left = insns, settle_at = 0;
        while (left) {
            const uint64_t chunk = left < 100000 ? left : 100000;
            const uint64_t ran_chunk = mrc_m68k_run(m, chunk);
            if (!ran_chunk) break;
            left -= ran_chunk < left ? ran_chunk : left;
            const mrc_pclink_state state = mrc_pclink_state_of(install_link);
            if (state == MRC_PCLINK_FAILED) break;
            if (state == MRC_PCLINK_DONE) {
                const uint64_t now = mrc_m68k_elapsed_ns(m);
                if (!settle_at) {
                    fprintf(stderr, "68k: transfer complete; settling for 1 guest second\n");
                    settle_at = now + 1000000000ULL;
                }
                if (now >= settle_at) break;
            }
        }
    } else {
        mrc_m68k_run(m, insns);
    }
    uint64_t ran = mrc_m68k_insns(m) - before;
    if (wav) { mrc_m68k_audio_sink(m, NULL, NULL); mrc_wav_close(wav); }
    if (dump) {
        unsigned long a = 0, l = 0; char path[256] = "";
        if (sscanf(dump, "%lx,%lu,%255s", &a, &l, path) == 3)
            mrc_m68k_dump(m, (uint32_t)a, (uint32_t)l, path);
        else
            fprintf(stderr, "bad --dump (want addr,len,path)\n");
    }
    if (dump_fb) {
        uint8_t pixels[PIC2000_SCREEN_W * PIC2000_SCREEN_H];
        FILE *fb = fopen(dump_fb, "wb");
        if (!fb) perror(dump_fb);
        else {
            mrc_m68k_lcd(m, pixels);
            fprintf(fb, "P5\n%u %u\n3\n", PIC2000_SCREEN_W, PIC2000_SCREEN_H);
            for (size_t i = 0; i < sizeof(pixels); i++)
                fputc((int)pixels[i], fb);
            fclose(fb);
            fprintf(stderr, "68k: dumped framebuffer to %s\n", dump_fb);
        }
    }
    fprintf(stderr, "68k: ran %llu instructions\n", (unsigned long long)ran);
    mrc_m68k_report(m);
    mrc_m68k_engine_report(m);
    bool saved = true;
    if (install_link) {
        const mrc_pclink_state state = mrc_pclink_state_of(install_link);
        fprintf(stderr, "68k: PC Link: %s\n", mrc_pclink_message(install_link));
        if (state != MRC_PCLINK_DONE) saved = false;
    }
    if (save_state && !mrc_m68k_save_state(m, save_state)) saved = false;
    /* Putting the device down saves it. There is nothing to press and
     * nothing to confirm: the next launch picks up here. */
    if (gui_result == 0 && have_device_state &&
        !mrc_m68k_save_state(m, device_state)) saved = false;
    mrc_m68k_free(m);
    return saved ? 0 : 1;
}
