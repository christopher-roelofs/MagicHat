#include "frontend/sdl/display.h"
#include "frontend/sdl/presentation.h"
#include <limits.h>
#include "host/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mrc_sdl_display_close(mrc_sdl_display *d)
{
    free(d->pixels);
    if (d->texture) SDL_DestroyTexture(d->texture);
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window) SDL_DestroyWindow(d->window);
    memset(d, 0, sizeof(*d));
}

bool mrc_sdl_display_open(mrc_sdl_display *d, const char *title,
                         int width, int height, uint8_t background)
{
    memset(d, 0, sizeof(*d));
    d->background = background;
    const char *rotation = SDL_getenv("MRC_DISPLAY_ROTATION");
    if (rotation && SDL_strcmp(rotation, "0") && !mrc_sdl_rotation()) {
        SDL_SetError("MRC_DISPLAY_ROTATION must be 0, 90, 180 or 270");
        return false;
    }
    if (mrc_sdl_sideways()) { int tmp=width; width=height; height=tmp; }
    const char *fullscreen = SDL_getenv("MRC_FULLSCREEN");
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (fullscreen && !SDL_strcmp(fullscreen, "1")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    d->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!d->window) return false;
    d->renderer = SDL_CreateRenderer(d->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d->renderer)
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
    if (!d->renderer) { mrc_sdl_display_close(d); return false; }
    SDL_RendererInfo info;
    if (!SDL_GetRendererInfo(d->renderer, &info))
        fprintf(stderr, "[sdl-display] renderer %s, vsync %s\n", info.name,
            info.flags & SDL_RENDERER_PRESENTVSYNC ? "on" : "off");
    return true;
}

bool mrc_sdl_display_resize(mrc_sdl_display *d, unsigned width, unsigned height)
{
    if (d->texture && width == d->width && height == d->height) return true;
    if (!width || !height || width > INT_MAX / sizeof(uint32_t) ||
        height > INT_MAX || (size_t)width > SIZE_MAX / sizeof(uint32_t) / height)
        return false;
    SDL_Texture *texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
    if (!texture) return false;
    uint32_t *pixels = calloc((size_t)width * height, sizeof(*pixels));
    if (!pixels) { SDL_DestroyTexture(texture); return false; }
    SDL_DestroyTexture(d->texture);
    free(d->pixels);
    d->texture = texture;
    d->pixels = pixels;
    d->width = width; d->height = height;
    return true;
}

bool mrc_sdl_display_present(mrc_sdl_display *d, unsigned panel_width,
                             unsigned panel_height, bool integer_scale)
{
    if (!d->texture || !panel_width || !panel_height) return false;
    SDL_Rect dst;
    mrc_sdl_panel_rect(d->renderer, panel_width, panel_height, integer_scale,
                       mrc_ui_inset_left(d->ui), mrc_ui_inset_right(d->ui),
                       &dst);
    /* RenderCopyEx rotates around the destination center. Use the unrotated
     * rectangle with that same center so its final bounds equal dst. */
    SDL_FRect copy = {(float)dst.x, (float)dst.y, (float)dst.w, (float)dst.h};
    if (mrc_sdl_sideways()) {
        copy.w = dst.h; copy.h = dst.w;
        copy.x = dst.x + (dst.w - copy.w) / 2.0f;
        copy.y = dst.y + (dst.h - copy.h) / 2.0f;
    }
    if (SDL_UpdateTexture(d->texture, NULL, d->pixels, (int)(d->width * 4)) != 0 ||
        SDL_SetRenderDrawColor(d->renderer, d->background, d->background,
                              d->background, 255) != 0 ||
        SDL_RenderClear(d->renderer) != 0 ||
        SDL_RenderCopyExF(d->renderer, d->texture, NULL, &copy,
                         mrc_sdl_rotation(), NULL, SDL_FLIP_NONE) != 0)
        return false;
    mrc_ui_draw(d->ui);
    SDL_RenderPresent(d->renderer);
    return true;
}

int mrc_sdl_display_inset(const mrc_sdl_display *d)
{
    return d ? mrc_ui_rail_width(d->ui) : 0;
}

void mrc_sdl_display_gray(uint32_t *out, const uint8_t *in, unsigned count,
                          mrc_frame_format format)
{
    for (unsigned i = 0; i < count; i++) {
        uint32_t v = format == MRC_FRAME_LCD2 ? 255 - (in[i] & 3) * 85 : in[i];
        out[i] = 0xff000000u | v << 16 | v << 8 | v;
    }
}

bool mrc_sdl_can_save_state(const mrc_runtime *m)
{
    return m && m->ops->save_state && mrc_state_path();
}

bool mrc_sdl_save_state(mrc_runtime *m)
{
    const char *path = mrc_state_path();
    if (!m->ops->save_state || !path) {
        fprintf(stderr, "[gui] this device has nowhere to save its state\n");
        return false;
    }
    if (!m->ops->save_state(m->board, path)) {
        fprintf(stderr, "[gui] could not save state to %s\n", path);
        return false;
    }
    fprintf(stderr, "[gui] state saved to %s\n", path);
    return true;
}

bool mrc_sdl_display_event(mrc_sdl_display *d, const SDL_Event *e,
                           mrc_runtime *m, Uint64 *wall_ref)
{
    if (e->type == SDL_APP_WILLENTERBACKGROUND) {
        if (!d->backgrounded) {
            d->backgrounded = true;
            d->pause_at = SDL_GetPerformanceCounter();
            m->ops->pen(m->board, false, 0, 0);
            if (m->ops->keyboard_release) m->ops->keyboard_release(m->board);
            /* Going to the background is the most likely way a session ends
             * on a tablet: the system can take the process at any point
             * afterwards without asking. So this is the save that matters
             * most, and it is the whole machine. */
            if (mrc_sdl_can_save_state(m)) mrc_sdl_save_state(m);
        }
        return true;
    }
    if (e->type == SDL_APP_DIDENTERFOREGROUND) {
        if (d->backgrounded) *wall_ref += SDL_GetPerformanceCounter() - d->pause_at;
        d->backgrounded = false;
        return true;
    }
    if (e->type == SDL_RENDER_DEVICE_RESET) {
        SDL_DestroyTexture(d->texture); d->texture = NULL;
        d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, d->width, d->height);
        return true;
    }
    /*
     * Android used to send commands here from a toolbar of its own: power,
     * save, rotate, close. They are all on the control rail now, which is the
     * same rail the desktop draws, so there is nothing left to relay -- a
     * second set of controls was a second thing to keep in step. Closing
     * still arrives, as an ordinary quit.
     */
    return false;
}
