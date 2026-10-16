/* HD-2D post-processing (see hdfx.h).
 *
 * Everything soft is computed at a quarter of the frame's resolution and
 * brought back with bilinear filtering: a 4x frame (1368x896) costs a 342x224
 * blur, which keeps the pass to a few milliseconds.
 */

#include "hdfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static HdFxParams g_params;
static bool g_init;

void HdFx_Defaults(HdFxParams *p) {
  memset(p, 0, sizeof(*p));
  p->focus_center = 0.55f;
  p->focus_width = 0.18f;
  p->bloom_threshold = 0.75f;
  p->contrast = 1.0f;
  p->saturation = 1.0f;
}

HdFxParams *HdFx_Params(void) {
  if (!g_init) {
    g_init = true;
    HdFx_Defaults(&g_params);
  }
  return &g_params;
}

bool HdFx_Set(HdFxParams *p, const char *name, float v) {
  struct { const char *n; float *f; float lo, hi; } k[] = {
    {"tiltshift", &p->tiltshift, 0, 1},       {"focus_center", &p->focus_center, 0, 1},
    {"focus_width", &p->focus_width, 0, 1},   {"bloom", &p->bloom, 0, 2},
    {"bloom_threshold", &p->bloom_threshold, 0, 1}, {"vignette", &p->vignette, 0, 1},
    {"contrast", &p->contrast, 0.5f, 2},      {"saturation", &p->saturation, 0, 2},
  };
  for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    if (!strcmp(name, k[i].n)) {
      *k[i].f = v < k[i].lo ? k[i].lo : v > k[i].hi ? k[i].hi : v;
      return true;
    }
  return false;
}

bool HdFx_Active(void) {
  const HdFxParams *p = HdFx_Params();
  return p->tiltshift > 0 || p->bloom > 0 || p->vignette > 0 ||
         p->contrast != 1.0f || p->saturation != 1.0f;
}

/* ---- small float images ---- */

typedef struct Img { int w, h; float *rgb; } Img;

static void ImgAlloc(Img *im, int w, int h) {
  if (im->w != w || im->h != h || !im->rgb) {
    free(im->rgb);
    im->w = w, im->h = h;
    im->rgb = calloc((size_t)w * h * 3, sizeof(float));
  }
}

/* Box-average a factor x factor block per small pixel (rows y0..y1). */
typedef struct DsJob { Img *dst; const uint32_t *px; int pitch, f, y0, y1; } DsJob;
static void *DownsampleRows(void *arg);

static void Downsample(Img *dst, const uint32_t *px, int w, int h, int pitch, int f) {
  ImgAlloc(dst, w / f, h / f);
  enum { kThreads = 8 };
  pthread_t th[kThreads];
  DsJob jobs[kThreads];
  for (int i = 0; i < kThreads; i++) {
    jobs[i] = (DsJob){dst, px, pitch, f, dst->h * i / kThreads, dst->h * (i + 1) / kThreads};
    if (i && pthread_create(&th[i], NULL, DownsampleRows, &jobs[i]) != 0)
      DownsampleRows(&jobs[i]), th[i] = 0;
  }
  DownsampleRows(&jobs[0]);
  for (int i = 1; i < kThreads; i++)
    if (th[i])
      pthread_join(th[i], NULL);
}

static void *DownsampleRows(void *arg) {
  const DsJob *j = arg;
  Img *dst = j->dst;
  const uint32_t *px = j->px;
  const int pitch = j->pitch, f = j->f;
  const float inv = 1.0f / (255.0f * f * f);
  for (int y = j->y0; y < j->y1; y++)
    for (int x = 0; x < dst->w; x++) {
      unsigned r = 0, g = 0, b = 0;
      for (int j = 0; j < f; j++) {
        const uint32_t *s = px + (size_t)(y * f + j) * pitch + x * f;
        for (int i = 0; i < f; i++)
          r += (s[i] >> 16) & 0xff, g += (s[i] >> 8) & 0xff, b += s[i] & 0xff;
      }
      float *d = dst->rgb + ((size_t)y * dst->w + x) * 3;
      d[0] = r * inv, d[1] = g * inv, d[2] = b * inv;
    }
  return NULL;
}

/* Separable box blur, radius r, applied `passes` times (~gaussian). */
static void Blur(Img *im, int r, int passes) {
  const int w = im->w, h = im->h;
  float *tmp = malloc(sizeof(float) * 3 * (size_t)(w > h ? w : h));
  for (int p = 0; p < passes; p++) {
    for (int y = 0; y < h; y++) {
      float *row = im->rgb + (size_t)y * w * 3;
      for (int c = 0; c < 3; c++) {
        float acc = 0;
        int n = 0;
        for (int x = -r; x <= r; x++)
          if (x >= 0 && x < w) acc += row[x * 3 + c], n++;
        for (int x = 0; x < w; x++) {
          tmp[x * 3 + c] = acc / n;
          const int out = x - r, in = x + r + 1;
          if (out >= 0) acc -= row[out * 3 + c], n--;
          if (in < w) acc += row[in * 3 + c], n++;
        }
      }
      memcpy(row, tmp, sizeof(float) * 3 * w);
    }
    /* Vertical pass row by row: one running sum per column, so memory is
     * walked in order (a column-wise walk thrashes the cache). */
    const size_t stride = (size_t)w * 3;
    float *acc = calloc(stride, sizeof(float));
    float *out = malloc(sizeof(float) * stride * h);
    for (int y = 0; y <= r && y < h; y++)
      for (size_t i = 0; i < stride; i++)
        acc[i] += im->rgb[(size_t)y * stride + i];
    int n = r + 1 < h ? r + 1 : h;
    for (int y = 0; y < h; y++) {
      const float inv = 1.0f / n;
      float *o = out + (size_t)y * stride;
      for (size_t i = 0; i < stride; i++)
        o[i] = acc[i] * inv;
      const int drop = y - r, add = y + r + 1;
      if (drop >= 0) {
        const float *d = im->rgb + (size_t)drop * stride;
        for (size_t i = 0; i < stride; i++) acc[i] -= d[i];
        n--;
      }
      if (add < h) {
        const float *d = im->rgb + (size_t)add * stride;
        for (size_t i = 0; i < stride; i++) acc[i] += d[i];
        n++;
      }
    }
    memcpy(im->rgb, out, sizeof(float) * stride * h);
    free(out);
    free(acc);
  }
  free(tmp);
}

/* Soft buffer packed to 8-bit ARGB for cheap integer bilinear sampling. */
static void ToPacked(const Img *im, uint32_t **out, int *cap) {
  const int n = im->w * im->h;
  if (*cap < n) {
    free(*out);
    *out = malloc(sizeof(uint32_t) * n);
    *cap = n;
  }
  for (int i = 0; i < n; i++) {
    const float *c = im->rgb + (size_t)i * 3;
    int r = (int)(c[0] * 255.0f), g = (int)(c[1] * 255.0f), b = (int)(c[2] * 255.0f);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    (*out)[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
  }
}

static inline uint32_t Lerp8(uint32_t a, uint32_t b, int t) {   /* t: 0..256 */
  const uint32_t rb = ((((a & 0xff00ff) * (256 - t)) + ((b & 0xff00ff) * t)) >> 8) & 0xff00ff;
  const uint32_t g = ((((a & 0x00ff00) * (256 - t)) + ((b & 0x00ff00) * t)) >> 8) & 0x00ff00;
  return rb | g;
}

typedef struct Job {
  uint32_t *px;
  int w, h, pitch, y0, y1, f;
  const uint32_t *dof, *glow;
  int sw, sh;
  const int16_t *cx0;
  const uint8_t *cax;
  const int32_t *vcol;
} Job;

static int g_bloom256, g_sat256, g_con256, g_vig;
static const HdFxParams *g_p;

static void *RunRows(void *arg) {
  const Job *j = arg;
  const HdFxParams *p = g_p;
  for (int y = j->y0; y < j->y1; y++) {
    const float fy = (y + 0.5f) / j->f - 0.5f;
    int sy0 = fy < 0 ? 0 : (int)fy;
    if (sy0 > j->sh - 2) sy0 = j->sh - 2;
    int ay = (int)((fy - sy0) * 256.0f);
    ay = ay < 0 ? 0 : ay > 256 ? 256 : ay;
    int wdof = 0;
    if (p->tiltshift > 0) {
      const float d = fabsf((float)y / j->h - p->focus_center) - p->focus_width;
      const float span = 0.5f - p->focus_width;
      float t = d <= 0 ? 0 : (d >= span ? 1 : d / span);
      wdof = (int)(t * t * p->tiltshift * 256.0f);
    }
    const float dyf = y - j->h * 0.5f;
    const int32_t vrow = (int32_t)(dyf * dyf);
    uint32_t *row = j->px + (size_t)y * j->pitch;
    const uint32_t *d0 = j->dof ? j->dof + (size_t)sy0 * j->sw : NULL;
    const uint32_t *g0 = j->glow ? j->glow + (size_t)sy0 * j->sw : NULL;
    for (int x = 0; x < j->w; x++) {
      const uint32_t s = row[x];
      int r = (s >> 16) & 0xff, g = (s >> 8) & 0xff, b = s & 0xff;
      const int sx = j->cx0[x], ax = j->cax[x];
      if (wdof) {
        const uint32_t t = Lerp8(Lerp8(d0[sx], d0[sx + 1], ax),
                                 Lerp8(d0[sx + j->sw], d0[sx + j->sw + 1], ax), ay);
        r += ((int)((t >> 16) & 0xff) - r) * wdof >> 8;
        g += ((int)((t >> 8) & 0xff) - g) * wdof >> 8;
        b += ((int)(t & 0xff) - b) * wdof >> 8;
      }
      if (g0) {
        const uint32_t t = Lerp8(Lerp8(g0[sx], g0[sx + 1], ax),
                                 Lerp8(g0[sx + j->sw], g0[sx + j->sw + 1], ax), ay);
        r += (int)((t >> 16) & 0xff) * g_bloom256 >> 8;
        g += (int)((t >> 8) & 0xff) * g_bloom256 >> 8;
        b += (int)(t & 0xff) * g_bloom256 >> 8;
      }
      if (g_sat256 != 256) {
        const int l = (54 * r + 183 * g + 19 * b) >> 8;
        r = l + ((r - l) * g_sat256 >> 8);
        g = l + ((g - l) * g_sat256 >> 8);
        b = l + ((b - l) * g_sat256 >> 8);
      }
      if (g_con256 != 256) {
        r = 128 + ((r - 128) * g_con256 >> 8);
        g = 128 + ((g - 128) * g_con256 >> 8);
        b = 128 + ((b - 128) * g_con256 >> 8);
      }
      if (g_vig) {
        int v = 65536 - (int)(((int64_t)(vrow + j->vcol[x]) * g_vig) >> 16);
        v = v < 0 ? 0 : v;
        r = r * v >> 16, g = g * v >> 16, b = b * v >> 16;
      }
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      row[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
  }
  return NULL;
}

void HdFx_Apply(uint32_t *px, int w, int h, int pitch, const uint32_t *emissive,
                int epitch) {
  const HdFxParams *p = HdFx_Params();
  if (!HdFx_Active() || w < 16 || h < 16)
    return;
  const int f = w >= 1024 ? 4 : 2;   /* soft buffers at 1/4 (4x frames) or 1/2 */
  static Img base, dof, glow, em;
  static uint32_t *pdof, *pglow;
  static int cdof, cglow;
  Downsample(&base, px, w, h, pitch, f);
  const size_t n = (size_t)base.w * base.h * 3;
  if (p->tiltshift > 0) {
    ImgAlloc(&dof, base.w, base.h);
    memcpy(dof.rgb, base.rgb, n * sizeof(float));
    Blur(&dof, 3, 2);
    ToPacked(&dof, &pdof, &cdof);
  }
  if (p->bloom > 0) {
    ImgAlloc(&glow, base.w, base.h);
    const float t = p->bloom_threshold;
    for (size_t i = 0; i < n; i += 3) {
      const float *c = base.rgb + i;
      const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
      const float k = l > t ? (l - t) / (1 - t + 1e-3f) : 0;
      glow.rgb[i] = c[0] * k, glow.rgb[i + 1] = c[1] * k, glow.rgb[i + 2] = c[2] * k;
    }
    if (emissive) {
      Downsample(&em, emissive, w, h, epitch, f);
      for (size_t i = 0; i < n; i++)
        glow.rgb[i] += em.rgb[i];
    }
    Blur(&glow, 4, 2);
    ToPacked(&glow, &pglow, &cglow);
  }
  static int16_t *cx0;
  static uint8_t *cax;
  static int32_t *vcol;
  static int cw;
  if (cw != w) {
    free(cx0), free(cax), free(vcol);
    cx0 = malloc(sizeof(int16_t) * w), cax = malloc(w), vcol = malloc(sizeof(int32_t) * w);
    cw = w;
  }
  for (int x = 0; x < w; x++) {
    const float fx = (x + 0.5f) / f - 0.5f;
    int sx = fx < 0 ? 0 : (int)fx;
    if (sx > base.w - 2) sx = base.w - 2;
    int ax = (int)((fx - sx) * 255.0f);
    cx0[x] = (int16_t)sx, cax[x] = (uint8_t)(ax < 0 ? 0 : ax > 255 ? 255 : ax);
    const float dx = x - w * 0.5f;
    vcol[x] = (int32_t)(dx * dx);
  }
  g_p = p;
  g_bloom256 = (int)(p->bloom * 256.0f);
  g_sat256 = (int)(p->saturation * 256.0f + 0.5f);
  g_con256 = (int)(p->contrast * 256.0f + 0.5f);
  /* v = 1 - vignette * 1.2 * r^2 / rmax^2, as a 16.16 multiplier on r^2 */
  const float rmax2 = w * 0.5f * w * 0.5f + h * 0.5f * h * 0.5f;
  g_vig = (int)(p->vignette * 1.2f / rmax2 * 65536.0f * 65536.0f);
  enum { kThreads = 8 };
  pthread_t th[kThreads];
  Job jobs[kThreads];
  for (int i = 0; i < kThreads; i++) {
    jobs[i] = (Job){px, w, h, pitch, h * i / kThreads, h * (i + 1) / kThreads, f,
                    p->tiltshift > 0 ? pdof : NULL, p->bloom > 0 ? pglow : NULL,
                    base.w, base.h, cx0, cax, vcol};
    if (i && pthread_create(&th[i], NULL, RunRows, &jobs[i]) != 0)
      RunRows(&jobs[i]), th[i] = 0;
  }
  RunRows(&jobs[0]);
  for (int i = 1; i < kThreads; i++)
    if (th[i])
      pthread_join(th[i], NULL);
}
