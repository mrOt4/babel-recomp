/* launcher_textprompt.c -- see launcher_textprompt.h. Own file: <switch.h>
 * brings typedefs the launcher's other headers must not see. */

#include "launcher_textprompt.h"

#include <string.h>

#if defined(__SWITCH__)
#include <switch.h>

int launcher_text_prompt_available(void) { return 1; }

int launcher_text_prompt(const char* header, const char* initial, int password,
                         char* out, size_t cap) {
    SwkbdConfig kbd;
    if (!out || cap == 0 || R_FAILED(swkbdCreate(&kbd, 0))) return 0;
    swkbdConfigMakePresetDefault(&kbd);
    if (header) swkbdConfigSetHeaderText(&kbd, header);
    if (initial && !password) swkbdConfigSetInitialText(&kbd, initial);
    if (password) swkbdConfigSetPasswordFlag(&kbd, 1);
    swkbdConfigSetStringLenMax(&kbd, (u32)(cap - 1));
    char buf[256] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf) < cap ? sizeof(buf) : cap);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return 0;
    strncpy(out, buf, cap - 1);
    out[cap - 1] = 0;
    memset(buf, 0, sizeof(buf));
    return 1;
}
#else
int launcher_text_prompt_available(void) { return 0; }
int launcher_text_prompt(const char* header, const char* initial, int password,
                         char* out, size_t cap) {
    (void)header; (void)initial; (void)password; (void)out; (void)cap;
    return 0;
}
#endif
