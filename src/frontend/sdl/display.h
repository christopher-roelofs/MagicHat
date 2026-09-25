#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime/machine.h"
#include "frontend/sdl/ui.h"

/* Host presentation resources only. Boards still decode their framebuffer;
 * frontends retain execution pacing, input, and optional pixel effects. */
typedef struct mrc_sdl_display {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    unsigned width, height;
    uint8_t background;
    /* The control rail, drawn by present() and subtracted from the panel.
     * Null until a frontend opens one, which is what a frontend with no
     * controls yet still does. */
    mrc_ui *ui;
    bool backgrounded;
    Uint64 pause_at;
} mrc_sdl_display;

bool mrc_sdl_display_open(mrc_sdl_display *d, const char *title,
                         int width, int height, uint8_t background);
bool mrc_sdl_display_resize(mrc_sdl_display *d, unsigned width, unsigned height);
bool mrc_sdl_display_present(mrc_sdl_display *d, unsigned panel_width,
                             unsigned panel_height, bool integer_scale);
void mrc_sdl_display_close(mrc_sdl_display *d);
/* How much of the width the rail takes, for laying the panel out the same
 * way present() does. */
int mrc_sdl_display_inset(const mrc_sdl_display *d);
void mrc_sdl_display_gray(uint32_t *out, const uint8_t *in, unsigned count,
                          mrc_frame_format format);

/* Shared mobile lifecycle and host controls; called on the emulator thread. */
/*
 * Put the machine down: the whole thing, to the file this device keeps it in.
 *
 * Both windows do this the same way and for the same reasons -- the save
 * button, going to the background, closing -- so it lives here rather than
 * twice. It reports what happened and says where; a device that cannot save
 * says so rather than appearing to have saved.
 */
bool mrc_sdl_save_state(mrc_runtime *m);
/* Whether there is anywhere to save to, which is what decides whether a
 * window offers the button at all. */
bool mrc_sdl_can_save_state(const mrc_runtime *m);

bool mrc_sdl_display_event(mrc_sdl_display *, const SDL_Event *, mrc_runtime *, Uint64 *wall_ref);
