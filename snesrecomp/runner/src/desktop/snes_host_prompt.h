#ifndef SNES_HOST_PROMPT_H
#define SNES_HOST_PROMPT_H
/* A yes/no question over the frozen game, asked between frames.
 * The game requests it (e.g. from a frame callback); the host shows it
 * before the next frame runs and calls done(1 = yes, 0 = no). Strings are
 * copied; '\n' breaks lines. Pad Up/Down + A, B = no; keyboard arrows,
 * Enter, Escape. */
void snes_host_request_prompt(const char *question, const char *yes, const char *no,
                              void (*done)(int yes));
/* Reset the console (as the Reset hotkey) before the next frame. */
void snes_host_request_reset(void);
#endif
