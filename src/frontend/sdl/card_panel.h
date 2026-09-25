#ifndef MRC_SDL_CARD_PANEL_H
#define MRC_SDL_CARD_PANEL_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"
#include "runtime/machine.h"

void mrc_card_panel_open(mrc_ui *, mrc_runtime *);
void mrc_card_panel_close(mrc_ui *);
bool mrc_card_panel_showing(void);
bool mrc_card_panel_row(mrc_ui *, mrc_runtime *, int);
/* Returns true when Back only dismissed the card's file picker. */
bool mrc_card_panel_back(mrc_ui *, mrc_runtime *);
bool mrc_card_panel_event(mrc_ui *, mrc_runtime *, const SDL_Event *);
void mrc_card_panel_tick(mrc_ui *, mrc_runtime *);

#endif
