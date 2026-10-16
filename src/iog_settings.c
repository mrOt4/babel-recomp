/*
 * Illusion of Gaia's items in the in-game settings menu
 * (snes_settings_menu.h). Each change applies at once where the game can
 * take it mid-session, and is written to the mod selection (state.toml) so
 * the launcher and the next launch agree.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mod_runtime.h"
#include "present_filter.h"
#include "snes_overlay_draw.h"
#include "snes_settings_menu.h"
#include "snes/msu1.h"

#include "iog_i18n.h"
#include "iog_randomizer.h"

int IogHoldAdvanceGet(void);
void IogHoldAdvanceSet(int v);

/* ---- HD scaling (iog.graphics/hd) ---------------------------------------- */
static const char *const kScaling[] = {"ORIGINAL", "SHARP PIXELS", "SMOOTH (XBR)"};

static int ScalingGet(void *ud) { (void)ud; return PresentFilter_Get(); }
static void ScalingSet(void *ud, int v)
{
    (void)ud;
    PresentFilter_Set(v);
    snes_mod_runtime_persist_feature_c("iog.graphics", "hd", v != 0, v ? "filter" : NULL,
                                       v == 2 ? "smooth" : "sharp");
}

/* ---- hold to advance (iog.enhancements/hold_advance) ---------------------- */
static const char *const kAdvance[] = {"OFF", "SLOW", "NORMAL", "FAST"};
static const char *const kAdvanceIds[] = {"normal", "slow", "normal", "fast"};

static int AdvanceGet(void *ud) { (void)ud; return IogHoldAdvanceGet(); }
static void AdvanceSet(void *ud, int v)
{
    (void)ud;
    IogHoldAdvanceSet(v);
    snes_mod_runtime_persist_feature_c("iog.enhancements", "hold_advance", v != 0,
                                       v ? "speed" : NULL, kAdvanceIds[v & 3]);
}

/* ---- language (iog.languages/language) ------------------------------------ */
static const char *const kLangNames[] = {"ENGLISH", "ESPAÑOL", "FRANÇAIS", "DEUTSCH"};
static const char *const kLangCodes[] = {"original", "es", "fr", "de"};
static int g_lang_choice = -1;

static int LangGet(void *ud)
{
    (void)ud;
    if (g_lang_choice >= 0)
        return g_lang_choice;
    const char *cur = IogI18nLanguage();
    for (int i = 0; i < 4; i++)
        if (!strcmp(cur, kLangCodes[i]))
            return i;
    return 0;
}
static void LangSet(void *ud, int v)
{
    (void)ud;
    /* Skip languages whose ROM is not in languages/ (or chosen on the Mods
     * page): Left/Right steps over them. */
    const int start = LangGet(NULL);
    const int dir = ((v - start + 4) % 4) == 3 ? -1 : 1;
    for (int n = 0; n < 4 && !IogI18nAvailable(kLangCodes[v]); n++)
        v = (v + dir + 4) % 4;
    if (!IogI18nAvailable(kLangCodes[v]) || IogRandomizerActive())
        return;
    g_lang_choice = v;
    IogI18nSetLanguage(kLangCodes[v]);
    snes_mod_runtime_persist_feature_c("iog.languages", "language", v != 0,
                                       v ? "code" : NULL, kLangCodes[v]);
}

/* ---- MSU-1 (iog.enhancements/msu1): next launch --------------------------- */
static int g_msu = -1;
static int MsuGet(void *ud)
{
    (void)ud;
    if (g_msu < 0)
        g_msu = snes_mod_runtime_feature_enabled_c("iog.enhancements", "msu1") > 0;
    return g_msu;
}
static void MsuSet(void *ud, int v)
{
    (void)ud;
    g_msu = v != 0;
    snes_mod_runtime_persist_feature_c("iog.enhancements", "msu1", g_msu, NULL, NULL);
}

/* ---- cheats (iog.cheats): live ---------------------------------------------- */
int IogCheatGet(int which);
void IogCheatSet(int which, int on);
static int CheatGet(void *ud) { return IogCheatGet((int)(intptr_t)ud); }
static void CheatSet(void *ud, int v) { IogCheatSet((int)(intptr_t)ud, v != 0); }

/* ---- the in-game menus in the game's language -------------------------------
 * English is the source; Spanish while the game runs in Spanish. */
static const char *const kEs[][2] = {
    {"SETTINGS", "AJUSTES"}, {"SELECT", "ELEGIR"}, {"LEFT/RIGHT CHANGE", "IZQ/DER CAMBIA"},
    {"BACK TO GAME", "VOLVER AL JUEGO"}, {"APPLIES ON THE NEXT LAUNCH", "SE APLICA AL REINICIAR"},
    {"PRESS A AGAIN", "PULSA A OTRA VEZ"}, {"ON", "SÍ"}, {"OFF", "NO"},
    {"DISPLAY", "PANTALLA"}, {"FULLSCREEN", "PANTALLA COMPLETA"}, {"FPS COUNTER", "CONTADOR DE FPS"}, {"HD MODE 7", "MODO 7 EN HD"},
    {"COMPANION SCREEN", "PANTALLA COMPAÑERA"}, {"DUAL", "DOBLE"}, {"FLIP GRIP", "FLIP GRIP"},
    {"FLIP GRIP R", "FLIP GRIP R"}, {"AUDIO", "SONIDO"}, {"VOLUME", "VOLUMEN"},
    {"RETROACHIEVEMENTS", "RETROACHIEVEMENTS"}, {"ACHIEVEMENTS", "LOGROS"},
    {"SYSTEM", "SISTEMA"}, {"SAVE STATES", "ESTADOS GUARDADOS"}, {"RESET GAME", "REINICIAR JUEGO"},
    {"QUIT GAME", "SALIR DEL JUEGO"}, {"HD SCALING", "ESCALADO HD"}, {"ORIGINAL", "ORIGINAL"},
    {"SHARP PIXELS", "PÍXELES NÍTIDOS"}, {"SMOOTH (XBR)", "SUAVE (XBR)"}, {"LANGUAGE", "IDIOMA"},
    {"HOLD TO ADVANCE TEXT", "MANTENER PARA AVANZAR"}, {"SLOW", "LENTO"}, {"NORMAL", "NORMAL"},
    {"FAST", "RÁPIDO"}, {"MSU-1 SOUNDTRACK", "BANDA SONORA MSU-1"},
    {"CHEATS (ACHIEVEMENTS OFF)", "TRUCOS (SIN LOGROS)"}, {"INFINITE HP", "VIDA INFINITA"},
    {"DP 99", "DP 99"}, {"STATS BOOST", "MEJORA DE ESTADÍSTICAS"}, {"ALL 50 RED JEWELS", "LAS 50 JOYAS ROJAS"}, {"GAME COMPLETED", "JUEGO COMPLETADO"},
};

static const char *Translate(const char *en)
{
    if (strcmp(IogI18nLanguage(), "es"))
        return NULL;
    for (size_t i = 0; i < sizeof(kEs) / sizeof(kEs[0]); i++)
        if (!strcmp(kEs[i][0], en))
            return kEs[i][1];
    return NULL;
}

#include "iog_inventory.h"
void SnesRa_SetBlocked(const char *why);
static void ActJewels(void *ud)
{
    (void)ud;
    if (IogInventoryAllRedJewels() >= 0)
        SnesRa_SetBlocked("cheats are on");
}

int IogCompletionPercent(void);
static void CompletionText(void *ud, int v, char *out, size_t cap)
{
    (void)ud; (void)v;
    const int pct = IogCompletionPercent();
    if (pct < 0) snprintf(out, cap, "-");
    else snprintf(out, cap, "%d%%", pct);
}

void IogSettingsInstall(void)
{
    snes_ui_set_translator(Translate);
    SnesSettingItem it;
    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Header;
    it.label = "ILLUSION OF GAIA";
    snes_settings_menu_add(&it);

    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Info;
    it.label = "GAME COMPLETED";
    it.value_text = CompletionText;
    snes_settings_menu_add(&it);

    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Choice;
    it.label = "HD SCALING";
    it.choices = kScaling;
    it.count = 3;
    it.get = ScalingGet;
    it.set = ScalingSet;
    snes_settings_menu_add(&it);

    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Choice;
    it.label = "LANGUAGE";
    it.choices = kLangNames;
    it.count = 4;
    it.get = LangGet;
    it.set = LangSet;
    snes_settings_menu_add(&it);

    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Choice;
    it.label = "HOLD TO ADVANCE TEXT";
    it.choices = kAdvance;
    it.count = 4;
    it.get = AdvanceGet;
    it.set = AdvanceSet;
    snes_settings_menu_add(&it);

    if (msu1_available()) {
        memset(&it, 0, sizeof(it));
        it.kind = kSnesSetting_Toggle;
        it.label = "MSU-1 SOUNDTRACK";
        it.get = MsuGet;
        it.set = MsuSet;
        it.restart = 1;
        snes_settings_menu_add(&it);
    }

    static const char *const kCheatLabels[] = {"INFINITE HP", "DP 99", "STATS BOOST"};
    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Header;
    it.label = "CHEATS (ACHIEVEMENTS OFF)";
    snes_settings_menu_add(&it);
    for (int i = 0; i < 3; i++) {
        memset(&it, 0, sizeof(it));
        it.kind = kSnesSetting_Toggle;
        it.label = kCheatLabels[i];
        it.get = CheatGet;
        it.set = CheatSet;
        it.ud = (void *)(intptr_t)i;
        snes_settings_menu_add(&it);
    }
    memset(&it, 0, sizeof(it));
    it.kind = kSnesSetting_Action;
    it.label = "ALL 50 RED JEWELS";
    it.action = ActJewels;
    it.confirm = 1;
    snes_settings_menu_add(&it);
}
