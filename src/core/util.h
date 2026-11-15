#pragma once
#include <stddef.h>
#include <stdint.h>

/* Allocation never returns NULL: out of memory aborts. jd_alloc zeroes. */
void *jd_alloc(size_t n);
void *jd_realloc(void *p, size_t n);
char *jd_strdup(const char *s);
char *jd_strndup(const char *s, size_t n);
char *jd_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int jd_streq(const char *a, const char *b);

typedef struct {
    char *s;
    size_t len, cap;
} jd_buf;

void jd_buf_add(jd_buf *b, const char *s, size_t n);
void jd_buf_adds(jd_buf *b, const char *s);
void jd_buf_addf(jd_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jd_buf_free(jd_buf *b);

int jd_mkdirs(const char *path);
/* Written to path.tmp, fsynced and renamed over path, so a crash leaves the old file or the new one. */
int jd_write_file(const char *path, const void *data, size_t n, int mode);
char *jd_read_file(const char *path, size_t *n);

/* $XDG_CONFIG_HOME/jackdaw and $XDG_DATA_HOME/jackdaw, per the XDG Base Directory Specification */
const char *jd_config_dir(void);
const char *jd_data_dir(void);
/* XDG_DOWNLOAD_DIR from user-dirs.dirs, else ~/Downloads */
const char *jd_download_dir(void);
/* dir/name, or dir/name (2), (3)... when taken; name stripped of path separators */
char *jd_unique_path(const char *dir, const char *name);

int64_t jd_now(void);
