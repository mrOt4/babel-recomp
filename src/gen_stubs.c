/* Hand-written bodies for code the recompiler could not translate.
 *
 * Empty at scaffold time. Fill this in as regeneration reports unresolved
 * targets — and fill it in with real behaviour, never with a value invented
 * to get past the gap. An invented result turns a loud failure into a silent
 * wrong one (recomp-ai-rules/PRINCIPLES.md, "No stubs"). If a routine cannot
 * be translated yet, leave it to the interpreter tier instead.
 */

#include "cpu_state.h"
#include "common_rtl.h"

/*
 * Wrapper around RtlUploadSpcImageFromDpLive for Illusion of Gaia.
 *
 * The game stores the SPC upload stream pointer in $46/$47 (low/high)
 * and $48 (bank) with D=0, but the runtime helper reads DP+0/+1/+2.
 * Temporarily set D=$46 so the helper finds the pointer, then restore
 * the caller D and report success/failure via the boolean the runtime
 * helper returns (logged to stderr on failure by the helper itself).
 */
RecompReturn IogSpcUpload(CpuState *cpu) {
    uint16_t saved_d = cpu->D;
    cpu->D = 0x46;
    RtlUploadSpcImageFromDpLive(cpu);
    cpu->D = saved_d;
    return RECOMP_RETURN_NORMAL;
}
