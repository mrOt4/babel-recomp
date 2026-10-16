/* HD packs (see hdpack.h). */

#include "hdpack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "hdfx.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- hashing ------------------------------------------------------------- */

static uint64_t Fnv(uint64_t h, const uint16_t *words, int n) {
  for (int i = 0; i < n; i++) {
    h = (h ^ (words[i] & 0xff)) * 0x100000001b3ull;
    h = (h ^ (words[i] >> 8)) * 0x100000001b3ull;
  }
  return h;
}
static const uint64_t kFnvOffset = 0xcbf29ce484222325ull;

static uint64_t TileHash(const struct Ppu *ppu, unsigned addr, int bpp) {
  uint16_t w[32];
  const uint16_t *vram = PpuRenderVram(ppu);
  int n = bpp * 4;
  for (int i = 0; i < n; i++)
    w[i] = vram[(addr + i) & 0x7fff];
  return Fnv(kFnvOffset, w, n);
}

/* Only the colours the character actually uses: games animate palette
 * entries (water, lights) that most characters sharing the palette never
 * show, and those must not change the key. Each used entry contributes its
 * index and colour, in index order. */
static uint64_t PaletteHash(const struct Ppu *ppu, unsigned addr, int pal_base, int bpp) {
  const uint16_t *vram = PpuRenderVram(ppu);
  uint8_t used[256] = {0};
  for (int y = 0; y < 8; y++) {
    uint16_t w[4];
    for (int plane = 0; plane < bpp; plane += 2)
      w[plane / 2] = vram[(addr + plane * 4 + y) & 0x7fff];
    for (int x = 0; x < 8; x++) {
      int v = 0;
      for (int plane = 0; plane < bpp; plane += 2)
        v |= ((w[plane / 2] >> (7 - x)) & 1) << plane | ((w[plane / 2] >> (15 - x)) & 1) << (plane + 1);
      used[v] = 1;
    }
  }
  uint64_t h = kFnvOffset;
  for (int v = 1; v < (1 << bpp); v++)
    if (used[v]) {
      uint16_t e[2] = {(uint16_t)v, ppu->cgram[(pal_base + v) & 0xff]};
      h = Fnv(h, e, 2);
    }
  return h;
}

/* ---- packs --------------------------------------------------------------- */

typedef struct Image {
  uint32_t *px;        /* ARGB */
  uint32_t *emissive;  /* optional light map, same size, NULL: none */
  int w, h, scale;
} Image;

typedef struct Entry {
  uint64_t tile, pal;  /* pal 0: any palette */
  int image;           /* -1: empty slot */
  int x, y;            /* top-left in the (4x) image, pixels */
  uint32_t *small;     /* 8x8 1x version */
} Entry;

static Image *g_images;
static int g_nimages;
static Entry *g_map;
static uint32_t g_map_cap;   /* power of two */
static int g_ntiles;

static uint32_t Slot(uint64_t tile, uint64_t pal) {
  uint64_t k = tile ^ (pal * 0x9e3779b97f4a7c15ull);
  return (uint32_t)(k ^ (k >> 29)) & (g_map_cap - 1);
}

static const Entry *Find(uint64_t tile, uint64_t pal) {
  if (!g_map_cap)
    return NULL;
  for (uint32_t i = Slot(tile, pal);; i = (i + 1) & (g_map_cap - 1)) {
    const Entry *e = &g_map[i];
    if (e->image < 0)
      return NULL;
    if (e->tile == tile && e->pal == pal)
      return e;
  }
}

static bool Insert(const Entry *e) {
  if ((uint32_t)(g_ntiles + 1) * 2 > g_map_cap) {
    uint32_t old_cap = g_map_cap;
    Entry *old = g_map;
    g_map_cap = old_cap ? old_cap * 2 : 4096;
    g_map = malloc(sizeof(Entry) * g_map_cap);
    for (uint32_t i = 0; i < g_map_cap; i++)
      g_map[i].image = -1;
    g_ntiles = 0;
    for (uint32_t i = 0; i < old_cap; i++)
      if (old[i].image >= 0)
        (void)Insert(&old[i]);
    free(old);
  }
  if (Find(e->tile, e->pal))
    return false;   /* an earlier pack already covers it */
  uint32_t i = Slot(e->tile, e->pal);
  while (g_map[i].image >= 0)
    i = (i + 1) & (g_map_cap - 1);
  g_map[i] = *e;
  g_ntiles++;
  return true;
}

/* Decode a PNG to ARGB stored at 4x. */
static uint32_t *DecodeAt4x(const char *dir, const char *name, int scale, int *ow, int *oh);

static int LoadImage(const char *dir, const char *name, int scale) {
  int w4, h4;
  uint32_t *px = DecodeAt4x(dir, name, scale, &w4, &h4);
  if (!px)
    return -1;
  Image img = {px, NULL, w4, h4, scale};
  g_images = realloc(g_images, sizeof(Image) * (g_nimages + 1));
  g_images[g_nimages] = img;
  return g_nimages++;
}

static uint32_t *DecodeAt4x(const char *dir, const char *name, int scale, int *ow, int *oh) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  int w, h, comp;
  unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
  if (!rgba) {
    fprintf(stderr, "[hdpack] cannot read %s\n", path);
    return NULL;
  }
  /* Stored at 4x whatever the pack's scale, so drawing never rescales. */
  const int k = 4 / scale;
  uint32_t *out = malloc((size_t)w * k * h * k * 4);
  for (int y = 0; y < h * k; y++)
    for (int x = 0; x < w * k; x++) {
      const unsigned char *p = rgba + 4 * ((size_t)(y / k) * w + x / k);
      out[(size_t)y * w * k + x] = (uint32_t)p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2];
    }
  stbi_image_free(rgba);
  *ow = w * k, *oh = h * k;
  return out;
}

int HdPack_AddDir(const char *dir) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/hdpack.txt", dir);
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  char line[512];
  int scale = 1, image = -1, added = 0, lineno = 0;
  while (fgets(line, sizeof(line), f)) {
    lineno++;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == '\n' || *s == '\r' || !*s)
      continue;
    char name[400], th[32], ph[32];
    int bpp, col, row;
    if (sscanf(s, "scale %d", &scale) == 1) {
      if (scale != 1 && scale != 2 && scale != 4) {
        fprintf(stderr, "[hdpack] %s:%d: scale must be 1, 2 or 4\n", path, lineno);
        scale = 1;
      }
    } else if (sscanf(s, "image %399s", name) == 1) {
      image = LoadImage(dir, name, scale);
    } else if (sscanf(s, "emissive %399s", name) == 1) {
      /* Light map for the last image: same layout, alpha < 128 = no light. */
      int w4, h4;
      uint32_t *e = image >= 0 ? DecodeAt4x(dir, name, scale, &w4, &h4) : NULL;
      if (e && (w4 != g_images[image].w || h4 != g_images[image].h)) {
        fprintf(stderr, "[hdpack] %s:%d: emissive map size differs from its image\n", path, lineno);
        free(e);
        e = NULL;
      }
      if (e)
        g_images[image].emissive = e;
    } else if (!strncmp(s, "fx ", 3)) {
      char fx[32];
      float v;
      if (sscanf(s, "fx %31s %f", fx, &v) != 2 || !HdFx_Set(HdFx_Params(), fx, v))
        fprintf(stderr, "[hdpack] %s:%d: unknown fx setting\n", path, lineno);
    } else if (sscanf(s, "tile %d %31s %31s %d %d", &bpp, th, ph, &col, &row) == 5) {
      if (image < 0)
        continue;
      const Image *img = &g_images[image];
      Entry e = {strtoull(th, NULL, 16), strcmp(ph, "*") ? strtoull(ph, NULL, 16) : 0,
                 image, col * 32, row * 32, NULL};
      if (e.x + 32 > img->w || e.y + 32 > img->h) {
        fprintf(stderr, "[hdpack] %s:%d: cell outside the image\n", path, lineno);
        continue;
      }
      (void)bpp;
      e.small = malloc(64 * 4);
      for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++) {
          /* Average of the opaque samples; transparent if most are not. */
          uint32_t r = 0, g = 0, b = 0, n = 0;
          for (int sy = 0; sy < 4; sy++)
            for (int sx = 0; sx < 4; sx++) {
              uint32_t p = img->px[(size_t)(e.y + cy * 4 + sy) * img->w + e.x + cx * 4 + sx];
              if ((p >> 24) >= 128)
                r += (p >> 16) & 0xff, g += (p >> 8) & 0xff, b += p & 0xff, n++;
            }
          e.small[cy * 8 + cx] = n >= 8 ? 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n) : 0;
        }
      if (Insert(&e))
        added++;
      else
        free(e.small);
    } else {
      fprintf(stderr, "[hdpack] %s:%d: not understood\n", path, lineno);
    }
  }
  fclose(f);
  fprintf(stderr, "[hdpack] %s: %d tiles\n", dir, added);
  return added;
}

int HdPack_TileCount(void) { return g_ntiles; }

/* ---- dump ---------------------------------------------------------------- */

/* One cell per character. A character drawn with several palettes (fades,
 * flashes, palette swaps) keeps the one it was on screen with longest; the
 * template notes how many it met, so a modder can add per-palette lines. */
enum { kSheetCells = 32, kSheetPx = kSheetCells * 8, kMaxVariants = 8 };

typedef struct DumpVariant {
  uint64_t pal;
  uint32_t seen;
  uint16_t *colors;    /* 1 << bpp CGRAM words */
} DumpVariant;

typedef struct DumpEntry {
  uint64_t tile;
  int bpp, sheet, cell, nvar;
  uint8_t idx[64];     /* colour index per pixel */
  DumpVariant var[kMaxVariants];
} DumpEntry;

static char *g_dump_dir;
static uint32_t **g_sheets;
static int g_nsheets;
static DumpEntry *g_dumped;
static int g_ndumped, g_dump_cap, g_dump_dirty;
static int *g_dump_index;        /* open addressing over tile hash, -1 empty */
static uint32_t g_dump_index_cap;

static int *DumpSlot(uint64_t tile) {
  uint32_t j = (uint32_t)(tile ^ (tile >> 31)) & (g_dump_index_cap - 1);
  for (; g_dump_index[j] >= 0; j = (j + 1) & (g_dump_index_cap - 1))
    if (g_dumped[g_dump_index[j]].tile == tile)
      break;
  return &g_dump_index[j];
}

static void DumpGrowIndex(void) {
  if ((uint32_t)(g_ndumped + 1) * 2 <= g_dump_index_cap)
    return;
  free(g_dump_index);
  g_dump_index_cap = g_dump_index_cap ? g_dump_index_cap * 2 : 8192;
  g_dump_index = malloc(sizeof(int) * g_dump_index_cap);
  for (uint32_t i = 0; i < g_dump_index_cap; i++)
    g_dump_index[i] = -1;
  for (int i = 0; i < g_ndumped; i++)
    *DumpSlot(g_dumped[i].tile) = i;
}

static void DumpTile(const struct Ppu *ppu, const PpuHdTile *t, uint64_t tile, uint64_t pal) {
  DumpGrowIndex();
  int *slot = DumpSlot(tile);
  DumpEntry *e;
  if (*slot >= 0) {
    e = &g_dumped[*slot];
  } else {
    if (g_ndumped == g_dump_cap) {
      g_dump_cap = g_dump_cap ? g_dump_cap * 2 : 1024;
      g_dumped = realloc(g_dumped, sizeof(DumpEntry) * g_dump_cap);
    }
    *slot = g_ndumped;
    e = &g_dumped[g_ndumped];
    memset(e, 0, sizeof(*e));
    e->tile = tile, e->bpp = t->bpp;
    e->cell = g_ndumped % (kSheetCells * kSheetCells);
    if (e->cell == 0) {
      g_sheets = realloc(g_sheets, sizeof(uint32_t *) * (g_nsheets + 1));
      g_sheets[g_nsheets++] = calloc(kSheetPx * kSheetPx, 4);
    }
    e->sheet = g_nsheets - 1;
    const uint16_t *vram = PpuRenderVram(ppu);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        int v = 0;
        for (int plane = 0; plane < t->bpp; plane += 2) {
          uint16_t w = vram[(t->addr + plane * 4 + y) & 0x7fff];
          v |= ((w >> (7 - x)) & 1) << plane | ((w >> (15 - x)) & 1) << (plane + 1);
        }
        e->idx[y * 8 + x] = (uint8_t)v;
      }
    g_ndumped++;
    g_dump_dirty = 1;
  }
  for (int i = 0; i < e->nvar; i++)
    if (e->var[i].pal == pal) {
      e->var[i].seen++;
      return;
    }
  if (e->nvar == kMaxVariants)
    return;
  int n = 1 << e->bpp;
  DumpVariant *v = &e->var[e->nvar++];
  v->pal = pal, v->seen = 1;
  v->colors = malloc(sizeof(uint16_t) * n);
  for (int i = 0; i < n; i++)
    v->colors[i] = ppu->cgram[(t->pal_base + i) & 0xff];
  g_dump_dirty = 1;
}

static const DumpVariant *DumpBest(const DumpEntry *e) {
  const DumpVariant *best = &e->var[0];
  for (int i = 1; i < e->nvar; i++)
    if (e->var[i].seen > best->seen)
      best = &e->var[i];
  return best;
}

static void DumpDraw(const DumpEntry *e) {
  const DumpVariant *v = DumpBest(e);
  uint32_t *sheet = g_sheets[e->sheet];
  int cx = (e->cell % kSheetCells) * 8, cy = (e->cell / kSheetCells) * 8;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int i = e->idx[y * 8 + x];
      uint32_t argb = 0;
      if (i) {
        uint16_t c = v->colors[i];
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        argb = 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
      }
      sheet[(cy + y) * kSheetPx + cx + x] = argb;
    }
}

/* Minimal PNG writer: RGBA8, zlib "stored" blocks. Templates are small and
 * written rarely; size is not a concern, having no dependency is. */
static uint32_t Crc(uint32_t crc, const uint8_t *p, size_t n) {
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

static void Put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = (uint8_t)v;
}

static void Chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t hdr[8];
  Put32(hdr, n);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t crc = Crc(Crc(0, (const uint8_t *)type, 4), data, n);
  uint8_t c[4];
  Put32(c, crc);
  fwrite(c, 1, 4, f);
}

static bool WritePng(const char *path, const uint32_t *argb, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  Put32(ihdr, w), Put32(ihdr + 4, h);
  ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
  Chunk(f, "IHDR", ihdr, 13);
  size_t raw_n = (size_t)h * (1 + 4 * w);
  uint8_t *raw = malloc(raw_n);
  for (int y = 0; y < h; y++) {
    uint8_t *r = raw + (size_t)y * (1 + 4 * w);
    r[0] = 0;
    for (int x = 0; x < w; x++) {
      uint32_t p = argb[(size_t)y * w + x];
      r[1 + 4 * x] = p >> 16, r[2 + 4 * x] = p >> 8, r[3 + 4 * x] = (uint8_t)p;
      r[4 + 4 * x] = p >> 24;
    }
  }
  size_t blocks = (raw_n + 65534) / 65535;
  size_t z_n = 2 + raw_n + blocks * 5 + 4;
  uint8_t *z = malloc(z_n), *q = z;
  *q++ = 0x78, *q++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t off = 0; off < raw_n;) {
    size_t len = raw_n - off > 65535 ? 65535 : raw_n - off;
    *q++ = off + len == raw_n;
    *q++ = (uint8_t)len, *q++ = (uint8_t)(len >> 8);
    *q++ = (uint8_t)~len, *q++ = (uint8_t)(~len >> 8);
    memcpy(q, raw + off, len);
    for (size_t i = 0; i < len; i++) {
      a = (a + raw[off + i]) % 65521;
      b = (b + a) % 65521;
    }
    q += len, off += len;
  }
  Put32(q, b << 16 | a);
  Chunk(f, "IDAT", z, (uint32_t)z_n);
  Chunk(f, "IEND", NULL, 0);
  free(raw);
  free(z);
  return fclose(f) == 0;
}

void HdPack_Flush(void) {
  if (!g_dump_dir || !g_dump_dirty)
    return;
  char path[1024];
  for (int i = 0; i < g_ndumped; i++)
    DumpDraw(&g_dumped[i]);
  for (int s = 0; s < g_nsheets; s++) {
    snprintf(path, sizeof(path), "%s/tiles_%03d.png", g_dump_dir, s);
    WritePng(path, g_sheets[s], kSheetPx, kSheetPx);
  }
  snprintf(path, sizeof(path), "%s/hdpack.txt", g_dump_dir);
  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "[hdpack] cannot write %s\n", path);
    return;
  }
  fprintf(f, "# SNESRecomp HD pack template, dumped from your own game.\n"
             "# Redraw the sheets (keep each 8x8 cell's place), resize them by\n"
             "# 2 or 4 for more detail and set `scale` to match. Delete the\n"
             "# lines of tiles you do not change. Palette hash '*' = any palette.\n"
             "scale 1\n");
  int sheet = -1;
  for (int i = 0; i < g_ndumped; i++) {
    const DumpEntry *e = &g_dumped[i];
    if (e->sheet != sheet)
      fprintf(f, "image tiles_%03d.png\n", sheet = e->sheet);
    fprintf(f, "tile %d %016llx %016llx %d %d", e->bpp, (unsigned long long)e->tile,
            (unsigned long long)DumpBest(e)->pal, e->cell % kSheetCells, e->cell / kSheetCells);
    if (e->nvar > 1)
      fprintf(f, "   # seen with %d%s palettes", e->nvar, e->nvar == kMaxVariants ? "+" : "");
    fputc('\n', f);
  }
  fclose(f);
  g_dump_dirty = 0;
  fprintf(stderr, "[hdpack] dumped %d tiles to %s\n", g_ndumped, g_dump_dir);
}

void HdPack_SetDumpDir(const char *dir) {
  free(g_dump_dir);
  g_dump_dir = dir && *dir ? strdup(dir) : NULL;
  static int registered;
  if (g_dump_dir && !registered) {
    registered = 1;
    atexit(HdPack_Flush);
  }
}

/* ---- lookup -------------------------------------------------------------- */

/* Characters repeat all over a frame; hashing each once per frame is
 * enough (VRAM and CGRAM change between frames, rarely within one). */
enum { kCacheSize = 4096 };
typedef struct CacheSlot {
  uint32_t key, frame;
  const Entry *entry;
} CacheSlot;
static CacheSlot g_cache[kCacheSize];

static bool Lookup(void *ctx, struct Ppu *ppu, const PpuHdTile *t, PpuHdImage *out) {
  (void)ctx;
  const uint32_t key = t->addr | (uint32_t)t->pal_base << 15 | (uint32_t)t->bpp << 23 | 1u << 31;
  CacheSlot *c = &g_cache[(key ^ (key >> 12) ^ (key >> 19)) & (kCacheSize - 1)];
  if (c->key != key || c->frame != t->frame) {
    uint64_t tile = TileHash(ppu, t->addr, t->bpp);
    uint64_t pal = PaletteHash(ppu, t->addr, t->pal_base, t->bpp);
    const Entry *e = Find(tile, pal);
    if (!e)
      e = Find(tile, 0);
    if (!e && g_dump_dir)
      DumpTile(ppu, t, tile, pal);
    c->key = key, c->frame = t->frame, c->entry = e;
  }
  /* Keep the template on disk current (about once a second while new
   * characters appear), so a session that ends abruptly leaves a usable one. */
  static uint32_t last_flush;
  if (g_dump_dir && g_dump_dirty && t->frame - last_flush > 60) {
    last_flush = t->frame;
    HdPack_Flush();
  }
  if (!c->entry)
    return false;
  const Image *img = &g_images[c->entry->image];
  out->pixels = img->px + (size_t)c->entry->y * img->w + c->entry->x;
  out->stride = img->w;
  out->small = c->entry->small;
  out->emissive = img->emissive ? img->emissive + (size_t)c->entry->y * img->w + c->entry->x : NULL;
  return true;
}

void HdPack_Install(struct Ppu *ppu) {
  if (!ppu)
    return;
  PpuSetHdLookup(ppu, (g_ntiles || g_dump_dir) ? Lookup : NULL, NULL);
}
