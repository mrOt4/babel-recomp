/* touch_pad.h -- on-screen SNES pad for touch screens (Android).
 *
 * The pad lives in the bars beside the picture: a d-pad on the left, the face
 * buttons on the right, L/R at the top corners, Select/Start under the
 * clusters and a settings button. It is shown while no physical controller is
 * connected. The host feeds it finger events, ORs TouchPad_Buttons() into the
 * player 1 word, and the presenter draws TouchPad_Image() over the frame. */
#ifndef SNESRECOMP_TOUCH_PAD_H
#define SNESRECOMP_TOUCH_PAD_H

#include <stdbool.h>
#include <stdint.h>

#include "sdl_compat.h"

/* Shown or hidden by the host (e.g. hidden while a gamepad is connected). */
void TouchPad_SetVisible(bool visible);
bool TouchPad_Visible(void);

/* A finger event; true when the pad consumed it. win_w/win_h: window size. */
bool TouchPad_HandleEvent(const SDL_Event *ev, int win_w, int win_h);

/* SNES_PAD_* bits currently held. */
uint32_t TouchPad_Buttons(void);

/* True once per tap of the settings button. */
bool TouchPad_TakeMenuTap(void);

/* The pad's picture for a drawable of the given size: ARGB (straight alpha),
 * w x h, and a serial that changes whenever the pixels do. NULL when hidden. */
const uint32_t *TouchPad_Image(int drawable_w, int drawable_h, int *w, int *h,
                               unsigned *serial);

#endif
