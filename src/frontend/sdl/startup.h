#pragma once
#include <SDL2/SDL.h>

/* Prefer the native desktop backend before either probing or opening a
 * window. SDL's X11 default can hang in window creation under XWayland.
 * Explicit user settings (including dummy for tests) retain priority. */
static inline void mrc_sdl_prepare_video(void)
{
#if defined(__linux__)
    const char *wayland = SDL_getenv("WAYLAND_DISPLAY");
    if (wayland && *wayland && !SDL_getenv("SDL_VIDEODRIVER")) {
        /* Driver preference lists are supported since SDL 2.0.22. */
        SDL_version version;
        SDL_GetVersion(&version);
        const char *drivers = SDL_VERSIONNUM(version.major, version.minor,
                                             version.patch) >= SDL_VERSIONNUM(2, 0, 22)
                            ? "wayland,x11" : "wayland";
        SDL_setenv("SDL_VIDEODRIVER", drivers, 0);
    }
#endif
}
