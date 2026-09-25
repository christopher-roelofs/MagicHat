#include "frontend/sdl/lcd.h"
#include "frontend/sdl/display.h"

#include <stdlib.h>
#include <string.h>

bool mrc_gui_lcd = false;
int  mrc_gui_tint = MRC_TINT_NONE;
bool mrc_gui_smooth = false;
bool mrc_gui_integer = false;

#define LCD_CELL_MAX 8u        /* keeps the texture sane when maximised */
#define LCD_GAP    0.78f       /* how much darker a cell's edge is */
#define LCD_GHOST  0.35f       /* how much of the previous frame persists */

typedef struct { uint8_t lit[3], ink[3]; } lcd_palette;

/* Endpoints sampled from photographs of the real machines: the Sony PIC
 * series' mint-green backlight, the amber variant, and a plain grey STN. */
static const lcd_palette lcd_palettes[] = {
    /* No tint is the panel's own greyscale, unchanged. */
    [MRC_TINT_NONE]  = {{0xFF, 0xFF, 0xFF}, {0x00, 0x00, 0x00}},
    [MRC_TINT_GREEN] = {{0x6E, 0xDC, 0x9A}, {0x10, 0x3A, 0x28}},
    [MRC_TINT_AMBER] = {{0xF0, 0xC0, 0x50}, {0x38, 0x22, 0x08}},
    [MRC_TINT_GREY]  = {{0xC8, 0xCC, 0xC0}, {0x20, 0x22, 0x1E}},
};

static uint32_t lcd_mix(const lcd_palette *p, float ink, float dim)
{
    float r = (p->lit[0] + (p->ink[0] - p->lit[0]) * ink) * dim;
    float g = (p->lit[1] + (p->ink[1] - p->lit[1]) * ink) * dim;
    float b = (p->lit[2] + (p->ink[2] - p->lit[2]) * ink) * dim;
    return 0xFF000000u | ((uint32_t)(r + 0.5f) << 16) |
           ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}

const char *mrc_tint_name(int tint)
{
    switch (tint) {
    case MRC_TINT_GREEN: return "green";
    case MRC_TINT_AMBER: return "amber";
    case MRC_TINT_GREY:  return "grey";
    default:             return "none";
    }
}

int mrc_tint_by_name(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "green")) return MRC_TINT_GREEN;
    if (!strcmp(name, "amber")) return MRC_TINT_AMBER;
    if (!strcmp(name, "grey"))  return MRC_TINT_GREY;
    if (!strcmp(name, "none"))  return MRC_TINT_NONE;
    return -1;
}

struct mrc_lcd {
    uint8_t *prev;             /* the last frame, for the ghost */
    size_t   prev_size;
    bool     have_prev;
    unsigned cell;             /* what the texture is currently sized for */
    bool     smooth;           /* the filtering the texture is set to */
    bool     smooth_known;     /* false after a new texture, which defaults */
};

mrc_lcd *mrc_lcd_create(void)
{
    return calloc(1, sizeof(mrc_lcd));
}

void mrc_lcd_free(mrc_lcd *lcd)
{
    if (!lcd) return;
    free(lcd->prev);
    free(lcd);
}

void mrc_lcd_forget(mrc_lcd *lcd)
{
    if (lcd) lcd->have_prev = false;
}

/*
 * How many host pixels per guest pixel. Enough that a cell has room for its
 * own edge, never so many that maximising the window asks for a texture
 * nobody wanted.
 */
/*
 * One frame byte as coverage from 0 to 255.
 *
 * Boards do not all hand over the same thing. The DataRover gives eight-bit
 * grey; the PIC-2000 gives a two-bit level, nought to three, darker as it
 * rises. The plain path has always gone through mrc_sdl_display_gray, which
 * knows the difference -- but the tint and structure paths did not, and
 * divided a level of three by 255. Every pixel came out as full ink and the
 * PIC-2000 showed a flat green rectangle with the machine nowhere in it.
 *
 * It survived review because the effect was written for the DataRover and
 * only ever saw one format, and it survived my own offscreen check because
 * that harness decoded the framebuffer itself and passed eight-bit grey.
 */
static unsigned frame_gray(uint8_t value, mrc_frame_format format)
{
    return format == MRC_FRAME_LCD2 ? 255u - (value & 3u) * 85u : value;
}

static unsigned cell_for(unsigned drawn_width, unsigned panel_width)
{
    if (!mrc_gui_lcd || !panel_width) return 1;
    unsigned want = (drawn_width + panel_width / 2) / panel_width;
    if (want < 2) want = 2;
    if (want > LCD_CELL_MAX) want = LCD_CELL_MAX;
    return want;
}

bool mrc_lcd_render(mrc_lcd *lcd, mrc_sdl_display *display,
                    const uint8_t *gray, unsigned w, unsigned h,
                    mrc_frame_format format, unsigned drawn_width,
                    mrc_lcd_frame *out)
{
    if (!lcd || !display || !w || !h) return false;
    unsigned cell = cell_for(drawn_width, w);

    if (cell != lcd->cell) {
        if (!mrc_sdl_display_resize(display, w * cell, h * cell)) return false;
        lcd->cell = cell;
        lcd->have_prev = false;
        lcd->smooth_known = false;     /* a new texture takes the default */
    }

    /*
     * Filtering, set on the texture rather than through the render hint.
     *
     * The hint is read when a texture is created, so changing it only
     * matters if something then creates one -- and the display's resize
     * returns early when the size has not changed, which it usually has
     * not. The switch in the display panel therefore did nothing at all
     * unless the cell size happened to change at the same moment. Setting
     * it on the texture takes effect on the next frame and allocates
     * nothing.
     */
    if (!lcd->smooth_known || mrc_gui_smooth != lcd->smooth) {
        SDL_SetTextureScaleMode(display->texture, mrc_gui_smooth
                                ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        lcd->smooth = mrc_gui_smooth;
        lcd->smooth_known = true;
    }

    const unsigned tw = w * cell;
    uint32_t *argb = display->pixels;
    out->cell = cell;
    out->width = tw;
    out->height = h * cell;
    out->pixels = argb;

    if (!gray) {
        /* Opaque black, not zero. Zero is transparent black, which looks
         * the same only for as long as nothing ever blends this texture. */
        for (size_t i = 0; i < (size_t)tw * h * cell; i++) argb[i] = 0xFF000000u;
        lcd->have_prev = false;
        return true;
    }

    if (!mrc_gui_lcd && mrc_gui_tint == MRC_TINT_NONE) {
        /*
         * Default: the panel exactly as the machine drew it. Taking the
         * palette path here instead cost 16680 pixels an off-by-one from
         * float truncation -- invisible, but the plain view ought to be the
         * framebuffer and not a re-derivation of it.
         */
        mrc_sdl_display_gray(argb, gray, w * h, format);
        lcd->have_prev = false;
        return true;
    }

    const lcd_palette *pal = &lcd_palettes[mrc_gui_tint];
    if (!mrc_gui_lcd) {
        /* Tint without structure: one pixel each, recoloured. */
        for (unsigned i = 0; i < w * h; i++)
            argb[i] = lcd_mix(pal, 1.0f - frame_gray(gray[i], format) / 255.0f,
                              1.0f);
        lcd->have_prev = false;
        return true;
    }

    size_t need = (size_t)w * h;
    if (lcd->prev_size < need) {
        uint8_t *bigger = realloc(lcd->prev, need);
        if (!bigger) return false;
        lcd->prev = bigger;
        lcd->prev_size = need;
        lcd->have_prev = false;
    }
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned i = y * w + x;
            /* Ink coverage, with a little of the last frame still on the
             * glass. */
            float v = (float)frame_gray(gray[i], format);
            if (lcd->have_prev)
                v = v * (1.0f - LCD_GHOST) + lcd->prev[i] * LCD_GHOST;
            float ink = 1.0f - v / 255.0f;
            uint32_t body = lcd_mix(pal, ink, 1.0f);
            uint32_t edge = lcd_mix(pal, ink, LCD_GAP);
            for (unsigned sy = 0; sy < cell; sy++) {
                uint32_t *row = &argb[(y * cell + sy) * tw + x * cell];
                for (unsigned sx = 0; sx < cell; sx++)
                    row[sx] = (sx == cell - 1 || sy == cell - 1) ? edge : body;
            }
        }
    }
    /* The ghost holds coverage, not raw frame bytes, so that the blend
     * above is between two values in the same units. */
    for (size_t i = 0; i < need; i++)
        lcd->prev[i] = (uint8_t)frame_gray(gray[i], format);
    lcd->have_prev = true;
    return true;
}
