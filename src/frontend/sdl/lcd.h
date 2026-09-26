/*
 * lcd.h — what a panel looked like, for whichever machine is showing one.
 *
 * Two independent things. The structure is each guest pixel drawn as a cell
 * with a darker edge, plus a little of the previous frame still on the
 * glass; the tint is only the colour the backlight was. A DataRover's panel
 * had the structure and no tint, a Sony PIC had both, and wanting the green
 * without the dots or the dots without the green are both reasonable.
 *
 * This lives apart from either frontend because neither owns it. It began
 * inside the DataRover's, which meant the 68k machines could not have it --
 * and the PIC-2000 is the machine with the green backlight, so it wanted it
 * more than the DataRover did.
 *
 * Nothing here touches the guest. Every setting is read fresh each frame,
 * so changing one shows on the next and means nothing to the machine.
 */
#ifndef MH_SDL_LCD_H
#define MH_SDL_LCD_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime/machine.h"

/* The display this renders into. Declared rather than included because
 * display.h includes this one for the settings. */
typedef struct mh_sdl_display mh_sdl_display;

#include "frontend/sdl/look.h"

typedef struct mh_lcd mh_lcd;

mh_lcd *mh_lcd_create(void);
void mh_lcd_free(mh_lcd *lcd);
/* Drop the remembered frame, so the next one is not ghosted against a
 * picture from before whatever just happened. */
void mh_lcd_forget(mh_lcd *lcd);

typedef struct {
    unsigned cell;            /* host pixels per guest pixel, 1 when plain */
    unsigned width, height;   /* the texture's size, in host pixels        */
    uint32_t *pixels;         /* where the frame was written               */
} mh_lcd_frame;

/*
 * Fill the display's texture from one guest frame, resizing it when the
 * cell size changes. `gray` may be NULL, which blanks: the video is off or
 * pointed somewhere unreadable, and showing black rather than the last
 * frame keeps that state from being misread.
 *
 * `drawn_width` is how wide the panel is actually being drawn, which is
 * what decides how large a cell can be. Returns false only when the
 * texture could not be allocated, which the caller should treat as fatal.
 */
bool mh_lcd_render(mh_lcd *lcd, mh_sdl_display *display,
                    const uint8_t *gray, unsigned w, unsigned h,
                    mh_frame_format format, unsigned drawn_width,
                    mh_lcd_frame *out);

#endif /* MH_SDL_LCD_H */
