/*
 * state.h — where a device keeps what it was doing.
 *
 * A saved state is the whole machine, and it is the only thing these devices
 * save now. That replaces an arrangement where retained memory went one way
 * and a full snapshot went another: two formats, two lifetimes, and a save
 * button whose meaning depended on which machine was in the window.
 *
 * The file sits beside the firmware with .state in place of its extension,
 * the same convention the retained store used, so a device's state travels
 * with the device rather than with whatever directory it was launched from.
 *
 * Switching between devices is the case this is shaped for: save the machine
 * that is running, then either restore the next one's state or start it from
 * its ROM. Nothing else needs to happen, which is why there is no longer a
 * power button in the way.
 *
 * The path is set once by whoever opened the machine and read by the window,
 * which has the board and the operation to save it but no idea where. That is
 * the whole of what is shared here: not a callback, because a window that can
 * ask a board to save itself does not need anyone to do it on its behalf.
 */
#ifndef MRC_HOST_STATE_H
#define MRC_HOST_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The state file belonging to a firmware image. False when the result would
 * not fit, or would name the image itself -- a device launched from its own
 * state file would otherwise overwrite it with a machine that never booted.
 */
bool mrc_state_path_for(const char *rom, char *out, size_t cap);

/*
 * The file this session saves to and restores from, or NULL when it has none
 * -- a run that was told not to keep anything, or a board with no support for
 * it. A window offers its save button only when this and the board's own
 * save operation are both present.
 */
void mrc_state_set_path(const char *path);
const char *mrc_state_path(void);

/*
 * Which device is running, when one is. The launcher sets it; the window
 * reads it, so that a list of devices can mark the one you are looking at.
 * NULL when the machine was started from a plain ROM path and is not a
 * device at all, which is still how every test and script runs.
 */
void mrc_state_set_device_id(const char *id);
const char *mrc_state_device_id(void);

/*
 * Asking for a different device.
 *
 * Switching is: put this machine down, and start that one. The window cannot
 * do either -- it does not own the machine and would not know how to build
 * the other one -- so it leaves the name here and stops. Whoever started the
 * machine finds it on the way out, and the launcher, which already decides
 * which board a ROM is, starts the next one.
 *
 * That is why this is a request and not a call: the two devices may not even
 * be the same kind of machine, and the only code that knows how to choose is
 * the code that chose the first time.
 */
void mrc_state_request_device(const char *id);
bool mrc_state_device_requested(void);
/* Stop also ends the frontend loop, but returns to the unloaded device list.
 * Requests replace one another; consume only after machine teardown. */
void mrc_state_request_stop(void);
bool mrc_state_take_stop(void);
/* Takes it, clearing the request. Valid until the next call. */
const char *mrc_state_take_requested_device(void);

#ifdef __cplusplus
}
#endif
#endif /* MRC_HOST_STATE_H */
