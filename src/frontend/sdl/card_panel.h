#ifndef MH_SDL_CARD_PANEL_H
#define MH_SDL_CARD_PANEL_H

#include <stdbool.h>
#include "frontend/sdl/ui.h"
#include "runtime/machine.h"

void mh_card_panel_open(mh_ui *, mh_runtime *);
void mh_card_panel_close(mh_ui *);
bool mh_card_panel_showing(void);
bool mh_card_panel_row(mh_ui *, mh_runtime *, int);
/* Returns true when Back only dismissed the card's file picker. */
bool mh_card_panel_back(mh_ui *, mh_runtime *);
bool mh_card_panel_event(mh_ui *, mh_runtime *, const SDL_Event *);
void mh_card_panel_tick(mh_ui *, mh_runtime *);

#endif
