/*
 * display_panel.h — the rows behind the rail's display button.
 *
 * Shared for the same reason the effects themselves are: both frontends
 * offer exactly these four settings now, and a panel written twice is a
 * panel that ends up describing two different things.
 *
 * The rows are a static array the rail borrows rather than copies, so it
 * is rebuilt in place whenever a setting changes. That is what keeps what
 * is drawn and what is set from drifting apart.
 */
#ifndef MH_SDL_DISPLAY_PANEL_H
#define MH_SDL_DISPLAY_PANEL_H

#include "frontend/sdl/ui.h"

/* Bring the rows up to date with the settings as they are now. */
void mh_display_rows_refresh(void);
const mh_ui_row *mh_display_rows(void);
unsigned mh_display_row_count(void);

/*
 * Act on a row the rail reported, and rebuild. Ignores anything that is
 * not one of ours, so a frontend can pass every row it is given.
 */
void mh_display_row(int id);

#endif /* MH_SDL_DISPLAY_PANEL_H */
