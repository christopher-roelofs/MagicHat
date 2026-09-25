/*
 * devices_panel.h — the list behind the rail's devices button.
 *
 * A device is a machine you own, not a file you open. You make one once from
 * a firmware image and after that you pick it by name and carry on where you
 * were; the image it came from is copied in and never thought about again.
 *
 * So this panel is a list of devices and one way to make another. Choosing
 * the firmware for a new one opens the file picker, which is the only place
 * in the interface where a path is visible at all.
 *
 * Shared between the frontends for the same reason the display panel is:
 * both machines have exactly this to offer, and a list written twice is a
 * list that ends up describing two different things.
 */
#ifndef MRC_SDL_DEVICES_PANEL_H
#define MRC_SDL_DEVICES_PANEL_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"

/*
 * Open the list. `current` is the id of the device running now, or NULL when
 * the machine was started from a plain ROM path and is not one -- it is shown
 * as the one in use so that a list of similar names is not a guess.
 */
void mrc_devices_panel_open(mrc_ui *ui, const char *current);
void mrc_devices_panel_close(mrc_ui *ui);

/*
 * Hand it a row the rail reported. True when the panel consumed it. What the
 * caller does about a chosen device is the caller's business, because only it
 * knows how to put the running machine down and start another.
 */
bool mrc_devices_panel_row(mrc_ui *ui, int id);
bool mrc_devices_panel_back(mrc_ui *ui);

/*
 * The device chosen since this was last asked, by id, or NULL. Taking it
 * clears it. The string is valid until the next call.
 */
const char *mrc_devices_panel_taken(void);

/*
 * Collect anything the platform's own picker has produced. The list stays on
 * screen while that picker is up, so this is called once a frame; it does
 * nothing when there is nothing waiting.
 */
void mrc_devices_panel_tick(mrc_ui *ui);

/* Whether the panel currently on screen is this one. */
bool mrc_devices_panel_showing(void);

#endif /* MRC_SDL_DEVICES_PANEL_H */
