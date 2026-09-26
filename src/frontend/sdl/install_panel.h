/*
 * install_panel.h — putting software on the guest, from the window.
 *
 * The guest has always been able to receive a package over its own serial
 * link; what it needed was a computer at the other end. That was a Python
 * script talking to a pseudo-terminal, which meant a second program, a
 * terminal, and a path typed correctly twice -- and on a tablet, nothing at
 * all. The emulator is now the computer, so this is only the part a person
 * touches: choose a file, watch it go.
 *
 * It does not drive the guest. Magic Cap receives a package when someone
 * opens the Storeroom and taps the computer, and it is the guest's own
 * interface that decides what happens next, so the panel says what to do and
 * then reports what the link is doing. Pretending otherwise would mean
 * synthesising taps at coordinates that differ between machines and versions.
 */
#ifndef MH_SDL_INSTALL_PANEL_H
#define MH_SDL_INSTALL_PANEL_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"
#include "runtime/machine.h"

void mh_install_panel_open(mh_ui *ui, mh_runtime *m);
void mh_install_panel_close(mh_ui *ui);
bool mh_install_panel_showing(void);

/* Hand it a row the rail reported. True when the panel consumed it. */
bool mh_install_panel_row(mh_ui *ui, mh_runtime *m, int id);
bool mh_install_panel_back(mh_ui *ui, mh_runtime *m);

/*
 * Bring the progress up to date. The transfer runs as the machine runs, so
 * this is called once a frame while the panel is open; it is cheap and does
 * nothing when there is nothing to report.
 */
void mh_install_panel_tick(mh_ui *ui, mh_runtime *m);

#endif /* MH_SDL_INSTALL_PANEL_H */
