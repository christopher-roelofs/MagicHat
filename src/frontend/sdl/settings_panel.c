#include "frontend/sdl/settings_panel.h"
#include "frontend/sdl/card_panel.h"

#include <stdio.h>

/* Ids of our own, so the frontend's rows and the other panels' cannot be
 * mistaken for them. */
#define ROW_POWER 0x5000
#define ROW_SIDE  0x5001
#define ROW_CARDS 0x5002

static mrc_ui_row rows[5];
static unsigned   row_count;
static bool       showing;

static void build(mrc_ui *ui, mrc_runtime *m)
{
    row_count = 0;
    rows[row_count++] = (mrc_ui_row){
        .kind = MRC_UI_ROW_CHOICE,
        .label = "Controls on the",
        .value = mrc_ui_get_side(ui) == MRC_UI_LEFT ? "left" : "right",
        .id = ROW_SIDE,
    };
    if (m && m->ops->card_insert_sram) {
        rows[row_count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_ACTION,
            .label = "PC Cards",
            .value = "insert, eject, or configure",
            .id = ROW_CARDS,
        };
    }
    if (m && m->ops->power_button) {
        rows[row_count++] = (mrc_ui_row){
            .kind = MRC_UI_ROW_ACTION,
            .label = "Press power button",
            .id = ROW_POWER,
        };
    }
    mrc_ui_open_panel(ui, "Settings", rows, row_count);
    showing = true;
}

void mrc_settings_panel_open(mrc_ui *ui, mrc_runtime *m)
{
    build(ui, m);
}

void mrc_settings_panel_close(mrc_ui *ui)
{
    if (mrc_ui_panel_open(ui)) mrc_ui_close_panel(ui);
    showing = false;
}

bool mrc_settings_panel_showing(void)
{
    return showing;
}

bool mrc_settings_panel_row(mrc_ui *ui, mrc_runtime *m, int id)
{
    if (!showing) return false;
    switch (id) {
    case ROW_CARDS:
        /* The card sheet replaces Settings; keep only one panel owner. */
        mrc_settings_panel_close(ui);
        mrc_card_panel_open(ui, m);
        return true;
    case ROW_SIDE:
        mrc_ui_set_side(ui, mrc_ui_get_side(ui) == MRC_UI_LEFT
                            ? MRC_UI_RIGHT : MRC_UI_LEFT);
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
        mrc_settings_panel_close(ui);
        return true;
    default:
        return false;
    }
}
