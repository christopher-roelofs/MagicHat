#include "frontend/sdl/chooser.h"

#ifndef MRC_HAVE_SDL

bool mrc_chooser_available(void) { return false; }
bool mrc_chooser_run(struct mrc_shell *sh) { (void)sh; return false; }

#else

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "frontend/sdl/devices_panel.h"
#include "frontend/sdl/display.h"
#include "frontend/sdl/presentation.h"
#include "frontend/sdl/startup.h"
#include "frontend/sdl/shell.h"
#include "frontend/sdl/ui.h"
#include "host/state.h"

bool mrc_chooser_available(void) { return true; }

/*
 * The dark panel behind the rail.
 *
 * The same size a machine's window is, and black, so that a session with no
 * device yet looks like a device that is switched off rather than like a
 * different program. Picking one then fills the screen that was already
 * there, instead of replacing a small chooser with a large emulator.
 */
#define CHOOSER_W 480u
#define CHOOSER_H 320u

bool mrc_chooser_run(struct mrc_shell *sh)
{
    if (!mrc_shell_is_open(sh)) return false;
    /*
     * The window is already open and already the right shape; nothing is
     * created here. A device chosen in a moment attaches to this same one.
     */
    /*
     * Take the window at the size this panel expects. It is the size it opens
     * at, so today this changes nothing -- but the window is shared now, and
     * filling a fixed number of pixels into whatever the last machine left
     * behind is exactly the kind of thing that works until it does not.
     */
    if (!mrc_shell_attach(sh, "mcap", CHOOSER_W, CHOOSER_H)) return false;
    mrc_sdl_display *display = mrc_shell_display(sh);
    mrc_ui *ui = mrc_shell_ui(sh);
    if (!ui) {
        fprintf(stderr, "no control rail: nothing here can be chosen\n");
        return false;
    }
    /* Only the button that does something here. */
    mrc_ui_set_buttons(ui, 1u << MRC_UI_ICON_ROMS);

    /* Open on the list, because it is the only reason to be here. */
    mrc_devices_panel_open(ui, NULL);

    bool chose = false, running = true;
    while (running && !mrc_state_device_requested()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (mrc_ui_event(ui, &event)) {
                int action = mrc_ui_take_action(ui);
                if (action == MRC_UI_ICON_ROMS) {
                    /* The one button reopens the list rather than closing it:
                     * there is nothing behind it to look at. */
                    if (!mrc_devices_panel_showing())
                        mrc_devices_panel_open(ui, NULL);
                }
                int row = mrc_ui_take_row(ui);
                if (row == MRC_UI_ROW_DISMISS) {
                    mrc_devices_panel_close(ui);
                    mrc_devices_panel_open(ui, NULL);
                    continue;
                }
                if (row == MRC_UI_ROW_BACK) {
                    if (!mrc_devices_panel_back(ui)) running = false;
                    continue;
                }
                mrc_devices_panel_row(ui, row);
                continue;
            }
            if (event.type == SDL_QUIT) { running = false; break; }
            if (event.type == SDL_KEYDOWN &&
                (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_AC_BACK ||
                 (event.key.keysym.sym == SDLK_q &&
                  (event.key.keysym.mod & KMOD_CTRL)))) {
                running = false;
                break;
            }
        }
        /*
         * Tapping the panel away would leave nothing at all, so it comes
         * straight back. This window is the list.
         */
        if (running && !mrc_devices_panel_showing() &&
            !mrc_state_device_requested())
            mrc_devices_panel_open(ui, NULL);

        /* Anything the system's picker has produced since the last frame. */
        mrc_devices_panel_tick(ui);

        /* Blank, and then the rail over it. */
        for (unsigned i = 0; i < CHOOSER_W * CHOOSER_H; i++)
            display->pixels[i] = 0xFF000000u;
        if (!mrc_sdl_display_present(display, CHOOSER_W, CHOOSER_H, false)) {
            fprintf(stderr, "SDL present: %s\n", SDL_GetError());
            running = false;
        }
        SDL_Delay(16);
    }
    chose = mrc_state_device_requested();

    /* The panel goes; the window stays, because something is about to run
     * in it. */
    mrc_devices_panel_close(ui);
    return chose;
}

#endif /* MRC_HAVE_SDL */
