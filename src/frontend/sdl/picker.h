/*
 * picker.h — choosing a file, without a file dialog.
 *
 * SDL has no native picker and the tablet build could not use a desktop one
 * anyway, so this is a list of directory entries drawn as panel rows: the
 * same sheet the display settings use, the same taps, the same code on both
 * platforms. That is a feature rather than a compromise -- a native dialog on
 * the desktop and something else on Android would be two designs to keep in
 * step, which is the reason the rail exists at all.
 *
 * It is deliberately small. There is no typing, no sorting by column and no
 * preview: somewhere in the list is the ROM you want, and the job is to walk
 * to it and tap it. Directories first, then matching files; everything else
 * is hidden, because a list of a hundred names you cannot choose is worse
 * than no list.
 */
#ifndef MH_SDL_PICKER_H
#define MH_SDL_PICKER_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"

typedef struct mh_picker mh_picker;

/*
 * Open on a directory, showing files whose names end with any of the given
 * suffixes -- ".rom", ".image" and so on, compared without case. Passing
 * none shows every file.
 *
 * `start` may be NULL, which begins wherever the program was launched. A
 * path that cannot be read falls back to the home directory and then to the
 * filesystem root, so the picker always opens on something.
 */
mh_picker *mh_picker_open(mh_ui *ui, const char *title, const char *start,
                            const char *const *suffixes, unsigned suffix_count);
void mh_picker_close(mh_picker *p);

/*
 * Hand it a row the rail reported. Returns true when the picker consumed it,
 * which it does for its own rows -- navigating into a directory or choosing a
 * file. Anything else is left for the caller.
 */
bool mh_picker_row(mh_picker *p, int id);

/*
 * The file chosen since this was last asked, or NULL. The string belongs to
 * the picker and is valid until it is asked again or closed, so a caller that
 * wants to keep it copies it.
 */
const char *mh_picker_taken(mh_picker *p);

/* Where it is looking now, for a caller that wants to reopen there later. */
const char *mh_picker_directory(const mh_picker *p);

#endif /* MH_SDL_PICKER_H */
