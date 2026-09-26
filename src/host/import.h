/*
 * import.h — importing files from somewhere this program cannot walk.
 *
 * On a desktop a file picker is enough: the emulator can open any path the
 * person can, so the devices list browses for a ROM and copies it in.
 *
 * Android is not like that. An app sees its own private storage and almost
 * nothing else; the user's Downloads folder is not ours to read, and no
 * amount of walking the filesystem will find the image they just fetched.
 * The one way in is the system's own document picker, which hands back a
 * single file the person deliberately chose -- and that is a Java intent,
 * an activity result, and a copy, none of which can happen inside a run
 * loop drawing frames.
 *
 * So the platform is asked, and answers later. The list carries on drawing
 * while a picker it does not own is on top of it, and collects the path when
 * one arrives.
 */
#ifndef MH_HOST_IMPORT_H
#define MH_HOST_IMPORT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { MH_IMPORT_ROM = 0, MH_IMPORT_PACKAGE = 1, MH_IMPORT_CARD = 2 } mh_import_kind;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether this platform has a picker of its own. False on a desktop, where
 * the emulator's own file list is the better answer and this would only add
 * a dialog in front of it.
 */
bool mh_import_available(void);

/*
 * Ask for a ROM, package, or SRAM card copy. Returns immediately; false if
 * another request is still finishing. The person may cancel at any time.
 */
bool mh_import_request(mh_import_kind kind);

/* True between the request and an answer, so a list can say what it is
 * waiting for rather than looking broken. */
bool mh_import_pending(mh_import_kind kind);

/* Completed request, including cancellation (both buffers empty) or error.
 * Callers own the copied buffers. A result can only be taken by its owner. */
bool mh_import_result(mh_import_kind kind, char *path, size_t path_size,
                       char *error, size_t error_size);
/* Discard a late result when its menu/session was closed. */
void mh_import_cancel(mh_import_kind kind);

#ifdef __cplusplus
}
#endif
#endif /* MH_HOST_IMPORT_H */
