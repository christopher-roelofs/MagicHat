/*
 * gui.h — optional SDL window onto the machine.
 *
 * Built only when SDL2 is present; without it mh_gui_available() returns
 * false and mh_gui_run() explains why.
 */
#ifndef MH_GUI_H
#define MH_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "machines/datarover840/machine.h"

bool mh_gui_available(void);

/* Runs until the window is closed, or until `insns_total` instructions have
 * been executed (0 = no limit). */
/* Sound in the window is opt-in: see --audio in main.c and docs/OPEN_QUESTIONS.md. */
extern bool mh_gui_audio;

/* The panel's look is shared with the 68k frontend. Only the settings are
 * taken here: the rendering in lcd.h needs SDL, and a command line that
 * includes SDL on Android loses its own main() to SDL_main. */
#include "frontend/sdl/look.h"
extern bool mh_gui_touch_debug; /* draw a cross where the pen went down */

/* Runs the machine in a window the caller already owns, so that moving from
 * one device to another reshapes what is on screen rather than replacing it. */
struct mh_shell;
int  mh_gui_run(machine *m, uint64_t insns_total, struct mh_shell *shell);

#endif
