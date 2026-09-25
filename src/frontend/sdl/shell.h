/*
 * shell.h — the window, which outlives the machine inside it.
 *
 * Until now each frontend owned its whole world: it started SDL, opened a
 * window, made a renderer, built the rail, ran, and tore all of it down
 * again. So did the chooser. That is fine while a session is one machine, and
 * wrong the moment devices exist: switching meant destroying one world and
 * building another, which is why choosing a device made the window disappear
 * and a new one appear somewhere else.
 *
 * A window is not part of a machine. It is the thing you are looking at, and
 * the machines come and go inside it. So SDL, the display, the rail and the
 * panel look live here, above every board, and a frontend borrows them for as
 * long as its machine is running.
 *
 * What stays with the machine is what belongs to it: the audio device, whose
 * rate is a property of the board, and the run loop itself.
 */
#ifndef MRC_SDL_SHELL_H
#define MRC_SDL_SHELL_H

#include <stdbool.h>

/*
 * Deliberately no SDL headers here.
 *
 * The launcher owns the window and must name this type, and on Android
 * SDL.h redefines main to SDL_main -- so a command line that pulls SDL in
 * loses its own entry point to the one the Android bootstrap already
 * defines, and the link fails on a duplicate symbol. The three things a
 * shell holds are named by pointer instead, which needs no definitions.
 */
typedef struct mrc_sdl_display mrc_sdl_display;
typedef struct mrc_ui mrc_ui;
typedef struct mrc_lcd mrc_lcd;

typedef struct mrc_shell mrc_shell;

/* What a machine borrows. Valid while the shell is open. */
mrc_sdl_display *mrc_shell_display(mrc_shell *sh);
mrc_ui          *mrc_shell_ui(mrc_shell *sh);
mrc_lcd         *mrc_shell_lcd(mrc_shell *sh);
bool             mrc_shell_is_open(const mrc_shell *sh);

/*
 * Start SDL and open the window. `panel_w`/`panel_h` size the first texture
 * and choose the window's shape; a machine attaching later resizes it.
 *
 * False when there is no window to be had, which a caller should report
 * rather than carry on without: everything this program does happens in it.
 */
/* Returns NULL when there is no window to be had. */
mrc_shell *mrc_shell_open(const char *title, unsigned panel_w, unsigned panel_h);
void mrc_shell_close(mrc_shell *sh);

/*
 * Hand the window to a machine: its name in the title bar, its panel size on
 * the texture. Called once as each board starts, so that switching between a
 * PIC-2000 and a DataRover reshapes what is already on screen instead of
 * replacing it.
 *
 * The remembered frame is dropped, because ghosting a new machine's first
 * frame against the last one of the machine before it would be a picture of
 * neither.
 */
bool mrc_shell_attach(mrc_shell *sh, const char *title,
                      unsigned panel_w, unsigned panel_h);

/*
 * The window this process is using.
 *
 * The launcher opens it and the board CLIs find it here, because they are
 * reached through a fixed main(argc, argv) and there is nowhere to hand it to
 * them. NULL when nothing opened one, which is every headless run.
 */
void mrc_shell_set_current(mrc_shell *sh);
mrc_shell *mrc_shell_current(void);

#endif /* MRC_SDL_SHELL_H */
