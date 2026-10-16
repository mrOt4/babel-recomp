#ifndef IOG_RANDOMIZER_H
#define IOG_RANDOMIZER_H

#include <stdbool.h>

/* A randomizer seed replaces the ROM: mods keyed to stock ROM addresses
 * must stand down. */
bool IogRandomizerActive(void);

#endif
