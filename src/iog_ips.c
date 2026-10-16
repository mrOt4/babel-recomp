/*
 * ROM-patch mods (trusted plugin "iog.apocalypse"): apply an IPS patch to
 * the ROM image in memory, for hacks that change game code, such as
 * Apocalypse Gaia (the restored, unfinished final boss). The patch is not
 * distributed with this project: the player downloads it and picks it on the
 * Mods page (or drops it in the package's patch/ folder).
 *
 * Compiled (AOT) code was generated from the stock ROM, so a patch to code
 * only takes effect where the guest runs through the interpreter. With a
 * code patch active the frame driver is therefore told not to bounce into
 * compiled bodies at all (interp_bridge_set_interpret_all(1)): every
 * instruction is fetched from the patched image, exactly as on hardware.
 *
 * The stock ROM file is never modified; the patch lives only in memory.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "mod_runtime.h"
#include "iog_randomizer.h"
#include "snes/cart.h"
#include "snes/interp_bridge.h"
#include "snes/snes.h"

/* Apply an IPS file to rom[0..size). The image does not grow: a record past
 * the end refuses the whole patch (checked before anything is written). */
static bool ApplyIps(const char *path, uint8_t *rom, uint32_t size, int *records)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 8 && n < (16 << 20) ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n && !memcmp(d, "PATCH", 5);
    fclose(f);
    for (int pass = 0; ok && pass < 2; pass++) {
        long o = 5;
        *records = 0;
        for (;;) {
            if (o + 3 > n) {
                ok = false;
                break;
            }
            if (!memcmp(d + o, "EOF", 3))
                break;
            if (o + 5 > n) {
                ok = false;
                break;
            }
            const uint32_t off = d[o] << 16 | d[o + 1] << 8 | d[o + 2];
            uint32_t len = d[o + 3] << 8 | d[o + 4];
            o += 5;
            const bool rle = len == 0;
            if (rle) {
                if (o + 3 > n) {
                    ok = false;
                    break;
                }
                len = d[o] << 8 | d[o + 1];
            } else if (o + (long)len > n) {
                ok = false;
                break;
            }
            if ((uint64_t)off + len > size) {
                ok = false;
                break;
            }
            if (pass == 1) {
                if (rle)
                    memset(rom + off, d[o + 2], len);
                else
                    memcpy(rom + off, d + o, len);
            }
            o += rle ? 3 : (long)len;
            ++*records;
        }
    }
    free(d);
    return ok;
}

static void ActivateApocalypse(void)
{
    char path[1024];
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || IogRandomizerActive())
        return;   /* IoGR seeds carry their own (optional) Apocalypse Gaia */
    /* A file the player chose, else the one shipped in the package. */
    if (!snes_mod_runtime_feature_resource_path_c("iog.apocalypse", "apocalypse_gaia", "patch",
                                                  path, sizeof(path))) {
        char root[900];
        if (!snes_mod_runtime_package_root_c("iog.apocalypse", NULL, root, sizeof(root))) {
            fprintf(stderr, "[ips] Apocalypse Gaia: package not found\n");
            return;
        }
        snprintf(path, sizeof(path), "%s/patch/IOG_ApocalypseGaia_12.ips", root);
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            fprintf(stderr, "[ips] Apocalypse Gaia: no patch chosen. Download "
                            "IOG_ApocalypseGaia .ips (IOGR community) and pick it on the "
                            "Mods page, or put it at %s\n", path);
            return;
        }
        fclose(probe);
    }
    int records = 0;
    if (!ApplyIps(path, cart->rom, cart->romSize, &records)) {
        fprintf(stderr, "[ips] %s: not a usable IPS patch for this ROM\n", path);
        return;
    }
    interp_bridge_set_interpret_all(1);
    fprintf(stderr, "[ips] %s: %d records applied; running the patched code interpreted\n",
            path, records);
}

SNES_MOD_CONSTRUCTOR(register_iog_ips)
{
    snes_mod_register_activation_plugin("iog.apocalypse", ActivateApocalypse);
}
