#include "frontend/sdl/install_panel.h"
#include "frontend/sdl/picker.h"
#include "host/pclink.h"
#include "host/import.h"

#include <stdio.h>
#include <string.h>

/* Ids of our own, clear of the other panels' and the picker's. */
#define ROW_CHOOSE 0x4000

static mh_ui_row  rows[5];
static unsigned    row_count;
static bool        showing;
static mh_picker *picker;
static mh_pclink *link;
static char        status[160];
static char        notice[256];
static char        progress[64];
static char        last_directory[4096];

static void build(mh_ui *ui)
{
    row_count = 0;
    if (notice[0])
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_HEADING, .label = notice,
        };
    /*
     * What the link is doing, or why there is none.
     *
     * Shown whenever there is anything to say, not only once a transfer
     * exists -- a refusal written into a variable nobody draws is a button
     * that silently does nothing, which is the bug the devices list had.
     *
     * Magic Cap only starts listening once someone opens the Storeroom and
     * taps the computer, so a transfer that has not started yet is waiting on
     * the guest rather than broken, and the message says so.
     */
    if (link) snprintf(status, sizeof(status), "%s", mh_pclink_message(link));
    if (status[0])
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_HEADING, .label = status,
        };
    if (link) {
        uint32_t total = mh_pclink_total(link);
        uint32_t sent = mh_pclink_sent(link);
        snprintf(progress, sizeof(progress), "%u of %u bytes", sent, total);
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_CHOICE, .label = "Sent", .value = progress,
        };
    }
    rows[row_count++] = (mh_ui_row){
        .kind = MH_UI_ROW_ACTION,
        .label = link ? "Send another\xe2\x80\xa6" : "Choose a package\xe2\x80\xa6",
        .value = "over the guest's link",
        .id = ROW_CHOOSE,
    };
    mh_ui_open_panel(ui, "Install", rows, row_count);
    showing = true;
}

void mh_install_panel_open(mh_ui *ui, mh_runtime *m)
{
    (void)m;
    notice[0] = 0;
    if (picker) { mh_picker_close(picker); picker = NULL; }
    if (!link) status[0] = 0;   /* a fresh look, not a stale refusal */
    build(ui);
}

void mh_install_panel_close(mh_ui *ui)
{
    mh_import_cancel(MH_IMPORT_PACKAGE);
    if (picker) {
        const char *at = mh_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        mh_picker_close(picker);
        picker = NULL;
    }
    if (mh_ui_panel_open(ui)) mh_ui_close_panel(ui);
    showing = false;
}

bool mh_install_panel_showing(void)
{
    return showing;
}

bool mh_install_panel_back(mh_ui *ui, mh_runtime *m)
{
    if (!showing || !picker) return false;
    const char *at = mh_picker_directory(picker);
    if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
    mh_picker_close(picker); picker = NULL;
    build(ui);
    (void)m;
    return true;
}

void mh_install_panel_tick(mh_ui *ui, mh_runtime *m)
{
    if (showing && !picker) {
        char path[4096], error[256];
        if (mh_import_result(MH_IMPORT_PACKAGE, path, sizeof(path), error, sizeof(error))) {
            notice[0] = 0;
            if (error[0]) snprintf(notice, sizeof(notice), "%s", error);
            else if (path[0]) {
                link = m && m->ops->install ? m->ops->install(m->board, path) : NULL;
                if (!link) snprintf(status, sizeof(status), "Could not start that transfer");
            } else snprintf(notice, sizeof(notice), "File selection cancelled");
            build(ui);
        }
    }
    /* Only while the sheet is up and the picker is not covering it: the rows
     * are what is on screen, so rebuilding is how the numbers move. */
    if (!showing || picker || !link) return;
    uint32_t sent = mh_pclink_sent(link);
    static uint32_t shown = ~0u;
    static mh_pclink_state was = MH_PCLINK_FAILED;
    mh_pclink_state now = mh_pclink_state_of(link);
    /* A few hundred bytes at a time, so the sheet is not rebuilt per byte. */
    if (now == was && sent / 512 == shown / 512) return;
    shown = sent;
    was = now;
    build(ui);
}

bool mh_install_panel_row(mh_ui *ui, mh_runtime *m, int id)
{
    if (!showing) return false;

    if (picker) {
        if (!mh_picker_row(picker, id)) return false;
        const char *chosen = mh_picker_taken(picker);
        if (!chosen) return true;          /* walked into a directory */
        char path[4096];
        snprintf(path, sizeof(path), "%s", chosen);
        const char *at = mh_picker_directory(picker);
        if (at) snprintf(last_directory, sizeof(last_directory), "%s", at);
        mh_picker_close(picker);
        picker = NULL;
        link = m && m->ops->install ? m->ops->install(m->board, path) : NULL;
        if (!link)
            snprintf(status, sizeof(status), "could not start that transfer");
        build(ui);
        return true;
    }

    if (id == ROW_CHOOSE) {
        notice[0] = 0;
        if (!m || !m->ops->install) {
            snprintf(status, sizeof(status),
                     "this machine has no link to a computer yet");
            build(ui);
            return true;
        }
        if (mh_import_available()) {
            snprintf(notice, sizeof(notice), "%s", mh_import_request(MH_IMPORT_PACKAGE)
                     ? "Choose a package in the system file picker"
                     : "Another file import is still finishing");
            build(ui);
            return true;
        }
        /*
         * The extensions packages turn up under. They are only a filter for
         * the list -- what decides whether a file is a package is the
         * container signature, checked when the transfer opens, so a package
         * named something else still works if it is picked deliberately.
         */
        static const char *const kinds[] = { ".pkg", ".package", ".mc2" };
        picker = mh_picker_open(ui, "Choose a package",
                                 last_directory[0] ? last_directory : NULL,
                                 kinds, 3);
        if (!picker) build(ui);
        return true;
    }
    return false;
}
