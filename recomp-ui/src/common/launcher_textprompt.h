#ifndef LAUNCHER_TEXTPROMPT_H
#define LAUNCHER_TEXTPROMPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console's own on-screen keyboard (Switch: swkbd). Blocks until the
 * player confirms or cancels; returns 1 with `out` filled on confirm.
 * Returns 0 where there is no system keyboard (desktop: use a text field). */
int launcher_text_prompt_available(void);
int launcher_text_prompt(const char* header, const char* initial, int password,
                         char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
