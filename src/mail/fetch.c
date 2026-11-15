#include "fetch.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/util.h"

typedef struct {
    char *url;
    int ok;
} done;

struct jd_fetcher {
    char *dir;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int quit;
    char **queue;
    int nqueue;
    char **seen; /* queued, fetched or failed: never asked twice in a session */
    int nseen;
    done *results;
    int nresults;
};

char *jd_fetch_cache_path(jd_fetcher *f, const char *url)
{
    unsigned char hash[32];
    unsigned int len = 0;
    EVP_Digest(url, strlen(url), hash, &len, EVP_sha256(), NULL);
    char hex[65];
    for (unsigned i = 0; i < len && i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);
    return jd_printf("%s/%s", f->dir, hex);
}

typedef struct {
    jd_buf data;
    int too_big;
} sink;

static size_t on_data(char *ptr, size_t size, size_t n, void *userdata)
{
    sink *s = userdata;
    if (s->data.len + size * n > JD_FETCH_MAX) {
        s->too_big = 1;
        return 0;
    }
    jd_buf_add(&s->data, ptr, size * n);
    return size * n;
}

static int fetch_one(jd_fetcher *f, const char *url)
{
    CURL *c = curl_easy_init();
    if (!c) return 0;
    sink s = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)JD_FETCH_MAX);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "jackdaw");
    /* named, as for IMAP (jd_tls_trust): OpenSSL ignores these variables in an Android app */
    const char *file = getenv("SSL_CERT_FILE"), *dir = getenv("SSL_CERT_DIR");
    if (file && file[0]) curl_easy_setopt(c, CURLOPT_CAINFO, file);
    if (dir && dir[0]) curl_easy_setopt(c, CURLOPT_CAPATH, dir);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    CURLcode r = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    int ok = r == CURLE_OK && status >= 200 && status < 300 && s.data.len > 0 && !s.too_big;
    if (ok) {
        char *path = jd_fetch_cache_path(f, url);
        ok = jd_write_file(path, s.data.s, s.data.len, 0600) == 0;
        free(path);
    }
    jd_buf_free(&s.data);
    return ok;
}

static void *run(void *arg)
{
    jd_fetcher *f = arg;
    pthread_mutex_lock(&f->lock);
    while (!f->quit) {
        if (!f->nqueue) {
            pthread_cond_wait(&f->wake, &f->lock);
            continue;
        }
        char *url = f->queue[0];
        memmove(f->queue, f->queue + 1, sizeof *f->queue * (size_t)--f->nqueue);
        pthread_mutex_unlock(&f->lock);
        int ok = fetch_one(f, url);
        pthread_mutex_lock(&f->lock);
        f->results = jd_realloc(f->results, sizeof *f->results * (size_t)(f->nresults + 1));
        f->results[f->nresults++] = (done){url, ok};
    }
    pthread_mutex_unlock(&f->lock);
    return NULL;
}

static void init_curl(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

jd_fetcher *jd_fetcher_start(const char *cache_dir)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_curl);
    jd_fetcher *f = jd_alloc(sizeof *f);
    f->dir = jd_strdup(cache_dir);
    jd_mkdirs(f->dir);
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->wake, NULL);
    if (pthread_create(&f->thread, NULL, run, f) != 0) abort();
    return f;
}

void jd_fetcher_stop(jd_fetcher *f)
{
    if (!f) return;
    pthread_mutex_lock(&f->lock);
    f->quit = 1;
    pthread_cond_signal(&f->wake);
    pthread_mutex_unlock(&f->lock);
    pthread_join(f->thread, NULL);
    for (int i = 0; i < f->nqueue; i++) free(f->queue[i]);
    for (int i = 0; i < f->nseen; i++) free(f->seen[i]);
    for (int i = 0; i < f->nresults; i++) free(f->results[i].url);
    free(f->queue), free(f->seen), free(f->results), free(f->dir);
    pthread_mutex_destroy(&f->lock);
    pthread_cond_destroy(&f->wake);
    free(f);
}

void jd_fetch(jd_fetcher *f, const char *url)
{
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return;
    pthread_mutex_lock(&f->lock);
    for (int i = 0; i < f->nseen; i++)
        if (strcmp(f->seen[i], url) == 0) {
            pthread_mutex_unlock(&f->lock);
            return;
        }
    f->seen = jd_realloc(f->seen, sizeof *f->seen * (size_t)(f->nseen + 1));
    f->seen[f->nseen++] = jd_strdup(url);
    char *path = jd_fetch_cache_path(f, url);
    int cached = access(path, R_OK) == 0;
    free(path);
    if (cached) {
        f->results = jd_realloc(f->results, sizeof *f->results * (size_t)(f->nresults + 1));
        f->results[f->nresults++] = (done){jd_strdup(url), 1};
    } else {
        f->queue = jd_realloc(f->queue, sizeof *f->queue * (size_t)(f->nqueue + 1));
        f->queue[f->nqueue++] = jd_strdup(url);
        pthread_cond_signal(&f->wake);
    }
    pthread_mutex_unlock(&f->lock);
}

int jd_fetch_poll(jd_fetcher *f, char **url, int *ok)
{
    pthread_mutex_lock(&f->lock);
    int have = f->nresults > 0;
    if (have) {
        *url = f->results[0].url;
        *ok = f->results[0].ok;
        memmove(f->results, f->results + 1, sizeof *f->results * (size_t)--f->nresults);
    }
    pthread_mutex_unlock(&f->lock);
    return have;
}
