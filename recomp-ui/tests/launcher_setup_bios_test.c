/* First-run wizard: picking a retail BIOS this binary has no backend for.
 *
 * Regression it exists for: a precompiled build handed to someone else opens
 * the wizard asking for BIOS + disc. Browsing a retail dump used to suspend
 * the wizard and pop "Switch BIOS?" over it -- which hid the disc rows AND
 * disabled its own Generate & rebuild button, because that button requires a
 * disc and the player had just been prevented from choosing one. Anyone who
 * picked the BIOS before the disc hit a dead end with no way forward.
 *
 * The flow now stages the pick in place: the wizard stays up, keeps its disc
 * rows, and swaps its own Confirm/Continue button for Generate & rebuild.
 *
 * Includes the model translation unit (as launcher_discs_test.c does) so the
 * static staging/revert helpers are reachable.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to BIOS setup. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

/* Stand-in for the host verifier: "linked.bin" is compiled into this build,
 * any other readable dump is a valid retail image that is not (needs_regen),
 * and the empty path is OpenBIOS. Mirrors psxrecomp's ae_bios_verify. */
static int fake_bios_verify(const char* path, RecompLauncherCBiosVerify* out) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        out->ok = 1;
        snprintf(out->detail, sizeof(out->detail), "Using bundled OpenBIOS.");
        return 1;
    }
    if (strstr(path, "linked.bin")) {
        out->ok = 1;
        snprintf(out->detail, sizeof(out->detail), "SCPH1001.BIN (CRC OK).");
        return 1;
    }
    out->ok = 0;
    out->needs_regen = 1;
    snprintf(out->detail, sizeof(out->detail),
             "This BIOS is not compiled into the current build.");
    return 1;
}

static int fake_prepare(const char* src, char* out, size_t out_cap,
                        char* err, size_t err_cap,
                        RecompLauncherCPrepareProgressFn progress,
                        void* progress_ctx) {
    (void)err; (void)err_cap; (void)progress; (void)progress_ctx;
    snprintf(out, out_cap, "%s", src ? src : "");
    return 1;
}

/* A built (not setup-host) codegen build: sources present, so the wizard is in
 * media-confirm mode -- the state a received package lands in. */
static LauncherModel* make_model(const char* dir, char* disc_out, size_t cap) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) return NULL;
    m->setup_wizard_supported = true;
    m->has_bios = true;
    m->bios_verify_cb = fake_bios_verify;
    m->prepare_with_progress_cb = fake_prepare;
    m->prepare_use_selected_rom = true;
    m->prepare_required_before_continue = false;
    m->setup_wizard_open = true;
    m->setup_page = 1;
    snprintf(disc_out, cap, "%s/setupbios_game.cue", dir);
    snprintf(m->rom_size, sizeof(m->rom_size), "--");
    return m;
}

static void test_staged_in_wizard(const char* dir) {
    char disc[512];
    char bios[512];
    LauncherModel* m = make_model(dir, disc, sizeof(disc));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(bios, sizeof(bios), "%s/setupbios_retail.bin", dir);

    launcher_model_request_bios_path(m, bios);

    expect(!m->bios_confirm_open,
           "no nested Switch BIOS? modal while the wizard is open");
    expect(m->setup_wizard_open && !m->setup_wizard_suspended_for_bios,
           "the wizard stays up, so the disc rows stay reachable");
    expect(m->bios_switch_uncommitted && !strcmp(m->s.bios_path, bios),
           "the pick is staged on the model, not discarded");
    expect(launcher_model_setup_needs_bios_regen(m),
           "the wizard's primary button becomes Generate & rebuild");
    expect(!launcher_model_can_finish_setup(m),
           "Confirm/Continue could not have worked here");

    /* Without a disc the action is honestly blocked -- but reachable. */
    expect(!launcher_model_can_start_bios_regen(m), "blocked with no disc");
    expect(launcher_model_setup_bios_regen_blocker(m) != NULL &&
           strstr(launcher_model_setup_bios_regen_blocker(m), "disc") != NULL,
           "the tooltip names the disc as the missing piece");

    /* ...and picking the disc afterwards unblocks it, in either order. */
    snprintf(m->rom_full, sizeof(m->rom_full), "%s", disc);
    m->rom_present = true;
    snprintf(m->rom_size, sizeof(m->rom_size), "512 MiB");
    expect(launcher_model_can_start_bios_regen(m),
           "BIOS-then-disc reaches Generate & rebuild");
    expect(launcher_model_setup_bios_regen_blocker(m) == NULL,
           "nothing left blocking it");

    free(m);
}

static void test_second_pick_keeps_revert_target(const char* dir) {
    char disc[512];
    char a[512], b[512];
    LauncherModel* m = make_model(dir, disc, sizeof(disc));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(a, sizeof(a), "%s/setupbios_a.bin", dir);
    snprintf(b, sizeof(b), "%s/setupbios_b.bin", dir);

    launcher_model_request_bios_path(m, a);
    launcher_model_request_bios_path(m, b);
    expect(!strcmp(m->s.bios_path, b), "second pick replaces the first");
    expect(m->bios_revert_path[0] == '\0',
           "revert target is still OpenBIOS, not the first unlinked pick");

    /* Failing the job must put the player back where they started. */
    lm_bios_revert_uncommitted(m);
    expect(m->s.bios_path[0] == '\0' && !m->bios_switch_uncommitted,
           "a failed Generate reverts to the original selection");

    free(m);
}

static void test_openbios_clears_staging(const char* dir) {
    char disc[512];
    char bios[512];
    LauncherModel* m = make_model(dir, disc, sizeof(disc));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(bios, sizeof(bios), "%s/setupbios_retail.bin", dir);

    launcher_model_request_bios_path(m, bios);
    expect(m->bios_switch_uncommitted, "staged");
    /* "Use OpenBIOS" is the escape hatch: play now, never rebuild. */
    launcher_model_request_bios_path(m, "");
    expect(m->s.bios_path[0] == '\0' && m->setup_bios_ok,
           "OpenBIOS hot-swaps");
    expect(!m->bios_switch_uncommitted && m->bios_revert_path[0] == '\0',
           "the abandoned staging cannot resurrect itself later");
    expect(!launcher_model_setup_needs_bios_regen(m),
           "the button goes back to Confirm/Continue");

    free(m);
}

/* Outside the wizard (Settings -> BIOS) the confirm modal is still right:
 * a disc is already mounted there, so its Generate button is live. */
static void test_dashboard_still_confirms(const char* dir) {
    char disc[512];
    char bios[512];
    LauncherModel* m = make_model(dir, disc, sizeof(disc));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(bios, sizeof(bios), "%s/setupbios_retail.bin", dir);
    m->setup_wizard_open = false;

    launcher_model_request_bios_path(m, bios);
    expect(m->bios_confirm_open, "Switch BIOS? still opens from the dashboard");
    expect(!m->bios_switch_uncommitted,
           "nothing is staged until the player confirms");

    free(m);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    test_staged_in_wizard(dir);
    test_second_pick_keeps_revert_target(dir);
    test_openbios_clears_staging(dir);
    test_dashboard_still_confirms(dir);
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
