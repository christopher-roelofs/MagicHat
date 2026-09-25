/* SDL front end for the PIC-2000. Hardware remains in machine68k.cpp; this
 * file only presents the framebuffer and translates host pointer events. */
#include "frontend/sdl/gui68k.h"

#include <stdio.h>
#include <stdlib.h>
static bool audio_enabled = true;
static bool host_battery_enabled = true;
void mrc_gui68k_set_host_battery(bool enabled) { host_battery_enabled = enabled; }
void mrc_gui68k_set_audio(bool enabled) { audio_enabled = enabled; }

#ifdef MRC_HAVE_SDL
#include <SDL2/SDL.h>
#include "frontend/sdl/presentation.h"
#include "frontend/sdl/display.h"
#include "frontend/sdl/shell.h"
#include "frontend/sdl/startup.h"
#include "frontend/sdl/pen_queue.h"
#include "host/power.h"
#include "host/state.h"
#include "frontend/sdl/lcd.h"
#include "frontend/sdl/display_panel.h"
#include "frontend/sdl/devices_panel.h"
#include "frontend/sdl/settings_panel.h"
#include "frontend/sdl/install_panel.h"
#include "frontend/sdl/card_panel.h"
#include "host/state.h"

#define FRAME_HZ 60u
#define PEN_HOLD_INSNS 1000000ull

/* Some Android/Bluetooth SDL backends preserve the symbolic Caps Lock key
 * while reporting an unknown or inconsistent scancode. */
static unsigned keyboard_usage(const SDL_KeyboardEvent *key)
{
    return key->keysym.sym == SDLK_CAPSLOCK ? SDL_SCANCODE_CAPSLOCK :
           (unsigned)key->keysym.scancode;
}

static SDL_Renderer *renderer;
static SDL_Window *window;
/* The control rail, file-scope beside the window and renderer it belongs
 * with, because the panel geometry is worked out before the display
 * structure is in scope. */
static mrc_ui *g_ui;
/* The hardware option key is a level the guest samples, so the interface
 * holds it rather than pressing it: on a touchscreen there is no way to
 * hold a modifier and tap at the same time with one finger. */
typedef struct {
    SDL_AudioDeviceID device;
    int16_t samples[512];
    unsigned count;
} pic_audio;
static void audio_sample(void *ctx, int16_t sample)
{
    pic_audio *a = ctx;
    a->samples[a->count++] = sample;
    if (a->count == 512) {
        /* Bound host latency if rendering falls behind. Device DMA always
         * runs, even if the host cannot accept a batch. */
        if (SDL_GetQueuedAudioSize(a->device) < 44100 * 2 / 4)
            SDL_QueueAudio(a->device, a->samples, sizeof(a->samples));
        a->count = 0;
    }
}

/*
 * What a rail button means. The frontend answers rather than the rail,
 * because the two machines answer differently and the rail knows about
 * neither of them.
 */
static void mrc_gui_action(mrc_runtime *m, int action, mrc_sdl_display *d)
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
        /*
         * Whether it is held is the machine's answer, not ours. The line
         * lives in a device register and travels in the state, so a window
         * keeping its own copy would contradict a restored machine -- the
         * button dark while the guest still saw the key down, which is what
         * a device put down with it held came back as.
         */
        if (m->ops->option && m->ops->option_held) {
            bool now = !m->ops->option_held(m->board);
            m->ops->option(m->board, now);
            mrc_ui_set_lit(g_ui, MRC_UI_ICON_OPTION, now);
        }
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
    (void)d;
}

static void panel_rect(SDL_Rect *dst)
{
    mrc_sdl_panel_rect(renderer, PIC2000_SCREEN_W, PIC2000_SCREEN_H,
                       mrc_gui_integer,
                       mrc_ui_inset_left(g_ui), mrc_ui_inset_right(g_ui), dst);
}

static bool panel_point(int ex, int ey, unsigned *px, unsigned *py)
{
    SDL_Rect dst;
    panel_rect(&dst);
    int output_w = 0, output_h = 0, window_w = 0, window_h = 0;
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    SDL_GetWindowSize(window, &window_w, &window_h);
    if (window_w <= 0 || window_h <= 0)
        return false;
    const int x = (int)((long long)ex * output_w / window_w);
    const int y = (int)((long long)ey * output_h / window_h);
    return mrc_sdl_panel_point(&dst, x, y, PIC2000_SCREEN_W,
                               PIC2000_SCREEN_H, px, py);
}


static void save_screen(const uint8_t *pixels, const char *machine_id)
{
    static unsigned sequence;
    char path[64];
    snprintf(path, sizeof(path), "%s-%04u.pgm", machine_id, sequence++);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%u %u\n255\n", PIC2000_SCREEN_W, PIC2000_SCREEN_H);
    for (unsigned i = 0; i < PIC2000_SCREEN_W * PIC2000_SCREEN_H; i++)
        fputc(255 - pixels[i] * 85, f);
    fclose(f);
    fprintf(stderr, "[68k-gui] screen -> %s\n", path);
}

bool mrc_gui68k_available(void) { return true; }

static int pic2000_present(mrc_runtime *m, uint64_t insns_total,
                           struct mrc_shell *sh)
{
    /*
     * The window is the shell's, not ours. It was here before this machine
     * started and will be here after it stops, which is what makes switching
     * devices reshape what is on screen instead of replacing it.
     */
    const char *title = !strcmp(m->machine_id, "envoy") ?
        "mcap — Motorola Envoy" : !strcmp(m->machine_id, "hix300") ?
        "mcap — Sony HIX-300" : "mcap — Sony PIC-2000";
    if (!mrc_shell_attach(sh, title, PIC2000_SCREEN_W, PIC2000_SCREEN_H))
        return 1;
    mrc_sdl_display *display = mrc_shell_display(sh);
    window = display->window;
    renderer = display->renderer;
    mrc_lcd *lcd = mrc_shell_lcd(sh);
    g_ui = mrc_shell_ui(sh);
    /*
     * Which buttons this machine has. Only the ones that already do something
     * are offered: a rail of controls that quietly do nothing is worse than a
     * shorter rail. Set on every attach, because the next machine in this
     * window may answer differently.
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

    pic_audio audio = {0};
    if (audio_enabled && m->ops->audio_sink && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {0};
        want.freq = (int)m->ops->audio_rate(m->board);
        want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512;
        audio.device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (audio.device) {
            m->ops->audio_sink(m->board, audio_sample, &audio);
            SDL_PauseAudioDevice(audio.device, 0);
            fprintf(stderr, "%s audio: mono %d Hz\n", m->machine_id, want.freq);
        } else fprintf(stderr, "%s audio unavailable: %s\n", m->machine_id, SDL_GetError());
    }

    static uint8_t pixels[PIC2000_SCREEN_W * PIC2000_SCREEN_H];
    const uint64_t per_frame = PIC2000_CPU_HZ / FRAME_HZ;
    const uint64_t start = m->ops->slots(m->board);
    mrc_pen_queue pen_events = {0};
    bool mouse_down = false, running = true;
    Uint32 battery_poll_at = SDL_GetTicks();
    bool battery_reported = false;
    Uint64 wall_ref = SDL_GetPerformanceCounter();
    const Uint64 perf_hz = SDL_GetPerformanceFrequency();

    fprintf(stderr, "68k GUI: click to touch; F2 saves the screen; F4 power; F11 "
                    "toggles fullscreen; Ctrl-Q exits\n");
    /* A device chosen from the list ends this machine's turn: the window
     * closes the way it would on a quit, the state is saved on the way out,
     * and the launcher starts the other one. */
    while (running && !mrc_state_device_requested() &&
           !m->ops->stopped(m->board) &&
           (!insns_total || m->ops->slots(m->board) - start < insns_total)) {
        Uint32 now_ms = SDL_GetTicks();
        if (host_battery_enabled && (m->ops->host_battery || m->ops->host_adapter) &&
            (int32_t)(now_ms - battery_poll_at) >= 0) {
            battery_poll_at = now_ms + 1000;
            mrc_host_power power;
            if (mrc_host_power_read(&power)) {
                if (m->ops->host_adapter) m->ops->host_adapter(m->board, power.on_ac);
                if (power.has_battery && power.percent >= 0 && m->ops->host_battery) {
                    bool mapped = m->ops->host_battery(m->board, power.percent);
                    if (!battery_reported) {
                        fprintf(stderr, "%s: host battery %d%% via %s; %s\n",
                                m->machine_id, power.percent, mrc_host_power_source(),
                                mapped ? "main ADC follows host charge (approximate mapping)" :
                                         "sensor mapping unavailable or explicitly overridden");
                        battery_reported = true;
                    }
                }
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (mrc_card_panel_event(g_ui, m, &event)) continue;
            /* Release events must survive host panel/event consumption. */
            if (event.type == SDL_KEYUP && m->ops->keyboard_key)
                m->ops->keyboard_key(m->board, keyboard_usage(&event.key), false, false);
            if (m->ops->keyboard_release &&
                ((event.type == SDL_WINDOWEVENT &&
                  (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                   event.window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event.window.event == SDL_WINDOWEVENT_MINIMIZED)) ||
                 event.type == SDL_APP_WILLENTERBACKGROUND))
                m->ops->keyboard_release(m->board);
            /* The rail sees everything first. A press that lands on it is
             * not a press on the guest's screen. */
            if (mrc_ui_event(g_ui, &event)) {
                mrc_gui_action(m, mrc_ui_take_action(g_ui), display);
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
            if (mrc_sdl_display_event(display, &event, m, &wall_ref)) {
                if (display->backgrounded) {
                    if (audio.device) SDL_ClearQueuedAudio(audio.device);
                    audio.count = 0;
                    mrc_pen_clear(&pen_events);
                    mouse_down = false;
                }
                continue;
            }
            switch (event.type) {
            case SDL_QUIT: running = false; break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT &&
                    !mouse_down) {
                    unsigned px, py;
                    if (panel_point(event.button.x, event.button.y, &px, &py)) {
                        mouse_down = true;
                        if (!mrc_pen_enqueue(&pen_events, m->ops->slots(m->board),
                                m->nominal_slots_hz, PEN_HOLD_INSNS,
                                event.button.timestamp, true, true, px, py))
                            running = false;
                        if (getenv("MRC_TOUCH_TRACE"))
                            fprintf(stderr, "[68k-gui-touch] down (%u,%u)\n",
                                    px, py);
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT && mouse_down) {
                    mouse_down = false;
                    if (!mrc_pen_enqueue(&pen_events, m->ops->slots(m->board),
                            m->nominal_slots_hz, PEN_HOLD_INSNS,
                            event.common.timestamp, false, false, 0, 0))
                        running = false;
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_down) {
                    unsigned px, py;
                    if (panel_point(event.motion.x, event.motion.y, &px, &py) &&
                        !mrc_pen_enqueue(&pen_events, m->ops->slots(m->board),
                            m->nominal_slots_hz, PEN_HOLD_INSNS,
                            event.motion.timestamp, false, true, px, py))
                        running = false;
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                    m->ops->power_button)
                    m->ops->power_button(m->board, false);
                if (mouse_down &&
                    (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                     event.window.event == SDL_WINDOWEVENT_LEAVE)) {
                    mouse_down = false;
                    if (!mrc_pen_enqueue(&pen_events, m->ops->slots(m->board),
                            m->nominal_slots_hz, PEN_HOLD_INSNS,
                            event.common.timestamp, false, false, 0, 0))
                        running = false;
                }
                break;
            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_F4 && m->ops->power_button)
                    m->ops->power_button(m->board, false);
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_AC_BACK) { running = false; break; }
                if (event.key.keysym.sym == SDLK_q &&
                    (event.key.keysym.mod & KMOD_CTRL)) running = false;
                else if (event.key.keysym.sym == SDLK_F2) save_screen(pixels, m->machine_id);
                else if (event.key.keysym.sym == SDLK_F4 && !event.key.repeat &&
                         m->ops->power_button)
                    m->ops->power_button(m->board, true);
                else if (event.key.keysym.sym == SDLK_F11) {
                    Uint32 flags = SDL_GetWindowFlags(window);
                    SDL_SetWindowFullscreen(window,
                        flags & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 :
                        SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                else if (event.key.keysym.sym != SDLK_F4 && m->ops->keyboard_key)
                    m->ops->keyboard_key(m->board, keyboard_usage(&event.key),
                                         true, event.key.repeat != 0);
                break;
            default: break;
            }
        }

        if (display->backgrounded) { SDL_Delay(20); continue; }

        Uint64 now = SDL_GetPerformanceCounter();
        const uint64_t target = mrc_sdl_owed_slots(now, wall_ref, perf_hz, m->nominal_slots_hz);
        const uint64_t completed = m->ops->slots(m->board) - start;
        uint64_t chunk = target > completed ? target - completed : 0;
        if (chunk > per_frame * 4)
            chunk = per_frame * 4;
        if (insns_total) {
            uint64_t left = insns_total - (m->ops->slots(m->board) - start);
            if (chunk > left) chunk = left;
        }
        /* Split execution at trajectory deadlines so the guest can sample
         * intermediate positions before the contact is released. */
        for (;;) {
            uint64_t current = m->ops->slots(m->board);
            while (pen_events.head && pen_events.head->at <= current) {
                mrc_pen_event delivered;
                if (!mrc_pen_deliver(&pen_events, current,
                        m->nominal_slots_hz, &delivered)) break;
                const mrc_pen_event *e = &delivered;
                m->ops->pen(m->board, e->down, e->x, e->y);
                if (getenv("MRC_TOUCH_TRACE"))
                    fprintf(stderr, "[68k-gui-touch] applied %s (%u,%u) at %llu\n",
                            e->down ? "point" : "release", e->x, e->y,
                            (unsigned long long)current);

            }
            if (!chunk || m->ops->stopped(m->board)) break;
            uint64_t run = chunk;
            if (pen_events.head && run > pen_events.head->at - current)
                run = pen_events.head->at - current;
            uint64_t done = m->ops->run_slots(m->board, run);
            if (!done) break;
            chunk -= done;
        }

        /*
         * One frame, through the shared panel look. The PIC-2000 is the
         * machine with the mint-green backlight, so it has more use for
         * this than the DataRover it was written for.
         */
        unsigned frame_w, frame_h;
        bool have = m->ops->frame(m->board, pixels, &frame_w, &frame_h) &&
                    frame_w == PIC2000_SCREEN_W && frame_h == PIC2000_SCREEN_H;
        {
            SDL_Rect dst;
            panel_rect(&dst);
            mrc_lcd_frame view;
            if (!mrc_lcd_render(lcd, display, have ? pixels : NULL,
                                PIC2000_SCREEN_W, PIC2000_SCREEN_H,
                                m->frame_format,
                                (unsigned)(mrc_sdl_sideways() ? dst.h : dst.w),
                                &view)) {
                fprintf(stderr, "[68k-gui] cannot allocate the panel texture\n");
                running = false;
                break;
            }
        }
        mrc_install_panel_tick(g_ui, m);
        mrc_card_panel_tick(g_ui, m);
        mrc_devices_panel_tick(g_ui);
        if (!mrc_sdl_display_present(display, PIC2000_SCREEN_W,
                                     PIC2000_SCREEN_H, mrc_gui_integer)) {
            fprintf(stderr, "SDL present: %s\n", SDL_GetError());
            running = false;
        }
        SDL_Delay(1);
    }

    mrc_pen_clear(&pen_events);
    if (m->ops->keyboard_release) m->ops->keyboard_release(m->board);
    m->ops->pen(m->board, false, 0, 0);
    if (audio.device) {
        m->ops->audio_sink(m->board, NULL, NULL);
        SDL_CloseAudioDevice(audio.device);
    }
    /*
     * Whatever sheet was open belongs to this machine's turn, not the next
     * one's -- the install panel keeps this machine's own mrc_runtime, and
     * the devices list was showing this machine as the one "in use". Left
     * open, either would still be on screen, answering for a machine that
     * has already stepped aside.
     */
    if (mrc_install_panel_showing()) mrc_install_panel_close(g_ui);
    if (mrc_card_panel_showing()) mrc_card_panel_close(g_ui);
    if (mrc_settings_panel_showing()) mrc_settings_panel_close(g_ui);
    if (mrc_devices_panel_showing()) mrc_devices_panel_close(g_ui);
    if (mrc_ui_panel_open(g_ui)) mrc_ui_close_panel(g_ui);

    /*
     * The audio device was this machine's and goes with it. The window was
     * not: it belongs to the shell, and the next machine attaches to the one
     * already on screen.
     */
    renderer = NULL; window = NULL; g_ui = NULL;
    return 0;
}

#else

bool mrc_gui68k_available(void) { return false; }
static int pic2000_present(mrc_runtime *m, uint64_t insns_total,
                           struct mrc_shell *sh)
{
    (void)m; (void)insns_total; (void)sh;
    fprintf(stderr, "mcap: SDL2 support was not built\n");
    return 1;
}

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
int mrc_gui68k_run(m68k_machine *board, uint64_t slots, struct mrc_shell *shell)
{
    mrc_runtime view = mrc_pic2000_runtime(board);
    return pic2000_present(&view, slots, shell);
}
