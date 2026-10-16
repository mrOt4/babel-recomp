/* n3ds_host.c -- Nintendo 3DS: start-up and the presenter (RendererFuncs).
 *
 * SDL's N3DS video driver only offers a software framebuffer, which would
 * scale and rotate every frame on the CPU. This writes the picture straight
 * into the top screen's framebuffer instead, pixel for pixel: the SNES frame
 * (256x224) fits the 400x240 screen unscaled and is centred. Larger frames
 * (the host's 512x448 overlays) are reduced by an integer factor.
 *
 * The top framebuffer is 240x400 RGBA8, stored column by column from the
 * bottom of the screen: screen pixel (x, y) lives at [x * 240 + 239 - y].
 * SDL owns gfxInit (RGBA8, double buffered); this only fills and swaps. */
#if defined(__3DS__)
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdl_compat.h"
#include "types.h"
#include "util.h"
#include "snes/msu1.h"

/* libctru gives the main thread 32 KB of stack unless told otherwise; the
 * host keeps several kilobyte-sized buffers on it. */
u32 __stacksize__ = 1024 * 1024;

enum { kScreenW = 400, kScreenH = 240, kMaxW = 1024, kMaxH = 480 };

static uint32_t *s_stage;
static int s_w, s_h;

static void ClearScreen(gfxScreen_t screen) {
  /* Both buffers of the pair: the borders are drawn once, never again. */
  for (int i = 0; i < 2; i++) {
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &fw, &fh);
    memset(fb, 0, (size_t)fw * fh * 4);
    gfxScreenSwapBuffers(screen, false);
    gspWaitForVBlank();
  }
}

static bool N3dsRenderer_Init(SDL_Window *window) {
  (void)window;
  s_stage = (uint32_t *)malloc((size_t)kMaxW * kMaxH * sizeof(uint32_t));
  if (!s_stage) return false;
  ClearScreen(GFX_TOP);
  ClearScreen(GFX_BOTTOM);
  return true;
}

static void N3dsRenderer_Destroy(void) {
  free(s_stage);
  s_stage = NULL;
}

static void N3dsRenderer_GetOutputSize(int *width, int *height) {
  *width = kScreenW;
  *height = kScreenH;
}

static void N3dsRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  s_w = width < kMaxW ? width : kMaxW;
  s_h = height < kMaxH ? height : kMaxH;
  *pixels = (uint8 *)s_stage;
  *pitch = s_w * 4;
}

static void N3dsRenderer_EndDraw(void) {
  int step = 1;
  while (s_w / step > kScreenW || s_h / step > kScreenH) step++;
  const int w = s_w / step, h = s_h / step;
  const int x0 = (kScreenW - w) / 2, y0 = (kScreenH - h) / 2;
  u32 *fb = (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
  for (int x = 0; x < w; x++) {
    const uint32_t *src = s_stage + (size_t)x * step;
    u32 *col = fb + (size_t)(x0 + x) * kScreenH + (kScreenH - 1 - y0);
    for (int y = 0; y < h; y++) {
      /* XRGB8888 -> the framebuffer's RGBA8 (0xRRGGBBAA). */
      col[-y] = (src[(size_t)y * step * s_w] << 8) | 0xFF;
    }
  }
  gfxFlushBuffers();
  gfxScreenSwapBuffers(GFX_TOP, false);
}

static const struct RendererFuncs kN3dsRendererFuncs = {
  N3dsRenderer_Init,
  N3dsRenderer_Destroy,
  N3dsRenderer_GetOutputSize,
  N3dsRenderer_BeginDraw,
  N3dsRenderer_EndDraw,
};

/* Called with the game's folder as the working directory. New 3DS: the
 * 804 MHz clock and the L2 cache (ignored on the old model, which also does
 * without MSU-1). A console has no
 * environment, so env.txt (KEY=VALUE lines), if present, stands in for it:
 * the same diagnostic switches as on a PC. */
void N3ds_Startup(void) {
  osSetSpeedupEnable(true);
  /* The original 3DS has no time to spare for streaming CD audio. */
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  if (!is_new) msu1_set_unavailable();
  FILE *f = fopen("env.txt", "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (line[0] == '#' || !eq) continue;
    *eq = 0;
    setenv(line, eq + 1, 1);
  }
  fclose(f);
}

void N3dsRenderer_Create(struct RendererFuncs *funcs) {
  *funcs = kN3dsRendererFuncs;
}
#endif /* __3DS__ */
