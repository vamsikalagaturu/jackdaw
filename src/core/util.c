#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *jd_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) abort();
    return p;
}

void *jd_realloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) abort();
    return p;
}

void jd_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

char *jd_strdup(const char *s)
{
    return s ? jd_strndup(s, strlen(s)) : NULL;
}

char *jd_strndup(const char *s, size_t n)
{
    char *d = jd_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}

char *jd_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) abort();
    va_end(ap);
    return s;
}

int jd_streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

void jd_buf_add(jd_buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = jd_realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void jd_buf_adds(jd_buf *b, const char *s)
{
    jd_buf_add(b, s, strlen(s));
}

void jd_buf_addf(jd_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) abort();
    jd_buf_add(b, s, (size_t)n);
    free(s);
}

void jd_buf_free(jd_buf *b)
{
    free(b->s);
    *b = (jd_buf){0};
}

int jd_mkdirs(const char *path)
{
    char *p = jd_strdup(path);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        if (mkdir(p, 0700) && errno != EEXIST) {
            free(p);
            return -1;
        }
        *c = '/';
    }
    int r = mkdir(p, 0700) && errno != EEXIST ? -1 : 0;
    free(p);
    return r;
}

int jd_write_file(const char *path, const void *data, size_t n, int mode)
{
    char *tmp = jd_printf("%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    const char *p = data;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        p += w;
        left -= (size_t)w;
    }
    int ok = left == 0 && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

char *jd_read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    jd_buf b = {0};
    char chunk[65536];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) jd_buf_add(&b, chunk, r);
    fclose(f);
    if (!b.s) b.s = jd_strdup("");
    if (n) *n = b.len;
    return b.s;
}

static const char *xdg_dir(char **cache, const char *var, const char *fallback)
{
    if (*cache) return *cache;
    const char *base = getenv(var);
    if (base && base[0] == '/')
        *cache = jd_printf("%s/" JD_APP_NAME, base);
    else
        *cache = jd_printf("%s/%s/" JD_APP_NAME, getenv("HOME") ? getenv("HOME") : "/tmp", fallback);
    return *cache;
}

const char *jd_config_dir(void)
{
    static char *dir;
    return xdg_dir(&dir, "XDG_CONFIG_HOME", ".config");
}

const char *jd_data_dir(void)
{
    static char *dir;
    return xdg_dir(&dir, "XDG_DATA_HOME", ".local/share");
}

/* https://www.freedesktop.org/wiki/Software/xdg-user-dirs/ : XDG_DOWNLOAD_DIR="$HOME/Downloads" */
const char *jd_download_dir(void)
{
    static char *dir;
    if (dir) return dir;
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char *path = cfg && cfg[0] == '/' ? jd_printf("%s/user-dirs.dirs", cfg) : jd_printf("%s/.config/user-dirs.dirs", home);
    char *text = jd_read_file(path, NULL);
    free(path);
    for (char *line = text ? strstr(text, "XDG_DOWNLOAD_DIR=\"") : NULL; line && !dir;) {
        char *v = line + strlen("XDG_DOWNLOAD_DIR=\""), *end = strchr(v, '"');
        if (!end) break;
        *end = 0;
        dir = strncmp(v, "$HOME", 5) == 0 ? jd_printf("%s%s", home, v + 5) : jd_strdup(v);
    }
    free(text);
    if (!dir) dir = jd_printf("%s/Downloads", home);
    return dir;
}

char *jd_unique_path(const char *dir, const char *name)
{
    char *safe = jd_strdup(name && name[0] ? name : "attachment");
    for (char *c = safe; *c; c++)
        if (*c == '/' || *c == '\\' || (unsigned char)*c < 0x20) *c = '_';
    while (safe[0] == '.') safe[0] = '_';
    char *path = jd_printf("%s/%s", dir, safe);
    const char *dot = strrchr(safe, '.');
    size_t stem = dot && dot != safe ? (size_t)(dot - safe) : strlen(safe);
    for (int i = 2; access(path, F_OK) == 0 && i < 1000; i++) {
        free(path);
        path = jd_printf("%s/%.*s (%d)%s", dir, (int)stem, safe, i, safe + stem);
    }
    free(safe);
    return path;
}

int64_t jd_now(void)
{
    return (int64_t)time(NULL);
}
