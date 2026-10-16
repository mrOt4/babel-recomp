/* snes_ra.c -- RetroAchievements glue. See snes_ra.h.
 *
 * The client lifecycle follows the ALEKS Switch ports (token-only storage,
 * deferred token login, softcore only, "offline is a state, not an error");
 * the network is asynchronous here (ra_http.c) so a request never costs a
 * frame. */

#include "snes_ra.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include "ra_http.h"
#include "../common_cpu_infra.h"
#include "../common_rtl.h"
#include "../host_paths.h"
#include "../snes_osd.h"
#include "../snes/cart.h"
#include "../snes/snes.h"

static rc_client_t *s_client;
static int s_enabled;
static char s_app_name[64] = "snesrecomp", s_app_version[32] = "0";
static char s_blocked[128];
static char s_hash[33];
static int s_have_game_request;      /* a hash is waiting for login */
static int s_game_loaded;
static uint64_t s_generation;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_status[128] = "Not logged in";
static char s_game[160] = "No game loaded";

static void set_status(const char *fmt, const char *arg) {
  pthread_mutex_lock(&s_mu);
  snprintf(s_status, sizeof(s_status), fmt, arg ? arg : "");
  pthread_mutex_unlock(&s_mu);
}
static void set_game(const char *s) {
  pthread_mutex_lock(&s_mu);
  snprintf(s_game, sizeof(s_game), "%s", s);
  pthread_mutex_unlock(&s_mu);
}

/* ---- token storage ------------------------------------------------------ */
static void token_path(char *out, size_t cap) {
  if (!snesrecomp_exe_dir_path("retroachievements.token", out, cap))
    snprintf(out, cap, "retroachievements.token");
}

static void save_token(const char *user, const char *token) {
  char path[1024];
  token_path(path, sizeof(path));
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "%s\n%s\n", user, token);
  fclose(f);
}

static int load_token(char *user, size_t ucap, char *token, size_t tcap) {
  char path[1024];
  token_path(path, sizeof(path));
  FILE *f = fopen(path, "r");
  int ok = 0;
  if (!f) return 0;
  if (fgets(user, (int)ucap, f) && fgets(token, (int)tcap, f)) {
    user[strcspn(user, "\r\n")] = 0;
    token[strcspn(token, "\r\n")] = 0;
    ok = user[0] && token[0];
  }
  fclose(f);
  return ok;
}

/* ---- memory --------------------------------------------------------------
 * rcheevos' flat SNES space (rc_consoles / consoleinfo.c):
 *   0x000000-0x01FFFF  WRAM, 128 KB ($7E:0000)
 *   0x020000-0x09FFFF  cartridge RAM (the cart's SRAM, as large as it is)
 * A short read tells rc_client it hit a boundary; nothing unmapped is ever
 * invented. */
static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes,
                                     rc_client_t *client) {
  (void)client;
  uint32_t n = 0;
  if (!g_snes || !g_snes->ram) return 0;
  const Cart *cart = g_snes->cart;
  while (n < num_bytes) {
    const uint32_t a = address + n;
    if (a < 0x20000u) {
      buffer[n++] = g_snes->ram[a];
    } else if (a >= 0x20000u && a < 0xA0000u && cart && cart->ram &&
               a - 0x20000u < cart->ramSize) {
      buffer[n++] = cart->ram[a - 0x20000u];
    } else {
      break;
    }
  }
  return n;
}

/* ---- server -------------------------------------------------------------- */
typedef struct {
  rc_client_server_callback_t callback;
  void *callback_data;
} RaPending;

static void http_done(int status, const char *body, size_t len, void *ud) {
  RaPending *p = (RaPending *)ud;
  rc_api_server_response_t resp;
  memset(&resp, 0, sizeof(resp));
  resp.body = body;
  resp.body_length = len;
  /* rc_client's own codes for "no answer at all": retried later. */
  resp.http_status_code = status > 0 ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  p->callback(&resp, p->callback_data);
  free(p);
}

static void RC_CCONV server_call(const rc_api_request_t *req, rc_client_server_callback_t cb,
                                 void *cb_data, rc_client_t *client) {
  (void)client;
  RaPending *p = (RaPending *)malloc(sizeof(RaPending));
  if (!p) return;
  p->callback = cb;
  p->callback_data = cb_data;
  RaHttp_Request(req->url, req->post_data && req->post_data[0] ? req->post_data : NULL,
                 req->content_type, http_done, p);
}

static void RC_CCONV log_message(const char *message, const rc_client_t *client) {
  (void)client;
  fprintf(stderr, "[ra] %s\n", message);
}

/* ---- events --------------------------------------------------------------- */
static void RC_CCONV event_handler(const rc_client_event_t *ev, rc_client_t *client) {
  (void)client;
  char msg[192];
  switch (ev->type) {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
    if (ev->achievement) {
      snprintf(msg, sizeof(msg), "Achievement unlocked: %s (%u)", ev->achievement->title,
               ev->achievement->points);
      snes_osd_push(msg, 5000);
      fprintf(stderr, "[ra] unlocked %u: %s\n", ev->achievement->id, ev->achievement->title);
    }
    break;
  case RC_CLIENT_EVENT_GAME_COMPLETED:
    snes_osd_push("All achievements unlocked!", 6000);
    break;
  case RC_CLIENT_EVENT_SERVER_ERROR:
    if (ev->server_error)
      fprintf(stderr, "[ra] server error (%s): %s\n",
              ev->server_error->api ? ev->server_error->api : "?",
              ev->server_error->error_message ? ev->server_error->error_message : "");
    break;
  case RC_CLIENT_EVENT_DISCONNECTED:
    snes_osd_push("RetroAchievements: offline, unlocks will be sent later", 4000);
    break;
  case RC_CLIENT_EVENT_RECONNECTED:
    snes_osd_push("RetroAchievements: back online", 3000);
    break;
  default:
    break;
  }
}

/* ---- game ----------------------------------------------------------------- */
static void RC_CCONV load_game_done(int result, const char *error, rc_client_t *client,
                                    void *ud) {
  (void)ud;
  char line[160];
  if (result != RC_OK) {
    snprintf(line, sizeof(line), "Not available: %s", error ? error : "unknown game");
    set_game(line);
    fprintf(stderr, "[ra] load game failed (%d): %s\n", result, error ? error : "");
    return;
  }
  const rc_client_game_t *game = rc_client_get_game_info(client);
  rc_client_user_game_summary_t sum;
  memset(&sum, 0, sizeof(sum));
  rc_client_get_user_game_summary(client, &sum);
  snprintf(line, sizeof(line), "%s: %u/%u unlocked",
           game && game->title ? game->title : "Game", sum.num_unlocked_achievements,
           sum.num_promoted_achievements);
  set_game(line);
  s_game_loaded = 1;
  fprintf(stderr, "[ra] game %u loaded, %u achievements\n", game ? game->id : 0u,
          sum.num_promoted_achievements);
  char toast[200];
  snprintf(toast, sizeof(toast), "RetroAchievements: %u of %u unlocked",
           sum.num_unlocked_achievements, sum.num_promoted_achievements);
  snes_osd_push(toast, 4000);
}

static void begin_load_game(void) {
  if (!s_client || !s_have_game_request || s_blocked[0]) return;
  if (!rc_client_get_user_info(s_client)) return;   /* after login */
  s_have_game_request = 0;
  set_game("Loading achievements...");
  rc_client_begin_load_game(s_client, s_hash, load_game_done, NULL);
}

void SnesRa_LoadGameFromFile(const char *rom_path) {
  if (!rom_path || !rom_path[0]) return;
  FILE *f = fopen(rom_path, "rb");
  if (!f) {
    set_game("No ROM to identify");
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *rom = size > 0 && size <= (16 << 20) ? (uint8_t *)malloc((size_t)size) : NULL;
  int ok = rom && fread(rom, 1, (size_t)size, f) == (size_t)size;
  fclose(f);
  /* rcheevos' SNES hasher drops a copier header itself. */
  ok = ok && rc_hash_generate_from_buffer(s_hash, RC_CONSOLE_SUPER_NINTENDO, rom, (size_t)size);
  free(rom);
  if (!ok) {
    set_game("ROM not recognised");
    return;
  }
  fprintf(stderr, "[ra] ROM hash %s\n", s_hash);
  s_have_game_request = 1;
  if (s_blocked[0]) {
    set_game(s_blocked);
    return;
  }
  begin_load_game();
}

void SnesRa_SetBlocked(const char *reason) {
  snprintf(s_blocked, sizeof(s_blocked), "%s", reason ? reason : "");
  if (!s_blocked[0]) return;
  set_game(s_blocked);
  /* Mid-session (e.g. New Game+ applied on a continued file): stop here. */
  if (s_client && s_game_loaded) {
    rc_client_unload_game(s_client);
    s_game_loaded = 0;
    char msg[192];
    snprintf(msg, sizeof(msg), "RetroAchievements off: %s", s_blocked);
    snes_osd_push(msg, 4000);
  }
  s_have_game_request = 0;
}

/* ---- login ---------------------------------------------------------------- */
static void RC_CCONV login_done(int result, const char *error, rc_client_t *client, void *ud) {
  (void)ud;
  if (result != RC_OK) {
    set_status("Login failed: %s", error ? error : "no answer");
    fprintf(stderr, "[ra] login failed (%d): %s\n", result, error ? error : "");
    return;
  }
  const rc_client_user_t *u = rc_client_get_user_info(client);
  if (u && u->username && u->token)
    save_token(u->username, u->token);
  set_status("Logged in as %s", u && u->display_name ? u->display_name : "?");
  fprintf(stderr, "[ra] logged in\n");   /* never the token */
  begin_load_game();
}

void SnesRa_Login(const char *user, const char *password) {
  if (!s_client) SnesRa_Init(s_app_name, s_app_version);
  if (!s_client || !user || !user[0] || !password || !password[0]) return;
  set_status("Logging in...", NULL);
  rc_client_begin_login_with_password(s_client, user, password, login_done, NULL);
}

void SnesRa_Logout(void) {
  char path[1024];
  if (s_client) rc_client_logout(s_client);
  token_path(path, sizeof(path));
  remove(path);
  s_game_loaded = 0;
  set_status("Not logged in", NULL);
  set_game("No game loaded");
}

int SnesRa_LoggedIn(void) {
  return s_client && rc_client_get_user_info(s_client) != NULL;
}

/* ---- lifecycle -------------------------------------------------------------- */
void SnesRa_SetEnabled(int enabled) {
  s_enabled = enabled != 0;
  if (!s_enabled && s_client)
    SnesRa_Shutdown();
}

int SnesRa_Enabled(void) { return s_enabled; }

void SnesRa_Init(const char *app_name, const char *app_version) {
  if (app_name) snprintf(s_app_name, sizeof(s_app_name), "%s", app_name);
  if (app_version) snprintf(s_app_version, sizeof(s_app_version), "%s", app_version);
  if (s_client) return;
  if (!RaHttp_Available()) {
    set_status("Not available in this build (no network support)", NULL);
    return;
  }
  s_client = rc_client_create(read_memory, server_call);
  if (!s_client) {
    set_status("Unavailable", NULL);
    return;
  }
  char ua[256], clause[128];
  rc_client_get_user_agent_clause(s_client, clause, sizeof(clause));
  snprintf(ua, sizeof(ua), "%s/%s %s", s_app_name, s_app_version, clause);
  if (!RaHttp_Init(ua)) {
    rc_client_destroy(s_client);
    s_client = NULL;
    set_status("Network unavailable", NULL);
    return;
  }
  rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
  rc_client_set_hardcore_enabled(s_client, 0);   /* softcore, always: see header */
  rc_client_set_event_handler(s_client, event_handler);
  rc_client_set_allow_background_memory_reads(s_client, 0);
  s_generation = RtlStateGeneration();
  char user[128], token[256];
  if (load_token(user, sizeof(user), token, sizeof(token))) {
    set_status("Signing in...", NULL);
    rc_client_begin_login_with_token(s_client, user, token, login_done, NULL);
  } else {
    set_status("Not logged in", NULL);
  }
}

void SnesRa_Shutdown(void) {
  if (!s_client) return;
  /* Network first: completions still queued call back into the client. */
  RaHttp_Shutdown();
  rc_client_destroy(s_client);
  s_client = NULL;
  s_game_loaded = 0;
}

void SnesRa_Frame(void) {
  if (!s_client) return;
  /* A reset or a save-state load moved the game somewhere new: start every
   * achievement's tracking over, so nothing triggers off the jump itself.
   * Run-ahead rollbacks keep the generation and do not reach here. */
  const uint64_t gen = RtlStateGeneration();
  if (gen != s_generation) {
    s_generation = gen;
    if (s_game_loaded) rc_client_reset(s_client);
  }
  rc_client_do_frame(s_client);
}

void SnesRa_Idle(void) {
  if (s_client) rc_client_idle(s_client);
}

void SnesRa_Status(char *out, size_t cap) {
  pthread_mutex_lock(&s_mu);
  snprintf(out, cap, "%s", s_enabled || s_client ? s_status : "Off");
  pthread_mutex_unlock(&s_mu);
}

void SnesRa_GameStatus(char *out, size_t cap) {
  pthread_mutex_lock(&s_mu);
  snprintf(out, cap, "%s", s_game);
  pthread_mutex_unlock(&s_mu);
}
