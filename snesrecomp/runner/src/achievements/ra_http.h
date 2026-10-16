#ifndef SNESRECOMP_RA_HTTP_H
#define SNESRECOMP_RA_HTTP_H

/*
 * Asynchronous HTTPS for RetroAchievements.
 *
 * Requests run on one worker thread, in order, so a slow or absent network
 * never stalls the game: an unlock submitted mid-fight costs the frame
 * nothing. The completion callback runs ON THE WORKER THREAD; rc_client
 * locks its own state, which is the one caller.
 *
 * Backends: libcurl (Linux, macOS, Switch -- on Switch the libnx TLS
 * backend, so the console's CA store), WinHTTP (Windows). Where neither is
 * built in, every request completes at once with status -1 ("offline").
 */

#include <stddef.h>

/* status: HTTP status code, or negative for a transport failure. body is
 * NUL-terminated (len excludes the NUL) and freed after the callback. */
typedef void (*RaHttpDone)(int status, const char *body, size_t len, void *userdata);

/* Starts the worker (and the platform's network stack). Idempotent. */
int RaHttp_Init(const char *user_agent);
/* Stops the worker; requests still queued complete with status -2. */
void RaHttp_Shutdown(void);
/* True when this build has a real network backend. */
int RaHttp_Available(void);

/* Queue a GET (post_data NULL) or POST. Strings are copied. */
void RaHttp_Request(const char *url, const char *post_data, const char *content_type,
                    RaHttpDone done, void *userdata);

#endif
