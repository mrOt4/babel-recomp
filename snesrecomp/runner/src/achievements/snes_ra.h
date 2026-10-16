#ifndef SNESRECOMP_SNES_RA_H
#define SNESRECOMP_SNES_RA_H

/*
 * RetroAchievements for snesrecomp ports (rcheevos rc_client).
 *
 * SOFTCORE ONLY. Hardcore's contract forbids save states, rewind, run-ahead
 * and cheats; this framework offers all of them, so hardcore would be a
 * false claim to the RetroAchievements service. rc_client is created with
 * hardcore off and nothing turns it on.
 *
 * Identification hashes the player's own ROM FILE (never the patched copy in
 * memory), so mods that rewrite the cartridge image at run time do not
 * change what the game is recognised as. Only the session token is stored
 * (retroachievements.token beside the executable), never the password.
 * No network, a server error or an unknown ROM never blocks the game.
 *
 * Threading: everything here is called from the game/UI thread; network
 * completions arrive on ra_http's worker and only touch rc_client (which
 * locks itself) and this module's status strings (behind a mutex).
 */

#include <stddef.h>
#include <stdint.h>

/* Brings the client up (idempotent). `app_name`/`app_version` go in the
 * User-Agent the service requires. Logs in with the stored token, if any. */
void SnesRa_Init(const char *app_name, const char *app_version);
void SnesRa_Shutdown(void);

/* Enabled = the player wants achievements (config). Off: no client, no
 * network traffic at all. */
void SnesRa_SetEnabled(int enabled);
int SnesRa_Enabled(void);

/* The game: hashes `rom_path` and starts loading its achievements once
 * logged in. Call once the ROM is known, before the first frame. */
void SnesRa_LoadGameFromFile(const char *rom_path);

/* A game that knows its session is not a fair one (cheats, a randomizer...)
 * says so before the first frame: achievements are then not loaded, and the
 * reason is shown instead. NULL clears it. */
void SnesRa_SetBlocked(const char *reason);

/* Once per REAL emulated frame (not run-ahead's speculative ones). Notices
 * resets and save-state loads by itself (RtlStateGeneration). */
void SnesRa_Frame(void);
/* While the game is paused or a menu owns the screen. */
void SnesRa_Idle(void);

/* Account (launcher / settings). Asynchronous: watch SnesRa_Status(). */
void SnesRa_Login(const char *user, const char *password);
void SnesRa_Logout(void);
int SnesRa_LoggedIn(void);
/* One line for a settings page, e.g. "Logged in as Foo" / "Login failed". */
void SnesRa_Status(char *out, size_t cap);
/* "Illusion of Gaia: 12/45 unlocked", or why there is no game. */
void SnesRa_GameStatus(char *out, size_t cap);

#endif
