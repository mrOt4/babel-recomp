#ifndef SNES_HOST_CAPTURE_H
#define SNES_HOST_CAPTURE_H

/*
 * Speculative capture: run the game forward from the current point with
 * inputs the caller supplies, render the last frame, copy its picture, and
 * put EVERYTHING back (guest state, audio, frame counter) -- the run-ahead
 * machinery, used to look at a screen the player is not on. A game uses it
 * to borrow its own art: e.g. open its pause menu off-screen and keep the
 * picture for a companion screen.
 *
 * Call only between frames on the game thread: from the game's
 * after_run_frame hook. Refused (returns 0) during netplay, or when the
 * machine cannot snapshot.
 */

#include <stdint.h>

typedef uint32_t (*SnesSpecInputFn)(int frame, void *ud);
/* Nonzero: stop here; the next frame is the one rendered and captured. */
typedef int (*SnesSpecStopFn)(int frame, void *ud);

/* Render the captured frame at full brightness, without forced blank: the
 * screen a fade is still bringing in, as it will look when it is in. */
#define SNES_SPEC_FULL_BRIGHTNESS 1

/* out: ARGB, *w x *h written (the PPU field, possibly wider than 256 in
 * widescreen); capacity in pixels. Returns 1 on success. */
int snes_host_speculative_capture(SnesSpecInputFn input, SnesSpecStopFn stop, void *ud,
                                  int max_frames, int flags, uint32_t *out, int capacity_px,
                                  int *w, int *h);

/* Called once the captured frame is drawn, before everything is put back:
 * the moment to copy anything else of that screen (VRAM tiles, palettes).
 * Set before a capture; cleared by it. */
typedef void (*SnesSpecGrabFn)(void *ud);
void snes_host_speculative_set_grab(SnesSpecGrabFn grab);

#endif
