#ifndef IOG_WIDESCREEN_H
#define IOG_WIDESCREEN_H

/* Per-frame widescreen margin policy; call before the PPU rasterizes. */
void IogWidescreenBeginFrame(void);
/* Before the guest frame runs: widen the actor culling window. */
void IogWidescreenBeforeFrame(void);

#endif
