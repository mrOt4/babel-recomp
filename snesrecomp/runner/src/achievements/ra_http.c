/* ra_http.c -- see ra_http.h. */

#include "ra_http.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SNESRECOMP_RA_CURL)
#include <curl/curl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#define SNESRECOMP_RA_WINHTTP 1
#endif

#if defined(__SWITCH__)
#include <switch.h>
#endif

typedef struct RaReq {
  char *url, *post, *ctype;
  RaHttpDone done;
  void *userdata;
  struct RaReq *next;
} RaReq;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static RaReq *s_head, *s_tail;
static pthread_t s_thread;
static int s_running, s_stop;
static char s_user_agent[256];

static char *dup_or_null(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *d = (char *)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

/* ---- growable body buffer ---------------------------------------------- */
typedef struct { char *data; size_t len, cap; } RaBuf;

static int buf_append(RaBuf *b, const void *p, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n + 1) cap *= 2;
    if (cap > (16u << 20)) return 0;   /* RA answers are small; refuse floods */
    char *d = (char *)realloc(b->data, cap);
    if (!d) return 0;
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
  return 1;
}

/* ---- backends ------------------------------------------------------------ */
#if defined(SNESRECOMP_RA_CURL)
static size_t curl_write(char *p, size_t size, size_t n, void *ud) {
  return buf_append((RaBuf *)ud, p, size * n) ? size * n : 0;
}

static int do_request(const RaReq *r, RaBuf *out) {
  CURL *c = curl_easy_init();
  if (!c) return -1;
  struct curl_slist *hdr = NULL;
  char ct[160];
  curl_easy_setopt(c, CURLOPT_URL, r->url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, s_user_agent);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  if (r->post) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->post);
    snprintf(ct, sizeof(ct), "Content-Type: %s",
             r->ctype && r->ctype[0] ? r->ctype : "application/x-www-form-urlencoded");
    hdr = curl_slist_append(hdr, ct);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  }
  CURLcode rc = curl_easy_perform(c);
  long status = -1;
  if (rc == CURLE_OK)
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  else
    fprintf(stderr, "[ra] http: %s\n", curl_easy_strerror(rc));
  curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  return (int)status;
}

static int net_init(void) {
#if defined(__SWITCH__)
  if (R_FAILED(socketInitializeDefault())) return 0;
#endif
  return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}
static void net_exit(void) {
  curl_global_cleanup();
#if defined(__SWITCH__)
  socketExit();
#endif
}

#elif defined(SNESRECOMP_RA_WINHTTP)
static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static int do_request(const RaReq *r, RaBuf *out) {
  int status = -1;
  wchar_t *wurl = widen(r->url), *wua = widen(s_user_agent);
  URL_COMPONENTS uc;
  wchar_t host[256], path[2048];
  memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host; uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
  wchar_t extra[2048]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
  HINTERNET ses = NULL, con = NULL, req = NULL;
  if (!wurl || !wua || !WinHttpCrackUrl(wurl, 0, 0, &uc)) goto done;
  wcsncat(path, extra, 2047 - wcslen(path));
  ses = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) goto done;
  WinHttpSetTimeouts(ses, 10000, 10000, 30000, 30000);
  con = WinHttpConnect(ses, host, uc.nPort, 0);
  if (!con) goto done;
  req = WinHttpOpenRequest(con, r->post ? L"POST" : L"GET", path, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!req) goto done;
  {
    wchar_t ct[200] = L"";
    DWORD plen = r->post ? (DWORD)strlen(r->post) : 0;
    if (r->post) {
      char cta[160];
      snprintf(cta, sizeof(cta), "Content-Type: %s",
               r->ctype && r->ctype[0] ? r->ctype : "application/x-www-form-urlencoded");
      MultiByteToWideChar(CP_UTF8, 0, cta, -1, ct, 200);
    }
    if (!WinHttpSendRequest(req, r->post ? ct : WINHTTP_NO_ADDITIONAL_HEADERS,
                            r->post ? (DWORD)-1L : 0, r->post ? (LPVOID)r->post : NULL,
                            plen, plen, 0) ||
        !WinHttpReceiveResponse(req, NULL))
      goto done;
  }
  {
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
      DWORD avail = 0, got = 0;
      char chunk[8192];
      if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
      if (avail > sizeof(chunk)) avail = sizeof(chunk);
      if (!WinHttpReadData(req, chunk, avail, &got) || got == 0) break;
      if (!buf_append(out, chunk, got)) break;
    }
    status = (int)code;
  }
done:
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  if (ses) WinHttpCloseHandle(ses);
  free(wurl);
  free(wua);
  return status;
}
static int net_init(void) { return 1; }
static void net_exit(void) {}

#else
static int do_request(const RaReq *r, RaBuf *out) { (void)r; (void)out; return -1; }
static int net_init(void) { return 1; }
static void net_exit(void) {}
#endif

int RaHttp_Available(void) {
#if defined(SNESRECOMP_RA_CURL) || defined(SNESRECOMP_RA_WINHTTP)
  return 1;
#else
  return 0;
#endif
}

/* ---- worker -------------------------------------------------------------- */
static void free_req(RaReq *r) {
  free(r->url);
  free(r->post);
  free(r->ctype);
  free(r);
}

static void *worker(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&s_mu);
    while (!s_head && !s_stop)
      pthread_cond_wait(&s_cv, &s_mu);
    if (s_stop) {
      pthread_mutex_unlock(&s_mu);
      break;
    }
    RaReq *r = s_head;
    s_head = r->next;
    if (!s_head) s_tail = NULL;
    pthread_mutex_unlock(&s_mu);

    RaBuf body = {0};
    buf_append(&body, "", 0);
    int status = do_request(r, &body);
    if (r->done) r->done(status, body.data ? body.data : "", body.len, r->userdata);
    free(body.data);
    free_req(r);
  }
  return NULL;
}

int RaHttp_Init(const char *user_agent) {
  snprintf(s_user_agent, sizeof(s_user_agent), "%s", user_agent ? user_agent : "snesrecomp");
  if (s_running) return 1;
  if (!net_init()) {
    fprintf(stderr, "[ra] network stack unavailable\n");
    return 0;
  }
  s_stop = 0;
  if (pthread_create(&s_thread, NULL, worker, NULL) != 0) {
    net_exit();
    return 0;
  }
  s_running = 1;
  return 1;
}

void RaHttp_Shutdown(void) {
  if (!s_running) return;
  pthread_mutex_lock(&s_mu);
  s_stop = 1;
  pthread_cond_signal(&s_cv);
  pthread_mutex_unlock(&s_mu);
  pthread_join(s_thread, NULL);
  s_running = 0;
  /* Requests nobody will run: tell their owners. */
  RaReq *r = s_head;
  s_head = s_tail = NULL;
  while (r) {
    RaReq *n = r->next;
    if (r->done) r->done(-2, "", 0, r->userdata);
    free_req(r);
    r = n;
  }
  net_exit();
}

void RaHttp_Request(const char *url, const char *post_data, const char *content_type,
                    RaHttpDone done, void *userdata) {
  RaReq *r = (RaReq *)calloc(1, sizeof(RaReq));
  if (!r || !s_running) {
    free(r);
    if (done) done(-1, "", 0, userdata);
    return;
  }
  r->url = dup_or_null(url);
  r->post = dup_or_null(post_data);
  r->ctype = dup_or_null(content_type);
  r->done = done;
  r->userdata = userdata;
  pthread_mutex_lock(&s_mu);
  if (s_tail) s_tail->next = r; else s_head = r;
  s_tail = r;
  pthread_cond_signal(&s_cv);
  pthread_mutex_unlock(&s_mu);
}
