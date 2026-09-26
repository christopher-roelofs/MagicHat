#include "frontend/sdl/settings_panel.h"
#include "frontend/sdl/card_panel.h"

#include <stdio.h>

/* Ids of our own, so the frontend's rows and the other panels' cannot be
 * mistaken for them. */
#define ROW_POWER 0x5000
#define ROW_SIDE  0x5001
#define ROW_CARDS 0x5002

static mh_ui_row rows[5];
static unsigned   row_count;
static bool       showing;

static void build(mh_ui *ui, mh_runtime *m)
{
    row_count = 0;
    rows[row_count++] = (mh_ui_row){
        .kind = MH_UI_ROW_CHOICE,
        .label = "Controls on the",
        .value = mh_ui_get_side(ui) == MH_UI_LEFT ? "left" : "right",
        .id = ROW_SIDE,
    };
    if (m && m->ops->card_insert_sram) {
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_ACTION,
            .label = "PC Cards",
            .value = "insert, eject, or configure",
            .id = ROW_CARDS,
        };
    }
    if (m && m->ops->power_button) {
        rows[row_count++] = (mh_ui_row){
            .kind = MH_UI_ROW_ACTION,
            .label = "Press power button",
            .id = ROW_POWER,
        };
    }
    mh_ui_open_panel(ui, "Settings", rows, row_count);
    showing = true;
}

void mh_settings_panel_open(mh_ui *ui, mh_runtime *m)
{
    build(ui, m);
}

void mh_settings_panel_close(mh_ui *ui)
{
    if (mh_ui_panel_open(ui)) mh_ui_close_panel(ui);
    showing = false;
}

bool mh_settings_panel_showing(void)
{
    return showing;
}

bool mh_settings_panel_row(mh_ui *ui, mh_runtime *m, int id)
{
    if (!showing) return false;
    switch (id) {
    case ROW_CARDS:
        /* The card sheet replaces Settings; keep only one panel owner. */
        mh_settings_panel_close(ui);
        mh_card_panel_open(ui, m);
        return true;
    case ROW_SIDE:
        mh_ui_set_side(ui, mh_ui_get_side(ui) == MH_UI_LEFT
                            ? MH_UI_RIGHT : MH_UI_LEFT);
        build(ui, m);           /* the row shows which side it is now on */
        return true;
    case ROW_POWER:
        /*
         * A press and a release, not a blocking wait for the guest to finish.
         * The guest handles this like its physical power button; do not
         * promise a particular power state or next-boot behavior in the UI.
         */
        if (m && m->ops->power_button) {
            m->ops->power_button(m->board, true);
            m->ops->power_button(m->board, false);
            fprintf(stderr, "[gui] pressed power button\n");
        }
        mh_settings_panel_close(ui);
        return true;
    default:
        return false;
    }
}
