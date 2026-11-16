#include "release.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"

#if defined(__ANDROID__)
#define ASSET_SUFFIX ".apk"
#elif defined(__x86_64__)
#define ASSET_SUFFIX "/jackdaw-linux-x86_64"
#elif defined(__aarch64__)
#define ASSET_SUFFIX "/jackdaw-linux-aarch64"
#else
#define ASSET_SUFFIX "/jackdaw-linux"
#endif

/* the string after "key": from p on; GitHub's tags, URLs and digests hold no escapes */
static char *json_string(const char *p, const char *key, const char **after)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(p, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '"') return NULL;
    const char *end = strchr(++p, '"');
    if (!end) return NULL;
    if (after) *after = end + 1;
    return jd_strndup(p, (size_t)(end - p));
}

int jd_release_parse(const char *json, jd_release *out)
{
    *out = (jd_release){0};
    out->tag = json_string(json, "tag_name", NULL);
    if (!out->tag) return -1;
    const char *assets = strstr(json, "\"assets\"");
    char *page = json_string(json, "html_url", NULL);
    if (page && (!assets || strstr(json, page) < assets)) out->page = page;
    else free(page);
    /* an asset's digest comes before its download URL, within the same object */
    const char *p = assets ? assets : json, *start = p;
    char *url;
    while (!out->asset && (url = json_string(p, "browser_download_url", &p))) {
        size_t n = strlen(url), sn = strlen(ASSET_SUFFIX);
        if (n > sn && !strcmp(url + n - sn, ASSET_SUFFIX)) {
            out->asset = url;
            const char *d = start, *found = NULL, *next;
            while ((next = strstr(d, "\"digest\"")) && next < p) found = next, d = next + 1;
            char *digest = found ? json_string(found, "digest", NULL) : NULL;
            if (digest && !strncasecmp(digest, "sha256:", 7)) out->sha256 = jd_strdup(digest + 7);
            free(digest);
        } else {
            free(url);
        }
        start = p;
    }
    return 0;
}

void jd_release_free(jd_release *r)
{
    free(r->tag), free(r->page), free(r->asset), free(r->sha256);
    *r = (jd_release){0};
}

static void version_parts(const char *v, long out[3])
{
    if (*v == 'v' || *v == 'V') v++;
    for (int i = 0; i < 3; i++) {
        char *end;
        out[i] = strtol(v, &end, 10);
        if (*end != '.') {
            for (int k = i + 1; k < 3; k++) out[k] = 0;
            return;
        }
        v = end + 1;
    }
}

int jd_version_cmp(const char *a, const char *b)
{
    long x[3], y[3];
    version_parts(a, x), version_parts(b, y);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    /* SemVer 2.0 section 11: a pre-release ("0.2.2-dev+...") comes before its release ("0.2.2") */
    int pa = strchr(a, '-') != NULL, pb = strchr(b, '-') != NULL;
    return pa == pb ? 0 : pa ? -1 : 1;
}

static CURL *easy(const char *url, long timeout)
{
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "jackdaw");
    /* OpenSSL ignores these variables in an Android app (jd_tls_trust) */
    const char *file = getenv("SSL_CERT_FILE"), *dir = getenv("SSL_CERT_DIR");
    if (file && file[0]) curl_easy_setopt(c, CURLOPT_CAINFO, file);
    if (dir && dir[0]) curl_easy_setopt(c, CURLOPT_CAPATH, dir);
    return c;
}

static size_t to_buf(char *data, size_t size, size_t n, void *arg)
{
    jd_buf *b = arg;
    if (b->len + size * n > 4 << 20) return 0;
    jd_buf_add(b, data, size * n);
    return size * n;
}

static int failed(CURL *c, CURLcode r, char *err, int errlen)
{
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (r == CURLE_OK && status >= 200 && status < 300) return 0;
    if (r != CURLE_OK) snprintf(err, (size_t)errlen, "%s", curl_easy_strerror(r));
    else snprintf(err, (size_t)errlen, "the server answered %ld", status);
    return 1;
}

int jd_release_latest(jd_release *out, char *err, int errlen)
{
    *out = (jd_release){0};
    CURL *c = easy("https://api.github.com/repos/" JD_RELEASE_REPO "/releases/latest", 30);
    if (!c) return snprintf(err, (size_t)errlen, "no HTTP client"), -1;
    jd_buf b = {0};
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    CURLcode r = curl_easy_perform(c);
    int bad = failed(c, r, err, errlen);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    if (!bad) {
        jd_buf_add(&b, "", 1);
        if (jd_release_parse(b.s, out) != 0) bad = 1, snprintf(err, (size_t)errlen, "an answer without a release");
    }
    jd_buf_free(&b);
    return bad ? -1 : 0;
}

typedef struct {
    FILE *f;
    EVP_MD_CTX *md;
    volatile float *progress;
} sink;

static size_t to_file(char *data, size_t size, size_t n, void *arg)
{
    sink *s = arg;
    EVP_DigestUpdate(s->md, data, size * n);
    return fwrite(data, size, n, s->f) * size;
}

static int on_progress(void *arg, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
    sink *s = arg;
    if (total > 0 && s->progress) *s->progress = (float)now / (float)total;
    return 0;
}

int jd_release_download(const char *url, const char *sha256, const char *path, volatile float *progress, char *err,
                        int errlen)
{
    if (progress) *progress = 0;
    sink s = {.f = fopen(path, "wb"), .md = EVP_MD_CTX_new(), .progress = progress};
    if (!s.f || !s.md) {
        snprintf(err, (size_t)errlen, "cannot write %s", path);
        if (s.f) fclose(s.f);
        EVP_MD_CTX_free(s.md);
        return -1;
    }
    EVP_DigestInit_ex(s.md, EVP_sha256(), NULL);
    CURL *c = easy(url, 600);
    int bad = !c;
    if (c) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &s);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        bad = failed(c, curl_easy_perform(c), err, errlen);
        curl_easy_cleanup(c);
    } else {
        snprintf(err, (size_t)errlen, "no HTTP client");
    }
    bad |= fclose(s.f) != 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_DigestFinal_ex(s.md, md, &mdlen);
    EVP_MD_CTX_free(s.md);
    if (!bad && sha256) {
        char hex[2 * EVP_MAX_MD_SIZE + 1];
        for (unsigned i = 0; i < mdlen; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
        if (strcasecmp(hex, sha256)) bad = 1, snprintf(err, (size_t)errlen, "the download does not match its SHA-256");
    }
    if (bad) remove(path);
    else if (progress) *progress = 1;
    return bad ? -1 : 0;
}
