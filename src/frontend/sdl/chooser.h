/*
 * chooser.h — a window with no machine in it.
 *
 * The emulator has to be startable on its own. Before this, running it with
 * nothing and owning no devices printed instructions and quit, which means
 * the only way to get a first device was to already know the command for it
 * -- and the device list, which is the thing that makes one, could only be
 * reached from inside a machine that was already running.
 *
 * So: an empty panel, black, with the rail beside it. The devices list is
 * open from the start because it is the only reason to be here. Make one,
 * choose it, and the launcher starts it exactly as it starts any other.
 *
 * It is not a third window. It is the same window every machine runs in, with
 * nothing attached to it yet, so choosing a device fills the screen that is
 * already there instead of replacing one window with another. What it leaves
 * out is a guest.
 */
#ifndef MH_SDL_CHOOSER_H
#define MH_SDL_CHOOSER_H

#include <stdbool.h>

/*
 * Open it and stay until a device is chosen or the window is closed. True
 * when a device was chosen, which the caller collects with
 * mh_state_take_requested_device.
 *
 * False when the window was closed, and also when there is no window to be
 * had -- a build without SDL, or a display that will not open. A caller that
 * has nothing else to offer should say so rather than appear to have worked.
 */
struct mh_shell;
bool mh_chooser_run(struct mh_shell *shell);

/* Whether this build can open one at all. */
bool mh_chooser_available(void);

#endif /* MH_SDL_CHOOSER_H */
