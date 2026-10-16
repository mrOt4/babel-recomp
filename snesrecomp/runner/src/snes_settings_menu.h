#ifndef SNES_SETTINGS_MENU_H
#define SNES_SETTINGS_MENU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-game settings: a full-screen overlay over the frozen game, like the
 * save-state browser (snes_savestate_menu.h) and drawn the same way (a
 * software panel any presenter can show, pad or keyboard).
 *
 * The menu is a list of items registered at start-up: the host registers
 * its own (display, audio, achievements, system), a game adds its own
 * (snes_settings_menu_add from its after_config hook or a mod plugin).
 * Items read and write through callbacks, so the menu holds no settings of
 * its own; an item marked `restart` says its change applies on the next
 * launch.
 *
 * Host wiring, like the save-state browser: open it from a command
 * (snes_settings_menu_open), then while snes_settings_menu_is_open() run a
 * modal loop that calls poll_nav / handle_key and presents
 * snes_settings_menu_overlay_image().
 */

#define SNES_SETTINGS_W 512
#define SNES_SETTINGS_H 448

typedef enum {
  kSnesSetting_Header,   /* section title, not selectable */
  kSnesSetting_Info,     /* a line of text from value_text, not selectable */
  kSnesSetting_Toggle,   /* get/set 0 or 1; A or Left/Right flips */
  kSnesSetting_Choice,   /* get/set 0..count-1 over `choices` */
  kSnesSetting_Range,    /* get/set min..max by step */
  kSnesSetting_Action,   /* A runs action(); `confirm` asks for a second A */
} SnesSettingKind;

typedef struct SnesSettingItem {
  SnesSettingKind kind;
  const char *label;                  /* uppercase reads best (8x8 font) */
  int (*get)(void *ud);
  void (*set)(void *ud, int value);
  /* Optional text for the value (Range, Info); NULL: the number. */
  void (*value_text)(void *ud, int value, char *out, size_t cap);
  const char *const *choices;         /* Choice labels */
  int count;                          /* Choice: number of choices */
  int min, max, step;                 /* Range */
  void (*action)(void *ud);           /* Action */
  void *ud;
  int restart;                        /* applies on the next launch */
  int confirm;                        /* Action: press A twice */
} SnesSettingItem;

/* Items are copied (strings are not: keep them static). */
int snes_settings_menu_add(const SnesSettingItem *item);
void snes_settings_menu_clear(void);
int snes_settings_menu_count(void);

void snes_settings_menu_open(void);
void snes_settings_menu_close(void);
int snes_settings_menu_is_open(void);
/* Set while open when any item changed: the host persists its settings. */
int snes_settings_menu_take_dirty(void);

/* Pad: Up/Down select, Left/Right change, A toggles/runs, B closes. */
void snes_settings_menu_poll_nav(uint32_t inputs, uint32_t ticks_ms);
/* Keyboard (SDL keycodes): arrows, Enter/Space, Escape/Backspace. */
void snes_settings_menu_handle_key(int key, int repeat);

int snes_settings_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
