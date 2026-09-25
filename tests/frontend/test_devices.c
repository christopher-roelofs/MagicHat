/*
 * test_devices.c — picking a ROM and making a device out of it.
 *
 * The two halves of one motion, so they are tested as one: walk a directory,
 * choose a firmware image, and end up with a device that owns a copy of it.
 *
 * The property worth protecting is that last part. A device that pointed at
 * the file it was made from would be a device that stops existing when
 * someone tidies their downloads folder, so the test moves the original out
 * of the way afterwards and checks the device is still there and still
 * startable. That is the whole reason for copying rather than referring.
 */
#include "frontend/sdl/devices_panel.h"
#include "frontend/sdl/picker.h"
#include "host/devices.h"
#include "host/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/*
 * A firmware image the identifier recognises, because making a device now
 * asks what kind of machine the image is and refuses one it cannot place.
 *
 * Only the few bytes identification looks at: the reset pair, the opening of
 * the reset path, and the signature strings. No ROM is redistributed and none
 * is needed -- these are four megabytes of zeroes with a handful of bytes set.
 */
static void put_firmware(const char *path, bool envoy)
{
    static uint8_t image[4u * 1024 * 1024];
    memset(image, 0, sizeof(image));
    uint32_t base = envoy ? 0x02400000u : 0x0e000000u;
    uint32_t sp = 0x00100000u, pc = base + 0x200u;
    for (unsigned i = 0; i < 4; i++) {
        image[i] = (uint8_t)(sp >> (24 - i * 8));
        image[4 + i] = (uint8_t)(pc >> (24 - i * 8));
    }
    /* movea.l #imm,a7 then the suba/move-to-USP pair every reset path opens
     * with, which is what says this is one of these machines at all. */
    static const uint8_t reset[] = { 0x2e,0x7c,0x00,0x10,0x00,0x00,
                                     0x91,0xc8,0x4e,0x60 };
    memcpy(image + 0x200, reset, sizeof(reset));
    const char *tag = envoy ? ",MOTO,1,Motorola Envoy" : ",SONY,2,PIC-2000";
    memcpy(image + 0x300, tag, strlen(tag));

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    if (fwrite(image, 1, sizeof(image), f) != sizeof(image)) exit(1);
    fclose(f);
}

static bool exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

/* Find a row by the label it would be drawn with. The picker's ids are its
 * own business, so the test goes through what a person would see. */
static int row_with(const mrc_ui_row *rows, unsigned count, const char *label)
{
    for (unsigned i = 0; i < count; i++)
        if (rows[i].label && !strcmp(rows[i].label, label)) return rows[i].id;
    return -1;
}

/* Like row_with, but the row's own index rather than its id -- needed to
 * reach a row's action buttons, which are not visible through the id alone. */
static int row_index_with(const mrc_ui_row *rows, unsigned count, const char *label)
{
    for (unsigned i = 0; i < count; i++)
        if (rows[i].label && !strcmp(rows[i].label, label)) return (int)i;
    return -1;
}

static int row_labelled_like(const mrc_ui_row *rows, unsigned count,
                             const char *fragment)
{
    for (unsigned i = 0; i < count; i++)
        if (rows[i].label && strstr(rows[i].label, fragment)) return (int)i;
    return -1;
}

int main(void)
{
    char scratch[] = "/tmp/mrc-devices-XXXXXX";
    if (!mkdtemp(scratch)) { perror("mkdtemp"); return 1; }

    char roms[512], nested[512], store[512];
    snprintf(roms, sizeof(roms), "%s/roms", scratch);
    snprintf(nested, sizeof(nested), "%s/roms/inner", scratch);
    snprintf(store, sizeof(store), "%s/devices", scratch);
    mkdir(roms, 0700);
    mkdir(nested, 0700);
    setenv("MRC_DEVICES_DIR", store, 1);

    char pic[512], notes[512], inner[512];
    snprintf(pic, sizeof(pic), "%s/PIC-2000.rom", roms);
    snprintf(notes, sizeof(notes), "%s/readme.txt", roms);
    snprintf(inner, sizeof(inner), "%s/Envoy.IMAGE", nested);
    put_firmware(pic, false);
    put_file(notes, "not a rom");
    put_firmware(inner, true);

    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window *window = SDL_CreateWindow("picker test", 0, 0, 640, 480, 0);
    CHECK(window != NULL);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    CHECK(renderer != NULL);
    mrc_ui *ui = NULL;
    CHECK(mrc_ui_open(&ui, renderer, window));
    if (!ui) return 1;
    mrc_ui_set_buttons(ui, 1u << MRC_UI_ICON_ROMS);

    static const char *const kinds[] = { ".rom", ".image" };
    mrc_picker *p = mrc_picker_open(ui, "Choose firmware", roms, kinds, 2);
    CHECK(p != NULL);
    if (!p) return 1;

    /*
     * What the list shows: the directory, the firmware, and not the text
     * file. Matching ignores case, or an image named .IMAGE would be
     * invisible for no reason a person could see.
     */
    const mrc_ui_row *rows = mrc_ui_panel_rows(ui);
    unsigned count = mrc_ui_panel_row_count(ui);
    CHECK(row_with(rows, count, "inner") >= 0);
    CHECK(row_with(rows, count, "PIC-2000.rom") >= 0);
    CHECK(row_with(rows, count, "readme.txt") < 0);
    /* Directories first, so the way deeper is never buried under files. */
    CHECK(rows[count - 1].label && !strcmp(rows[count - 1].label, "PIC-2000.rom"));

    /* Down into a directory, and the case-insensitive match proves itself. */
    CHECK(mrc_picker_row(p, row_with(rows, count, "inner")));
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(row_with(rows, count, "Envoy.IMAGE") >= 0);
    CHECK(!mrc_picker_taken(p));          /* a directory is not a choice */

    /* And back out again: the first row is always the way out, and its
     * label carries the path so you can see where you are. */
    const char *before_up = mrc_picker_directory(p);
    CHECK(before_up && strstr(before_up, "inner"));
    CHECK(mrc_picker_row(p, rows[0].id)); /* the first row is the way out */
    CHECK(!strcmp(mrc_picker_directory(p), roms));

    /* Choosing a file hands back its whole path, once. */
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(mrc_picker_row(p, row_with(rows, count, "PIC-2000.rom")));
    const char *chosen = mrc_picker_taken(p);
    CHECK(chosen && !strcmp(chosen, pic));
    CHECK(!mrc_picker_taken(p));          /* taking it clears it */

    /* A row that is not the picker's is not the picker's to answer for. */
    CHECK(!mrc_picker_row(p, 3));

    char keep[512];
    snprintf(keep, sizeof(keep), "%s", pic);
    mrc_picker_close(p);

    /* Now the device. */
    mrc_device made;
    CHECK(mrc_device_create(keep, NULL, &made));
    /*
     * Named after the machine, not after the file. "Sony PIC-2000" is what
     * someone owns; the filename is often a download with a version or a
     * mirror's name stuck to it, and says nothing about what it is.
     */
    CHECK(!strcmp(made.name, "Sony PIC-2000"));
    CHECK(!strcmp(made.id, "sony-pic-2000"));  /* and a legible directory */
    CHECK(!strcmp(made.machine, "pic2000"));  /* read out of the firmware */
    CHECK(!made.has_state);                   /* never run */

    char rom_copy[4096], state[4096];
    CHECK(mrc_device_paths(&made, rom_copy, sizeof(rom_copy),
                           state, sizeof(state)));
    CHECK(exists(rom_copy));
    CHECK(!exists(state));

    /* The point of the copy: the original can go and the device remains. */
    CHECK(unlink(keep) == 0);
    CHECK(exists(rom_copy));
    mrc_device again;
    CHECK(mrc_device_by_id("sony-pic-2000", &again));
    CHECK(!strcmp(again.name, "Sony PIC-2000"));

    /* A name given by hand is taken as given, refusal included: choosing a
     * name already in use is a mistake worth being told about. */
    put_firmware(keep, false);
    CHECK(!mrc_device_create(keep, "Sony PIC-2000", NULL));

    /*
     * Left to itself it counts up instead. Owning two of the same machine is
     * ordinary, and refusing the second would be refusing the ordinary case.
     */
    mrc_device twin;
    CHECK(mrc_device_create(keep, NULL, &twin));
    CHECK(!strcmp(twin.name, "Sony PIC-2000 (2)"));
    CHECK(mrc_device_delete(&twin));

    /* An image nothing recognises is refused rather than filed as a device
     * that cannot start. */
    CHECK(!mrc_device_create(notes, "Mystery", NULL));

    /* Running it writes a state, and the listing then says so. */
    put_file(state, "pretend machine");
    mrc_device listed[MRC_DEVICE_MAX];
    unsigned n = mrc_devices_list(listed, MRC_DEVICE_MAX);
    CHECK(n == 1);
    CHECK(n && listed[0].has_state);

    /* Renaming keeps the device; only what it is called changes. */
    CHECK(mrc_device_rename(&made, "Sony Magic Link"));
    CHECK(mrc_device_by_id("sony-pic-2000", &again));
    CHECK(!strcmp(again.name, "Sony Magic Link"));

    /* A second device, of another kind, and both are listed together --
     * devices are not filed by machine, they are labelled with it. */
    mrc_device second;
    CHECK(mrc_device_create(inner, "Spare", &second));
    CHECK(!strcmp(second.machine, "envoy"));
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 2);
    CHECK(mrc_device_by_id("spare", &again) && !strcmp(again.machine, "envoy"));

    /* Deleting takes the whole thing: machine, firmware and history. */
    CHECK(mrc_device_delete(&second));
    CHECK(!mrc_device_by_id(second.id, &again));
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 1);
    CHECK(mrc_device_delete(&made));
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 0);

    /*
     * And the same thing through the panel the rail actually opens, driven by
     * taps rather than by calling the store directly.
     *
     * This is the part a reader would otherwise have to take on trust: that
     * the list, the picker and the store are joined up, and that a device made
     * this way appears in the list afterwards. Everything above tested the
     * pieces; this tests that pressing things works.
     */
    /* The picker opens where a person was last looking, and on a first run
     * that is home. Putting the firmware there is how this drives the real
     * default rather than a path handed in for the test. */
    setenv("HOME", roms, 1);

    mrc_devices_panel_open(ui, NULL);
    CHECK(mrc_devices_panel_showing());
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    /* Nothing yet, so a heading saying so and the one way forward. */
    CHECK(count == 2);
    CHECK(rows[0].kind == MRC_UI_ROW_HEADING);
    int new_row = row_with(rows, count, "New device\xe2\x80\xa6");
    CHECK(new_row >= 0);

    /* Pressing it turns the sheet into a file picker. */
    CHECK(mrc_devices_panel_row(ui, new_row));
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    int rom_row = row_with(rows, count, "PIC-2000.rom");
    CHECK(rom_row >= 0);
    CHECK(row_with(rows, count, "readme.txt") < 0);

    /* Choosing firmware makes the device and puts the list back, with it on. */
    CHECK(mrc_devices_panel_row(ui, rom_row));
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(count == 2);                       /* the device, and New device */
    CHECK(row_with(rows, count, "Sony PIC-2000") >= 0);
    /* Labelled with what kind of machine it is and that it has not run. */
    CHECK(rows[0].value && strstr(rows[0].value, "PIC-2000"));
    CHECK(rows[0].value && strstr(rows[0].value, "not started"));

    /*
     * The same machine twice is an ordinary thing to own.
     *
     * Nothing in the list asks for a name, so a second device made from the
     * same firmware would once have collided with the first and been refused.
     * It counts up instead, and both are in the list afterwards.
     */
    CHECK(mrc_devices_panel_row(ui, new_row));    /* the picker again */
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(row_with(rows, count, "readme.txt") < 0);   /* filtered out */
    int same = row_with(rows, count, "PIC-2000.rom");
    CHECK(same >= 0);
    CHECK(mrc_devices_panel_row(ui, same));
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(rows[0].kind != MRC_UI_ROW_HEADING);        /* nothing went wrong */
    CHECK(row_with(rows, count, "Sony PIC-2000") >= 0);
    CHECK(row_with(rows, count, "Sony PIC-2000 (2)") >= 0);

    /* Reopening is a fresh look rather than a stale warning. */
    mrc_devices_panel_close(ui);
    mrc_devices_panel_open(ui, NULL);
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    CHECK(rows[0].kind != MRC_UI_ROW_HEADING);

    /* Choosing it asks for a switch, which is the frontend's to carry out. */
    CHECK(!mrc_state_device_requested());
    CHECK(mrc_devices_panel_row(ui, rows[0].id));
    CHECK(mrc_state_device_requested());
    CHECK(!strcmp(mrc_state_take_requested_device(), "sony-pic-2000"));
    CHECK(!mrc_state_device_requested());

    /* The one already running is not offered as somewhere to go. */
    mrc_devices_panel_open(ui, "sony-pic-2000");
    rows = mrc_ui_panel_rows(ui);
    CHECK(mrc_devices_panel_row(ui, rows[0].id));
    CHECK(!mrc_state_device_requested());
    CHECK(rows[0].value && strstr(rows[0].value, "in use"));

    /*
     * Each device row carries its own play and trash can, beside the row
     * body the checks above already cover. Play repeats what tapping the
     * row does -- same id, same guard against the device already running --
     * so only the trash can is new behaviour to prove.
     */
    rows = mrc_ui_panel_rows(ui);
    count = mrc_ui_panel_row_count(ui);
    int spare = row_index_with(rows, count, "Sony PIC-2000 (2)");
    int current = row_index_with(rows, count, "Sony PIC-2000");
    CHECK(spare >= 0 && current >= 0);
    if (spare >= 0 && current >= 0) {
        CHECK(rows[spare].action_count == 2);
        CHECK(rows[spare].action_icon[0] == MRC_UI_ICON_PLAY);
        CHECK(rows[spare].action_id[0] == rows[spare].id);   /* same as the row */
        CHECK(rows[spare].action_icon[1] == MRC_UI_ICON_TRASH);
        int trash = rows[spare].action_id[1];
        CHECK(rows[current].action_icon[0] == MRC_UI_ICON_STOP);
        int current_body = rows[current].id;

        /*
         * Deleting is not undoable, so a first tap only asks. Nothing is
         * gone yet, and the question names the device by name.
         */
        CHECK(mrc_devices_panel_row(ui, trash));
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        CHECK(row_labelled_like(rows, count, "Delete \"Sony PIC-2000 (2)\"") >= 0);
        mrc_device still[MRC_DEVICE_MAX];
        CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 2);

        /*
         * Tapping anything else -- here, the other device's body, which is a
         * no-op because it is the one already running -- cancels the
         * question rather than answering it.
         */
        CHECK(mrc_devices_panel_row(ui, current_body));
        CHECK(!mrc_state_device_requested());       /* the no-op fired, not a switch */
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        CHECK(row_labelled_like(rows, count, "Delete \"") < 0);   /* the question is gone */
        CHECK(row_with(rows, count, "Sony PIC-2000 (2)") >= 0);   /* the device is not */
        CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 2);

        /*
         * -1 is what an ordinary hover reports too, not only a completed tap
         * on nothing, and the frontend calls this on every one of those --
         * every mouse move or finger drag across the sheet between the two
         * taps a real confirmation needs. It must not read as cancelling
         * anything, or a delete could never survive the pointer moving at
         * all on the way to confirming it.
         */
        CHECK(mrc_devices_panel_row(ui, trash));
        CHECK(!mrc_devices_panel_row(ui, -1));
        CHECK(!mrc_devices_panel_row(ui, -1));
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        CHECK(row_labelled_like(rows, count, "Delete \"Sony PIC-2000 (2)\"") >= 0);
        CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 2);   /* still armed */
        CHECK(mrc_devices_panel_row(ui, trash));                /* now confirm it */
        CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 1);
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        CHECK(row_with(rows, count, "Sony PIC-2000 (2)") < 0);
        CHECK(row_with(rows, count, "Sony PIC-2000") >= 0);   /* untouched */

        /*
         * Recreate it and prove the other direction: a genuine tap on the
         * confirmation row's own body -- not a hover, a real completed tap,
         * now with an id of its own rather than the -1 that could not be
         * told apart from one -- still cancels rather than confirming.
         */
        char again_source[512];
        snprintf(again_source, sizeof(again_source), "%s/again.rom", roms);
        put_firmware(again_source, false);
        mrc_device recreated;
        CHECK(mrc_device_create(again_source, "Sony PIC-2000 (2)", &recreated));
        /* Made directly against the store rather than through the panel, so
         * the panel's own idea of the list is stale until it looks again. */
        mrc_devices_panel_open(ui, "sony-pic-2000");
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        spare = row_index_with(rows, count, "Sony PIC-2000 (2)");
        CHECK(spare >= 0);
        if (spare >= 0) {
            trash = rows[spare].action_id[1];
            CHECK(mrc_devices_panel_row(ui, trash));
            rows = mrc_ui_panel_rows(ui);
            count = mrc_ui_panel_row_count(ui);
            int confirm_row = row_labelled_like(rows, count, "Delete \"Sony PIC-2000 (2)\"");
            CHECK(confirm_row >= 0);
            if (confirm_row >= 0) {
                CHECK(rows[confirm_row].id >= 0);   /* a real id, not -1 */
                CHECK(mrc_devices_panel_row(ui, rows[confirm_row].id));
            }
            rows = mrc_ui_panel_rows(ui);
            count = mrc_ui_panel_row_count(ui);
            CHECK(row_labelled_like(rows, count, "Delete \"") < 0);   /* cancelled */
            CHECK(row_with(rows, count, "Sony PIC-2000 (2)") >= 0);   /* not deleted */
            CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 2);
        }

        /* Arming it again and tapping the same trash can a second time is
         * what actually deletes it. */
        CHECK(mrc_devices_panel_row(ui, trash));
        CHECK(mrc_devices_panel_row(ui, trash));
        CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 1);
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        CHECK(row_with(rows, count, "Sony PIC-2000 (2)") < 0);
        CHECK(row_with(rows, count, "Sony PIC-2000") >= 0);   /* untouched */

        /*
         * The device in use refuses its own trash can outright -- its files
         * are open under the running machine -- rather than arming a delete
         * that would pull them out from under it.
         */
        rows = mrc_ui_panel_rows(ui);
        count = mrc_ui_panel_row_count(ui);
        current = row_index_with(rows, count, "Sony PIC-2000");
        CHECK(current >= 0);
        if (current >= 0) {
            CHECK(mrc_devices_panel_row(ui, rows[current].action_id[1]));
            rows = mrc_ui_panel_rows(ui);
            count = mrc_ui_panel_row_count(ui);
            CHECK(rows[0].kind == MRC_UI_ROW_HEADING);
            CHECK(rows[0].label && strstr(rows[0].label, "Press Stop"));
            CHECK(row_labelled_like(rows, count, "Delete \"") < 0);  /* not armed */
            CHECK(mrc_devices_list(still, MRC_DEVICE_MAX) == 1);     /* not deleted */
        }
    }

    /* The last, previously running device can stop without being deleted,
     * then be resumed or deleted from the unloaded chooser. */
    mrc_devices_panel_open(ui, "sony-pic-2000");
    rows = mrc_ui_panel_rows(ui);
    CHECK(rows[0].action_icon[0] == MRC_UI_ICON_STOP);
    CHECK(mrc_devices_panel_row(ui, rows[0].action_id[0]));
    CHECK(mrc_state_device_requested());
    CHECK(mrc_state_take_requested_device() == NULL);
    CHECK(mrc_state_take_stop());
    CHECK(!mrc_state_take_stop());
    CHECK(!mrc_state_device_requested());
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 1);
    mrc_devices_panel_close(ui);
    mrc_devices_panel_open(ui, NULL); /* launcher does this after teardown */
    rows = mrc_ui_panel_rows(ui);
    CHECK(rows[0].action_icon[0] == MRC_UI_ICON_PLAY);
    CHECK(mrc_devices_panel_row(ui, rows[0].action_id[0]));
    CHECK(!strcmp(mrc_state_take_requested_device(), "sony-pic-2000"));
    int last_trash = rows[0].action_id[1];
    CHECK(mrc_devices_panel_row(ui, last_trash));
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 1);
    CHECK(mrc_devices_panel_row(ui, last_trash));
    CHECK(mrc_devices_list(listed, MRC_DEVICE_MAX) == 0);
    CHECK(row_with(mrc_ui_panel_rows(ui), mrc_ui_panel_row_count(ui), "no devices yet") >= 0);
    mrc_state_request_stop();
    mrc_state_request_device("replacement");
    CHECK(!mrc_state_take_stop());
    CHECK(!strcmp(mrc_state_take_requested_device(), "replacement"));
    mrc_state_request_device("replacement");
    mrc_state_request_stop();
    CHECK(mrc_state_take_requested_device() == NULL);
    CHECK(mrc_state_take_stop());
    mrc_devices_panel_close(ui);
    CHECK(!mrc_devices_panel_showing());
    {
        mrc_device leftover[MRC_DEVICE_MAX];
        unsigned k = mrc_devices_list(leftover, MRC_DEVICE_MAX);
        for (unsigned i = 0; i < k; i++) mrc_device_delete(&leftover[i]);
    }

    mrc_ui_close(ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", scratch);
    if (system(cleanup)) { /* a temp directory left behind is not a failure */ }

    if (failures) fprintf(stderr, "%d device checks failed\n", failures);
    else printf("all device and picker checks passed\n");
    return failures ? 1 : 0;
}
