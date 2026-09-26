#include "frontend/sdl/chooser.h"

#ifndef MH_HAVE_SDL

bool mh_chooser_available(void) { return false; }
bool mh_chooser_run(struct mh_shell *sh) { (void)sh; return false; }

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

bool mh_chooser_available(void) { return true; }

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

bool mh_chooser_run(struct mh_shell *sh)
{
    if (!mh_shell_is_open(sh)) return false;
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
    if (!mh_shell_attach(sh, "MagicHat", CHOOSER_W, CHOOSER_H)) return false;
    mh_sdl_display *display = mh_shell_display(sh);
    mh_ui *ui = mh_shell_ui(sh);
    if (!ui) {
        fprintf(stderr, "no control rail: nothing here can be chosen\n");
        return false;
    }
    /* Only the button that does something here. */
    mh_ui_set_buttons(ui, 1u << MH_UI_ICON_ROMS);

    /* Open on the list, because it is the only reason to be here. */
    mh_devices_panel_open(ui, NULL);

    bool chose = false, running = true;
    while (running && !mh_state_device_requested()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (mh_ui_event(ui, &event)) {
                int action = mh_ui_take_action(ui);
                if (action == MH_UI_ICON_ROMS) {
                    /* The one button reopens the list rather than closing it:
                     * there is nothing behind it to look at. */
                    if (!mh_devices_panel_showing())
                        mh_devices_panel_open(ui, NULL);
                }
                int row = mh_ui_take_row(ui);
                if (row == MH_UI_ROW_DISMISS) {
                    mh_devices_panel_close(ui);
                    mh_devices_panel_open(ui, NULL);
                    continue;
                }
                if (row == MH_UI_ROW_BACK) {
                    if (!mh_devices_panel_back(ui)) running = false;
                    continue;
                }
                mh_devices_panel_row(ui, row);
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
        if (running && !mh_devices_panel_showing() &&
            !mh_state_device_requested())
            mh_devices_panel_open(ui, NULL);

        /* Anything the system's picker has produced since the last frame. */
        mh_devices_panel_tick(ui);

        /* Blank, and then the rail over it. */
        for (unsigned i = 0; i < CHOOSER_W * CHOOSER_H; i++)
            display->pixels[i] = 0xFF000000u;
        if (!mh_sdl_display_present(display, CHOOSER_W, CHOOSER_H, false)) {
            fprintf(stderr, "SDL present: %s\n", SDL_GetError());
            running = false;
        }
        SDL_Delay(16);
    }
    chose = mh_state_device_requested();

    /* The panel goes; the window stays, because something is about to run
     * in it. */
    mh_devices_panel_close(ui);
    return chose;
}

#endif /* MH_HAVE_SDL */
