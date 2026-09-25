/*
 * test_ui.c — the control rail's geometry, its hit testing, and the one
 * property everything else depends on: that the guest is laid out in
 * exactly the space the rail does not occupy.
 *
 * That last one is the reason this test exists. The rail's width is used
 * twice, once to draw the panel and once to map a pen down onto it, and if
 * those two ever disagree then every touch lands somewhere the guest is
 * not. It is the same class of mistake as the pointer-scale bug on
 * Wayland, which cost a day, so it is asserted rather than assumed.
 *
 * Every geometric claim is made twice, once with the rail on each edge.
 * A rail that can move is a rail whose arithmetic can be right on one side
 * and wrong on the other, and the second run costs nothing.
 *
 * Runs against SDL's dummy video driver, so it needs no display.
 */
#include "frontend/sdl/display.h"
#include "frontend/sdl/presentation.h"
#include <stdio.h>

static int failures;
static const char *which;      /* which side, for the failure line */
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d (%s): %s (%s)\n", __LINE__, which, #x, \
            SDL_GetError()); \
    failures++; } } while (0)

/* A press and a release, as SDL would deliver them. */
static void click(mrc_ui *ui, int x, int y, int up_x, int up_y)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = x; e.button.y = y;
    mrc_ui_event(ui, &e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.x = up_x; e.button.y = up_y;
    mrc_ui_event(ui, &e);
}

static void resize(mrc_sdl_display *d, int w, int h)
{
    SDL_Event size;
    SDL_zero(size);
    size.type = SDL_WINDOWEVENT;
    size.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    SDL_SetWindowSize(d->window, w, h);
    mrc_ui_event(d->ui, &size);
}

/* Everything that depends on where the rail is, for one side of it. */
static void check_side(mrc_sdl_display *d, mrc_ui_side side)
{
    mrc_ui *ui = d->ui;
    which = side == MRC_UI_LEFT ? "left" : "right";
    mrc_ui_set_side(ui, side);
    CHECK(mrc_ui_get_side(ui) == side);

    resize(d, 900, 600);
    mrc_ui_set_buttons(ui, (1u << MRC_UI_ICON_COUNT) - 1u);
    const int rail = mrc_ui_rail_width(ui);
    CHECK(rail > 0 && rail <= 160);
    CHECK(rail == 600 / MRC_UI_ICON_COUNT);

    /* Exactly one inset carries the width, and it is the right one. */
    CHECK(mrc_ui_inset_left(ui) + mrc_ui_inset_right(ui) == rail);
    CHECK((side == MRC_UI_LEFT ? mrc_ui_inset_left(ui)
                               : mrc_ui_inset_right(ui)) == rail);

    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    const int edge = side == MRC_UI_LEFT ? 0 : ow - rail;   /* the rail's x */

    SDL_Rect dst;
    mrc_sdl_panel_rect(d->renderer, 480, 320, false,
                       mrc_ui_inset_left(ui), mrc_ui_inset_right(ui), &dst);
    CHECK(dst.x >= mrc_ui_inset_left(ui));
    CHECK(dst.x + dst.w <= ow - mrc_ui_inset_right(ui));

    /* A point inside the rail is not a point on the panel. */
    unsigned px, py;
    CHECK(!mrc_sdl_panel_point(&dst, edge + rail / 2, oh / 2, 480, 320,
                               &px, &py));

    /* Pressing a button reports it, once, and only that one. */
    CHECK(mrc_ui_take_action(ui) == -1);
    click(ui, edge + rail / 2, rail / 2, edge + rail / 2, rail / 2);
    CHECK(mrc_ui_take_action(ui) == 0);
    CHECK(mrc_ui_take_action(ui) == -1);
    click(ui, edge + rail / 2, rail + rail / 2, edge + rail / 2, rail + rail / 2);
    CHECK(mrc_ui_take_action(ui) == 1);

    /* Sliding off before release takes the press back, as a button should. */
    click(ui, edge + rail / 2, rail / 2, ow / 2, rail / 2);
    CHECK(mrc_ui_take_action(ui) == -1);

    /* A press on the guest's side of the edge belongs to the guest. */
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = side == MRC_UI_LEFT ? rail + 20 : ow - rail - 20;
    e.button.y = oh / 2;
    CHECK(!mrc_ui_event(ui, &e));
    CHECK(mrc_ui_take_action(ui) == -1);

    /* Offering fewer buttons moves the rest up, so the rail is shorter on
     * a machine with less to say rather than showing dead controls. */
    mrc_ui_set_buttons(ui, (1u << MRC_UI_ICON_POWER) |
                           (1u << MRC_UI_ICON_STORAGE));
    click(ui, edge + rail / 2, rail / 2, edge + rail / 2, rail / 2);
    CHECK(mrc_ui_take_action(ui) == MRC_UI_ICON_POWER);
    click(ui, edge + rail / 2, rail + rail / 2, edge + rail / 2, rail + rail / 2);
    CHECK(mrc_ui_take_action(ui) == MRC_UI_ICON_STORAGE);
    click(ui, edge + rail / 2, 2 * rail + rail / 2,
          edge + rail / 2, 2 * rail + rail / 2);
    CHECK(mrc_ui_take_action(ui) == -1);
    mrc_ui_set_buttons(ui, (1u << MRC_UI_ICON_COUNT) - 1u);

    /*
     * Fullscreen. The panel is sized inside the space the rail leaves but
     * positioned against the whole window: centring it in the leftover
     * space alone pushes it away from the rail by the rail's whole width,
     * which reads as crooked because the eye centres on the window and not
     * on the region. With slack it sits on the centre; without slack it is
     * flush against the rail, which is the only place left.
     */
    resize(d, 1920, 1080);
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    mrc_sdl_panel_rect(d->renderer, 480, 320, false,
                       mrc_ui_inset_left(ui), mrc_ui_inset_right(ui), &dst);
    {
        int left = dst.x, right = ow - (dst.x + dst.w);
        CHECK(left - right <= 1 && right - left <= 1);
        CHECK(dst.x >= mrc_ui_inset_left(ui));
        CHECK(dst.x + dst.w <= ow - mrc_ui_inset_right(ui));
    }

    resize(d, 560, 600);
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    mrc_sdl_panel_rect(d->renderer, 480, 320, false,
                       mrc_ui_inset_left(ui), mrc_ui_inset_right(ui), &dst);
    if (side == MRC_UI_LEFT) CHECK(dst.x == mrc_ui_rail_width(ui));
    else CHECK(dst.x + dst.w == ow - mrc_ui_rail_width(ui));
    CHECK(dst.x >= 0 && dst.x + dst.w <= ow);

    /*
     * The panel: the sheet a rail button opens.
     *
     * Its geometry is mirrored from ui.c rather than exposed, which is
     * normal for a layout test and is the point -- if the sheet moves and
     * this is not updated, the failure says so.
     */
    resize(d, 900, 600);
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    {
        static const mrc_ui_row rows[] = {
            { MRC_UI_ROW_ACTION,  "one",   NULL, false, 10 },
            { MRC_UI_ROW_TOGGLE,  "two",   NULL, true,  11 },
            { MRC_UI_ROW_HEADING, "group", NULL, false, 0  },
            { MRC_UI_ROW_ACTION,  "three", NULL, false, 12 },
        };
        const int margin = rail / 4, head_h = rail * 3 / 4;
        const int sheet_x = (side == MRC_UI_LEFT ? rail : 0) + margin;
        const int sheet_w = ow - rail - 2 * margin;
        const int mid_x = sheet_x + sheet_w / 2;
        const int first = margin + head_h;         /* top of row zero */

        CHECK(!mrc_ui_panel_open(ui));
        mrc_ui_open_panel(ui, "Test", rows, 4);
        CHECK(mrc_ui_panel_open(ui));
        CHECK(mrc_ui_take_row(ui) == -1);

        /* Each row answers with its own name, not its position. */
        click(ui, mid_x, first + rail / 2, mid_x, first + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 10);
        click(ui, mid_x, first + rail + rail / 2, mid_x, first + rail + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 11);
        /* A heading is a label, not a control. */
        click(ui, mid_x, first + 2 * rail + head_h / 2,
              mid_x, first + 2 * rail + head_h / 2);
        CHECK(mrc_ui_take_row(ui) == -1);
        /* And the row below it is still reachable. */
        click(ui, mid_x, first + 2 * rail + head_h + rail / 2,
              mid_x, first + 2 * rail + head_h + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 12);
        CHECK(mrc_ui_panel_open(ui));   /* changing a setting keeps it open */

        /*
         * While it is open the guest must not see presses meant for it,
         * including the one that dismisses it: a tap on the guest closes
         * the panel and stops there rather than also reaching through.
         */
        SDL_Event press;
        SDL_zero(press);
        press.type = SDL_MOUSEBUTTONDOWN;
        press.button.button = SDL_BUTTON_LEFT;
        press.button.x = side == MRC_UI_LEFT ? ow - 4 : 4;
        press.button.y = oh - 4;
        CHECK(mrc_ui_event(ui, &press));
        press.type = SDL_MOUSEBUTTONUP;
        CHECK(mrc_ui_event(ui, &press));
        CHECK(mrc_ui_panel_open(ui)); /* owner must clean up its picker/IME */
        CHECK(mrc_ui_take_row(ui) == MRC_UI_ROW_DISMISS);
        mrc_ui_close_panel(ui);

        /* The rail still answers for itself while a panel is open, which
         * is what lets the button that opened it put it away. */
        mrc_ui_open_panel(ui, "Test", rows, 4);
        click(ui, edge + rail / 2, rail / 2, edge + rail / 2, rail / 2);
        CHECK(mrc_ui_take_action(ui) == 0);
        mrc_ui_close_panel(ui);
        CHECK(!mrc_ui_panel_open(ui));
    }

    /*
     * A row's own action buttons: a play and a trash can beside a device,
     * say. Right-aligned as a group, slot 0 nearer the label, each a square
     * the row's own height -- mirrored from ui.c the same way the sheet
     * above is, and for the same reason.
     */
    resize(d, 900, 600);
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    {
        static const mrc_ui_row rows[] = {
            { MRC_UI_ROW_ACTION, "plain", NULL, false, 30 },
            { MRC_UI_ROW_ACTION, "device", NULL, false, 31,
              .action_count = 2,
              .action_icon = { MRC_UI_ICON_PLAY, MRC_UI_ICON_TRASH },
              .action_id = { 40, 41 } },
        };
        const int margin = rail / 4, head_h = rail * 3 / 4;
        const int sheet_x = (side == MRC_UI_LEFT ? rail : 0) + margin;
        const int sheet_w = ow - rail - 2 * margin;
        const int right = sheet_x + sheet_w;
        const int first = margin + head_h;          /* top of row zero */
        const int second = first + rail;             /* top of row one */
        const int play_x = right - 2 * rail + rail / 2;
        const int trash_x = right - rail + rail / 2;

        mrc_ui_open_panel(ui, "Test", rows, 2);

        /* The plain row has none: a tap anywhere on it, including where a
         * button would sit on the other row, is the row itself. */
        click(ui, right - rail / 2, first + rail / 2,
              right - rail / 2, first + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 30);

        /* The device row's body, left of its buttons, is still the row. */
        click(ui, sheet_x + rail / 4, second + rail / 2,
              sheet_x + rail / 4, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 31);

        /* Its two buttons report their own ids instead, not the row's. */
        click(ui, play_x, second + rail / 2, play_x, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 40);
        click(ui, trash_x, second + rail / 2, trash_x, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == 41);

        /* A press that lands on one button and lifts on the other belongs
         * to neither, the same as a rail button taken back by sliding off. */
        click(ui, play_x, second + rail / 2, trash_x, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == -1);
        click(ui, trash_x, second + rail / 2, play_x, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == -1);

        /* And a press that starts on a button but lifts on the row body
         * beside it is taken back the same way -- it is not the row either. */
        click(ui, trash_x, second + rail / 2, sheet_x + rail / 4, second + rail / 2);
        CHECK(mrc_ui_take_row(ui) == -1);

        mrc_ui_close_panel(ui);
    }

    /*
     * Drawn, and actually there: the rail's own column is its colour and
     * the guest's is not, which is what "takes space rather than floats
     * over" means in pixels.
     */
    resize(d, 900, 600);
    for (unsigned i = 0; i < 480u * 320u; i++) d->pixels[i] = 0xff00ff00u;
    CHECK(mrc_sdl_display_present(d, 480, 320, false));
    SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
    uint32_t *screen = SDL_malloc((size_t)ow * oh * 4);
    CHECK(screen != NULL);
    if (screen) {
        CHECK(SDL_RenderReadPixels(d->renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                                   screen, ow * 4) == 0);
        mrc_sdl_panel_rect(d->renderer, 480, 320, false,
                           mrc_ui_inset_left(ui), mrc_ui_inset_right(ui), &dst);
        int inside = side == MRC_UI_LEFT ? 2 : ow - 3;   /* within the rail */
        CHECK(screen[(oh - 2) * ow + inside] == 0xff18181au);
        CHECK(screen[(dst.y + dst.h / 2) * ow + dst.x + dst.w / 2] ==
              0xff00ff00u);
        CHECK(screen[(dst.y + dst.h / 2) * ow + inside] != 0xff00ff00u);
        SDL_free(screen);
    }
}

int main(void)
{
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("MRC_DISPLAY_ROTATION", "0", 1);
    SDL_setenv("MRC_UI_RAIL_SIDE", "left", 1);
    which = "setup";
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);

    mrc_sdl_display d;
    CHECK(mrc_sdl_display_open(&d, "ui test", 900, 600, 24));
    CHECK(mrc_sdl_display_resize(&d, 480, 320));
    CHECK(mrc_ui_open(&d.ui, d.renderer, d.window));
    if (!d.ui) return 1;

    /* With nothing offered there is no rail at all, so a frontend that has
     * no controls pays nothing and needs no special case. */
    CHECK(mrc_ui_rail_width(d.ui) == 0);
    CHECK(mrc_ui_inset_left(d.ui) == 0 && mrc_ui_inset_right(d.ui) == 0);
    CHECK(mrc_sdl_display_inset(&d) == 0);

    /* Text has a size, and a longer string is wider than a shorter one. */
    mrc_ui_set_buttons(d.ui, (1u << MRC_UI_ICON_COUNT) - 1u);
    CHECK(mrc_ui_text_height(d.ui) > 0);
    CHECK(mrc_ui_text_width(d.ui, "PIC-2000") > mrc_ui_text_width(d.ui, "PIC"));
    CHECK(mrc_ui_text_width(d.ui, "") == 0);
    CHECK(mrc_ui_text_width(d.ui, "é—…↑") == mrc_ui_text_width(d.ui, "abcd"));
    CHECK(mrc_ui_text_width(d.ui, "\xf0\x9f\x98\x80") == mrc_ui_text_width(d.ui, "?"));
    CHECK(mrc_ui_text_width(d.ui, "\xe2") == mrc_ui_text_width(d.ui, "?"));

    check_side(&d, MRC_UI_LEFT);
    check_side(&d, MRC_UI_RIGHT);

    resize(&d, 480, 320);
    mrc_ui_set_buttons(d.ui, 255); /* all eight guest rail controls */
    CHECK(mrc_ui_rail_width(d.ui) * 8 <= 320);
    mrc_ui_row row = { .kind=MRC_UI_ROW_ACTION, .label="test", .id=42 };
    mrc_ui_open_panel(d.ui,"Test",&row,1);
    SDL_Event e; SDL_zero(e);
    e.type=SDL_KEYDOWN; e.key.keysym.sym=SDLK_ESCAPE;
    CHECK(mrc_ui_event(d.ui,&e));
    CHECK(mrc_ui_take_row(d.ui)==MRC_UI_ROW_BACK);
    e.key.keysym.sym=SDLK_a;
    CHECK(mrc_ui_event(d.ui,&e)); /* must not type into the covered guest */
    CHECK(mrc_ui_take_row(d.ui)==-1);
    e.type=SDL_MOUSEBUTTONDOWN; e.button.button=SDL_BUTTON_RIGHT;
    e.button.x=100; e.button.y=70;
    CHECK(mrc_ui_event(d.ui,&e));
    e.type=SDL_MOUSEBUTTONUP; CHECK(mrc_ui_event(d.ui,&e));
    CHECK(mrc_ui_take_row(d.ui)==-1);

    mrc_ui_close(d.ui);
    d.ui = NULL;
    mrc_sdl_display_close(&d);
    SDL_Quit();
    if (failures) fprintf(stderr, "%d ui checks failed\n", failures);
    else printf("all control rail checks passed, both sides\n");
    return failures ? 1 : 0;
}
