#include "frontend/sdl/ui.h"

#include <stdlib.h>
#include <string.h>

/*
 * Sizes come from the output resolution rather than from asking whether
 * this is a touch device. Both boards here have touchscreens and so does
 * the tablet, so that question separates nothing; how many pixels there
 * are to spend does. A twentieth of the height lands near forty-four
 * points on a desktop and near a hundred on the tablet, which is the
 * touch target Android asks for, and it is clamped at both ends so a
 * very small or very large display stays usable.
 */
#define RAIL_MIN 56
#define RAIL_MAX 160
/* The margin is a fraction of the rail rather than a fixed number of
 * pixels, so an icon occupies the same share of the button at every
 * density instead of shrinking into a large one. */
#define RAIL_MARGIN(rail) ((rail) / 8)

struct mrc_ui {
    SDL_Renderer *renderer;
    /*
     * The window the renderer draws into, kept rather than asked for.
     *
     * SDL_RenderGetWindow would answer this, and it is a 2.0.22 function:
     * the Orange Pi this is cross-checked on ships an older SDL, and a
     * build there failed to link over one call. The caller has the window
     * in hand anyway -- it created both -- so taking it is cheaper than a
     * version test, and leaves nothing to work around.
     */
    SDL_Window *window;
    SDL_Texture  *font[MRC_UI_FONT_COUNT];
    SDL_Texture  *icons[MRC_UI_ICON_SIZES];
    unsigned      buttons;      /* which are offered  */
    unsigned      lit;          /* which are lit      */
    int           rail;         /* width in output pixels */
    int           font_index, icon_index;
    mrc_ui_side   side;
    int           hot, pressed; /* under the pointer, and held */
    int           action;       /* taken by the frontend */
    int           out_w, out_h;

    /* The open panel, if any. The rows are borrowed from the frontend. */
    const char       *title;
    const mrc_ui_row *rows;
    unsigned          row_count;
    bool              panel;
    int               row_hot, row_pressed, row_action;
    /* Which of a row's own action buttons (not the row body) is under the
     * pointer or held, or -1. Meaningless unless the matching row_hot or
     * row_pressed is >= 0. */
    int               slot_hot, slot_pressed;
    /*
     * How far the rows have been scrolled, in output pixels.
     *
     * A settings panel is a handful of rows and never needed this. A list of
     * devices, or of files to pick one from, is as long as it is, and the
     * panel used to simply stop drawing at the bottom edge -- which looks
     * exactly like a list that ends there.
     *
     * Dragged rather than only wheeled, because the tablet has no wheel.
     * That means a press has to wait to find out what it was: past a few
     * pixels of movement it is a scroll and the row under it must not fire.
     */
    int               scroll, scroll_max;
    int               drag_from, drag_scroll;
    bool              dragging;
    bool              back_pressed;
};

/* An alpha strip becomes a white texture whose alpha is the coverage, so
 * one texture serves every colour through the renderer's colour mod. */
static SDL_Texture *strip_texture(SDL_Renderer *r, const unsigned char *alpha,
                                  int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    uint32_t *pixels = malloc((size_t)w * h * 4);
    if (!pixels) { SDL_DestroyTexture(t); return NULL; }
    for (int i = 0; i < w * h; i++)
        pixels[i] = (uint32_t)alpha[i] << 24 | 0x00FFFFFFu;
    SDL_UpdateTexture(t, NULL, pixels, w * 4);
    free(pixels);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void choose_sizes(mrc_ui *ui)
{
    SDL_GetRendererOutputSize(ui->renderer, &ui->out_w, &ui->out_h);
    if (ui->out_h <= 0) ui->out_h = 480;
    int rail = ui->out_h / 14;
    if (rail < RAIL_MIN) rail = RAIL_MIN;
    if (rail > RAIL_MAX) rail = RAIL_MAX;
    unsigned buttons = 0;
    for (unsigned k = 0; k < MRC_UI_ICON_COUNT; ++k)
        if (ui->buttons & (1u << k)) ++buttons;
    if (buttons && rail * (int)buttons > ui->out_h)
        rail = ui->out_h / (int)buttons;
    if (rail < 1) rail = 1;
    ui->rail = rail;

    /* The largest icon that leaves a margin, and the text to match. */
    int room = rail - 2 * RAIL_MARGIN(rail);
    ui->icon_index = 0;
    for (int k = 0; k < MRC_UI_ICON_SIZES; k++)
        if ((int)mrc_ui_icons[0].sizes[k].size <= room) ui->icon_index = k;
    /* Text scaled to the rail as well, rather than one size below a
     * threshold and another above it: a rail half again as wide with the
     * same label on it reads as a mistake. */
    int text_room = rail * 5 / 14;
    ui->font_index = 0;
    for (int k = 0; k < MRC_UI_FONT_COUNT; k++)
        if ((int)mrc_ui_fonts[k].size <= text_room) ui->font_index = k;
}

bool mrc_ui_open(mrc_ui **out, SDL_Renderer *renderer, SDL_Window *window)
{
    *out = NULL;
    mrc_ui *ui = calloc(1, sizeof *ui);
    if (!ui) return false;
    ui->renderer = renderer;
    ui->window = window;
    ui->hot = ui->pressed = ui->action = -1;
    ui->buttons = 0;
    {
        const char *where = SDL_getenv("MRC_UI_RAIL_SIDE");
        ui->side = where && !SDL_strcmp(where, "right")
                 ? MRC_UI_RIGHT : MRC_UI_LEFT;
    }

    for (int k = 0; k < MRC_UI_FONT_COUNT; k++) {
        const mrc_ui_font_data *f = &mrc_ui_fonts[k];
        ui->font[k] = strip_texture(renderer, f->alpha,
                                    (int)(f->cell_w * MRC_UI_FONT_COLUMNS),
                                    (int)(f->cell_h * ((f->count + MRC_UI_FONT_COLUMNS - 1) / MRC_UI_FONT_COLUMNS)));
        if (!ui->font[k]) { mrc_ui_close(ui); return false; }
    }
    for (int k = 0; k < MRC_UI_ICON_SIZES; k++) {
        unsigned size = mrc_ui_icons[0].sizes[k].size;
        unsigned char *strip = malloc((size_t)size * size * MRC_UI_ICON_COUNT);
        if (!strip) { mrc_ui_close(ui); return false; }
        for (int i = 0; i < MRC_UI_ICON_COUNT; i++)
            memcpy(strip + (size_t)i * size * size,
                   mrc_ui_icons[i].sizes[k].alpha, (size_t)size * size);
        ui->icons[k] = strip_texture(renderer, strip, (int)size,
                                     (int)(size * MRC_UI_ICON_COUNT));
        free(strip);
        if (!ui->icons[k]) { mrc_ui_close(ui); return false; }
    }
    choose_sizes(ui);
    *out = ui;
    return true;
}

void mrc_ui_close(mrc_ui *ui)
{
    if (!ui) return;
    for (int k = 0; k < MRC_UI_FONT_COUNT; k++)
        if (ui->font[k]) SDL_DestroyTexture(ui->font[k]);
    for (int k = 0; k < MRC_UI_ICON_SIZES; k++)
        if (ui->icons[k]) SDL_DestroyTexture(ui->icons[k]);
    free(ui);
}

void mrc_ui_set_buttons(mrc_ui *ui, unsigned mask)
{
    if (ui) { ui->buttons = mask; choose_sizes(ui); }
}

void mrc_ui_set_lit(mrc_ui *ui, int icon, bool lit)
{
    if (!ui || icon < 0 || icon >= MRC_UI_ICON_COUNT) return;
    if (lit) ui->lit |= 1u << icon;
    else ui->lit &= ~(1u << icon);
}

void mrc_ui_set_side(mrc_ui *ui, mrc_ui_side side)
{
    if (!ui) return;
    ui->side = side;
    /* A press in flight belonged to a button that has just moved. */
    ui->pressed = ui->hot = -1;
}

mrc_ui_side mrc_ui_get_side(const mrc_ui *ui)
{
    return ui ? ui->side : MRC_UI_LEFT;
}

int mrc_ui_rail_width(const mrc_ui *ui)
{
    return ui && ui->buttons ? ui->rail : 0;
}

int mrc_ui_inset_left(const mrc_ui *ui)
{
    return ui && ui->side == MRC_UI_LEFT ? mrc_ui_rail_width(ui) : 0;
}

int mrc_ui_inset_right(const mrc_ui *ui)
{
    return ui && ui->side == MRC_UI_RIGHT ? mrc_ui_rail_width(ui) : 0;
}

/* The rail's own left edge, which is the far side of the output when the
 * rail is on the right. */
static int rail_x(const mrc_ui *ui)
{
    return ui->side == MRC_UI_LEFT ? 0 : ui->out_w - ui->rail;
}

/* Where the nth offered button sits, counting only the offered ones. */
static bool button_rect(const mrc_ui *ui, int icon, SDL_Rect *r)
{
    if (!(ui->buttons & (1u << icon))) return false;
    int slot = 0;
    for (int k = 0; k < icon; k++) if (ui->buttons & (1u << k)) slot++;
    r->x = rail_x(ui);
    r->y = slot * ui->rail;
    r->w = ui->rail;
    r->h = ui->rail;
    return true;
}

static int button_at(const mrc_ui *ui, int x, int y)
{
    int left = rail_x(ui);
    if (!ui->buttons || x < left || x >= left + ui->rail) return -1;
    for (int k = 0; k < MRC_UI_ICON_COUNT; k++) {
        SDL_Rect r;
        if (button_rect(ui, k, &r) && y >= r.y && y < r.y + r.h) return k;
    }
    return -1;
}

/*
 * Events arrive in window coordinates and the rail is laid out in the
 * renderer's output coordinates, which differ wherever the window is
 * scaled. Converting here keeps every caller from having to know.
 */
static void to_output(const mrc_ui *ui, int wx, int wy, int *ox, int *oy)
{
    int ww = 0, wh = 0;
    if (ui->window) SDL_GetWindowSize(ui->window, &ww, &wh);
    *ox = ww > 0 ? (int)((int64_t)wx * ui->out_w / ww) : wx;
    *oy = wh > 0 ? (int)((int64_t)wy * ui->out_h / wh) : wy;
}

/* Defined below, beside the rest of the panel. */
static int row_at(const mrc_ui *ui, int x, int y);
static int row_action_at(const mrc_ui *ui, int x, int y, int *slot);
static void panel_extent(mrc_ui *ui);
static void panel_scroll_by(mrc_ui *ui, int delta);
static bool in_panel(const mrc_ui *ui, int x, int y);
static void panel_geometry(const mrc_ui *ui, SDL_Rect *sheet, int *row_h,
                           int *head_h);

static bool back_rect(const mrc_ui *ui, SDL_Rect *r)
{
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);
    int w = ui->rail * 2;
    r->x = sheet.x + sheet.w - w;
    r->y = sheet.y;
    r->w = w;
    r->h = head_h;
    return true;
}

/*
 * A press and release inside the panel. Returns true when the panel took
 * the event, which it does for anything on the sheet and, while it is
 * open, for anything off the rail -- tapping the guest is how a panel is
 * dismissed, so that press must not also reach the guest.
 */
static bool panel_event(mrc_ui *ui, int x, int y, int phase)
{
    if (!ui->panel) return false;
    bool on_sheet = in_panel(ui, x, y);
    bool on_rail = button_at(ui, x, y) >= 0;
    SDL_Rect back;
    bool on_back = back_rect(ui, &back) && x >= back.x && x < back.x + back.w &&
                   y >= back.y && y < back.y + back.h;
    if (on_rail) return false;          /* the rail still answers for itself */

    if (phase == 0) {                   /* moved */
        if (ui->row_pressed >= 0 || ui->dragging) {
            /*
             * A press that has moved far enough is a scroll, and the row it
             * started on must not fire when the finger comes up. The
             * threshold is a fraction of a row so that a steady tap on a
             * touchscreen -- which always wobbles a little -- still counts
             * as a tap.
             */
            int moved = y - ui->drag_from;
            if (!ui->dragging && (moved > ui->rail / 6 || moved < -ui->rail / 6)) {
                ui->dragging = true;
                ui->row_pressed = -1;
                ui->slot_pressed = -1;
            }
            if (ui->dragging) {
                ui->scroll = ui->drag_scroll - moved;
                panel_extent(ui);
                ui->row_hot = -1;
                ui->slot_hot = -1;
                return true;
            }
        }
        ui->row_hot = on_sheet ? row_action_at(ui, x, y, &ui->slot_hot) : -1;
        if (ui->row_hot < 0) ui->slot_hot = -1;
        return true;
    }
    if (phase == 1) {                   /* pressed */
        ui->back_pressed = on_back;
        ui->row_pressed = on_sheet ? row_action_at(ui, x, y, &ui->slot_pressed) : -1;
        if (ui->row_pressed < 0) ui->slot_pressed = -1;
        ui->drag_from = y;
        ui->drag_scroll = ui->scroll;
        ui->dragging = false;
        return true;
    }
    /* released */
    if (ui->dragging) {
        ui->dragging = false;
        ui->row_pressed = -1;
        ui->slot_pressed = -1;
        return true;
    }
    if (ui->back_pressed && on_back) {
        ui->row_action = MRC_UI_ROW_BACK;
        ui->back_pressed = false;
        ui->row_pressed = -1;
        ui->slot_pressed = -1;
        return true;
    }
    ui->back_pressed = false;
    if (!on_sheet) { ui->row_action = MRC_UI_ROW_DISMISS; return true; }
    int slot = -1;
    int row = row_action_at(ui, x, y, &slot);
    if (row >= 0 && row == ui->row_pressed && slot == ui->slot_pressed) {
        ui->row_action = slot >= 0 ? ui->rows[row].action_id[slot]
                                   : ui->rows[row].id;
    }
    ui->row_pressed = -1;
    ui->slot_pressed = -1;
    return true;
}

bool mrc_ui_event(mrc_ui *ui, const SDL_Event *e)
{
    if (!ui || !ui->buttons) return false;
    int x, y;
    switch (e->type) {
    case SDL_KEYDOWN:
        if (!ui->panel) return false;
        if (!e->key.repeat && (e->key.keysym.sym == SDLK_ESCAPE ||
                              e->key.keysym.sym == SDLK_AC_BACK))
            ui->row_action = MRC_UI_ROW_BACK;
        return true;
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
        return ui->panel;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED)
            choose_sizes(ui);
        return false;
    case SDL_MOUSEWHEEL:
        if (ui->panel) {
            panel_scroll_by(ui, -e->wheel.y * ui->rail / 2);
            return true;
        }
        return false;
    case SDL_MOUSEMOTION:
        to_output(ui, e->motion.x, e->motion.y, &x, &y);
        if (panel_event(ui, x, y, 0)) return true;
        ui->hot = button_at(ui, x, y);
        return ui->pressed >= 0 || ui->hot >= 0;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button != SDL_BUTTON_LEFT)
            return ui->panel;
        to_output(ui, e->button.x, e->button.y, &x, &y);
        if (panel_event(ui, x, y, 1)) return true;
        ui->pressed = button_at(ui, x, y);
        return ui->pressed >= 0;
    case SDL_MOUSEBUTTONUP: {
        if (e->button.button != SDL_BUTTON_LEFT)
            return ui->panel;
        to_output(ui, e->button.x, e->button.y, &x, &y);
        if (panel_event(ui, x, y, 2)) return true;
        if (ui->pressed < 0) return false;
        /* Only if it comes up on the button it went down on, so a press
         * can be taken back by sliding off, as a button should. */
        if (button_at(ui, x, y) == ui->pressed) ui->action = ui->pressed;
        ui->pressed = -1;
        return true;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION: {
        /* Touch arrives normalised to the window. */
        int ox = (int)(e->tfinger.x * (float)ui->out_w);
        int oy = (int)(e->tfinger.y * (float)ui->out_h);
        if (panel_event(ui, ox, oy,
                        e->type == SDL_FINGERDOWN ? 1 :
                        e->type == SDL_FINGERMOTION ? 0 : 2))
            return true;
        int hit = button_at(ui, ox, oy);
        if (e->type == SDL_FINGERDOWN) {
            ui->pressed = hit;
            ui->hot = hit;
            return hit >= 0;
        }
        if (e->type == SDL_FINGERMOTION) {
            ui->hot = hit;
            return ui->pressed >= 0;
        }
        if (ui->pressed < 0) return false;
        if (hit == ui->pressed) ui->action = ui->pressed;
        ui->pressed = -1;
        ui->hot = -1;
        return true;
    }
    default:
        return false;
    }
}

int mrc_ui_take_action(mrc_ui *ui)
{
    if (!ui) return -1;
    int a = ui->action;
    ui->action = -1;
    return a;
}

static void set_colour(SDL_Texture *t, SDL_Color c)
{
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
}

/* --------------------------------------------------------------- panel */

void mrc_ui_open_panel(mrc_ui *ui, const char *title,
                       const mrc_ui_row *rows, unsigned count)
{
    if (!ui) return;
    ui->title = title;
    ui->rows = rows;
    ui->row_count = count;
    ui->panel = true;
    ui->row_hot = ui->row_pressed = ui->row_action = -1;
    ui->slot_hot = ui->slot_pressed = -1;
    ui->back_pressed = false;
    /* A panel opens at the top, including one reopened with a new list. */
    ui->scroll = 0;
    ui->dragging = false;
    panel_extent(ui);
}

const mrc_ui_row *mrc_ui_panel_rows(const mrc_ui *ui)
{
    return ui && ui->panel ? ui->rows : NULL;
}

unsigned mrc_ui_panel_row_count(const mrc_ui *ui)
{
    return ui && ui->panel ? ui->row_count : 0;
}

void mrc_ui_close_panel(mrc_ui *ui)
{
    if (!ui) return;
    ui->panel = false;
    ui->rows = NULL;
    ui->row_count = 0;
    ui->row_hot = ui->row_pressed = -1;
    ui->slot_hot = ui->slot_pressed = -1;
    ui->back_pressed = false;
}

bool mrc_ui_panel_open(const mrc_ui *ui) { return ui && ui->panel; }

int mrc_ui_take_row(mrc_ui *ui)
{
    if (!ui) return -1;
    int r = ui->row_action;
    ui->row_action = -1;
    return r;
}

/* The sheet, and the rows inside it. Everything is derived from the rail's
 * width so that one measurement sets the whole scale. */
static void panel_geometry(const mrc_ui *ui, SDL_Rect *sheet, int *row_h,
                           int *head_h)
{
    int margin = ui->rail / 4;
    int content_x = ui->side == MRC_UI_LEFT ? ui->rail : 0;
    int content_w = ui->out_w - ui->rail;
    sheet->x = content_x + margin;
    sheet->y = margin;
    sheet->w = content_w - 2 * margin;
    sheet->h = ui->out_h - 2 * margin;
    if (sheet->w < 1) sheet->w = 1;
    if (sheet->h < 1) sheet->h = 1;
    *row_h = ui->rail;
    *head_h = (int)(ui->rail * 3 / 4);
}

/* How tall all the rows are together, and how far they can be scrolled. */
static void panel_extent(mrc_ui *ui)
{
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);
    int total = 0;
    for (unsigned k = 0; k < ui->row_count; k++)
        total += ui->rows[k].kind == MRC_UI_ROW_HEADING ? head_h : row_h;
    int visible = sheet.h - head_h;
    ui->scroll_max = total > visible ? total - visible : 0;
    if (ui->scroll > ui->scroll_max) ui->scroll = ui->scroll_max;
    if (ui->scroll < 0) ui->scroll = 0;
}

static void panel_scroll_by(mrc_ui *ui, int delta)
{
    ui->scroll += delta;
    panel_extent(ui);
}

/* Which row a point is on, or -1. Headings are not rows you can press. */
static int row_at(const mrc_ui *ui, int x, int y)
{
    if (!ui->panel || !ui->rows) return -1;
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);
    if (x < sheet.x || x >= sheet.x + sheet.w) return -1;
    int at = sheet.y + head_h - ui->scroll;   /* below the title */
    for (unsigned k = 0; k < ui->row_count; k++) {
        int h = ui->rows[k].kind == MRC_UI_ROW_HEADING ? head_h : row_h;
        if (y >= at && y < at + h &&
            at >= sheet.y + head_h && at + h <= sheet.y + sheet.h)
            return ui->rows[k].kind == MRC_UI_ROW_HEADING ? -1 : (int)k;
        at += h;
    }
    return -1;
}

/*
 * Where a row's own action button sits, right-aligned as a group with slot
 * 0 nearer the label -- a play button left of a trash can, say. Square, and
 * the same height as the row, which is what makes it a button rather than
 * a mark: the rail's own buttons are squares of the rail's width for the
 * same reason.
 */
static bool row_action_rect(const SDL_Rect *sheet, int at, int h,
                            const mrc_ui_row *row, unsigned slot, SDL_Rect *r)
{
    if (slot >= row->action_count) return false;
    int right = sheet->x + sheet->w;
    r->x = right - h * (int)(row->action_count - slot);
    r->y = at;
    r->w = h;
    r->h = h;
    return true;
}

/* The y a given row starts at, below the title and after everything drawn
 * above it. Rows can differ in height (a heading is shorter), so this walks
 * the same way draw_panel does rather than multiplying by a row index. */
static int row_top(const mrc_ui *ui, int row, int row_h, int head_h,
                   const SDL_Rect *sheet)
{
    int at = sheet->y + head_h - ui->scroll;
    for (int k = 0; k < row; k++)
        at += ui->rows[k].kind == MRC_UI_ROW_HEADING ? head_h : row_h;
    return at;
}

/*
 * Which row a point is on, and which of that row's own action buttons, if
 * any -- pressing the trash can is not pressing the row it sits in. `slot`
 * is set to -1 when the point is on the row body instead, and whenever the
 * row itself is -1.
 */
static int row_action_at(const mrc_ui *ui, int x, int y, int *slot)
{
    *slot = -1;
    int row = row_at(ui, x, y);
    if (row < 0) return row;
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);
    int at = row_top(ui, row, row_h, head_h, &sheet);
    const mrc_ui_row *r = &ui->rows[row];
    for (unsigned s = 0; s < r->action_count; s++) {
        SDL_Rect ar;
        if (row_action_rect(&sheet, at, row_h, r, s, &ar) &&
            x >= ar.x && x < ar.x + ar.w && y >= ar.y && y < ar.y + ar.h) {
            *slot = (int)s;
            break;
        }
    }
    return row;
}

/* True when the point is anywhere on the sheet, which is what stops a tap
 * meant for the panel from reaching the guest behind it. */
static bool in_panel(const mrc_ui *ui, int x, int y)
{
    if (!ui->panel) return false;
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);
    return x >= sheet.x && x < sheet.x + sheet.w &&
           y >= sheet.y && y < sheet.y + sheet.h;
}

int mrc_ui_text_height(const mrc_ui *ui)
{
    return ui ? (int)mrc_ui_fonts[ui->font_index].cell_h : 0;
}

/* Decode one UTF-8 character; malformed input is a visible replacement. */
static unsigned text_code(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned c = *p++, n = 0, minimum = 0;
    if (c < 0x80) { *text = (const char *)p; return c; }
    if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; minimum = 0x10000; }
    else { *text = (const char *)p; return 0xfffd; }
    for (unsigned i = 0; i < n; ++i) {
        if ((p[i] & 0xc0) != 0x80) { *text = (const char *)p; return 0xfffd; }
        c = (c << 6) | (p[i] & 63);
    }
    *text = (const char *)(p + n);
    return c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ? 0xfffd : c;
}

int mrc_ui_text_width(const mrc_ui *ui, const char *s)
{
    if (!ui || !s) return 0;
    unsigned n = 0;
    while (*s) { text_code(&s); ++n; }
    return (int)(n * mrc_ui_fonts[ui->font_index].advance);
}

int mrc_ui_text(mrc_ui *ui, int x, int y, const char *s, SDL_Color colour)
{
    if (!ui || !s) return 0;
    const mrc_ui_font_data *f = &mrc_ui_fonts[ui->font_index];
    SDL_Texture *t = ui->font[ui->font_index];
    set_colour(t, colour);
    int at = x;
    while (*s) {
        unsigned code = text_code(&s), glyph = f->count - 1;
        for (unsigned k = 0; k < f->count; ++k)
            if (mrc_ui_glyphs[k] == code) { glyph = k; break; }
        {
            SDL_Rect src = { (int)((glyph % MRC_UI_FONT_COLUMNS) * f->cell_w),
                             (int)((glyph / MRC_UI_FONT_COLUMNS) * f->cell_h),
                             (int)f->cell_w, (int)f->cell_h };
            SDL_Rect dst = { at, y, src.w, src.h };
            SDL_RenderCopy(ui->renderer, t, &src, &dst);
        }
        at += (int)f->advance;
    }
    return at - x;
}

/* Ellipsize without splitting UTF-8 or drawing into a neighbour's area. */
static void text_fit(mrc_ui *ui, int x, int y, int width, const char *s, SDL_Color colour)
{
    if (!s || width <= 0) return;
    if (mrc_ui_text_width(ui, s) <= width) { mrc_ui_text(ui,x,y,s,colour); return; }
    int cells = width / (int)mrc_ui_fonts[ui->font_index].advance;
    if (cells < 1) return;
    char shortened[1024];
    const char *end = s;
    for (int k = 0; k < cells - 1 && *end; ++k) {
        const char *next = end; text_code(&next);
        if (next - s > (int)sizeof(shortened) - 4) break;
        end = next;
    }
    size_t n = (size_t)(end - s);
    memcpy(shortened, s, n);
    memcpy(shortened + n, "\xe2\x80\xa6", 4);
    mrc_ui_text(ui,x,y,shortened,colour);
}

/*
 * A filled box with its corners left out, which is as much as SDL2 draws
 * without a geometry API and reads better beside the icons than a bare
 * rectangle.
 *
 * The corner is clamped to a third of the shorter side. Asking for half
 * leaves the middle band no height at all, and the box comes out as two
 * thin caps with a gap between them -- which is exactly what the first
 * switches looked like.
 */
static void panel_box(SDL_Renderer *r, const SDL_Rect *box, int corner)
{
    int limit = (box->w < box->h ? box->w : box->h) / 3;
    if (corner > limit) corner = limit;
    if (corner < 1) { SDL_RenderFillRect(r, box); return; }
    SDL_Rect mid = { box->x, box->y + corner, box->w, box->h - 2 * corner };
    SDL_Rect cap = { box->x + corner, box->y, box->w - 2 * corner, corner };
    SDL_RenderFillRect(r, &mid);
    SDL_RenderFillRect(r, &cap);
    cap.y = box->y + box->h - corner;
    SDL_RenderFillRect(r, &cap);
}

static void draw_panel(mrc_ui *ui)
{
    if (!ui->panel || !ui->rows) return;
    SDL_Rect sheet;
    int row_h, head_h;
    panel_geometry(ui, &sheet, &row_h, &head_h);

    /* A scrim over the guest so the sheet reads as being in front of it,
     * and so the guest's own picture does not compete with the text. */
    SDL_Rect content = { ui->side == MRC_UI_LEFT ? ui->rail : 0, 0,
                         ui->out_w - ui->rail, ui->out_h };
    SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 150);
    SDL_RenderFillRect(ui->renderer, &content);

    SDL_SetRenderDrawColor(ui->renderer, 30, 30, 33, 245);
    panel_box(ui->renderer, &sheet, row_h / 6);

    const SDL_Color white = { 236, 238, 242, 255 };
    const SDL_Color dim   = { 150, 153, 160, 255 };
    const SDL_Color accent= { 132, 178, 255, 255 };

    int pad = ui->rail / 3;
    int text_h = mrc_ui_text_height(ui);
    SDL_Rect back;
    back_rect(ui, &back);
    text_fit(ui, sheet.x + pad, sheet.y + (head_h - text_h) / 2,
             back.x - sheet.x - 2 * pad, ui->title, dim);
    mrc_ui_text(ui, back.x + (back.w - mrc_ui_text_width(ui, "Back")) / 2,
                back.y + (head_h - text_h) / 2, "Back", accent);

    int at = sheet.y + head_h - ui->scroll;
    for (unsigned k = 0; k < ui->row_count; k++) {
        const mrc_ui_row *row = &ui->rows[k];
        int h = row->kind == MRC_UI_ROW_HEADING ? head_h : row_h;
        if (at + h > sheet.y + sheet.h) break;   /* below the sheet */
        if (at < sheet.y + head_h) { at += h; continue; }  /* above it */

        /*
         * The whole row lights up for a tap on its body, but not when what
         * is hot or pressed is one of its own action buttons -- that button
         * gets its own highlight below, and lighting the row underneath it
         * too would read as the row itself being about to fire.
         */
        bool row_pressed = (int)k == ui->row_pressed && ui->slot_pressed < 0;
        bool row_hot = (int)k == ui->row_hot && ui->slot_hot < 0;
        if (row_pressed || row_hot) {
            SDL_Rect hl = { sheet.x, at, sheet.w, h };
            SDL_SetRenderDrawColor(ui->renderer,
                row_pressed ? 58 : 44, row_pressed ? 92 : 44,
                row_pressed ? 140 : 48, 255);
            SDL_RenderFillRect(ui->renderer, &hl);
        }
        int ty = at + (h - text_h) / 2;
        int text_left = sheet.x + pad;
        int text_right = sheet.x + sheet.w - pad - h * (int)row->action_count;
        if (row->kind == MRC_UI_ROW_TOGGLE) text_right -= ui->rail;
        int space = text_right - text_left;
        bool has_value = (row->kind == MRC_UI_ROW_CHOICE || row->kind == MRC_UI_ROW_ACTION) && row->value;
        int vw = has_value ? mrc_ui_text_width(ui, row->value) : 0;
        bool fits = mrc_ui_text_width(ui, row->label) + vw + pad <= space;
        bool stacked = has_value && h >= 2 * text_h && !fits;
        int value_width = stacked ? space : (fits || vw < space/2 ? vw : space/2);
        int label_width = has_value && !stacked ? space - value_width - pad : space;
        text_fit(ui, text_left, stacked ? at + (h - 2 * text_h)/2 : ty,
                 label_width, row->label, row->kind == MRC_UI_ROW_HEADING ? dim : white);

        if (row->kind == MRC_UI_ROW_TOGGLE) {
            /* A switch: a track with the knob at one end. */
            int kw = ui->rail * 2 / 3, kh = ui->rail / 3;
            SDL_Rect track = { sheet.x + sheet.w - pad - kw,
                               at + (h - kh) / 2, kw, kh };
            SDL_SetRenderDrawColor(ui->renderer,
                row->on ? 58 : 70, row->on ? 110 : 70,
                row->on ? 170 : 74, 255);
            panel_box(ui->renderer, &track, kh / 2);
            SDL_Rect knob = { row->on ? track.x + kw - kh : track.x,
                              track.y, kh, kh };
            SDL_SetRenderDrawColor(ui->renderer, 236, 238, 242, 255);
            panel_box(ui->renderer, &knob, kh / 2);
        } else if ((row->kind == MRC_UI_ROW_CHOICE ||
                    row->kind == MRC_UI_ROW_ACTION) && row->value) {
            text_fit(ui, stacked ? text_left : text_right - value_width,
                     stacked ? at + (h - 2 * text_h)/2 + text_h : ty,
                     value_width, row->value, accent);
        }

        if (row->kind != MRC_UI_ROW_HEADING && row->action_count) {
            unsigned isize = mrc_ui_icons[0].sizes[ui->icon_index].size;
            SDL_Texture *icons = ui->icons[ui->icon_index];
            for (unsigned s = 0; s < row->action_count; s++) {
                SDL_Rect ar;
                if (!row_action_rect(&sheet, at, h, row, s, &ar)) continue;
                bool pressed = (int)k == ui->row_pressed && (int)s == ui->slot_pressed;
                bool hot = (int)k == ui->row_hot && (int)s == ui->slot_hot;
                if (pressed || hot) {
                    SDL_SetRenderDrawColor(ui->renderer,
                        pressed ? 58 : 44, pressed ? 92 : 44,
                        pressed ? 140 : 48, 255);
                    SDL_RenderFillRect(ui->renderer, &ar);
                }
                SDL_Color tint = pressed
                    ? (SDL_Color){ 255, 255, 255, 255 }
                    : (SDL_Color){ 196, 198, 204, 255 };
                set_colour(icons, tint);
                int icon = row->action_icon[s];
                SDL_Rect src = { 0, (int)((unsigned)icon * isize),
                                 (int)isize, (int)isize };
                SDL_Rect dst = { ar.x + (ar.w - (int)isize) / 2,
                                 ar.y + (ar.h - (int)isize) / 2,
                                 (int)isize, (int)isize };
                SDL_RenderCopy(ui->renderer, icons, &src, &dst);
            }
        }
        at += h;
    }
}

void mrc_ui_draw(mrc_ui *ui)
{
    if (!ui || !ui->buttons) return;
    SDL_GetRendererOutputSize(ui->renderer, &ui->out_w, &ui->out_h);

    SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
    draw_panel(ui);
    int left = rail_x(ui);
    SDL_Rect rail = { left, 0, ui->rail, ui->out_h };
    SDL_SetRenderDrawColor(ui->renderer, 24, 24, 26, 255);
    SDL_RenderFillRect(ui->renderer, &rail);
    /* A hairline on the edge that faces the guest, so the two do not blend
     * into each other on a dark screen. */
    int edge = ui->side == MRC_UI_LEFT ? left + ui->rail - 1 : left;
    SDL_SetRenderDrawColor(ui->renderer, 70, 70, 74, 255);
    SDL_RenderDrawLine(ui->renderer, edge, 0, edge, ui->out_h);

    unsigned size = mrc_ui_icons[0].sizes[ui->icon_index].size;
    SDL_Texture *icons = ui->icons[ui->icon_index];
    for (int k = 0; k < MRC_UI_ICON_COUNT; k++) {
        SDL_Rect box;
        if (!button_rect(ui, k, &box)) continue;

        bool lit = (ui->lit & (1u << k)) != 0;
        if (ui->pressed == k || lit) {
            SDL_SetRenderDrawColor(ui->renderer, 58, 92, 140, 255);
            SDL_RenderFillRect(ui->renderer, &box);
        } else if (ui->hot == k) {
            SDL_SetRenderDrawColor(ui->renderer, 44, 44, 48, 255);
            SDL_RenderFillRect(ui->renderer, &box);
        }
        SDL_Color tint = lit || ui->pressed == k
            ? (SDL_Color){ 255, 255, 255, 255 }
            : (SDL_Color){ 196, 198, 204, 255 };
        set_colour(icons, tint);
        SDL_Rect src = { 0, (int)((unsigned)k * size), (int)size, (int)size };
        SDL_Rect dst = { box.x + (box.w - (int)size) / 2,
                         box.y + (box.h - (int)size) / 2,
                         (int)size, (int)size };
        SDL_RenderCopy(ui->renderer, icons, &src, &dst);
    }
}
