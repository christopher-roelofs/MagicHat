/*
 * look.h — the panel look settings, and nothing that needs SDL.
 *
 * Separate from lcd.h on purpose. A command line sets these, and including
 * SDL's header to do it is not free: on Android SDL_main.h renames `main`
 * to `SDL_main`, so the moment the 68k command line pulled in SDL through
 * this, both command lines claimed that symbol and the app would not link.
 *
 * So the settings live here, where a parser can reach them, and the
 * rendering that uses them lives in lcd.h, where SDL belongs.
 */
#ifndef MH_SDL_LOOK_H
#define MH_SDL_LOOK_H

#include <stdbool.h>

/*
 * Two independent things. The structure is each guest pixel drawn as a
 * cell with a darker edge, plus a little of the previous frame still on
 * the glass; the tint is only the colour the backlight was. A DataRover's
 * panel had the structure and no tint, a Sony PIC had both, and wanting
 * the green without the dots or the dots without the green are both
 * reasonable.
 */
enum { MH_TINT_NONE = 0, MH_TINT_GREEN, MH_TINT_AMBER, MH_TINT_GREY };

extern bool mh_gui_lcd;       /* dot structure and ghosting          */
extern int  mh_gui_tint;      /* one of the above                    */
extern bool mh_gui_smooth;    /* linear filtering instead of nearest */
extern bool mh_gui_integer;   /* scale by whole pixels only          */

const char *mh_tint_name(int tint);
/* Parses green|amber|grey|none, returning -1 for anything else. */
int mh_tint_by_name(const char *name);

#endif /* MH_SDL_LOOK_H */
