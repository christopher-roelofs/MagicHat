#ifndef MH_GUI68K_H
#define MH_GUI68K_H

#include <stdint.h>

#include "machines/pic2000/machine68k.h"

bool mh_gui68k_available(void);
void mh_gui68k_set_audio(bool enabled);
void mh_gui68k_set_host_battery(bool enabled);
/* Runs the machine in a window the caller already owns, so that moving from
 * one device to another reshapes what is on screen rather than replacing it. */
struct mh_shell;
int  mh_gui68k_run(m68k_machine *m, uint64_t insns_total,
                    struct mh_shell *shell);

#endif
