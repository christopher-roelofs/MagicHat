#include "frontend/sdl/shell.h"

#include "frontend/sdl/display.h"
#include "frontend/sdl/lcd.h"
#include "frontend/sdl/startup.h"
#include "frontend/sdl/ui.h"

#include <stdlib.h>

struct mrc_shell {
    mrc_sdl_display display;
    mrc_ui         *ui;
    mrc_lcd        *lcd;
    bool            open;
};

mrc_sdl_display *mrc_shell_display(mrc_shell *sh) { return sh ? &sh->display : NULL; }
mrc_ui *mrc_shell_ui(mrc_shell *sh) { return sh ? sh->ui : NULL; }
mrc_lcd *mrc_shell_lcd(mrc_shell *sh) { return sh ? sh->lcd : NULL; }
bool mrc_shell_is_open(const mrc_shell *sh) { return sh && sh->open; }

#include <stdio.h>

static mrc_shell *g_current;

void mrc_shell_set_current(mrc_shell *sh) { g_current = sh; }
mrc_shell *mrc_shell_current(void) { return g_current; }

mrc_shell *mrc_shell_open(const char *title, unsigned panel_w, unsigned panel_h)
{
    mrc_shell *sh = calloc(1, sizeof(*sh));
    if (!sh) return NULL;
    /*
     * The native backend, chosen before the video subsystem is touched. SDL
     * prefers X11 when it can find one, and under XWayland its window
     * creation can simply never return -- a window that never appears and a
     * process that sits in a poll saying nothing.
     */
    mrc_sdl_prepare_video();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(sh);
        return NULL;
    }
    fprintf(stderr, "[shell] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    /*
     * Fit the window to the display. A window wider than the screen is
     * clipped by the compositor, and the clipped part cannot be clicked.
     */
    int win_w = (int)panel_w * 2, win_h = (int)panel_h * 2;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 &&
        usable.h > 0 && (usable.w < win_w || usable.h < win_h)) {
        win_w = usable.w;
        win_h = win_w * (int)panel_h / (int)panel_w;
        if (win_h > usable.h) {
            win_h = usable.h;
            win_w = win_h * (int)panel_w / (int)panel_h;
        }
        fprintf(stderr, "[shell] display usable area %dx%d, opening at %dx%d\n",
                usable.w, usable.h, win_w, win_h);
    }

    if (!mrc_sdl_display_open(&sh->display, title, win_w, win_h, 24)) {
        fprintf(stderr, "SDL display: %s\n", SDL_GetError());
        SDL_Quit();
        free(sh);
        return NULL;
    }
    if (!mrc_sdl_display_resize(&sh->display, panel_w, panel_h)) {
        fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
        mrc_sdl_display_close(&sh->display);
        SDL_Quit();
        free(sh);
        return NULL;
    }
    sh->lcd = mrc_lcd_create();
    if (!sh->lcd) {
        fprintf(stderr, "[shell] out of memory\n");
        mrc_sdl_display_close(&sh->display);
        SDL_Quit();
        free(sh);
        return NULL;
    }
    /* A rail that will not open is not fatal: the guest is still usable, it
     * simply has no controls beside it. */
    if (!mrc_ui_open(&sh->ui, sh->display.renderer, sh->display.window))
        fprintf(stderr, "[shell] no control rail: %s\n", SDL_GetError());
    sh->display.ui = sh->ui;
    sh->open = true;
    return sh;
}

bool mrc_shell_attach(mrc_shell *sh, const char *title,
                      unsigned panel_w, unsigned panel_h)
{
    if (!sh || !sh->open) return false;
    if (title && sh->display.window)
        SDL_SetWindowTitle(sh->display.window, title);
    /*
     * The panel size belongs to the board, and the two machines do not agree
     * about it. Resizing returns early when nothing changed, so attaching the
     * same machine again costs nothing.
     */
    if (!mrc_sdl_display_resize(&sh->display, panel_w, panel_h)) {
        fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
        return false;
    }
    mrc_lcd_forget(sh->lcd);
    return true;
}

void mrc_shell_close(mrc_shell *sh)
{
    if (!sh || !sh->open) return;
    mrc_lcd_free(sh->lcd);
    sh->lcd = NULL;
    mrc_ui_close(sh->ui);
    sh->ui = NULL;
    sh->display.ui = NULL;
    mrc_sdl_display_close(&sh->display);
    SDL_Quit();
    sh->open = false;
    if (g_current == sh) g_current = NULL;
    free(sh);
}
