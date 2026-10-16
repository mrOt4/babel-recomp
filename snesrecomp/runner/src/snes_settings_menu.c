/* snes_settings_menu.c -- see snes_settings_menu.h. */

#include "snes_settings_menu.h"
#include "snes_overlay_draw.h"
#include "desktop/sdl_compat.h"

#include <stdio.h>
#include <string.h>

#define W SNES_SETTINGS_W
#define H SNES_SETTINGS_H
#define MAX_ITEMS 96
#define ROW_H 22
#define LIST_Y 52
#define LIST_ROWS 16

static SnesSettingItem s_items[MAX_ITEMS];
static int s_count;
static int s_open, s_sel, s_scroll, s_dirty;
static int s_confirm;                    /* item awaiting its second A, or -1 */
static uint32_t s_prev_inputs;
static int s_repeat_dir;
static uint32_t s_repeat_next;
static uint32_t s_panel[W * H];

int snes_settings_menu_add(const SnesSettingItem *item) {
  if (!item || s_count >= MAX_ITEMS) return 0;
  s_items[s_count++] = *item;
  return 1;
}
void snes_settings_menu_clear(void) { s_count = 0; }
int snes_settings_menu_count(void) { return s_count; }

static int selectable(int i) {
  return i >= 0 && i < s_count && s_items[i].kind != kSnesSetting_Header &&
         s_items[i].kind != kSnesSetting_Info;
}

static void select_step(int dir) {
  int i = s_sel;
  for (int n = 0; n < s_count; n++) {
    i += dir;
    if (i < 0 || i >= s_count) return;   /* stop at the ends */
    if (selectable(i)) {
      s_sel = i;
      s_confirm = -1;
      return;
    }
  }
}

void snes_settings_menu_open(void) {
  if (s_open || s_count == 0) return;
  s_open = 1;
  s_confirm = -1;
  s_repeat_dir = 0;
  s_prev_inputs = ~0u;   /* whatever is held at opening is not a press */
  if (!selectable(s_sel)) {
    s_sel = -1;
    select_step(1);
  }
}

void snes_settings_menu_close(void) { s_open = 0; s_confirm = -1; }
int snes_settings_menu_is_open(void) { return s_open; }
int snes_settings_menu_take_dirty(void) {
  int d = s_dirty;
  s_dirty = 0;
  return d;
}

/* ---- changing values ------------------------------------------------------ */
static void change(int dir) {
  if (!selectable(s_sel)) return;
  SnesSettingItem *it = &s_items[s_sel];
  if (!it->get || !it->set) return;
  int v = it->get(it->ud), nv = v;
  switch (it->kind) {
  case kSnesSetting_Toggle:
    nv = !v;
    break;
  case kSnesSetting_Choice:
    if (it->count <= 0) return;
    nv = (v + (dir < 0 ? it->count - 1 : 1)) % it->count;
    break;
  case kSnesSetting_Range: {
    int step = it->step > 0 ? it->step : 1;
    nv = v + (dir < 0 ? -step : step);
    if (nv < it->min) nv = it->min;
    if (nv > it->max) nv = it->max;
    break;
  }
  default:
    return;
  }
  if (nv != v) {
    it->set(it->ud, nv);
    s_dirty = 1;
  }
}

static void activate(void) {
  if (!selectable(s_sel)) return;
  SnesSettingItem *it = &s_items[s_sel];
  if (it->kind == kSnesSetting_Action) {
    if (it->confirm && s_confirm != s_sel) {
      s_confirm = s_sel;
      return;
    }
    s_confirm = -1;
    if (it->action) it->action(it->ud);
    return;
  }
  change(+1);
}

void snes_settings_menu_poll_nav(uint32_t inputs, uint32_t ticks_ms) {
  if (!s_open) {
    s_prev_inputs = inputs;
    return;
  }
  const uint32_t pressed = inputs & ~s_prev_inputs;
  s_prev_inputs = inputs;
  if (pressed & SNES_PAD_B) {
    if (s_confirm >= 0) s_confirm = -1;
    else snes_settings_menu_close();
    return;
  }
  if (pressed & SNES_PAD_A) {
    activate();
    return;
  }
  if (pressed & SNES_PAD_LEFT) change(-1);
  if (pressed & SNES_PAD_RIGHT) change(+1);
  const int dir = (inputs & SNES_PAD_DOWN) ? 1 : (inputs & SNES_PAD_UP) ? -1 : 0;
  if (!dir) {
    s_repeat_dir = 0;
    return;
  }
  if (dir != s_repeat_dir) {
    s_repeat_dir = dir;
    s_repeat_next = ticks_ms + SNES_OVL_REPEAT_DELAY;
    select_step(dir);
  } else if ((int32_t)(ticks_ms - s_repeat_next) >= 0) {
    s_repeat_next = ticks_ms + SNES_OVL_REPEAT_RATE;
    select_step(dir);
  }
}

void snes_settings_menu_handle_key(int key, int repeat) {
  if (!s_open) return;
  if (key == (int)SDLK_UP) { select_step(-1); return; }
  if (key == (int)SDLK_DOWN) { select_step(1); return; }
  if (repeat) return;
  if (key == (int)SDLK_ESCAPE || key == (int)SDLK_BACKSPACE) {
    if (s_confirm >= 0) s_confirm = -1;
    else snes_settings_menu_close();
  } else if (key == (int)SDLK_LEFT) {
    change(-1);
  } else if (key == (int)SDLK_RIGHT) {
    change(+1);
  } else if (key == (int)SDLK_RETURN || key == (int)SDLK_SPACE) {
    activate();
  }
}

/* ---- panel ---------------------------------------------------------------- */
static void value_string(const SnesSettingItem *it, char *out, size_t cap) {
  out[0] = 0;
  int v = it->get ? it->get(it->ud) : 0;
  switch (it->kind) {
  case kSnesSetting_Toggle:
    snprintf(out, cap, "%s", snes_ui_tr(v ? "ON" : "OFF"));
    break;
  case kSnesSetting_Choice:
    snprintf(out, cap, "%s", it->choices && v >= 0 && v < it->count ? snes_ui_tr(it->choices[v]) : "?");
    break;
  case kSnesSetting_Range:
  case kSnesSetting_Info:
    if (it->value_text) it->value_text(it->ud, v, out, cap);
    else if (it->kind == kSnesSetting_Range) snprintf(out, cap, "%d", v);
    break;
  default:
    break;
  }
}

static void rasterize(void) {
  for (int i = 0; i < W * H; i++) s_panel[i] = 0xF00F1118u;
  snes_ovl_fill_rect(s_panel, W, H, 0, 0, W, 40, 0xFF171B25u);
  snes_ovl_draw_text(s_panel, W, H, 20, 12, snes_ui_tr("SETTINGS"), 0xFFFFD24Du, 2);

  /* Keep the selection in view. */
  if (s_sel >= 0) {
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + LIST_ROWS) s_scroll = s_sel - LIST_ROWS + 1;
    /* Show the section header above the first item when scrolled to it. */
    if (s_scroll > 0 && s_items[s_scroll - 1].kind == kSnesSetting_Header && s_sel - (s_scroll - 1) < LIST_ROWS)
      s_scroll--;
  }
  if (s_scroll < 0) s_scroll = 0;

  char val[96];
  for (int row = 0; row < LIST_ROWS; row++) {
    const int i = s_scroll + row;
    if (i >= s_count) break;
    const SnesSettingItem *it = &s_items[i];
    const int y = LIST_Y + row * ROW_H;
    if (it->kind == kSnesSetting_Header) {
      snes_ovl_draw_text(s_panel, W, H, 20, y + 8, snes_ui_tr(it->label), 0xFF9C7CFFu, 1);
      snes_ovl_fill_rect(s_panel, W, H, 20, y + 18, W - 40, 1, 0xFF3A3150u);
      continue;
    }
    const int sel = i == s_sel;
    if (sel) {
      snes_ovl_fill_rect(s_panel, W, H, 14, y + 2, W - 28, ROW_H - 2, 0xFF2B2830u);
      snes_ovl_stroke_rect(s_panel, W, H, 14, y + 2, W - 28, ROW_H - 2, 0xFFFFD24Du);
    }
    const uint32_t fg = it->kind == kSnesSetting_Info ? 0xFF8F97A6u
                        : sel ? 0xFFFFD24Du : 0xFFE2E5EBu;
    snes_ovl_draw_text(s_panel, W, H, 28, y + 9, snes_ui_tr(it->label), fg, 1);
    value_string(it, val, sizeof(val));
    if (it->kind == kSnesSetting_Action) {
      snprintf(val, sizeof(val), "%s", s_confirm == i ? snes_ui_tr("PRESS A AGAIN") : "");
    }
    if (val[0]) {
      char shown[64];
      if (it->kind == kSnesSetting_Choice || it->kind == kSnesSetting_Range ||
          it->kind == kSnesSetting_Toggle)
        snprintf(shown, sizeof(shown), sel ? "< %s >" : "%s", val);
      else
        snprintf(shown, sizeof(shown), "%s", val);
      const int len = snes_ovl_text_len(shown);
      const int x = it->kind == kSnesSetting_Info && strlen(it->label) == 0
                        ? 28 : W - 28 - len * 8;
      snes_ovl_draw_text(s_panel, W, H, x, y + 9, shown,
                         s_confirm == i ? 0xFFFF6B6Bu : sel ? 0xFFFFFFFFu : 0xFFB2B8C2u, 1);
    }
    if (it->restart && sel)
      snes_ovl_draw_text(s_panel, W, H, 28, H - 52, snes_ui_tr("APPLIES ON THE NEXT LAUNCH"), 0xFF8F97A6u, 1);
  }

  /* Hint row. */
  const int hy = H - 30;
  snes_ovl_fill_rect(s_panel, W, H, 0, hy - 8, W, 38, 0xFF171B25u);
  snes_ovl_draw_button(s_panel, W, H, 20, hy - 4, 'A', SNES_OVL_COL_A);
  snes_ovl_draw_text(s_panel, W, H, 44, hy + 1, snes_ui_tr("SELECT"), 0xFFE2E5EBu, 1);
  snes_ovl_draw_text(s_panel, W, H, 120, hy + 1, snes_ui_tr("LEFT/RIGHT CHANGE"), 0xFFE2E5EBu, 1);
  snes_ovl_draw_button(s_panel, W, H, 290, hy - 4, 'B', SNES_OVL_COL_B);
  snes_ovl_draw_text(s_panel, W, H, 314, hy + 1, snes_ui_tr("BACK TO GAME"), 0xFFE2E5EBu, 1);
}

int snes_settings_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
  if (!s_open) {
    if (pixels) *pixels = NULL;
    return 0;
  }
  rasterize();
  if (pixels) *pixels = s_panel;
  if (w) *w = W;
  if (h) *h = H;
  return 1;
}
