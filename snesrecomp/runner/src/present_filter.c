/* present_filter.c -- see present_filter.h. */

#include "present_filter.h"

static int g_filter = kPresentFilter_Config;

void PresentFilter_Set(int filter) {
  g_filter = filter >= kPresentFilter_Config && filter <= kPresentFilter_Smooth
                 ? filter : kPresentFilter_Config;
}

int PresentFilter_Get(void) { return g_filter; }
