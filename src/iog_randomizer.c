/*
 * Randomizer (trusted plugin "iog.randomizer").
 *
 * Plays a seed of the Illusion of Gaia Randomizer (IoGR, DontBaguMe et al.,
 * https://www.iogr.app, public domain): the player generates a randomized
 * ROM from their own US ROM (on the website, or locally with
 * tools/randomizer/generate.sh) and picks it on the Mods page. Nothing from
 * a ROM ships here.
 *
 * The launcher still verifies and loads the stock ROM; at activation, before
 * the first frame, the cartridge image is swapped for the seed (IoGR
 * expands it to 4 MiB of HiROM) and the whole game runs through the
 * interpreter, since the compiled code was generated from the stock ROM.
 * Mods that patch data at stock ROM addresses (languages, MSU-1, item
 * stacking, New Game Plus, the widescreen sprite window) stand down while a
 * seed is loaded: IogRandomizerActive().
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "mod_runtime.h"
#include "snes/cart.h"
#include "snes/interp_bridge.h"
#include "snes/snes.h"

#include "iog_randomizer.h"
#include "snes_ra.h"

bool IogRandomizerActive(void)
{
    return snes_mod_runtime_feature_enabled_c("iog.randomizer", "randomizer") > 0;
}

static uint8_t *IogReadFile(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n <= (8 << 20) + 0x200 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *size = d ? (size_t)n : 0;
    return d;
}

static void Activate(void)
{
    char path[1024];
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart)
        return;
    if (!snes_mod_runtime_feature_resource_path_c("iog.randomizer", "randomizer", "seed", path,
                                                  sizeof(path))) {
        fprintf(stderr, "[randomizer] no seed ROM selected\n");
        return;
    }
    size_t size = 0;
    uint8_t *rom = IogReadFile(path, &size);
    const uint8_t *img = rom;
    if (rom && size % 0x8000 == 0x200) {   /* copier header */
        img += 0x200;
        size -= 0x200;
    }
    /* An Illusion of Gaia HiROM image: map mode $31 at $FFD5 and the stock
     * reset vector area intact enough to boot (IoGR keeps both). */
    const bool ok = rom && (size == 0x200000 || size == 0x400000) &&
                    (img[0xFFD5] & 0x0F) == 0x01 && img[0xFFFC] == cart->rom[0xFFFC] &&
                    img[0xFFFD] == cart->rom[0xFFFD];
    if (!ok || !cart_replace_rom(cart, img, (uint32_t)size)) {
        fprintf(stderr, "[randomizer] %s is not an Illusion of Gaia (US) randomizer ROM\n", path);
        free(rom);
        return;
    }
    free(rom);
    g_rom = cart->rom;
    interp_bridge_set_interpret_all(1);
    SnesRa_SetBlocked("a randomizer seed is loaded");
    fprintf(stderr, "[randomizer] playing seed %s (%u KiB), interpreted\n", path,
            (unsigned)(size >> 10));
}

SNES_MOD_CONSTRUCTOR(register_iog_randomizer)
{
    snes_mod_register_activation_plugin("iog.randomizer", Activate);
}
