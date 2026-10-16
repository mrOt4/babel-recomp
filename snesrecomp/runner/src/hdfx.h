#ifndef SNESRECOMP_HDFX_H
#define SNESRECOMP_HDFX_H

/*
 * HD-2D post-processing on the presented frame: tilt-shift depth of field,
 * bloom (bright areas plus the HD packs' emissive maps), vignette and colour
 * grading. Presentation only: it never touches emulated state.
 *
 * Parameters come from HD packs (`fx <name> <value>` lines, see
 * docs/HD_PACKS.md) and can be overridden by the host (a mod's options).
 * All strengths 0 = off (the default): the frame is left untouched.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct HdFxParams {
  float tiltshift;        /* 0..1: blur strength towards the top/bottom */
  float focus_center;     /* 0..1: sharp band centre (fraction of height) */
  float focus_width;      /* 0..1: sharp band half-height */
  float bloom;            /* 0..2: glow strength */
  float bloom_threshold;  /* 0..1: luminance where glow starts */
  float vignette;         /* 0..1 */
  float contrast;         /* 1 = unchanged */
  float saturation;       /* 1 = unchanged */
} HdFxParams;

/* Defaults (all effects off). */
void HdFx_Defaults(HdFxParams *p);
/* Parse one `fx <name> <value>` setting into p. Returns false if unknown. */
bool HdFx_Set(HdFxParams *p, const char *name, float value);

/* Current parameters: pack settings merged with host overrides. */
HdFxParams *HdFx_Params(void);
bool HdFx_Active(void);

/* Apply to an ARGB frame. `emissive` (optional, same size/pitch) adds glow
 * where HD art marks light sources. */
void HdFx_Apply(uint32_t *px, int w, int h, int pitch_px, const uint32_t *emissive,
                int emissive_pitch_px);

#endif
