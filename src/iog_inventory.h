#ifndef IOG_INVENTORY_H
#define IOG_INVENTORY_H

/* Draw the inventory counters / equipped marker over the frame just
 * rasterised (no-op unless those mods are on and the inventory is open). */
void IogInventoryOverlay(void);

int IogInventoryAllRedJewels(void);
int IogInventoryPageShown(void);

#endif
