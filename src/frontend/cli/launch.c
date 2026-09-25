#include "rom/identify.h"
#include "frontend/sdl/chooser.h"
#include "frontend/sdl/shell.h"
#include "frontend/sdl/datarover_gui.h"
#include "host/devices.h"
#include "host/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mrc_datarover_main(int argc, char **argv);
#ifdef MRC_HAVE_PIC2000
int mrc_pic2000_main(int argc, char **argv);
#endif

/* Skip option values while inspecting the common launcher switches. This
 * keeps text such as --input "--device" from becoming a launcher option.
 * --trace and --tap differ between the board CLIs; their numeric arguments
 * do not contain launcher switches and are left to the selected parser. */
static unsigned value_count(const char *option)
{
    static const char *const single[] = {
        "--rom", "--ram", "--reset-pc", "--serial", "--card-at", "--log-card",
        "--sram1", "--sram2", "--card1", "--card2", "--power-at", "--net",
        "--net-pcap", "--ne2000", "--modem", "--tint", "-n", "--input", "--input-after",
        "--log-exceptions", "--log-codec", "--watch-pc", "--coverage",
        "--watch-log", "--watch-after", "--audio-wav", "--dump-fb", "--taps",
        "--aux-adc", "--codec-gpio", "--option-key", "--mfio-in", "--mfio-at", "--io-at", "--pwrint-at", "--pcmcia-fill",
        "--taps-px", "--drag-px", "--tap-hold", "--tap-every", "--dump-ram",
        "--save-state", "--load-state", "--install", "--trace-after", "--cpi", "--sample",
        "--watch-read", "--watch-write", "--dump", "--force-irq", "--adc",
        "--touch", "--adc-chan", "--probe-preset", "--wav", "--cpu-engine",
        "--trace-state", "--trace-state-count", "--trace-bus"
    };
    if (!strcmp(option, "--tap-px")) return 3;
    if (!strcmp(option, "--fb-watch")) return 2;
    for (size_t i=0; i<sizeof(single)/sizeof(*single); ++i)
        if (!strcmp(option,single[i])) return 1;
    return 0;
}

/*
 * Switching devices, which is the only thing here that is not dispatch.
 *
 * A window cannot switch by itself: it does not own the machine, and the
 * device it is switching to need not even be the same kind of machine. So it
 * leaves a name behind and stops, its own main saves the state on the way out
 * as it would for any other close, and control comes back here -- to the one
 * piece of code that already knows how to look at a ROM and decide which
 * board it is. Starting the next device is then the same act as starting the
 * first one, with a different path.
 *
 * Returns the ROM path to run next, or NULL when nothing was asked for.
 */
static const char *next_device(char *rom_out, size_t cap)
{
    const char *id = mrc_state_take_requested_device();
    if (!id) return NULL;
    mrc_device d;
    if (!mrc_device_by_id(id, &d)) {
        fprintf(stderr, "launch: there is no device called \"%s\"\n", id);
        return NULL;
    }
    if (!mrc_device_paths(&d, rom_out, cap, NULL, 0)) return NULL;
    fprintf(stderr, "launch: switching to \"%s\"\n", d.name);
    mrc_state_set_device_id(d.id);
    return rom_out;
}

/*
 * Options that belong to the device that was running, not to the session.
 *
 * Switching carries the window's settings across -- the tint, the engine,
 * whether there is sound -- because those are how you like to look at a
 * machine. It must not carry the other device's memory card, or a state file
 * named on the command line, or an instruction to start that one from its
 * ROM: every one of those is a statement about the device being left behind,
 * and applying it to the next one would be a surprise at best and someone
 * else's card in your slot at worst.
 *
 * A deny-list rather than an allow-list, because an option this does not
 * recognise is far more likely to be a diagnostic than a second identity.
 */
static bool belongs_to_device(const char *option)
{
    static const char *const theirs[] = {
        "--rom", "--ram", "--load-state", "--save-state", "--fresh",
        "--temporary", "--sram1", "--sram2", "--card1", "--card2",
        "--ne2000", "--modem", "--modem-pty", "--net", "--net-pcap", "--card-at", "--reset-pc",
        "--boot-monitor",
    };
    for (size_t i = 0; i < sizeof(theirs)/sizeof(*theirs); i++)
        if (!strcmp(option, theirs[i])) return true;
    return false;
}

/* What board this image is, by looking at it. MRC_ROM_UNKNOWN when it cannot
 * be read or cannot be told. */
static mrc_rom_device identify(const char *rom)
{
    FILE *f = fopen(rom,"rb");
    if (!f) { perror(rom); return MRC_ROM_UNKNOWN; }
    if (fseek(f,0,SEEK_END)) { fclose(f); return MRC_ROM_UNKNOWN; }
    long size = ftell(f);
    if (size <= 0 || size > 64*1024*1024 || fseek(f,0,SEEK_SET)) {
        fprintf(stderr,"ROM is empty, unreadable, or larger than 64 MiB\n");
        fclose(f); return MRC_ROM_UNKNOWN;
    }
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data,1,(size_t)size,f) != (size_t)size) {
        fprintf(stderr,"Cannot read ROM\n"); free(data); fclose(f);
        return MRC_ROM_UNKNOWN;
    }
    fclose(f);
    mrc_rom_device detected = mrc_rom_identify(data,(size_t)size);
    free(data);
    fprintf(stderr,"ROM identification: %s\n",mrc_rom_device_name(detected));
    return detected;
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *device = "auto", *make = NULL, *make_name = NULL;
    bool gui_set = false, help = false, headless = false;
    /* What was typed, plus room for a --rom this may supply and a --gui it
     * may append. */
    char **args = calloc((size_t)argc+4, sizeof(*args));
    if (!args) return 1;
    int count = 1;
    args[0] = argv[0];
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--device")) {
            if (++i == argc) { fprintf(stderr,"--device needs a value\n"); free(args); return 2; }
            device = argv[i];
            continue;
        }
        /* Consumed here rather than passed on: making a device is the
         * launcher's business, and by the time a board CLI runs there is
         * nothing left to decide. */
        if (!strcmp(argv[i], "--new-device")) {
            if (++i == argc) { fprintf(stderr,"--new-device needs a firmware image\n"); free(args); return 2; }
            make = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--name")) {
            if (++i == argc) { fprintf(stderr,"--name needs a value\n"); free(args); return 2; }
            make_name = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--rom") && i+1<argc) {
#ifdef _WIN32
            /* One separator from here on, the one the device store uses,
             * whichever one the shell handed over. The machines are passed
             * these same strings, so rewrite them rather than a copy. */
            for (char *p = argv[i+1]; *p; p++)
                if (*p == '\\') *p = '/';
#endif
            rom = argv[i+1];
        }
        if (!strcmp(argv[i], "--gui") || !strcmp(argv[i], "--headless")) gui_set = true;
        if (!strcmp(argv[i], "--headless")) headless = true;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) help = true;
        unsigned values = value_count(argv[i]);
        args[count++] = argv[i];
        while (values-- && i+1<argc) args[count++] = argv[++i];
    }
    /*
     * One window for the whole session.
     *
     * It is opened here, above every board, and each machine attaches to it as
     * it starts. That is what makes switching devices reshape what is on
     * screen instead of closing a window and opening another somewhere else --
     * and it is why the chooser is not a separate window either, only this one
     * with nothing in it yet.
     *
     * A run told to be headless never opens one, and a run that cannot open
     * one says so rather than carrying on without.
     */
    mrc_shell *shell = NULL;
    if (!help && !headless && mrc_chooser_available()) {
        shell = mrc_shell_open("mcap", 480, 320);
        if (!shell) { free(args); return 1; }
        mrc_shell_set_current(shell);
    }

    /*
     * Making a device, which is how the first one comes into being: the
     * window's list can only be reached once something is already running.
     * The image is copied in, and what starts is the device rather than the
     * file -- so the firmware can be moved or deleted straight afterwards.
     */
    char made_rom[4096];
    if (make) {
        mrc_device fresh;
        if (!mrc_device_create(make, make_name, &fresh)) { free(args); return 1; }
        if (!mrc_device_paths(&fresh, made_rom, sizeof(made_rom), NULL, 0)) {
            free(args); return 1;
        }
        if (rom) {
            fprintf(stderr, "--new-device and --rom name different machines\n");
            free(args); return 2;
        }
        rom = made_rom;
        args[count++] = "--rom";
        args[count++] = made_rom;
        mrc_state_set_device_id(fresh.id);
    }

    /*
     * Nothing named: pick up whatever was last in use.
     *
     * Running the emulator with no arguments should do what closing and
     * reopening a device does, because that is what a person means by it.
     * The listing is newest first, by when each device was last put down, so
     * the first one is the one they were holding.
     */
    /* Both of these are pointed at by `args`, so they outlive the blocks
     * that fill them. */
    char recent[4096], picked[4096];
    if (!rom && !help) {
        mrc_device last;
        if (mrc_devices_list(&last, 1) &&
            mrc_device_paths(&last, recent, sizeof(recent), NULL, 0)) {
            rom = recent;
            args[count++] = "--rom";
            args[count++] = recent;
            mrc_state_set_device_id(last.id);
            fprintf(stderr, "Resuming \"%s\"\n", last.name);
        }
    }

    mrc_rom_device selected = MRC_ROM_UNKNOWN;
    if (!strcmp(device,"datarover840")) selected = MRC_ROM_DATAROVER840;
    else if (!strcmp(device,"pic2000")) selected = MRC_ROM_PIC2000;
    else if (!strcmp(device,"envoy")) selected = MRC_ROM_ENVOY;
    else if (!strcmp(device,"hix300")) selected = MRC_ROM_HIX300;
    else if (strcmp(device,"auto")) {
        fprintf(stderr,"--device must be auto, datarover840, pic2000, envoy, or hix300\n"); free(args); return 2;
    }
    if (rom) {
        mrc_rom_device detected = identify(rom);
        if (detected == MRC_ROM_UNKNOWN && access(rom, R_OK)) { free(args); return 1; }
        if (selected == MRC_ROM_UNKNOWN) selected = detected;
        else if (detected != MRC_ROM_UNKNOWN && detected != selected) {
            fprintf(stderr,"--device conflicts with the detected ROM\n"); free(args); return 2;
        } else fprintf(stderr,"Machine selected explicitly: %s\n",mrc_rom_device_name(selected));
        if (selected == MRC_ROM_UNKNOWN) {
            fprintf(stderr,"Cannot select a machine from ROM contents. For an experimental ROM, specify --device datarover840, --device pic2000, --device envoy, or --device hix300.\n"); free(args); return 2;
        }
    }
    /*
     * A ROM inside the device store is a device, and saying so is what lets
     * the list mark the one you are looking at. Started from a path anywhere
     * else, this is just a ROM and there is no device to name.
     */
    if (rom) {
        const char *root = mrc_devices_root();
        size_t n = root ? strlen(root) : 0;
        if (root && !strncmp(rom, root, n) && rom[n] == '/') {
            char id[MRC_DEVICE_ID_MAX];
            const char *rest = rom + n + 1, *slash = strchr(rest, '/');
            size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
            if (len && len < sizeof(id)) {
                memcpy(id, rest, len);
                id[len] = 0;
                mrc_state_set_device_id(id);
            }
        }
    }
    if (help)
        fprintf(stderr,"mcap selects the machine from ROM contents.\n"
                       "  --device auto|datarover840|pic2000|envoy|hix300  (default auto)\n"
                       "Use --device hix300 --help (or envoy/pic2000) for 68k options.\n");
    /*
     * Nothing to run and nothing to pick up: open the window anyway, dark,
     * with the device list on it.
     *
     * The emulator has to be startable on its own. Printing instructions and
     * quitting meant the only way to get a first device was to already know
     * the command for it, and the list that makes one could only be reached
     * from inside a machine that was already running -- which there was no
     * way to start. The window looks like a device that is switched off,
     * because that is what it is.
     */
    if (!rom && !help) {
        if (!mrc_chooser_available()) {
            fprintf(stderr,
                "No devices yet, and this build has no window to make one in.\n"
                "  mcap --new-device <image> [--name \"Whatever you call it\"]\n");
            free(args);
            return 2;
        }
        if (!shell) {
            fprintf(stderr, "No devices yet, and no window to make one in.\n"
                            "  mcap --new-device <image>\n");
            free(args);
            return 2;
        }
        if (!mrc_chooser_run(shell)) {          /* closed, not failed */
            mrc_shell_close(shell);
            free(args);
            return 0;
        }
        if (!next_device(picked, sizeof(picked))) { free(args); return 0; }
        rom = picked;
        args[count++] = "--rom";
        args[count++] = picked;
        selected = identify(rom);
        if (selected == MRC_ROM_UNKNOWN) {
            fprintf(stderr, "launch: cannot tell what kind of machine that is\n");
            free(args);
            return 2;
        }
    }
    int result;
    int parsed_count = count;
    char next_rom[4096];
    for (;;) {
        count = parsed_count;      /* undo any --gui appended last time round */
        if (selected == MRC_ROM_PIC2000 || selected == MRC_ROM_ENVOY ||
            selected == MRC_ROM_HIX300) {
#ifdef MRC_HAVE_PIC2000
            if (!help && !gui_set && mrc_gui_available()) args[count++] = "--gui";
            result = mrc_pic2000_main(count,args);
#else
            (void)gui_set;
            fprintf(stderr,"68k device detected, but this build lacks 68k machine support. Reconfigure with -DMRC_BUILD_M68K_MACHINES=ON.\n"); result = 2;
#endif
        } else if (selected == MRC_ROM_DATAROVER840 || help || !rom) {
            result = mrc_datarover_main(count,args);
        } else {
            fprintf(stderr,"%s is identified, but its hardware is not implemented.\n",mrc_rom_device_name(selected)); result = 2;
        }

        /*
         * Whether a different device was asked for while that one was running.
         * It has already saved itself and shut down; this is the same act as
         * starting the first one, with another path.
         */
        if (mrc_state_take_stop()) {
            /* The board main has now saved, flushed cards, and freed it.
             * Do not run startup's auto-resume while showing the list. */
            mrc_state_set_device_id(NULL);
            mrc_state_set_path(NULL);
            if (result || !shell || !mrc_chooser_run(shell)) break;
        }
        if (!next_device(next_rom, sizeof(next_rom))) break;
        rom = next_rom;

        /* Keep the session's settings, drop the last device's identity. */
        int kept = 1;
        for (int i = 1; i < parsed_count; i++) {
            unsigned values = value_count(args[i]);
            if (belongs_to_device(args[i])) { i += (int)values; continue; }
            args[kept++] = args[i];
            while (values-- && i + 1 < parsed_count) args[kept++] = args[++i];
        }
        args[kept++] = "--rom";
        args[kept++] = next_rom;
        count = parsed_count = kept;   /* the new baseline for --gui */

        /* Its board is decided the way the first one's was: by looking. */
        selected = identify(rom);
        if (selected == MRC_ROM_UNKNOWN) {
            fprintf(stderr,"launch: cannot tell what kind of machine that device is\n");
            result = 2;
            break;
        }
    }
    if (shell) mrc_shell_close(shell);
    free(args);
    return result;
}
