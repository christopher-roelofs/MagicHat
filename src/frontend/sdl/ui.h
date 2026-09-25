/*
 * ui.h — the emulator's own controls, drawn beside the guest.
 *
 * A vertical rail of buttons down the left edge, identical on a desktop and
 * on a tablet because it is the same code drawing into the same renderer
 * that already draws the guest. That is the whole reason it is here rather
 * than in each platform's native toolkit: two toolkits means two designs
 * that drift, and the Android side already runs SDL.
 *
 * The rail takes space rather than floating over the guest. Magic Cap uses
 * its panel out to the edges, so an overlay would both hide guest pixels
 * and swallow taps meant for them. Every frontend lays the guest out inside
 * mrc_sdl_panel_rect, which subtracts the rail, so the drawing and the pen
 * mapping cannot disagree about where the guest is.
 *
 * Nothing here knows what a machine is. The rail reports which button was
 * pressed and the frontend decides what that means, because the two
 * frontends answer differently -- a DataRover has a state to save and a
 * PIC-2000 has not.
 */
#ifndef MRC_SDL_UI_H
#define MRC_SDL_UI_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include "frontend/sdl/ui_assets.h"

/* Returned by mrc_ui_take_row when the panel header's Back button is tapped. */
#define MRC_UI_ROW_BACK (-2)
#define MRC_UI_ROW_DISMISS (-3)

typedef struct mrc_ui mrc_ui;

/*
 * Which edge the rail lives on. Left-handers and right-handers hold a
 * tablet differently, and the rail wants to be under the thumb that is
 * not holding it.
 *
 * MRC_UI_RAIL_SIDE=right in the environment picks it at start-up, the way
 * MRC_DISPLAY_ROTATION picks the rotation, and a settings panel can change
 * it while running.
 */
typedef enum { MRC_UI_LEFT, MRC_UI_RIGHT } mrc_ui_side;
void mrc_ui_set_side(mrc_ui *ui, mrc_ui_side side);
mrc_ui_side mrc_ui_get_side(const mrc_ui *ui);

/*
 * Fails only when the renderer will not give up a texture, in which case
 * the caller carries on without a rail rather than refusing to start.
 *
 * `window` is the one the renderer draws into. It is passed rather than
 * asked for because SDL_RenderGetWindow arrived in 2.0.22 and the boards
 * this is cross-checked on are older, and because the caller created both
 * and has it to hand.
 */
bool mrc_ui_open(mrc_ui **ui, SDL_Renderer *renderer, SDL_Window *window);
void mrc_ui_close(mrc_ui *ui);

/*
 * Which buttons this machine has, as a mask of 1 << MRC_UI_ICON_*. A
 * button that is not offered is not drawn and cannot be pressed, so the
 * rail is shorter on a machine with less to say rather than showing
 * controls that do nothing.
 */
void mrc_ui_set_buttons(mrc_ui *ui, unsigned mask);

/* Light a button, for the ones that are a state rather than an action:
 * the option key is held down or it is not. */
void mrc_ui_set_lit(mrc_ui *ui, int icon, bool lit);

/*
 * How much of the output width the rail occupies, which is what the panel
 * layout has to subtract. Zero when there is no rail, so a frontend that
 * never opened one needs no special case.
 *
 * The two insets are the same number on whichever side the rail is, and
 * zero on the other. Layout takes both rather than a width and a side, so
 * that it never has to know which is which.
 */
int mrc_ui_rail_width(const mrc_ui *ui);
int mrc_ui_inset_left(const mrc_ui *ui);
int mrc_ui_inset_right(const mrc_ui *ui);

/*
 * Offer an event. True means the interface took it and the guest must not
 * see it, which is how a tap on the rail stops being a tap on the screen.
 */
bool mrc_ui_event(mrc_ui *ui, const SDL_Event *event);

/* Draw the rail over whatever has been rendered, before presenting. */
void mrc_ui_draw(mrc_ui *ui);

/*
 * The button pressed since this was last asked, or -1. Taking it clears
 * it, so a frontend that forgets to ask does not act on a stale press
 * three seconds later.
 */
int mrc_ui_take_action(mrc_ui *ui);

/*
 * A panel: the sheet a rail button opens.
 *
 * Its rows come from the frontend rather than from here, because what a
 * machine can be asked is a fact about the machine. The DataRover has a
 * panel structure and a backlight tint to offer and the PIC-2000 has
 * neither, and a panel that knew about both would have to be told which
 * anyway.
 */
typedef enum {
    MRC_UI_ROW_ACTION,    /* something to do, once                       */
    MRC_UI_ROW_TOGGLE,    /* on or off, shown as a switch                */
    MRC_UI_ROW_CHOICE,    /* one of several; `value` names the current   */
    MRC_UI_ROW_HEADING,   /* a label over a group; cannot be pressed     */
} mrc_ui_row_kind;

typedef struct {
    mrc_ui_row_kind kind;
    const char *label;
    const char *value;    /* CHOICE only: what it is set to now */
    bool        on;       /* TOGGLE only */
    int         id;       /* the frontend's own name for this row */

    /*
     * Small buttons at the row's right edge, in addition to the row body
     * itself -- a play button and a trash can beside a device, say. Zero by
     * default: a row that does not mention this gets none, which is what
     * the count has to mean regardless of what is left in the arrays next
     * to it, since every row literal that predates this leaves them unset.
     *
     * Taking one reports its own id through mrc_ui_take_row, in place of
     * the row's -- pressing the trash can is not pressing the row.
     */
    unsigned    action_count;      /* 0, 1, or 2 */
    int         action_icon[2];    /* MRC_UI_ICON_*, valid below action_count */
    int         action_id[2];
} mrc_ui_row;

/*
 * Show these rows. The array is borrowed, not copied, so it has to outlive
 * the panel -- a static array in the frontend, rebuilt in place when a
 * setting changes, which is what keeps the two from disagreeing about what
 * is on screen.
 */
void mrc_ui_open_panel(mrc_ui *ui, const char *title,
                       const mrc_ui_row *rows, unsigned count);
void mrc_ui_close_panel(mrc_ui *ui);
bool mrc_ui_panel_open(const mrc_ui *ui);

/*
 * The row activated since this was last asked, by its id, or -1. Taking it
 * clears it. The panel stays open, because changing one setting usually
 * means changing another.
 */
int mrc_ui_take_row(mrc_ui *ui);

/* What the panel is showing. The rows are the caller's own array, handed
 * back; mostly useful for asking what is on screen without keeping a second
 * copy of it. */
const mrc_ui_row *mrc_ui_panel_rows(const mrc_ui *ui);
unsigned mrc_ui_panel_row_count(const mrc_ui *ui);

/* Text, for whoever is drawing a panel. Returns the width it drew. */
int mrc_ui_text(mrc_ui *ui, int x, int y, const char *s, SDL_Color colour);
int mrc_ui_text_width(const mrc_ui *ui, const char *s);
int mrc_ui_text_height(const mrc_ui *ui);

#endif /* MRC_SDL_UI_H */
