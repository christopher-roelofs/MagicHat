/*
 * settings_panel.h — the things you reach for rarely.
 *
 * The power action simulates pressing and releasing the physical button.
 * The guest determines what that does; it is separate from closing the
 * emulator window and saving its state.
 *
 * Shared between the frontends because both machines answer the same way,
 * and because a settings sheet written twice becomes two different sheets.
 */
#ifndef MRC_SDL_SETTINGS_PANEL_H
#define MRC_SDL_SETTINGS_PANEL_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"
#include "runtime/machine.h"

/* Open the sheet for this machine. Rows it cannot offer are left out rather
 * than shown doing nothing. */
void mrc_settings_panel_open(mrc_ui *ui, mrc_runtime *m);
void mrc_settings_panel_close(mrc_ui *ui);
bool mrc_settings_panel_showing(void);

/*
 * Hand it a row the rail reported. True when the panel consumed it.
 *
 * Acting on a row is done here, because everything on this sheet is a
 * question the runtime can answer for itself.
 */
bool mrc_settings_panel_row(mrc_ui *ui, mrc_runtime *m, int id);

#endif /* MRC_SDL_SETTINGS_PANEL_H */
