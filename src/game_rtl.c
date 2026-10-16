/*
 * Illusion Of Gaia Usa — per-game runtime glue.
 *
 * THIS FILE IS THE PORT. Everything else the scaffold produced is layout,
 * build wiring, and packaging; this is where the actual work happens.
 *
 * The framework does not, and cannot, drive an arbitrary SNES game on its
 * own. Two responsibilities always land on the game host:
 *
 *   1. Deciding what "one frame" means for THIS title, and returning from
 *      run_frame() at that boundary. A real ROM's reset vector never
 *      returns — it enters a main loop that waits on vblank — so somebody
 *      has to choose the yield point.
 *   2. Delivering NMI/IRQ at the hardware edge (see
 *      snesrecomp/docs/FRAME_MODEL_HOSTS.md).
 *
 * What follows is the shape every working port in this ecosystem converged
 * on, built from documented bridge entry points:
 *
 *      boot from the reset vector
 *      -> deliver NMI at the vblank edge, gated on NMITIMEN
 *      -> run the guest in slices until it parks or the frame's clock is out
 *      -> service a raster IRQ whenever the comparator asserts
 *      -> rasterize the field with HDMA per line
 *
 * It is a starting point, not a finished port: a title with unusual timing
 * will need the slice loop tightened (MetalWarriorsSNESRecomp's src/mw_rtl.c
 * and GundamWingEndlessDuelSNESRecomp's src/game_rtl.c are the two most
 * developed examples, and the second walks the beam explicitly while the CPU
 * is parked). What it will NOT do is sit at a black screen doing nothing,
 * which is what a driver that delivers no interrupt always does.
 *
 * Why NMI is not gated on WAI: an earlier version of this template delivered
 * an interrupt only when interp_bridge_lle_took_wai() was true. Real titles
 * overwhelmingly wait on a WRAM flag set by their own NMI handler, not on
 * WAI — Super Metroid spins `LDA $05B4 / BNE`, Zelda 3 on $0012, Super Mario
 * World on $0010 — so that branch never fired and the guest parked forever
 * on its first vblank wait. Quiescence on a read-only cycle IS the signal
 * that only an interrupt can make progress; that is the edge to act on.
 */

#include "game_rtl.h"

#include <stdio.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"          /* SimpleHdma_*, g_snesrecomp_last_hdmaen */
#include "cpu_state.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"

extern CpuState g_cpu;
extern Ppu *g_ppu;

/* One NTSC frame: 262 scanlines x 1364 master clocks. Bounds a productive
 * MMIO loop so it cannot run across several vblanks atomically. */
#define GAME_MASTER_CYCLES_PER_FRAME 357368ull

/* How many times a frame may re-enter the guest after it parks on a poll.
 * Each slice ends at a deterministic read-only cycle or at an IRQ; the bound
 * only stops a pathological loop from spinning the host. */
#define GAME_MAX_SLICES_PER_FRAME 64

/* 0 until the first frame has booted from the reset vector. */
static uint32_t g_resume_pc;

static uint32_t read_vector(uint32_t addr)
{
    /* Read through the guest bus so a mapper or coprocessor window resolves
     * the same way the CPU sees it. */
    uint32_t lo = snes_read(g_snes, addr);
    uint32_t hi = snes_read(g_snes, addr + 1u);
    return (hi << 8) | lo;
}

static uint32_t reset_vector(void) { return read_vector(0x00FFFCu); }
static uint32_t nmi_vector(void)   { return read_vector(0x00FFEAu); }
static uint32_t irq_vector(void)   { return read_vector(0x00FFEEu); }

/* Run one interrupt handler to its RTI, entered as hardware enters it: the
 * frame is pushed at the PC the guest was interrupted AT, so the handler's
 * terminal RTI returns into that instruction stream. */
static void game_run_interrupt(uint32_t vector, uint64_t frame_end)
{
    cpu_push_interrupt_frame_at(&g_cpu, g_resume_pc);
    interp_bridge_set_master_deadline(frame_end);
    (void)interp_bridge_run_interrupt(&g_cpu, vector);
    /* Clearing matters: a deadline left armed stays true for every AOT block
     * prologue afterwards, which turns every compiled body into an immediate
     * yield-unwind. */
    interp_bridge_set_master_deadline(0);
    {
        uint32_t resume = interp_bridge_lle_resume_pc();
        if (resume)
            g_resume_pc = resume;
    }
}

void GameRunOneFrame(void)
{
    const uint64_t frame_end = g_cpu.master_cycles + GAME_MASTER_CYCLES_PER_FRAME;
    const int booting = (g_resume_pc == 0);
    int slice;


    if (booting) {
        g_resume_pc = reset_vector();
        /* Real SNES reset state: native mode, 8-bit A/X, interrupts
         * disabled. The runtime initialises to (M1X0), but the AOT
         * body emitted for I_RESET only exists as M1X1, so the dispatch
         * would miss and fall to the interpreter — which then never
         * bounces back into generated code from the reset path. */
        g_cpu.emulation = 1;
        g_cpu.m_flag = 1;
        g_cpu.x_flag = 1;
        g_cpu._flag_I = 1;
        /* Enter the guest through the AOT dispatch table so the compiled
         * I_RESET body runs. The interpreter only bounces to AOT on
         * JSR/JSL, and reset leaves the bank via JML, so entering via
         * the interp never reaches generated code. */
        /* Stay on the interpreter path for boot. The AOT loop at
         * $82:8051 (BPL waiting on $4210 NMI) has no per-instruction
         * deadline check, so dispatching via cpu_dispatch_pc wedges the
         * process. The interpreter checks the deadline per instruction
         * and bounces to AOT bodies on JSR/JSL — exactly what we want. */
    }

    /* Vblank edge. NMITIMEN gates it: delivering before the guest has enabled
     * NMI would land an interrupt frame in the middle of its SEI boot
     * sequence. Nothing is delivered on the very first frame either — reset
     * has not run yet, so there is no instruction stream to interrupt. */
    if (!booting) {
        /* Set inNmi and LEAVE it set. Hardware keeps the $4210 bit 7
         * pending until the guest reads $4210, and snes_readReg() clears
         * it. A port that waits on $4210 directly (rather than on a WRAM
         * flag set by the NMI handler, like SMW/Zelda 3) never sees the
         * edge if we clear it here before the guest has a chance to read.
         * The flag latches at every vblank regardless of NMITIMEN. */
        g_snes->inNmi = true;
        /* The interrupt itself is gated on NMITIMEN bit 7. IoG disables NMI
         * around its full-screen column redraw (code_02ABE6); the NMI
         * handler rewrites VMAIN and the DMA source bank, so forcing it
         * mid-loop turned the rest of the tilemap into garbage. */
        if (g_snes->nmiEnabled)
            game_run_interrupt(nmi_vector(), frame_end);
    }

    /* Run the guest until it parks on a read-only poll (its vblank wait) or
     * the frame's clock runs out. A single call is not enough: the guest
     * typically parks several times per frame — on HVBJOY, on a DMA-complete
     * flag, on its own state machine — and each park needs either an
     * interrupt or simply more time. */
    for (slice = 0; slice < GAME_MAX_SLICES_PER_FRAME; slice++) {
        if (g_cpu.master_cycles >= frame_end)
            break;
        interp_bridge_set_master_deadline(frame_end);
        interp_bridge_run_until_quiescent(&g_cpu, g_resume_pc);
        interp_bridge_set_master_deadline(0);
        {
            uint32_t resume = interp_bridge_lle_resume_pc();
            if (resume)
                g_resume_pc = resume;
        }

        /* A raster IRQ asserted while the guest ran: service it before
         * continuing, exactly as the CPU samples it between instructions. */
        if (g_snes->inIrq && !g_cpu._flag_I) {
            game_run_interrupt(irq_vector(), frame_end);
            continue;
        }
        /* Parked with no interrupt pending and clock left over: the guest is
         * waiting for the next vblank. Nothing more happens this frame. */
        if (interp_bridge_lle_took_wai())
            break;
    }
}

void GameDrawPpuFrame(void)
{
    SimpleHdma hdma_chans[8];
    Dma *dma = g_snes->dma;
    int trigger;
    int line, ch;

    /* Re-arm HDMA from the last $420C (HDMAEN) the guest wrote — typically
     * during the NMI just run. The framework records it for exactly this. */
    dma_startDma(dma, g_snesrecomp_last_hdmaen, true);
    for (ch = 0; ch < 8; ch++)
        SimpleHdma_Init(&hdma_chans[ch], &dma->channel[ch]);

    /* Mid-frame raster split, if the guest programmed the V comparator. */
    trigger = g_snes->vIrqEnabled ? (int)g_snes->vTimer : -1;

    /* From line 0, not line 1: starting at 1 leaves the top scanline holding
     * the previous frame's state, which shows up as a stripe of stale
     * tilemap above a HUD. */
    for (line = 0; line <= 224; line++) {
        /* HDMA runs in the H-blank BEFORE each visible line, and the raster
         * IRQ then selects the register set that line is drawn with — so
         * both must precede ppu_runLine for this line, not follow it. */
        for (ch = 0; ch < 8; ch++)
            SimpleHdma_DoLine(&hdma_chans[ch]);
        if (line == trigger) {
            g_snes->inIrq = true;
            cpu_push_interrupt_frame_at(&g_cpu, g_resume_pc);
            (void)interp_bridge_run_interrupt(&g_cpu, irq_vector());
            g_snes->inIrq = false;
            {
                uint32_t resume = interp_bridge_lle_resume_pc();
                if (resume)
                    g_resume_pc = resume;
            }
            trigger = g_snes->vIrqEnabled ? (int)g_snes->vTimer : -1;
        }
        ppu_runLine(g_ppu, line);
    }
}

/* Savestates. snes_saveload stores the emulated machine, but the guest's
 * program position lives here: g_resume_pc (where the guest parked at the
 * end of the frame) and the recompiler's CpuState registers. Without them a
 * load resumes the CURRENT instruction stream over the loaded RAM (from the
 * boot code, a load fell back to the title; in play, the game froze). */
enum { kStateMagic = 0x31474F49u /* "IOG1" */ };

typedef struct GameState {
    uint32_t magic, resume_pc;
    uint16_t A, X, Y, S, D;
    uint8_t DB, PB, P, emulation;
} GameState;

static void GameStateSave(SaveLoadInfo *sli)
{
    GameState st;
    memset(&st, 0, sizeof(st));
    /* Rewind snapshots every frame through here: read-only on the CPU. */
    CpuState tmp = g_cpu;
    cpu_mirrors_to_p(&tmp);
    st.magic = kStateMagic;
    st.resume_pc = g_resume_pc;
    st.A = g_cpu.A, st.X = g_cpu.X, st.Y = g_cpu.Y, st.S = g_cpu.S, st.D = g_cpu.D;
    st.DB = g_cpu.DB, st.PB = g_cpu.PB, st.P = tmp.P, st.emulation = g_cpu.emulation;
    sli->func(sli, &st, sizeof(st));
}

static void GameStateLoad(SaveLoadInfo *sli, uint32_t version)
{
    (void)version;
    GameState st;
    memset(&st, 0, sizeof(st));
    sli->func(sli, &st, sizeof(st));
    if (st.magic != kStateMagic)
        return;
    g_resume_pc = st.resume_pc;
    g_cpu.A = st.A, g_cpu.X = st.X, g_cpu.Y = st.Y, g_cpu.S = st.S, g_cpu.D = st.D;
    g_cpu.DB = st.DB, g_cpu.PB = st.PB, g_cpu.P = st.P, g_cpu.emulation = st.emulation;
    cpu_p_to_mirrors(&g_cpu);
}

const RtlGameInfo kGameInfo = {
    .title = "illusionofgaiausa",
    .initialize = NULL,
    .run_frame = &GameRunOneFrame,
    .draw_ppu_frame = &GameDrawPpuFrame,
    .save_name_prefix = "save",
    .state_save_extra = &GameStateSave,
    .state_load_extra = &GameStateLoad,
};

void GameSessionReset(void)
{
    /* Rematch / soft-return: clear anything that must not survive a new
     * session. recomp-ai-rules/NETPLAY.md §3 — sticky state that "has always
     * been fine" is the usual desync culprit, because single-player never
     * re-enters the boot path twice in one process. */
    g_resume_pc = 0;
}
