/* switch_safeboot.c -- hold ZL+R3 while the game starts to come back from a
 * picture that does not work (a shader preset the console cannot run, a
 * renderer that shows nothing): the shader is cleared and the default
 * OpenGL ES presenter restored before anything is drawn.
 *
 * After the ALEKS Switch ports' safe boot: the pad is read through libnx
 * BEFORE SDL starts (once SDL owns the HID session this reads nothing), and
 * over a ~600 ms window, because nobody holds a chord on an exact frame. */
#if defined(__SWITCH__)
#include <switch.h>

int SnesSwitch_SafeBootHeld(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  for (int i = 0; i < 60; i++) {
    padUpdate(&pad);
    const u64 b = padGetButtons(&pad);
    if ((b & HidNpadButton_ZL) && (b & HidNpadButton_StickR))
      return 1;
    svcSleepThread(10000000ull);   /* 10 ms */
  }
  return 0;
}
#endif
