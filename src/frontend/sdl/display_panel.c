#include "frontend/sdl/display_panel.h"
#include "frontend/sdl/lcd.h"

/*
 * The identifiers the rail hands back. They are ours, not positions, so
 * reordering the rows changes nothing about what a press means.
 */
enum { ROW_STRUCTURE = 1, ROW_TINT, ROW_SMOOTH, ROW_INTEGER };

static mrc_ui_row rows[6];

void mrc_display_rows_refresh(void)
{
    unsigned k = 0;
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_HEADING, "Panel", NULL, false, 0 };
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_TOGGLE, "Dot structure", NULL,
                              mrc_gui_lcd, ROW_STRUCTURE };
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_CHOICE, "Backlight",
                              mrc_tint_name(mrc_gui_tint), false, ROW_TINT };
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_HEADING, "Scaling", NULL, false, 0 };
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_TOGGLE, "Smooth", NULL,
                              mrc_gui_smooth, ROW_SMOOTH };
    rows[k++] = (mrc_ui_row){ MRC_UI_ROW_TOGGLE, "Whole pixels only", NULL,
                              mrc_gui_integer, ROW_INTEGER };
}

const mrc_ui_row *mrc_display_rows(void) { return rows; }
unsigned mrc_display_row_count(void) { return sizeof rows / sizeof *rows; }

void mrc_display_row(int id)
{
    switch (id) {
    case ROW_STRUCTURE: mrc_gui_lcd = !mrc_gui_lcd; break;
    case ROW_SMOOTH:    mrc_gui_smooth = !mrc_gui_smooth; break;
    case ROW_INTEGER:   mrc_gui_integer = !mrc_gui_integer; break;
    case ROW_TINT:
        /* One row cycling the four is shorter than four rows, and a
         * backlight is a thing with an order rather than a set. */
        mrc_gui_tint = (mrc_gui_tint + 1) % (MRC_TINT_GREY + 1);
        break;
    default:
        return;         /* not ours */
    }
    mrc_display_rows_refresh();
}
