/*
 * test_lcd.c — the panel look, against the two frame formats boards use.
 *
 * This exists because of a bug nothing else would have caught. Boards do
 * not hand over the same thing: the DataRover gives eight-bit grey and the
 * PIC-2000 gives a two-bit level, nought to three. The plain path always
 * went through mh_sdl_display_gray, which knows the difference, but the
 * tint and structure paths divided a level of three by 255. Every pixel
 * came out as full ink, and the PIC-2000 showed a flat green rectangle
 * with the machine nowhere in it.
 *
 * It survived review because the effect was written for one board and only
 * ever saw one format, and it survived an offscreen check because that
 * harness decoded the framebuffer itself and passed eight-bit grey. So the
 * test below feeds each format exactly as its board does, and asks the one
 * question that would have failed: does a frame with light and dark in it
 * still have light and dark in it afterwards.
 */
#include "frontend/sdl/display.h"
#include "frontend/sdl/lcd.h"
#include <stdio.h>

static int failures;
static const char *which = "";
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d (%s): %s\n", __LINE__, which, #x); \
    failures++; } } while (0)

#define W 64u
#define H 32u

/* Half the frame at its lightest, half at its darkest. */
static void fill(uint8_t *frame, mh_frame_format format)
{
    uint8_t light = format == MH_FRAME_LCD2 ? 0 : 255;
    uint8_t dark  = format == MH_FRAME_LCD2 ? 3 : 0;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            frame[y * W + x] = x < W / 2 ? light : dark;
}

/* The two halves, as drawn. Sampled well inside each so a cell's darker
 * edge is never what gets picked up. */
static void halves(const mh_lcd_frame *view, uint32_t *light, uint32_t *dark)
{
    unsigned row = view->height / 2;
    *light = view->pixels[row * view->width + view->width / 4];
    *dark  = view->pixels[row * view->width + view->width * 3 / 4];
}

static void check_format(mh_sdl_display *d, mh_lcd *lcd,
                         mh_frame_format format, const char *name)
{
    static uint8_t frame[W * H];
    mh_lcd_frame view;
    uint32_t light, dark;
    which = name;
    fill(frame, format);

    /* Plain: no tint, no structure. */
    mh_gui_lcd = false;
    mh_gui_tint = MH_TINT_NONE;
    CHECK(mh_lcd_render(lcd, d, frame, W, H, format, W, &view));
    CHECK(view.cell == 1);
    halves(&view, &light, &dark);
    CHECK(light != dark);
    CHECK(light == 0xffffffffu && dark == 0xff000000u);

    /* Tint without structure. Still one host pixel each, and still two
     * different colours -- this is the case that was flat. */
    mh_gui_tint = MH_TINT_GREEN;
    CHECK(mh_lcd_render(lcd, d, frame, W, H, format, W, &view));
    CHECK(view.cell == 1);
    halves(&view, &light, &dark);
    CHECK(light != dark);
    /* Green, and the lit half really is the lighter one. */
    CHECK(((light >> 8) & 0xff) > ((dark >> 8) & 0xff));

    /* Structure as well: more host pixels per guest pixel, still not flat. */
    mh_gui_lcd = true;
    CHECK(mh_lcd_render(lcd, d, frame, W, H, format, W * 4, &view));
    CHECK(view.cell > 1);
    CHECK(view.width == W * view.cell && view.height == H * view.cell);
    halves(&view, &light, &dark);
    CHECK(light != dark);
    CHECK(((light >> 8) & 0xff) > ((dark >> 8) & 0xff));

    /* Structure without tint is grey, and still has both ends. */
    mh_gui_tint = MH_TINT_NONE;
    CHECK(mh_lcd_render(lcd, d, frame, W, H, format, W * 4, &view));
    halves(&view, &light, &dark);
    CHECK(light != dark);

    /* Nothing to show is black, and does not keep the last frame. */
    CHECK(mh_lcd_render(lcd, d, NULL, W, H, format, W * 4, &view));
    halves(&view, &light, &dark);
    CHECK(light == 0xff000000u && dark == 0xff000000u);

    mh_gui_lcd = false;
    mh_gui_tint = MH_TINT_NONE;
}

int main(void)
{
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    mh_sdl_display d;
    if (!mh_sdl_display_open(&d, "lcd test", 256, 128, 0)) return 1;
    mh_lcd *lcd = mh_lcd_create();
    if (!lcd) return 1;

    check_format(&d, lcd, MH_FRAME_GRAY8, "eight-bit grey");
    check_format(&d, lcd, MH_FRAME_LCD2, "two-bit levels");

    /*
     * Smoothing is chosen when a texture is made, not when it is drawn, so
     * changing it has to make a new one. Before this it appeared to do
     * nothing until something else happened to resize the texture.
     *
     * Asked of the texture itself rather than by watching for a new
     * allocation: resizing to the same size can reuse the buffer, so a
     * changed pointer is neither necessary nor sufficient.
     */
    which = "smoothing";
    static uint8_t frame[W * H];
    mh_lcd_frame view;
    SDL_ScaleMode mode;
    fill(frame, MH_FRAME_GRAY8);
    mh_gui_smooth = false;
    CHECK(mh_lcd_render(lcd, &d, frame, W, H, MH_FRAME_GRAY8, W, &view));
    CHECK(SDL_GetTextureScaleMode(d.texture, &mode) == 0);
    CHECK(mode == SDL_ScaleModeNearest);
    mh_gui_smooth = true;
    CHECK(mh_lcd_render(lcd, &d, frame, W, H, MH_FRAME_GRAY8, W, &view));
    CHECK(SDL_GetTextureScaleMode(d.texture, &mode) == 0);
    CHECK(mode == SDL_ScaleModeLinear);
    mh_gui_smooth = false;

    mh_lcd_free(lcd);
    mh_sdl_display_close(&d);
    SDL_Quit();
    if (failures) fprintf(stderr, "%d panel look checks failed\n", failures);
    else printf("all panel look checks passed, both frame formats\n");
    return failures ? 1 : 0;
}
