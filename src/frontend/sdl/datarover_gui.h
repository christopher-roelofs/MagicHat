/*
 * gui.h — optional SDL window onto the machine.
 *
 * Built only when SDL2 is present; without it mrc_gui_available() returns
 * false and mrc_gui_run() explains why.
 */
#ifndef MRC_GUI_H
#define MRC_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "machines/datarover840/machine.h"

bool mrc_gui_available(void);

/* Runs until the window is closed, or until `insns_total` instructions have
 * been executed (0 = no limit). */
/* Sound in the window is opt-in: see --audio in main.c and docs/OPEN_QUESTIONS.md. */
extern bool mrc_gui_audio;

/* The panel's look is shared with the 68k frontend. Only the settings are
 * taken here: the rendering in lcd.h needs SDL, and a command line that
 * includes SDL on Android loses its own main() to SDL_main. */
#include "frontend/sdl/look.h"
extern bool mrc_gui_touch_debug; /* draw a cross where the pen went down */

/* Runs the machine in a window the caller already owns, so that moving from
 * one device to another reshapes what is on screen rather than replacing it. */
struct mrc_shell;
int  mrc_gui_run(machine *m, uint64_t insns_total, struct mrc_shell *shell);

#endif
