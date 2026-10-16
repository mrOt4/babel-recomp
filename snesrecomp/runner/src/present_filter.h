#ifndef SNESRECOMP_PRESENT_FILTER_H
#define SNESRECOMP_PRESENT_FILTER_H

/*
 * How the presenters scale the SNES picture to the window (HD output).
 *
 *   kPresentFilter_Config  the [Graphics] LinearFiltering setting (nearest or
 *                          bilinear), as before
 *   kPresentFilter_Sharp   square, crisp pixels at any size: each SNES pixel
 *                          is a solid block, and only the one output pixel
 *                          on a block's edge is blended, so non-integer
 *                          scales neither blur nor make uneven pixels
 *   kPresentFilter_Smooth  pixel-art upscaler (xBR level 2): diagonal and
 *                          curved edges of the art are redrawn smooth at the
 *                          output resolution, flat areas stay crisp. Applies
 *                          to 1x frames; 4x frames (HD packs, Mode 7 at 4x)
 *                          get Sharp
 *
 * Smooth needs the OpenGL presenter; the SDL presenter shows Sharp for both
 * (SDL 3.4's pixel-art scale mode), or the Config filter on older SDL.
 * Set by a presentation plugin or the host; read on every present.
 */

enum {
  kPresentFilter_Config = 0,
  kPresentFilter_Sharp = 1,
  kPresentFilter_Smooth = 2,
};

void PresentFilter_Set(int filter);
int PresentFilter_Get(void);

#endif
