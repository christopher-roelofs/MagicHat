/*
 * gui_sdl.c — a window onto the machine.
 *
 * Shows the framebuffer, turns mouse input into touches and keystrokes into
 * UART A bytes. This is a front-end and nothing more: it does not model any
 * hardware, and every input it produces goes through the same device calls a
 * scripted run uses.
 */
#include "frontend/sdl/datarover_gui.h"

bool mrc_gui_touch_debug = false;
bool mrc_gui_audio = true;

#ifdef MRC_HAVE_SDL

#include <SDL2/SDL.h>
#include "frontend/sdl/presentation.h"
#include "frontend/sdl/display.h"
#include "frontend/sdl/shell.h"
#include "frontend/sdl/startup.h"
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include "host/state.h"
#include "frontend/sdl/lcd.h"
#include "frontend/sdl/display_panel.h"
#include "frontend/sdl/devices_panel.h"
#include "frontend/sdl/settings_panel.h"
#include "frontend/sdl/install_panel.h"
#include "frontend/sdl/card_panel.h"
#include "host/state.h"
#include "host/wav.h"

static SDL_Renderer *g_ren;
/* The control rail, file-scope beside the renderer it belongs with,
 * because the panel geometry is worked out before the display structure
 * is in scope. */
static mrc_ui *g_ui;   /* for mapping window points to the panel */
static SDL_Window   *g_win;

static int g_last_px = -1, g_last_py = -1;   /* last pen point, for --touch-debug */


/*
 * LCD emulation.
 *
 * The panel is 480x320 at 2bpp, so every pixel is one of four levels. That
 * makes a convincing panel cheap: pick the two endpoint colours of a real
 * LCD and interpolate, draw each panel pixel as a cell with a darker edge so
 * the dot structure shows, and blend a little of the previous frame back in
 * because an STN panel of this era was slow enough to smear visibly.
 *
 * Done on the CPU rather than in a shader. At 480x320x9 subpixels this is
 * about 1.4 M writes a frame, which is nothing, and it keeps the emulator
 * free of a GL dependency and working on any host SDL runs on.
 */
#define FRAME_HZ 60u

/*
 * A real pen tap has a duration, and the OS debounces and waits for a stable
 * reading before it dispatches one. The event loop drains every pending SDL
 * event and only then runs the machine, so a press and its release normally
 * arrive in the same batch: the guest saw pen-down and pen-up with zero
 * emulated time between them and nothing ever registered. Hold the pen down
 * for the same span the scripted --tap-hold uses (~54 ms of machine time)
 * before honouring the release.
 */
#define PEN_MIN_HOLD_INSNS 2000000ull

/* Some Android/Bluetooth SDL backends preserve the symbolic Caps Lock key
 * while reporting an unknown or inconsistent scancode. */
static unsigned keyboard_usage(const SDL_KeyboardEvent *key)
{
    return key->keysym.sym == SDLK_CAPSLOCK ? SDL_SCANCODE_CAPSLOCK :
           (unsigned)key->keysym.scancode;
}

/*
 * Sound. The SIB hands us one sample at a time from the emulated DMA ring; we
 * batch them and queue them to SDL. If the queue runs long -- the emulator
 * outpacing real time, or the machine having been paused -- samples are
 * dropped rather than allowed to build unbounded latency, since being a
 * fraction of a second behind is worse than a gap.
 */
#define AUDIO_BATCH      512


typedef struct {
    SDL_AudioDeviceID dev;
    mrc_runtime *machine;
    unsigned rate;
    Uint32 max_queued;
    bool failed;
    int16_t  buf[AUDIO_BATCH];
    unsigned n;
    uint64_t queued_batches, dropped_batches;
    uint64_t starved_polls, polls;
} audio_out;

/* Open on the first sample: reset registers do not describe the rate the
 * ROM will select. Reopen when software changes that rate during playback. */
static void audio_open(audio_out *a, unsigned rate)
{
    SDL_AudioSpec want, got;
    if (a->dev)
        SDL_CloseAudioDevice(a->dev);
    a->dev = 0;
    a->n = 0;
    a->rate = rate;
    memset(&want, 0, sizeof(want));
    want.freq = (int)(rate ? rate : MRC_AUDIO_RATE_FALLBACK);
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;

    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!a->dev) {
        fprintf(stderr, "GUI: no audio device (%s); running silent\n",
                SDL_GetError());
        a->failed = true;
        return;
    }
    a->max_queued = (Uint32)got.freq * sizeof(int16_t) / 4;
    /* Two device periods of silence give the producer a startup cushion.
     * This is host latency, not extra guest samples or a changed guest clock. */
    int16_t silence[512] = {0};
    for (unsigned n = 0; n < 2; n++) {
        if (SDL_QueueAudio(a->dev, silence, sizeof(silence)) < 0) {
            fprintf(stderr, "GUI: audio queue failed (%s)\n", SDL_GetError());
            SDL_CloseAudioDevice(a->dev);
            a->dev = 0;
            a->failed = true;
            return;
        }
    }
    SDL_PauseAudioDevice(a->dev, 0);
    fprintf(stderr, "GUI: audio out at %d Hz\n", got.freq);
}

static void audio_sample(void *ctx, int16_t sample)
{
    audio_out *a = ctx;
    if (a->failed)
        return;
    unsigned rate = a->machine->ops->audio_rate(a->machine->board);
    if (!a->dev || rate != a->rate)
        audio_open(a, rate);
    if (!a->dev)
        return;

    a->buf[a->n++] = sample;
    if (a->n < AUDIO_BATCH)
        return;

    Uint32 q = SDL_GetQueuedAudioSize(a->dev);
    a->polls++;
    if (q == 0)
        a->starved_polls++;
    if (q + a->n * sizeof(a->buf[0]) <= a->max_queued) {
        if (SDL_QueueAudio(a->dev, a->buf, a->n * sizeof(a->buf[0])) < 0) {
            fprintf(stderr, "GUI: audio queue failed (%s)\n", SDL_GetError());
            a->failed = true;
        } else {
            a->queued_batches++;
        }
    } else {
        a->dropped_batches++;
    }
    a->n = 0;
}

static void feed_key(mrc_runtime *m, const SDL_Event *e)
{
    /* UART A is the monitor console. Only send what a terminal would. */
    int sym = e->key.keysym.sym;
    uint8_t byte;

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
        byte = '\r';
    else if (sym == SDLK_BACKSPACE)
        byte = '\b';
    else if (sym == SDLK_TAB)
        byte = '\t';
    else if (sym == SDLK_ESCAPE)
        byte = 0x1B;
    else if (sym >= 0x20 && sym < 0x7F) {
        byte = (uint8_t)sym;
        if (e->key.keysym.mod & KMOD_SHIFT) {
            if (byte >= 'a' && byte <= 'z')
                byte = (uint8_t)(byte - 'a' + 'A');
        }
        if (e->key.keysym.mod & KMOD_CTRL)
            byte &= 0x1F;
    } else {
        return;
    }

    if (m->ops->key_byte) m->ops->key_byte(m->board, byte);
}

/*
 * Poking at the UI in the window and then wanting to keep what you found is
 * the common case, so the two things worth keeping have keys: F2 writes the
 * screen, F3 writes the whole machine. Both go next to the ROM under
 * shots/ and states/ with a sequence number, because naming them by hand
 * mid-exploration is what stops people from taking them.
 */
static void snapshot_key(mrc_runtime *m, int sym)
{
    static unsigned shot_seq, state_seq;
    char path[256];

    if (sym == SDLK_F2) {
        snprintf(path, sizeof(path), "shot%04u.pgm", shot_seq++);
        if (m->ops->save_screen(m->board, path))
            fprintf(stderr, "[gui] screen -> %s\n", path);
    } else {
        snprintf(path, sizeof(path), "gui%04u.state", state_seq++);
        if (m->ops->save_state(m->board, path))
            fprintf(stderr, "[gui] machine -> %s\n", path);
    }
}


/*
 * Where is the pointer, in the window's own pixels?
 *
 * Mouse event coordinates cannot be trusted to be in that space. Under
 * XWayland with a scaled display they arrive in a space half the size, while
 * SDL reports the window, its size in pixels and the renderer output all as
 * the same larger figure -- so nothing in the renderer knows a correction is
 * needed, and SDL_RenderWindowToLogical halves an already halved coordinate.
 *
 * Deriving a scale factor from a sample worked until the window was
 * maximised, because any cached factor is a guess about a relationship that
 * can change. SDL_GetGlobalMouseState does report in the same space as the
 * window's position and size, so the difference between them is the pointer
 * in window pixels by construction -- no factor, nothing to invalidate, and
 * it follows resizes and moves between differently scaled monitors for free.
 *
 * Falls back to the event coordinates if the global position is unavailable
 * or lands outside the window, which is the behaviour we had before.
 */
/*
 * Where the panel is drawn inside the window, in window pixels.
 *
 * SDL_RenderSetLogicalSize would do this, but then the letterbox maths lives
 * inside SDL while the pointer arrives in a different coordinate space, and
 * the two only agreed by accident: maximising the window broke clicks. Doing
 * it here means the same rectangle is used to draw and to map input, so they
 * cannot disagree.
 */
/*
 * What a rail button means. The frontend answers rather than the rail,
 * because the two machines answer differently and the rail knows about
 * neither of them.
 */
/* Defined below, beside the pen handling it belongs with. */
static void option_set(mrc_runtime *m, bool down);

static void rail_action(mrc_runtime *m, int action)
{
    if (mrc_card_panel_showing() && (action == MRC_UI_ICON_INSTALL ||
        action == MRC_UI_ICON_SETTINGS || action == MRC_UI_ICON_ROMS ||
        action == MRC_UI_ICON_DISPLAY)) mrc_card_panel_close(g_ui);
    switch (action) {
    case MRC_UI_ICON_INSTALL:
        if (mrc_install_panel_showing()) { mrc_install_panel_close(g_ui); break; }
        if (mrc_devices_panel_showing()) mrc_devices_panel_close(g_ui);
        if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
        if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
        if (mrc_ui_panel_open(g_ui)) mrc_ui_close_panel(g_ui);
        mrc_install_panel_open(g_ui, m);
        break;
    case MRC_UI_ICON_SETTINGS:
        /*
         * Switching the device off lives in here rather than on the rail.
         * Closing the window saves the machine and reopening carries on
         * mid-screen, so powering down is something you do when you mean it
         * -- and a button on the rail invites a press that costs a boot.
         */
        if (mrc_settings_panel_showing()) { mrc_settings_panel_close(g_ui); break; }
        if (mrc_devices_panel_showing()) mrc_devices_panel_close(g_ui);
        if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
        if (mrc_ui_panel_open(g_ui)) mrc_ui_close_panel(g_ui);
        mrc_settings_panel_open(g_ui, m);
        break;
    case MRC_UI_ICON_ROTATE: {
        /* Rotation is read from the environment on every use, so setting
         * it here turns the panel immediately and needs no other state. */
        static const char *const turns[] = { "0", "90", "180", "270" };
        unsigned now = mrc_sdl_rotation() / 90;
        SDL_setenv("MRC_DISPLAY_ROTATION", turns[(now + 1) % 4], 1);
        break;
    }
    case MRC_UI_ICON_STORAGE:
        mrc_sdl_save_state(m);
        break;
    case MRC_UI_ICON_OPTION:
        /* Held rather than pressed, and through the same one place the
         * line moves, so the rail and the right mouse button cannot end up
         * disagreeing about whether it is down. */
        if (m->ops->option)
            option_set(m, !m->ops->option_held(m->board));
        break;
    case MRC_UI_ICON_ROMS:
        /* Pressing it again puts the list away, the same as every other
         * button that opens a panel. */
        if (mrc_devices_panel_showing()) { mrc_devices_panel_close(g_ui); break; }
        if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
        if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
        if (mrc_ui_panel_open(g_ui)) mrc_ui_close_panel(g_ui);
        mrc_devices_panel_open(g_ui, mrc_state_device_id());
        break;
    case MRC_UI_ICON_DISPLAY:
        /* Pressing it again puts the panel away, so the button is the
         * whole control rather than the way in to one. */
        if (mrc_devices_panel_showing()) mrc_devices_panel_close(g_ui);
        if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
        if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
        if (mrc_ui_panel_open(g_ui)) { mrc_ui_close_panel(g_ui); break; }
        mrc_display_rows_refresh();
        mrc_ui_open_panel(g_ui, "Display", mrc_display_rows(),
                          mrc_display_row_count());
        break;
    default:
        break;
    }
}

static void panel_rect(SDL_Rect *dst)
{
    mrc_sdl_panel_rect(g_ren, PANEL_SCREEN_W, PANEL_SCREEN_H, mrc_gui_integer,
                       mrc_ui_inset_left(g_ui), mrc_ui_inset_right(g_ui), dst);
}

static float g_ptr_scale = 1.0f;   /* event units -> window pixels */

/*
 * What are mouse event coordinates measured in?
 *
 * Under XWayland with a scaled display they arrive in a space smaller than
 * the window SDL reports, so a click lands short of where it was aimed --
 * at 200% scaling, half way to the top left corner.
 *
 * SDL_GetGlobalMouseState reports in the same space as the window position
 * and size, so the ratio between that and the event gives the factor. It is
 * re-estimated on every motion rather than cached once: maximising the
 * window or dragging it to a differently scaled monitor changes the answer,
 * and a factor measured at startup was silently wrong afterwards.
 *
 * The event coordinates are still what gets scaled, rather than substituting
 * the current pointer position, so a queued event maps to where the pointer
 * was when it happened.
 */
static void note_pointer_scale(int ex, int ey)
{
    (void)ex; (void)ey;

    /*
     * Only XWayland needs this. On the native Wayland driver events are
     * already in window pixels, and SDL has no global pointer position to
     * compare them with: Wayland does not expose one, so SDL synthesizes it
     * from the window's (unknown, reported as 0,0) position and the last
     * event. Measured on the HackberryPi's 720x720 panel the "ratio" wandered
     * between 0.9 and 3.3 from one motion event to the next, and every click
     * was multiplied by whatever it happened to be.
     */
    const char *driver = SDL_GetCurrentVideoDriver();
    if (driver && !strcmp(driver, "wayland")) {
        g_ptr_scale = 1.0f;
        return;
    }

    /*
     * Both readings are taken now, so they describe the same instant.
     *
     * Comparing the event coordinates against the current global position
     * instead looked reasonable and was wrong: the pointer has moved on by
     * the time the queued event is handled, so the ratio came out as noise
     * (1.34, 1.2, 1.11 ... for a display where the true answer is 2).
     * SDL_GetMouseState reports in the same space as events, and
     * SDL_GetGlobalMouseState in the same space as the window, so sampling
     * the pair together gives the factor exactly whether or not the pointer
     * is moving.
     */
    int mx = 0, my = 0, gx = 0, gy = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetMouseState(&mx, &my);
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(g_win, &wx, &wy);
    SDL_GetWindowSize(g_win, &ww, &wh);

    int lx = gx - wx, ly = gy - wy;
    if (mx < 24 || my < 24 || lx <= 0 || ly <= 0 || lx >= ww || ly >= wh)
        return;

    float s = ((float)lx / mx + (float)ly / my) / 2.0f;
    if (s < 0.5f || s > 8.0f)
        return;
    if (s < g_ptr_scale * 0.98f || s > g_ptr_scale * 1.02f) {
        g_ptr_scale = s;
        if (getenv("MRC_TOUCH_TRACE"))
            fprintf(stderr, "GUI: pointer events are %.3gx window pixels\n", s);
    }
}

static void window_point(int ex, int ey, int *out_x, int *out_y)
{
    int ww=0, wh=0, rw=0, rh=0;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetRendererOutputSize(g_ren, &rw, &rh);
    *out_x = (int)(ex * g_ptr_scale * (ww > 0 ? (double)rw / ww : 1));
    *out_y = (int)(ey * g_ptr_scale * (wh > 0 ? (double)rh / wh : 1));
}

static bool panel_from_window(int ex, int ey, unsigned *px, unsigned *py)
{
    int wx, wy;
    SDL_Rect dst;
    window_point(ex, ey, &wx, &wy);
    panel_rect(&dst);
    return mrc_sdl_panel_point(&dst, wx, wy, PANEL_SCREEN_W,
                               PANEL_SCREEN_H, px, py);
}

static bool in_panel(int wx, int wy)
{
    unsigned px, py;
    return panel_from_window(wx, wy, &px, &py);
}

/*
 * The one place the option line moves. Idempotent, so the several paths that
 * end a press can all call it without caring which of them got there first.
 */
/*
 * Hold or release the option key.
 *
 * Whether it is currently held is the machine's answer, not ours. It is a
 * line the guest samples, it lives in a device register, and it travels in
 * the state -- so a window keeping its own copy would contradict a restored
 * machine, showing the key up while the guest still saw it down. That was
 * exactly the symptom: a device put down with the key held came back with
 * the button dark and the guest none the wiser.
 */
static void option_set(mrc_runtime *m, bool down)
{
    if (!m->ops->option || !m->ops->option_held) return;
    if (m->ops->option_held(m->board) == down) return;
    m->ops->option(m->board, down);
    mrc_ui_set_lit(g_ui, MRC_UI_ICON_OPTION, down);
    fprintf(stderr, "[option] %s\n", down ? "held" : "released");
}

static void set_pen_from_mouse(mrc_runtime *m, int wx, int wy, bool down)
{
    if (!down) {
        m->ops->pen(m->board, false, 0, 0);
        if (getenv("MRC_TOUCH_TRACE"))
            fprintf(stderr, "[gui-touch] release\n");
        return;
    }

    unsigned px, py;
    if (!panel_from_window(wx, wy, &px, &py)) {
        if (getenv("MRC_TOUCH_TRACE"))
            fprintf(stderr, "[gui-touch] win(%d,%d) -> outside panel, ignored\n",
                    wx, wy);
        return;
    }
    g_last_px = (int)px;
    g_last_py = (int)py;

    m->ops->pen(m->board, true, px, py);
    if (getenv("MRC_TOUCH_TRACE"))
        fprintf(stderr, "[gui-touch] win(%d,%d) -> px(%u,%u)\n",
                wx, wy, px, py);
}

static int datarover_present(mrc_runtime *m, uint64_t insns_total,
                             struct mrc_shell *sh)
{
    /*
     * The window opens at twice the panel and is resizable from there. A
     * --scale flag is not worth keeping when dragging a corner or maximising
     * does the same job and the pointer mapping follows the window either
     * way.
     */
    /*
     * The window is the shell's. It was open before this machine started and
     * stays open after it stops, which is what lets a device switch reshape
     * what is on screen rather than replace it.
     */
    if (!mrc_shell_attach(sh, "mcap — DataRover 840",
                          PANEL_SCREEN_W, PANEL_SCREEN_H))
        return 1;
    if (mrc_gui_audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        fprintf(stderr, "GUI: no audio: %s\n", SDL_GetError());
    mrc_sdl_display *display = mrc_shell_display(sh);
    SDL_Window *win = display->window;
    SDL_Renderer *ren = display->renderer;
    mrc_lcd *lcd = mrc_shell_lcd(sh);
    g_ui = mrc_shell_ui(sh);
    g_win = win;
    g_ren = ren;
    /*
     * Which buttons this machine has. Only the ones that already do something
     * are offered, and they are set on every attach because the next machine
     * in this window may answer differently.
     */
    mrc_ui_set_buttons(g_ui,
                       (m->ops->install ? 1u << MRC_UI_ICON_INSTALL : 0) |
                       1u << MRC_UI_ICON_SETTINGS |
                       1u << MRC_UI_ICON_ROMS |
                       1u << MRC_UI_ICON_ROTATE |
                       1u << MRC_UI_ICON_DISPLAY |
                       (mrc_sdl_can_save_state(m)
                        ? 1u << MRC_UI_ICON_STORAGE : 0) |
                       (m->ops->option ? 1u << MRC_UI_ICON_OPTION : 0));
    /*
     * And show what the machine came back as. A device put down with the
     * option key held restores with the line still asserted, so the button
     * has to start lit or it would be telling the opposite of the truth.
     */
    if (m->ops->option_held)
        mrc_ui_set_lit(g_ui, MRC_UI_ICON_OPTION,
                       m->ops->option_held(m->board));
    /*
     * With the LCD filter on we render each panel pixel as a cell, so the
     * texture is that many times larger. The renderer's logical size stays
     * the panel's, which is what keeps the aspect right in a resized or
     * fullscreen window and lets SDL map mouse coordinates back for us.
     */
    /*
     * The LCD cell size follows the window rather than being fixed.
     *
     * Rendering each panel pixel as a fixed 3x3 cell and then letting the
     * result be scaled to the window destroyed the very structure the filter
     * exists to draw: a 1180x700 window gives a 1050x700 destination, so the
     * 1440x960 texture was downscaled by 0.73 and nearest sampling dropped
     * about every fourth subpixel row. Against a grid of period 3 that beats
     * into visible banding, and at some sizes the dots disappear altogether.
     *
     * Choosing the cell from the destination instead means the blit is at or
     * very near 1:1, so the grid stays even at any window size and the dots
     * simply get chunkier as the window grows, which is what a real panel
     * does.
     */
    SDL_PumpEvents();
    note_pointer_scale(0, 0);
    const char *ov = getenv("MRC_POINTER_SCALE");
    if (ov) {
        float v = (float)atof(ov);
        if (v > 0.1f && v < 8.0f) {
            g_ptr_scale = v;
            fprintf(stderr, "GUI: pointer scale forced to %.3g\n", v);
        }
    }
    /*
     * Fill the window by default, keeping the aspect. Whole-pixel scaling is
     * sharper but drops to 1x the moment a window is a little too small for
     * 2x, which leaves the panel marooned in a wide border and looks broken
     * rather than crisp. --integer asks for it explicitly.
     */


    static audio_out audio;
    memset(&audio, 0, sizeof(audio));
    audio.machine = m;
    if (mrc_gui_audio)
        m->ops->audio_sink(m->board, audio_sample, &audio);

    static uint8_t gray[PANEL_SCREEN_W * PANEL_SCREEN_H];

    /*
     * Instructions per displayed frame. At the board's 36.864 MHz this is
     * real time — assuming one instruction per cycle, which we do model, and
     * which is wrong in ways docs/ACCURACY.md describes.
     */
    uint64_t per_frame = DR840_CPU_HZ / FRAME_HZ;
    uint64_t ran = 0;
    /*
     * Emulated time is paced against the clock, not against frames drawn.
     *
     * Running a fixed number of instructions per rendered frame makes the
     * emulated machine's clock a function of the host's frame rate, and any
     * frame that overruns its vsync slot loses that time for good -- there is
     * no catching up. It is invisible on screen and audible immediately: the
     * sound device consumes samples in real time, so a few percent short,
     * sustained, drains the queue and leaves gaps. Measured at 0.94-0.97x,
     * against 1.17x for the same work headless.
     *
     * So each pass asks how many instructions real time has earned and runs
     * that many. max_catchup bounds the debt: after a drag, a stall or a
     * suspend, the machine gives up what it missed rather than sprinting
     * through it, which would burst the sound and flood the input queue.
     */
    Uint64 perf_hz = SDL_GetPerformanceFrequency();
    Uint64 wall_ref = SDL_GetPerformanceCounter();
    uint64_t emulated = 0;
    const uint64_t max_catchup = per_frame * 4;
    Uint64 run_start = wall_ref;
    bool running = true;
    bool mouse_down = false;
    bool release_pending = false;
    uint64_t pen_press_insn = 0;
    /*
     * Experiment for finger input: a touchscreen reports its first motion
     * almost together with the press and in coarse steps, where a stylus
     * on the real digitizer is still for the OS's pen-down debounce. With
     * MRC_PEN_SETTLE_MS set, motion within that many milliseconds of
     * machine time after a press is held back and applied afterwards.
     */
    uint64_t pen_settle_slots = 0;
    bool pen_pending = false;
    int pen_pending_x = 0, pen_pending_y = 0;
    if (getenv("MRC_PEN_SETTLE_MS")) {
        pen_settle_slots = (uint64_t)strtoul(getenv("MRC_PEN_SETTLE_MS"), NULL, 0) *
                           (DR840_CPU_HZ / 1000u);
        fprintf(stderr, "GUI: pen settle %llu slots after each press\n",
                (unsigned long long)pen_settle_slots);
    }
    /*
     * The right button drives the board's Option input; see
     * docs/HARDWARE.md. Hold it and touch, which is what the ROM's own help
     * describes: "hold it down while you touch objects on the screen".
     *
     * It carries the key and nothing else. An earlier attempt also put the
     * pen down after a settle delay so that one right-click was a whole
     * option-tap, and that guessed at a lead time measured from exactly one
     * scripted tap. Holding right and clicking left needs no such guess.
     *
     * That attempt also latched, and this is the part worth being careful
     * about: it asserted the line in one place and lifted it in exactly one
     * other, the button's own release. Anything that swallowed that event --
     * a window manager taking the right-click, the pointer leaving, focus
     * moving away -- left the key held for the life of the process, and
     * every later left-click became an option click. So option_set() is the
     * only way the line moves, it is called from every path that could end
     * the press, and it is called at startup to put the pin in a known
     * state rather than trusting a static's initial value.
     */


    m->ops->option(m->board, false);

    {
        int ww, wh, ow, oh;
        SDL_Rect vp;
        float sx, sy;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_RenderGetViewport(ren, &vp);
        SDL_RenderGetScale(ren, &sx, &sy);
        int pw = ww, ph = wh;
#if SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
#endif
        fprintf(stderr, "GUI: driver %s, window %dx%d, in pixels %dx%d, "
                "renderer output %dx%d, viewport %d,%d %dx%d, scale %.3fx%.3f\n",
                SDL_GetCurrentVideoDriver(), ww, wh, pw, ph,
                ow, oh, vp.x, vp.y, vp.w, vp.h, sx, sy);
    }
    fprintf(stderr, "GUI: %ux%u panel. Click to touch, type for keyboard/console, "
            "hold the right button for the option key, "
            "F2 saves the screen, F3 the machine, F4 power, F11 fullscreen, "
            "Ctrl-Q or the close button stops.\n",
            PANEL_SCREEN_W, PANEL_SCREEN_H);

    /* A device chosen from the list ends this machine's turn: the window
     * closes the way it would on a quit, the state is saved on the way out,
     * and the launcher starts the other one. */
    while (running && !mrc_state_device_requested() &&
           (insns_total == 0 || ran < insns_total)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (mrc_card_panel_event(g_ui, m, &e)) continue;
            /* The rail sees everything first. A press that lands on it is
             * not a press on the guest's screen. */
            if (mrc_ui_event(g_ui, &e)) {
                rail_action(m, mrc_ui_take_action(g_ui));
                int row = mrc_ui_take_row(g_ui);
                if (row == MRC_UI_ROW_DISMISS) {
                    if (mrc_card_panel_showing()) mrc_card_panel_close(g_ui);
                    if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
                    if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
                    if (mrc_devices_panel_showing()) mrc_devices_panel_close(g_ui);
                    mrc_ui_close_panel(g_ui);
                    continue;
                }
                if (row == MRC_UI_ROW_BACK) {
                    if (mrc_card_panel_showing()) {
                        if (!mrc_card_panel_back(g_ui, m)) {
                            mrc_card_panel_close(g_ui);
                            mrc_settings_panel_open(g_ui, m);
                        }
                    } else if (mrc_install_panel_showing()) {
                        if (!mrc_install_panel_back(g_ui, m)) mrc_install_panel_close(g_ui);
                    } else if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
                    else if (mrc_devices_panel_showing()) {
                        if (!mrc_devices_panel_back(g_ui)) mrc_devices_panel_close(g_ui);
                    }
                    else if (mrc_ui_panel_open(g_ui)) mrc_ui_close_panel(g_ui);
                    continue;
                }
                if (!mrc_card_panel_row(g_ui, m, row) &&
                    !mrc_devices_panel_row(g_ui, row) &&
                    !mrc_settings_panel_row(g_ui, m, row) &&
                    !mrc_install_panel_row(g_ui, m, row))
                    mrc_display_row(row);
                continue;
            }
            if (mrc_sdl_display_event(display, &e, m, &wall_ref)) {
                if (display->backgrounded) {
                    if (audio.dev) SDL_ClearQueuedAudio(audio.dev);
                    audio.n = 0;
                    release_pending = false;
                                    mouse_down = false;
                }
                continue;
            }
            switch (e.type) {
            case SDL_QUIT:
                            running = false;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    option_set(m, true);
                    break;
                }
                if (e.button.button == SDL_BUTTON_LEFT &&
                    in_panel(e.button.x, e.button.y)) {
                    mouse_down = true;
                    release_pending = false;
                    pen_pending = false;
                    pen_press_insn = m->ops->slots(m->board);
                    set_pen_from_mouse(m, e.button.x, e.button.y, true);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                                    break;
                }
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mouse_down = false;
                    release_pending = true;
                }
                break;
            case SDL_MOUSEMOTION:
                /*
                 * Keep the pointer-space estimate current here rather than
                 * only at the first click. Moving onto a control generates
                 * motion events all the way, so the factor is settled before
                 * the button goes down, and it re-settles after a resize or
                 * a move to a differently scaled monitor.
                 */
                note_pointer_scale(e.motion.x, e.motion.y);
                if (mouse_down) {
                    if (pen_settle_slots &&
                        m->ops->slots(m->board) - pen_press_insn < pen_settle_slots) {
                        pen_pending = true;
                        pen_pending_x = e.motion.x;
                        pen_pending_y = e.motion.y;
                    } else
                        set_pen_from_mouse(m, e.motion.x, e.motion.y, true);
                }
                break;
            case SDL_WINDOWEVENT:
                /*
                 * Dragging the window to a monitor with different scaling
                 * changes what mouse events are measured in, so the ratio
                 * has to be measured again rather than trusted for the life
                 * of the process.
                 *
                 * Losing focus or the pointer also ends any press in
                 * progress: the button may well be released somewhere we
                 * will never hear about.
                 */
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                    e.window.event == SDL_WINDOWEVENT_LEAVE ||
                    e.window.event == SDL_WINDOWEVENT_HIDDEN ||
                    e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                                    if (e.window.event != SDL_WINDOWEVENT_LEAVE && m->ops->keyboard_release)
                        m->ops->keyboard_release(m->board);
                    if (m->ops->power_button) m->ops->power_button(m->board, false);
                }
                break;

            case SDL_KEYUP:
                if (m->ops->keyboard_key)
                    m->ops->keyboard_key(m->board,keyboard_usage(&e.key),false,false);
                if (e.key.keysym.sym == SDLK_F4 && m->ops->power_button)
                    m->ops->power_button(m->board, false);
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_AC_BACK) { running = false; break; }
                if (e.key.keysym.sym == SDLK_F4 && m->ops->power_button) {
                    m->ops->power_button(m->board, true);
                    break;
                }
                if (e.key.keysym.sym == SDLK_q &&
                    (e.key.keysym.mod & KMOD_CTRL)) {
                    running = false;
                    break;
                }
                if (e.key.keysym.sym == SDLK_F11 ||
                    (e.key.keysym.sym == SDLK_RETURN &&
                     (e.key.keysym.mod & KMOD_ALT))) {
                    Uint32 fs = SDL_GetWindowFlags(win) &
                                SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 :
                                            SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                if (e.key.keysym.sym == SDLK_F2 ||
                    e.key.keysym.sym == SDLK_F3) {
                    snapshot_key(m, e.key.keysym.sym);
                    break;
                }
                if (!m->ops->keyboard_key ||
                    !m->ops->keyboard_key(m->board,keyboard_usage(&e.key),true,e.key.repeat!=0))
                    feed_key(m, &e);
                break;
            default:
                break;
            }
        }

        if (display->backgrounded) { SDL_Delay(20); continue; }

        /* Test hook: drive the real GUI pen path without SDL input. */
        {
            static long frame = -1, tx = -1, ty = -1, hold = 60, at = 30;
            if (frame < 0) {
                const char *e = getenv("MRC_GUI_AUTOTAP");
                frame = 0;
                if (e) { hold = 60; at = 30; sscanf(e, "%ld,%ld,%ld,%ld", &tx, &ty, &hold, &at); }
            }
            if (tx >= 0) {
                if (frame == at) {
                    fprintf(stderr, "[autotap] press\n");
                    /*
                     * Ask the renderer where this panel pixel lands in the
                     * window. Multiplying by the scale factor was only right
                     * when the window was exactly panel*scale: as soon as it
                     * is resized there is a letterbox, and the synthetic
                     * click landed somewhere else entirely.
                     */
                    {
                        SDL_Rect dst;
                        panel_rect(&dst);
                        int wx, wy;
                        mrc_sdl_panel_to_output(&dst, tx, ty, PANEL_SCREEN_W,
                                                PANEL_SCREEN_H, &wx, &wy);
                        int ww=0, wh=0, rw=0, rh=0;
                        SDL_GetWindowSize(g_win, &ww, &wh);
                        SDL_GetRendererOutputSize(g_ren, &rw, &rh);
                        if (rw > 0 && rh > 0)
                            set_pen_from_mouse(m, (int)(wx * (double)ww / rw / g_ptr_scale),
                                               (int)(wy * (double)wh / rh / g_ptr_scale), true);
                    }
                    if (hold == 0) {
                        fprintf(stderr, "[autotap] release (same batch)\n");
                        set_pen_from_mouse(m, 0, 0, false);
                    }
                } else if (hold > 0 && frame == at + hold) {
                    fprintf(stderr, "[autotap] release\n");
                    set_pen_from_mouse(m, 0, 0, false);
                } else if (frame == at + hold + 270) {
                    running = false;
                }
            }
            frame++;
        }

        {
            Uint64 now = SDL_GetPerformanceCounter();
            uint64_t owed = mrc_sdl_owed_slots(now, wall_ref, perf_hz, m->nominal_slots_hz);
            uint64_t chunk = owed > emulated ? owed - emulated : 0;

            if (chunk > max_catchup) {
                chunk = max_catchup;
                wall_ref = now;
                emulated = chunk;
            } else {
                emulated += chunk;
            }

            /*
             * With vsync the present call does the waiting. Without it --
             * dummy driver, a compositor ignoring it, vsync off -- nothing
             * does, and the loop would spin through renders that emulate no
             * instructions at all. Give the clock a moment to earn some.
             */
            if (chunk == 0) {
                SDL_Delay(1);
            } else {
                m->ops->run_slots(m->board, chunk);
                ran += chunk;
            }
        }

        if (pen_pending && mouse_down &&
            m->ops->slots(m->board) - pen_press_insn >= pen_settle_slots) {
            pen_pending = false;
            set_pen_from_mouse(m, pen_pending_x, pen_pending_y, true);
        }
        if (release_pending &&
            m->ops->slots(m->board) - pen_press_insn >= PEN_MIN_HOLD_INSNS) {
            release_pending = false;
            set_pen_from_mouse(m, 0, 0, false);
        }

        /*
         * One frame, through the shared panel look: the cell size, the
         * texture it needs, and the conversion. All three used to live here
         * and are now in lcd.c, because the 68k machines want them too and
         * the PIC-2000 is the one with the green backlight.
         */
        unsigned w = 0, h = 0;
        mrc_lcd_frame view;
        {
            SDL_Rect dst;
            panel_rect(&dst);
            bool have = m->ops->frame(m->board, gray, &w, &h) &&
                        w == PANEL_SCREEN_W && h == PANEL_SCREEN_H;
            unsigned drawn = (unsigned)(mrc_sdl_sideways() ? dst.h : dst.w);
            if (!mrc_lcd_render(lcd, display, have ? gray : NULL,
                                PANEL_SCREEN_W, PANEL_SCREEN_H,
                                m->frame_format, drawn, &view)) {
                fprintf(stderr, "GUI: cannot allocate the panel texture\n");
                running = false;
                break;
            }
        }
        const unsigned cell = view.cell, tw = view.width;
        uint32_t *argb = view.pixels;

        /*
         * Mark where the pen was last put down. This separates two stages
         * that both have to be right for a tap to land: the window point ->
         * panel pixel mapping, which this shows, and the panel pixel -> raw
         * ADC conversion that the guest then reads through its own
         * calibration. If the cross sits under the cursor but the OS reacts
         * somewhere else, the fault is in the second stage, not this one.
         */
        if (mrc_gui_touch_debug && g_last_px >= 0) {
            for (int d = -6; d <= 6; d++) {
                int cx = g_last_px + d, cy = g_last_py;
                for (unsigned sy = 0; sy < cell; sy++)
                    for (unsigned sx = 0; sx < cell; sx++) {
                        if (cx >= 0 && cx < (int)PANEL_SCREEN_W)
                            argb[((cy * cell + sy) * tw) + cx * cell + sx] = 0xFFFF0000u;
                        int ex = g_last_px, ey = g_last_py + d;
                        if (ey >= 0 && ey < (int)PANEL_SCREEN_H)
                            argb[((ey * cell + sy) * tw) + ex * cell + sx] = 0xFFFF0000u;
                    }
            }
        }

        /* Save what is actually on the glass, filter and all, so the panel
         * emulation can be looked at without a screen recorder. */
        {
            static int shot_done = 0;
            const char *shot = getenv("MRC_LCD_SHOT");
            static unsigned shot_wait = 0;
            if (shot && !shot_done && ++shot_wait > 30 &&
                (!mrc_gui_touch_debug || g_last_px >= 0)) {
                FILE *pf = fopen(shot, "wb");
                if (pf) {
                    fprintf(pf, "P6\n%u %u\n255\n", tw, PANEL_SCREEN_H * cell);
                    for (unsigned i = 0; i < tw * PANEL_SCREEN_H * cell; i++) {
                        uint32_t v = argb[i];
                        fputc((v >> 16) & 0xFF, pf);
                        fputc((v >> 8) & 0xFF, pf);
                        fputc(v & 0xFF, pf);
                    }
                    fclose(pf);
                    fprintf(stderr, "GUI: panel shot -> %s\n", shot);
                }
                shot_done = 1;
            }
        }

        mrc_install_panel_tick(g_ui, m);
        mrc_card_panel_tick(g_ui, m);
        mrc_devices_panel_tick(g_ui);
        if (!mrc_sdl_display_present(display, PANEL_SCREEN_W,
                                     PANEL_SCREEN_H, mrc_gui_integer)) {
            fprintf(stderr, "SDL present: %s\n", SDL_GetError());
            running = false;
        }
    }

    {
        double wall = (double)(SDL_GetPerformanceCounter() - run_start) /
                      (double)perf_hz;
        fprintf(stderr, "pacing: %.2fs emulated in %.2fs wall (%.3fx)\n",
                (double)ran / (double)DR840_CPU_HZ, wall,
                wall > 0 ? (double)ran / (double)DR840_CPU_HZ / wall : 0.0);
    }

    if (m->ops->keyboard_release) m->ops->keyboard_release(m->board);
    if (mrc_gui_audio)
        m->ops->audio_sink(m->board, NULL, NULL);
    if (audio.dev) {
        fprintf(stderr,
                "audio: %llu batches queued, %llu dropped (%.1f%%), "
                "%llu of %llu polls found the queue empty\n",
                (unsigned long long)audio.queued_batches,
                (unsigned long long)audio.dropped_batches,
                audio.polls ? 100.0 * (double)audio.dropped_batches /
                              (double)audio.polls : 0.0,
                (unsigned long long)audio.starved_polls,
                (unsigned long long)audio.polls);
    SDL_CloseAudioDevice(audio.dev);
    if (mrc_card_panel_showing()) mrc_card_panel_close(g_ui);
    }

    /*
     * The audio device was this machine's and goes with it; the window was
     * not, and the next machine attaches to the one already on screen.
     */
    if (mrc_gui_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_ren = NULL; g_win = NULL; g_ui = NULL;
    return 0;
}

/*
 * Whether a window can actually be opened, not merely whether SDL was linked
 * in. Over ssh, in a container or under a CI runner there is no display, and
 * the difference matters now that a window is the default: the answer decides
 * whether the program opens one or runs headless. Ask SDL rather than reading
 * DISPLAY, which is right on X11 and wrong everywhere else.
 */
bool mrc_gui_available(void)
{
    mrc_sdl_prepare_video();
    if (SDL_WasInit(SDL_INIT_VIDEO))
        return true;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        return false;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return true;
}

#else  /* !MRC_HAVE_SDL */

static int datarover_present(mrc_runtime *m, uint64_t insns_total)
{
    fprintf(stderr, "this build has no GUI: SDL2 was not found at configure "
            "time\n");
    return 1;
}

bool mrc_gui_available(void) { return false; }

#endif

/*
 * Closing does not switch the device off.
 *
 * It used to, and had to: the only thing kept between sessions was the
 * battery-backed RAM, so the ROM needed a power-down to commit its shadow
 * clusters before the process went away. What was saved was a device that
 * had been shut down, and opening it again meant booting from scratch.
 *
 * A state is the whole machine, uncommitted memory included, so there is
 * nothing to flush and nothing to gain -- and a great deal to lose. Powering
 * down first deliberately threw away the session it was about to save, which
 * is why a device picked up again showed a black screen and had to be
 * switched on. Putting it down and picking it up should be one motion.
 */
int mrc_gui_run(machine *board, uint64_t slots, struct mrc_shell *shell)
{
    mrc_runtime view = mrc_datarover_runtime(board);
    return datarover_present(&view, slots, shell);
}
