#include "frontend/sdl/devices_panel.h"
#include "frontend/sdl/picker.h"
#include "host/devices.h"
#include "host/import.h"
#include "host/state.h"
#include "rom/identify.h"

#include <stdio.h>
#include <string.h>

/*
 * Row ids, in a range of their own so that the picker's and the frontend's
 * cannot be mistaken for them. The devices themselves start at BASE and run
 * in listing order; TRASH_BASE runs the same way for their trash cans.
 */
#define ROW_NEW    0x6000
#define ROW_BASE   0x6100
#define ROW_TRASH_BASE 0x6200
#define ROW_CANCEL_BASE 0x6300
#define ROW_STOP 0x6400

static mh_device devices[MH_DEVICE_MAX];
static unsigned   device_count;

/* The rows the rail borrows, so they outlive the call that built them. Each
 * label points at the name in `devices` beside it, and each value at the
 * line composed for it here. */
static mh_ui_row rows[MH_DEVICE_MAX + 3];
static char       marks[MH_DEVICE_MAX][64];
static unsigned   row_count;

/*
 * What went wrong, if anything did, shown at the top of the list.
 *
 * A window has no stderr anybody is watching. Without this, a device that
 * could not be made looks exactly like a device that was made and then
 * hidden -- the sheet goes back to the list and nothing else happens, which
 * is the most confusing thing an interface can do.
 */
static char notice[160];

static char current_id[MH_DEVICE_ID_MAX];
static char taken_id[MH_DEVICE_ID_MAX];
static bool have_taken;

static mh_picker *picker;
static bool showing;

/*
 * Deleting is not undoable -- the firmware copy and everything the device
 * ever saved go with it -- so the trash can arms rather than fires. The
 * first tap turns the row into a plain question and remembers which device;
 * a second tap on the same trash can carries it out. Anything else --
 * another row, the picker, reopening -- disarms it instead of deleting.
 */
static char armed_id[MH_DEVICE_ID_MAX];

/* Where the picker last looked, so making a second device does not start
 * again from the beginning. */
static char last_directory[4096];

/*
 * The confirmation row's label, filled in when it is built. One buffer:
 * only one device can be armed at a time, since arming any other disarms
 * whichever was armed before.
 */
static char armed_label[96];

static void build(mh_ui *ui)
{
    device_count = mh_devices_list(devices, MH_DEVICE_MAX);
    row_count = 0;
    if (notice[0])
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_HEADING, .label = notice,
        };
    for (unsigned i = 0; i < device_count; i++) {
        bool armed = armed_id[0] && !strcmp(devices[i].id, armed_id);
        if (armed) {
            /*
             * Asking, in place of the device's own row, rather than beside
             * it: a confirmation that left the original label and its play
             * button standing right next to the trash can that arms it would
             * be a confirmation waiting to be misread as an ordinary row.
             * Nothing on this row acts except the trash can itself -- the
             * body is not a second way to say yes, and tapping it, like
             * tapping anything else, cancels instead.
             *
             * Its id is a real one of its own rather than -1. This row is
             * shown while the rail is fielding ordinary pointer motion over
             * the sheet too, and every one of those arrives here the same
             * way a tap does; -1 would have meant both "nothing happened"
             * and "the row body was deliberately tapped", and there would
             * have been no telling which one a given call was. A stray
             * hover was cancelling the question before a real second tap
             * on the trash can could ever land on it.
             */
            snprintf(armed_label, sizeof(armed_label), "Delete \"%s\"?",
                     devices[i].name);
            rows[row_count++] = (mh_ui_row){
                .kind = MH_UI_ROW_ACTION,
                .label = armed_label,
                .value = "tap the trash can again to confirm",
                .id = (int)(ROW_CANCEL_BASE + i),
                .action_count = 1,
                .action_icon = { MH_UI_ICON_TRASH },
                .action_id = { (int)(ROW_TRASH_BASE + i) },
            };
            continue;
        }
        /*
         * What kind of machine it is, which is the thing a list of names
         * cannot tell you -- two devices called "Spare" are not the same
         * device -- and then its condition, if it has one worth saying.
         */
        const char *kind = mh_rom_device_name(
            mh_rom_device_from_slug(devices[i].machine));
        const char *state = NULL;
        if (current_id[0] && !strcmp(devices[i].id, current_id)) state = "in use";
        else if (!devices[i].has_state) state = "not started";
        snprintf(marks[i], sizeof(marks[i]), "%s%s%s", kind,
                 state ? " \xc2\xb7 " : "", state ? state : "");
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_ACTION,
            .label = devices[i].name,
            .value = marks[i],
            .id = (int)(ROW_BASE + i),
            .action_count = 2,
            .action_icon = { !strcmp(devices[i].id, current_id) ? MH_UI_ICON_STOP : MH_UI_ICON_PLAY,
                             MH_UI_ICON_TRASH },
            .action_id = { !strcmp(devices[i].id, current_id) ? ROW_STOP : (int)(ROW_BASE + i),
                           (int)(ROW_TRASH_BASE + i) },
        };
    }
    if (!device_count)
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_HEADING,
            .label = "no devices yet",
        };
    rows[row_count++] = (mh_ui_row){
        .kind = MH_UI_ROW_ACTION,
        .label = "New device\xe2\x80\xa6",
        .value = mh_import_pending(MH_IMPORT_ROM) ? "choosing a ROM\xe2\x80\xa6" : "from a ROM",
        .id = ROW_NEW,
    };
    mh_ui_open_panel(ui, "Devices", rows, row_count);
    showing = true;
}

void mh_devices_panel_open(mh_ui *ui, const char *current)
{
    snprintf(current_id, sizeof(current_id), "%s", current ? current : "");
    if (picker) { mh_picker_close(picker); picker = NULL; }
    notice[0] = 0;          /* reopening is a fresh look, not a stale warning */
    armed_id[0] = 0;        /* likewise: not a live question from before */
    build(ui);
}

/*
 * An image the platform went off to fetch. It arrives long after the button
 * was pressed -- a person browsing their files takes as long as they take --
 * so the list keeps drawing and picks it up whenever it appears.
 */
void mh_devices_panel_tick(mh_ui *ui)
{
    if (!showing || picker) return;
    char arrived[4096], error[256];
    static bool was_pending;
    if (mh_import_result(MH_IMPORT_ROM, arrived, sizeof(arrived), error, sizeof(error))) {
        mh_device made;
        if (error[0]) snprintf(notice, sizeof(notice), "%s", error);
        else if (!arrived[0] || mh_device_create(arrived, NULL, &made)) notice[0] = 0;
        else snprintf(notice, sizeof(notice), "%s",
                      mh_devices_last_error() ? mh_devices_last_error()
                                               : "could not make that device");
        build(ui);
    } else if (was_pending != mh_import_pending(MH_IMPORT_ROM)) {
        build(ui);          /* the row says whether we are still waiting */
    }
    was_pending = mh_import_pending(MH_IMPORT_ROM);
}

void mh_devices_panel_close(mh_ui *ui)
{
    mh_import_cancel(MH_IMPORT_ROM);
    if (picker) {
        /* Remember where it was looking: ROMs live together, so the next one
         * is almost certainly beside the last. */
        const char *at = mh_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        mh_picker_close(picker);
        picker = NULL;
    }
    if (mh_ui_panel_open(ui)) mh_ui_close_panel(ui);
    showing = false;
}

bool mh_devices_panel_showing(void)
{
    return showing;
}

bool mh_devices_panel_back(mh_ui *ui)
{
    if (!showing || !picker) return false;
    const char *at = mh_picker_directory(picker);
    if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
    mh_picker_close(picker); picker = NULL;
    build(ui);
    return true;
}

bool mh_devices_panel_row(mh_ui *ui, int id)
{
    if (!showing) return false;

    /* While the picker is up it answers for its own rows, and a chosen file
     * is a device to make. */
    if (picker) {
        if (!mh_picker_row(picker, id)) return false;
        const char *chosen = mh_picker_taken(picker);
        if (!chosen) return true;          /* walked into a directory */
        const char *at = mh_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        /*
         * NULL is the name, which means "call it after the file". This is
         * where being asked for one goes: a device is named once, when it is
         * made, and the filename is the default a person is offered rather
         * than the only thing they can have. Typing needs a text row the
         * panel does not have yet, so for now the default stands and the
         * device can be renamed afterwards.
         */
        mh_device made;
        bool ok = mh_device_create(chosen, NULL, &made);
        /* Say so where the person is looking, not only on a stream they
         * cannot see. */
        if (ok) notice[0] = 0;
        else snprintf(notice, sizeof(notice), "%s",
                      mh_devices_last_error() ? mh_devices_last_error()
                                               : "could not make that device");
        mh_picker_close(picker);
        picker = NULL;
        build(ui);
        return true;
    }

    /*
     * -1 is what a completed tap on nothing produces, but it is also what
     * the frontend hands over on every ordinary pointer motion across the
     * sheet -- the rail takes the event to update which row is hot, and
     * whatever it read back from mh_ui_take_row is passed on here whether
     * or not anything actually happened. There is no question to answer
     * about a hover, so it is not treated as one: nothing below this reacts
     * to an id that never named a row.
     */
    if (id < 0) return false;

    /*
     * Anything other than a second tap on the very trash can that armed it
     * cancels an armed delete -- another row, a different device's trash,
     * even a tap on the confirmation row's own inert body. Silence would
     * leave a question standing that the person has already walked away
     * from.
     */
    bool confirmed = false;
    bool is_ours = id == ROW_NEW || id == ROW_STOP ||
                  (id >= ROW_BASE && id < (int)(ROW_BASE + device_count)) ||
                  (id >= ROW_TRASH_BASE && id < (int)(ROW_TRASH_BASE + device_count));
    if (armed_id[0]) {
        for (unsigned i = 0; i < device_count; i++) {
            if (strcmp(devices[i].id, armed_id)) continue;
            confirmed = id == (int)(ROW_TRASH_BASE + i);
            break;
        }
        if (!confirmed) {
            armed_id[0] = 0;
            /*
             * Everything this id could still go on to do -- switch to
             * another device, arm a different one's delete, open the picker
             * -- rebuilds again before it returns. The one case that will
             * not is a tap this panel has no other business with (the
             * confirmation row's own inert body, or the window beyond the
             * sheet), which still has to clear the question off the screen,
             * so that is done here instead of left to happen downstream.
             */
            build(ui);
            if (!is_ours) return true;
        }
    }

    if (id >= ROW_TRASH_BASE && id < (int)(ROW_TRASH_BASE + device_count)) {
        const mh_device *d = &devices[id - ROW_TRASH_BASE];
        /*
         * The device in use is not offered for deletion: its files are open
         * under it, and a save from the running machine into a directory
         * that has just been removed is a worse failure than making the
         * person switch away first.
         */
        if (current_id[0] && !strcmp(d->id, current_id)) {
            snprintf(notice, sizeof(notice), "%s",
                     "Press Stop on this device before deleting it");
            armed_id[0] = 0;
        } else if (confirmed) {
            if (!mh_device_delete(d))
                snprintf(notice, sizeof(notice), "%s",
                         mh_devices_last_error() ? mh_devices_last_error()
                                                  : "could not delete that device");
            else notice[0] = 0;
            armed_id[0] = 0;
        } else {
            snprintf(armed_id, sizeof(armed_id), "%s", d->id);
        }
        build(ui);
        return true;
    }

    if (id == ROW_STOP && current_id[0]) {
        mh_state_request_stop();
        return true;
    }

    if (id == ROW_NEW) {
        /*
         * Where the platform keeps a picker of its own, use it: on Android
         * the image the person wants is in storage this program cannot walk,
         * and the system's document picker is the only way to reach it.
         */
        if (mh_import_available()) {
            if (!mh_import_request(MH_IMPORT_ROM))
                snprintf(notice, sizeof(notice), "Another file import is still finishing");
            build(ui);
            return true;
        }
        /*
         * The firmware for a new device. Both extensions the machines use,
         * because a person should not have to remember which of their images
         * was named which.
         */
        static const char *const kinds[] = { ".rom", ".image", ".bin" };
        picker = mh_picker_open(ui, "Choose firmware",
                                 last_directory[0] ? last_directory : NULL,
                                 kinds, 3);
        if (!picker) {
            fprintf(stderr, "[gui] could not open the file picker\n");
            build(ui);
        }
        return true;
    }

    if (id >= ROW_BASE && id < (int)(ROW_BASE + device_count)) {
        const mh_device *d = &devices[id - ROW_BASE];
        if (!strcmp(d->id, current_id)) return true;   /* already running it */
        snprintf(taken_id, sizeof(taken_id), "%s", d->id);
        have_taken = true;
        /*
         * Leave the name where whoever started this machine will find it.
         * Putting this one down and starting that one is not something a
         * panel can do, and the two need not even be the same kind of
         * machine.
         */
        mh_state_request_device(d->id);
        return true;
    }
    return false;
}

const char *mh_devices_panel_taken(void)
{
    if (!have_taken) return NULL;
    have_taken = false;
    return taken_id;
}
