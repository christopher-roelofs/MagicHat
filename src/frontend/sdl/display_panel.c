#include "frontend/sdl/display_panel.h"
#include "frontend/sdl/lcd.h"

/*
 * The identifiers the rail hands back. They are ours, not positions, so
 * reordering the rows changes nothing about what a press means.
 */
enum { ROW_STRUCTURE = 1, ROW_TINT, ROW_SMOOTH, ROW_INTEGER };

static mh_ui_row rows[6];

void mh_display_rows_refresh(void)
{
    unsigned k = 0;
    rows[k++] = (mh_ui_row){ MH_UI_ROW_HEADING, "Panel", NULL, false, 0 };
    rows[k++] = (mh_ui_row){ MH_UI_ROW_TOGGLE, "Dot structure", NULL,
                              mh_gui_lcd, ROW_STRUCTURE };
    rows[k++] = (mh_ui_row){ MH_UI_ROW_CHOICE, "Backlight",
                              mh_tint_name(mh_gui_tint), false, ROW_TINT };
    rows[k++] = (mh_ui_row){ MH_UI_ROW_HEADING, "Scaling", NULL, false, 0 };
    rows[k++] = (mh_ui_row){ MH_UI_ROW_TOGGLE, "Smooth", NULL,
                              mh_gui_smooth, ROW_SMOOTH };
    rows[k++] = (mh_ui_row){ MH_UI_ROW_TOGGLE, "Whole pixels only", NULL,
                              mh_gui_integer, ROW_INTEGER };
}

const mh_ui_row *mh_display_rows(void) { return rows; }
unsigned mh_display_row_count(void) { return sizeof rows / sizeof *rows; }

void mh_display_row(int id)
{
    switch (id) {
    case ROW_STRUCTURE: mh_gui_lcd = !mh_gui_lcd; break;
    case ROW_SMOOTH:    mh_gui_smooth = !mh_gui_smooth; break;
    case ROW_INTEGER:   mh_gui_integer = !mh_gui_integer; break;
    case ROW_TINT:
        /* One row cycling the four is shorter than four rows, and a
         * backlight is a thing with an order rather than a set. */
        mh_gui_tint = (mh_gui_tint + 1) % (MH_TINT_GREY + 1);
        break;
    default:
        return;         /* not ours */
    }
    mh_display_rows_refresh();
}
