/* touch_pad.c -- on-screen SNES pad for touch screens (see touch_pad.h).
 *
 * Geometry is in "pad space", half the drawable's resolution (the picture is
 * scaled up with filtering): u is a tenth of the shorter side, every control
 * is placed and sized from it, so the pad suits any phone or tablet. */
#include "touch_pad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../snes_overlay_draw.h"

enum { kMaxFingers = 10 };

#if SNESRECOMP_SDL3
#define FINGER_ID(ev) ((ev)->tfinger.fingerID)
#else
#define FINGER_ID(ev) ((ev)->tfinger.fingerId)
#endif

typedef struct { float x, y, r; uint32_t mask; char label; uint32_t col; } Disc;
typedef struct { float x, y, w, h; uint32_t mask; const char *label; } Pill;

static bool s_visible;
static int s_w, s_h;                 /* pad space */
static float s_u, s_dpad_x, s_dpad_y, s_dpad_r, s_menu_x, s_menu_y, s_menu_r;
static Disc s_face[4];
static Pill s_pill[4];
static struct { SDL_FingerID id; uint32_t mask; bool used; } s_finger[kMaxFingers];
static uint32_t s_held;
static bool s_menu_tap;
static uint32_t *s_img;
static unsigned s_serial;
static uint32_t s_drawn_held = ~0u;

void TouchPad_SetVisible(bool visible) {
  if (visible == s_visible) return;
  s_visible = visible;
  if (!visible) {
    memset(s_finger, 0, sizeof(s_finger));
    s_held = 0;
  }
}

bool TouchPad_Visible(void) { return s_visible; }
uint32_t TouchPad_Buttons(void) { return s_visible ? s_held : 0; }

bool TouchPad_TakeMenuTap(void) {
  const bool t = s_menu_tap;
  s_menu_tap = false;
  return t;
}

static void Layout(int w, int h) {
  if (w == s_w && h == s_h) return;
  s_w = w;
  s_h = h;
  const float u = (float)(w < h ? w : h) / 10.0f;
  s_u = u;
  /* Left cluster */
  s_dpad_x = 2.3f * u;
  s_dpad_y = (float)h - 3.3f * u;
  s_dpad_r = 1.9f * u;
  /* Right cluster: the SNES diamond */
  const float fx = (float)w - 2.6f * u, fy = (float)h - 3.3f * u, d = 1.25f * u, r = 0.72f * u;
  s_face[0] = (Disc){fx + d, fy, r, SNES_PAD_A, 'A', SNES_OVL_COL_A};
  s_face[1] = (Disc){fx, fy + d, r, SNES_PAD_B, 'B', SNES_OVL_COL_B};
  s_face[2] = (Disc){fx, fy - d, r, SNES_PAD_X, 'X', SNES_OVL_COL_X};
  s_face[3] = (Disc){fx - d, fy, r, SNES_PAD_Y, 'Y', 0xFFE8D34Au};
  /* Shoulders, Select/Start */
  const float pw = 2.6f * u, ph = 0.9f * u;
  s_pill[0] = (Pill){0.4f * u, 0.4f * u, pw, ph, SNES_PAD_L, "L"};
  s_pill[1] = (Pill){(float)w - 0.4f * u - pw, 0.4f * u, pw, ph, SNES_PAD_R, "R"};
  s_pill[2] = (Pill){s_dpad_x - 0.9f * u, (float)h - 0.9f * u, 1.8f * u, 0.65f * u,
                     SNES_PAD_SELECT, "SEL"};
  s_pill[3] = (Pill){fx - 0.9f * u, (float)h - 0.9f * u, 1.8f * u, 0.65f * u,
                     SNES_PAD_START, "START"};
  /* Settings */
  s_menu_x = 1.0f * u;
  s_menu_y = 2.2f * u;
  s_menu_r = 0.55f * u;
  free(s_img);
  s_img = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
  s_drawn_held = ~0u;
}

/* The buttons under a point of pad space (the d-pad gives up to two). */
static uint32_t HitTest(float x, float y) {
  const float dx = x - s_dpad_x, dy = y - s_dpad_y;
  const float dist = sqrtf(dx * dx + dy * dy);
  if (dist < s_dpad_r * 1.35f) {
    if (dist < s_dpad_r * 0.22f) return 0;            /* dead zone */
    const float a = atan2f(dy, dx);                    /* 0 = right, +pi/2 = down */
    const float sector = 0.39269908f;                  /* pi/8 */
    uint32_t m = 0;
    if (a > -3 * sector && a < 3 * sector) m |= SNES_PAD_RIGHT;
    if (a > 5 * sector || a < -5 * sector) m |= SNES_PAD_LEFT;
    if (a > sector && a < 7 * sector) m |= SNES_PAD_DOWN;
    if (a < -sector && a > -7 * sector) m |= SNES_PAD_UP;
    return m;
  }
  for (int i = 0; i < 4; i++) {
    const float ex = x - s_face[i].x, ey = y - s_face[i].y, rr = s_face[i].r * 1.3f;
    if (ex * ex + ey * ey < rr * rr) return s_face[i].mask;
  }
  for (int i = 0; i < 4; i++) {
    const Pill *p = &s_pill[i];
    const float m = 0.3f * s_u;
    if (x >= p->x - m && x < p->x + p->w + m && y >= p->y - m && y < p->y + p->h + m)
      return p->mask;
  }
  return 0;
}

bool TouchPad_HandleEvent(const SDL_Event *ev, int win_w, int win_h) {
  if (!s_visible || win_w <= 0 || win_h <= 0) return false;
  if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERUP && ev->type != SDL_FINGERMOTION)
    return false;
  Layout(win_w / 2, win_h / 2);
  const float x = ev->tfinger.x * (float)s_w, y = ev->tfinger.y * (float)s_h;
  int slot = -1, free_slot = -1;
  for (int i = 0; i < kMaxFingers; i++) {
    if (s_finger[i].used && s_finger[i].id == FINGER_ID(ev)) slot = i;
    else if (!s_finger[i].used && free_slot < 0) free_slot = i;
  }
  bool consumed = false;
  if (ev->type == SDL_FINGERUP) {
    if (slot >= 0) {
      s_finger[slot].used = false;
      consumed = true;
    }
  } else {
    const uint32_t m = HitTest(x, y);
    if (ev->type == SDL_FINGERDOWN) {
      const float mx = x - s_menu_x, my = y - s_menu_y;
      if (mx * mx + my * my < s_menu_r * s_menu_r * 2.0f) {
        s_menu_tap = true;
        return true;
      }
      if (m && free_slot >= 0) {
        s_finger[free_slot].used = true;
        s_finger[free_slot].id = FINGER_ID(ev);
        s_finger[free_slot].mask = m;
        consumed = true;
      }
    } else if (slot >= 0) {                            /* slide between buttons */
      s_finger[slot].mask = m;
      consumed = true;
    }
  }
  uint32_t held = 0;
  for (int i = 0; i < kMaxFingers; i++)
    if (s_finger[i].used) held |= s_finger[i].mask;
  s_held = held;
  return consumed;
}

static void TextCentered(const char *s, float cx, float cy, int scale, uint32_t col) {
  const int n = snes_ovl_text_len(s);
  snes_ovl_draw_text(s_img, s_w, s_h, (int)cx - n * 4 * scale, (int)cy - 4 * scale, s, col, scale);
}

static void Draw(void) {
  const uint32_t idle = 0x50FFFFFFu, lit = 0xA0FFFFFFu, base = 0x30FFFFFFu;
  memset(s_img, 0, (size_t)s_w * s_h * sizeof(uint32_t));
  const int scale = s_u >= 40 ? 3 : s_u >= 24 ? 2 : 1;
  /* d-pad: a base disc and four arms */
  snes_ovl_fill_disc(s_img, s_w, s_h, (int)s_dpad_x, (int)s_dpad_y, (int)s_dpad_r, base);
  static const struct { float dx, dy; uint32_t m; } kArm[4] = {
    {0, -1, SNES_PAD_UP}, {0, 1, SNES_PAD_DOWN}, {-1, 0, SNES_PAD_LEFT}, {1, 0, SNES_PAD_RIGHT}};
  for (int i = 0; i < 4; i++)
    snes_ovl_fill_disc(s_img, s_w, s_h, (int)(s_dpad_x + kArm[i].dx * s_dpad_r * 0.58f),
                       (int)(s_dpad_y + kArm[i].dy * s_dpad_r * 0.58f), (int)(s_dpad_r * 0.36f),
                       (s_held & kArm[i].m) ? lit : idle);
  for (int i = 0; i < 4; i++) {
    const Disc *b = &s_face[i];
    snes_ovl_fill_disc(s_img, s_w, s_h, (int)b->x, (int)b->y, (int)b->r,
                       (s_held & b->mask) ? lit : idle);
    char t[2] = {b->label, 0};
    TextCentered(t, b->x, b->y, scale, (b->col & 0x00FFFFFFu) | 0xE0000000u);
  }
  for (int i = 0; i < 4; i++) {
    const Pill *p = &s_pill[i];
    snes_ovl_fill_rect(s_img, s_w, s_h, (int)p->x, (int)p->y, (int)p->w, (int)p->h,
                       (s_held & p->mask) ? lit : idle);
    TextCentered(p->label, p->x + p->w / 2, p->y + p->h / 2, scale > 1 ? scale - 1 : 1,
                 0xE0FFFFFFu);
  }
  snes_ovl_fill_disc(s_img, s_w, s_h, (int)s_menu_x, (int)s_menu_y, (int)s_menu_r, idle);
  for (int i = -1; i <= 1; i++)                        /* the "menu" glyph */
    snes_ovl_fill_rect(s_img, s_w, s_h, (int)(s_menu_x - s_menu_r * 0.5f),
                       (int)(s_menu_y + i * s_menu_r * 0.4f) - 1, (int)s_menu_r, 2 + scale / 2,
                       0xE0FFFFFFu);
}

const uint32_t *TouchPad_Image(int drawable_w, int drawable_h, int *w, int *h,
                               unsigned *serial) {
  if (!s_visible || drawable_w <= 0 || drawable_h <= 0) return NULL;
  Layout(drawable_w / 2, drawable_h / 2);
  if (!s_img) return NULL;
  if (s_held != s_drawn_held) {
    Draw();
    s_drawn_held = s_held;
    s_serial++;
  }
  *w = s_w;
  *h = s_h;
  *serial = s_serial;
  return s_img;
}
