#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>
/* Clockwise host-only rotation; guest framebuffer and pen coordinates stay
 * in their native orientation. Read once per call so all translation units
 * use the same setting without frontend-specific state. */
static inline unsigned mh_sdl_rotation(void)
{
    const char *value = SDL_getenv("MH_DISPLAY_ROTATION");
    if (!value) return 0;
    if (!SDL_strcmp(value, "90")) return 90;
    if (!SDL_strcmp(value, "180")) return 180;
    if (!SDL_strcmp(value, "270")) return 270;
    return 0;
}
static inline bool mh_sdl_sideways(void)
{
    return mh_sdl_rotation() % 180 != 0;
}
/* Inverse of the clockwise presentation transform, including letterboxing. */
static inline bool mh_sdl_panel_point(const SDL_Rect *dst, int x, int y,
                                       unsigned width, unsigned height,
                                       unsigned *px, unsigned *py)
{
    if (dst->w <= 0 || dst->h <= 0 || x < dst->x || y < dst->y ||
        x >= dst->x + dst->w || y >= dst->y + dst->h) return false;
    unsigned rw = mh_sdl_sideways() ? height : width;
    unsigned rh = mh_sdl_sideways() ? width : height;
    unsigned u = (unsigned)((int64_t)(x - dst->x) * rw / dst->w);
    unsigned v = (unsigned)((int64_t)(y - dst->y) * rh / dst->h);
    switch (mh_sdl_rotation()) {
    case 90: *px = v; *py = height - 1 - u; break;
    case 180: *px = width - 1 - u; *py = height - 1 - v; break;
    case 270: *px = width - 1 - v; *py = u; break;
    default: *px = u; *py = v; break;
    }
    return true;
}
static inline void mh_sdl_panel_to_output(const SDL_Rect *dst,
    unsigned x, unsigned y, unsigned width, unsigned height, int *ox, int *oy)
{
    unsigned u=x, v=y, rw=width, rh=height;
    switch (mh_sdl_rotation()) {
    case 90: u=height-1-y; v=x; break;
    case 180: u=width-1-x; v=height-1-y; break;
    case 270: u=y; v=width-1-x; break;
    }
    if (mh_sdl_sideways()) { rw=height; rh=width; }
    *ox=dst->x+(int)(((int64_t)u*2+1)*dst->w/(2*rw));
    *oy=dst->y+(int)(((int64_t)v*2+1)*dst->h/(2*rh));
}
/* Rendering geometry shared by both frontends. Pointer acquisition remains
 * frontend-specific until the two existing high-DPI paths are reconciled.
 *
 * The two insets are what the control rail takes from the guest, which it
 * does rather than floating over it. One of them is the rail's width and
 * the other is zero; taking both rather than a width and a side means the
 * arithmetic below never asks which. Every caller passes the same values,
 * because the rectangle this returns is used both to draw the panel and to
 * map a pen down onto it: computing it two ways is how a touch ends up
 * landing somewhere the guest is not.
 *
 * The panel is sized inside the space the rail leaves but positioned
 * against the whole window, which are two different things and the
 * difference shows in fullscreen. Centring it in the leftover space alone
 * pushes it right by the rail's whole width -- fifty-four pixels at 1080p,
 * seventy-two at 1440p -- and it reads as crooked because the eye centres
 * on the window, not on the region. So it is centred on the window and
 * then held clear of the rail, which leaves it flush against the rail
 * exactly when the guest is wide enough that there was no choice anyway. */
static inline void mh_sdl_panel_rect(SDL_Renderer *renderer, unsigned width,
                                     unsigned height, bool integer,
                                     int inset_left, int inset_right,
                                     SDL_Rect *dst)
{
    if (mh_sdl_sideways()) { unsigned tmp=width; width=height; height=tmp; }
    int w=0,h=0;
    SDL_GetRendererOutputSize(renderer,&w,&h);
    if(w<=0 || h<=0) { w=(int)width; h=(int)height; }
    if(inset_left<0) inset_left=0;
    if(inset_right<0) inset_right=0;
    if(inset_left+inset_right>w-1) { inset_left=w>1?w-1:0; inset_right=0; }
    int usable=w-inset_left-inset_right;
    float sx=(float)usable/width, sy=(float)h/height;
    float scale=sx<sy?sx:sy;
    if(integer) { scale=(float)(int)scale; if(scale<1.0f) scale=1.0f; }
    dst->w=(int)(width*scale); dst->h=(int)(height*scale);
    dst->x=(w-dst->w)/2;
    if(dst->x<inset_left) dst->x=inset_left;
    if(dst->x+dst->w>w-inset_right) dst->x=w-inset_right-dst->w;
    dst->y=(h-dst->h)/2;
}
/* Preserve the existing performance-counter pacing conversion. */
static inline uint64_t mh_sdl_owed_slots(Uint64 now, Uint64 start,
                                         Uint64 frequency, uint32_t rate)
{
    return (uint64_t)((double)(now-start)/(double)frequency*(double)rate);
}
