#define _GNU_SOURCE
#include "oauth.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/util.h"

/* jackdaw's registration with Google, a "Desktop app" client. Google treats an installed app's secret as public
   (developers.google.com/identity/protocols/oauth2/native-app): PKCE and the loopback redirect protect the code. */
static const char client_id[] = "322585932063-c9crstu4qjevruf83dp3mhrlpvi8cib1.apps.googleusercontent.com";
static const char client_secret[] = "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";

static const char auth_url[] = "https://accounts.google.com/o/oauth2/v2/auth";
static const char token_url[] = "https://oauth2.googleapis.com/token";
static const char scope[] = "https://www.googleapis.com/auth/calendar";

enum { WAIT_SECONDS = 10 * 60 };

struct jd_oauth {
    int sock, port;
    char verifier[64], state[32];
    pthread_t thread;
    pthread_mutex_t lock;
    int quit, result;
    char *refresh;
    char error[512];
};

static void base64url(const unsigned char *in, int n, char *out)
{
    unsigned char b64[128];
    int len = EVP_EncodeBlock(b64, in, n);
    int k = 0;
    for (int i = 0; i < len; i++) {
        char c = (char)b64[i];
        if (c == '=') break;
        out[k++] = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    out[k] = 0;
}

static void random_text(char *out, int bytes)
{
    unsigned char b[48];
    if (RAND_bytes(b, bytes) != 1)
        for (int i = 0; i < bytes; i++) b[i] = (unsigned char)rand();
    base64url(b, bytes, out);
}

typedef struct {
    jd_buf body;
} sink;

static size_t on_body(char *p, size_t size, size_t n, void *data)
{
    sink *s = data;
    if (s->body.len + size * n > 1024 * 1024) return 0;
    jd_buf_add(&s->body, p, size * n);
    return size * n;
}

/* a form POST to Google's token endpoint; the parsed JSON answer, NULL with err when none came */
static cJSON *token_post(const char *form, long *status, char *err, int errlen)
{
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, (size_t)errlen, "no HTTP client");
        return NULL;
    }
    sink s = {0};
    curl_easy_setopt(c, CURLOPT_URL, token_url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "jackdaw");
    const char *file = getenv("SSL_CERT_FILE"), *dir = getenv("SSL_CERT_DIR");
    if (file && file[0]) curl_easy_setopt(c, CURLOPT_CAINFO, file);
    if (dir && dir[0]) curl_easy_setopt(c, CURLOPT_CAPATH, dir);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    CURLcode r = curl_easy_perform(c);
    *status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) {
        snprintf(err, (size_t)errlen, "Google: %s", curl_easy_strerror(r));
        jd_buf_free(&s.body);
        return NULL;
    }
    cJSON *j = s.body.s ? cJSON_Parse(s.body.s) : NULL;
    if (s.body.s) jd_wipe(s.body.s, s.body.len);
    jd_buf_free(&s.body);
    if (!j) snprintf(err, (size_t)errlen, "Google answered HTTP %ld without JSON", *status);
    return j;
}

static const char *json_str(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static char *form_escape(const char *s)
{
    char *e = curl_easy_escape(NULL, s, 0);
    char *out = jd_strdup(e ? e : "");
    curl_free(e);
    return out;
}

/* the code of the redirect to the token Google keeps for jackdaw */
static int exchange(jd_oauth *o, const char *code, char *err, int errlen)
{
    char *redirect = jd_printf("http://127.0.0.1:%d", o->port);
    char *c = form_escape(code), *r = form_escape(redirect), *id = form_escape(client_id), *sec = form_escape(client_secret);
    char *form = jd_printf("grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&client_secret=%s"
                           "&code_verifier=%s",
                           c, r, id, sec, o->verifier);
    free(redirect), free(c), free(r), free(id), free(sec);
    long status;
    cJSON *j = token_post(form, &status, err, errlen);
    jd_wipe(form, strlen(form));
    free(form);
    if (!j) return -1;
    const char *refresh = json_str(j, "refresh_token");
    int ok = refresh != NULL;
    if (ok) {
        pthread_mutex_lock(&o->lock);
        o->refresh = jd_strdup(refresh);
        pthread_mutex_unlock(&o->lock);
    } else {
        const char *why = json_str(j, "error_description"), *code = json_str(j, "error");
        snprintf(err, (size_t)errlen, "Google refused the sign-in: %s%s%s%s", code ? code : "no token", why ? " (" : "",
                 why ? why : "", why ? ")" : "");
    }
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static char *query_param(const char *query, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = query; p && *p;) {
        if (strncmp(p, name, n) == 0 && p[n] == '=') {
            const char *v = p + n + 1;
            size_t len = strcspn(v, "& ");
            int outlen = 0;
            char *raw = curl_easy_unescape(NULL, v, (int)len, &outlen);
            char *out = raw ? jd_strndup(raw, (size_t)outlen) : NULL;
            curl_free(raw);
            return out;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

static void reply(int fd, int ok, const char *text)
{
    char *page = jd_printf("<!doctype html><meta charset=utf-8><title>jackdaw</title>"
                           "<body style=\"font:16px sans-serif;margin:3em\"><h2>%s</h2><p>%s</p></body>",
                           ok ? "Signed in" : "Not signed in", text);
    char *head = jd_printf("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                           "Connection: close\r\n\r\n",
                           ok ? "200 OK" : "400 Bad Request", strlen(page));
    if (write(fd, head, strlen(head)) < 0 || write(fd, page, strlen(page)) < 0) {}
    free(page), free(head);
}

static void finish(jd_oauth *o, int result, const char *err)
{
    pthread_mutex_lock(&o->lock);
    o->result = result;
    if (err) snprintf(o->error, sizeof o->error, "%s", err);
    pthread_mutex_unlock(&o->lock);
}

static int stopping(jd_oauth *o)
{
    pthread_mutex_lock(&o->lock);
    int q = o->quit;
    pthread_mutex_unlock(&o->lock);
    return q;
}

static void *listen_thread(void *arg)
{
    jd_oauth *o = arg;
    int64_t until = jd_now() + WAIT_SECONDS;
    while (!stopping(o)) {
        if (jd_now() > until) {
            finish(o, JD_OAUTH_FAILED, "The sign-in was not finished in the browser within 10 minutes.");
            return NULL;
        }
        struct pollfd p = {o->sock, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        int fd = accept(o->sock, NULL, NULL);
        if (fd < 0) continue;
        char req[4096];
        size_t len = 0;
        struct pollfd pf = {fd, POLLIN, 0};
        while (len < sizeof req - 1 && poll(&pf, 1, 2000) > 0) {
            ssize_t r = read(fd, req + len, sizeof req - 1 - len);
            if (r <= 0) break;
            len += (size_t)r;
            req[len] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[len] = 0;
        /* "GET /?state=...&code=... HTTP/1.1" */
        char *line_end = strstr(req, "\r\n");
        if (line_end) *line_end = 0;
        char *q = strncmp(req, "GET /", 5) == 0 ? strchr(req, '?') : NULL;
        char *state = q ? query_param(q + 1, "state") : NULL;
        char *code = q ? query_param(q + 1, "code") : NULL;
        char *error = q ? query_param(q + 1, "error") : NULL;
        if (!state || strcmp(state, o->state) != 0) {
            /* the browser's favicon or anything else that is not Google's redirect */
            if (!q) {
                const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (write(fd, nf, strlen(nf)) < 0) {}
            } else {
                reply(fd, 0, "This answer was not for the sign-in jackdaw started.");
            }
            close(fd);
            free(state), free(code), free(error);
            continue;
        }
        char err[512] = "";
        int ok = 0;
        if (code) ok = exchange(o, code, err, sizeof err) == 0;
        else snprintf(err, sizeof err, "Google: %s", error ? error : "no code");
        if (error && strcmp(error, "access_denied") == 0) snprintf(err, sizeof err, "The access was not allowed.");
        reply(fd, ok, ok ? "jackdaw can read and change your calendars now. You can close this tab." : err);
        close(fd);
        free(state), free(code), free(error);
        finish(o, ok ? JD_OAUTH_DONE : JD_OAUTH_FAILED, ok ? NULL : err);
        return NULL;
    }
    return NULL;
}

jd_oauth *jd_oauth_start(const char *login_hint, char **url, char *err, int errlen)
{
    *url = NULL;
    jd_oauth *o = jd_alloc(sizeof *o);
    o->sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t alen = sizeof a;
    if (o->sock < 0 || bind(o->sock, (struct sockaddr *)&a, sizeof a) != 0 || listen(o->sock, 4) != 0 ||
        getsockname(o->sock, (struct sockaddr *)&a, &alen) != 0) {
        snprintf(err, (size_t)errlen, "No port for the sign-in to return to: %s", strerror(errno));
        if (o->sock >= 0) close(o->sock);
        free(o);
        return NULL;
    }
    o->port = ntohs(a.sin_port);
    random_text(o->verifier, 32);
    random_text(o->state, 16);
    unsigned char hash[32];
    unsigned int hlen = 0;
    EVP_Digest(o->verifier, strlen(o->verifier), hash, &hlen, EVP_sha256(), NULL);
    char challenge[64];
    base64url(hash, (int)hlen, challenge);
    char *redirect = jd_printf("http://127.0.0.1:%d", o->port);
    char *r = form_escape(redirect), *s = form_escape(scope), *id = form_escape(client_id);
    char *hint = login_hint && login_hint[0] ? form_escape(login_hint) : NULL;
    *url = jd_printf("%s?client_id=%s&redirect_uri=%s&response_type=code&scope=%s&code_challenge=%s"
                     "&code_challenge_method=S256&state=%s&access_type=offline&prompt=consent%s%s",
                     auth_url, id, r, s, challenge, o->state, hint ? "&login_hint=" : "", hint ? hint : "");
    free(redirect), free(r), free(s), free(id), free(hint);
    pthread_mutex_init(&o->lock, NULL);
    if (pthread_create(&o->thread, NULL, listen_thread, o) != 0) {
        snprintf(err, (size_t)errlen, "No thread for the sign-in");
        close(o->sock);
        pthread_mutex_destroy(&o->lock);
        free(*url);
        *url = NULL;
        free(o);
        return NULL;
    }
    return o;
}

int jd_oauth_poll(jd_oauth *o, char **refresh_token, char *err, int errlen)
{
    pthread_mutex_lock(&o->lock);
    int r = o->result;
    if (r == JD_OAUTH_DONE) {
        *refresh_token = o->refresh;
        o->refresh = NULL;
    } else if (r == JD_OAUTH_FAILED) {
        snprintf(err, (size_t)errlen, "%s", o->error);
    }
    pthread_mutex_unlock(&o->lock);
    return r;
}

void jd_oauth_stop(jd_oauth *o)
{
    if (!o) return;
    pthread_mutex_lock(&o->lock);
    o->quit = 1;
    pthread_mutex_unlock(&o->lock);
    pthread_join(o->thread, NULL);
    close(o->sock);
    if (o->refresh) jd_wipe(o->refresh, strlen(o->refresh));
    free(o->refresh);
    pthread_mutex_destroy(&o->lock);
    free(o);
}

int jd_oauth_refresh(const char *refresh_token, char **access, int64_t *expires, char *err, int errlen)
{
    *access = NULL;
    char *t = form_escape(refresh_token), *id = form_escape(client_id), *sec = form_escape(client_secret);
    char *form = jd_printf("grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s", t, id, sec);
    jd_wipe(t, strlen(t));
    free(t), free(id), free(sec);
    long status;
    cJSON *j = token_post(form, &status, err, errlen);
    jd_wipe(form, strlen(form));
    free(form);
    if (!j) return JD_OAUTH_FAILED;
    const char *tok = json_str(j, "access_token");
    const cJSON *life = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
    int r = JD_OAUTH_DONE;
    if (tok) {
        *access = jd_strdup(tok);
        *expires = jd_now() + (cJSON_IsNumber(life) ? (int64_t)life->valuedouble : 3600);
    } else {
        const char *e = json_str(j, "error");
        /* RFC 6749 5.2: the grant is no longer good; anything else may pass */
        r = e && strcmp(e, "invalid_grant") == 0 ? JD_OAUTH_REVOKED : JD_OAUTH_FAILED;
        const char *why = json_str(j, "error_description");
        snprintf(err, (size_t)errlen, "Google: %s%s%s%s",
                 r == JD_OAUTH_REVOKED ? "the sign-in is no longer valid" : e ? e : "no access token", why ? " (" : "",
                 why ? why : "", why ? ")" : "");
    }
    cJSON_Delete(j);
    return r;
}
