#pragma once
#include <stddef.h>

typedef struct {
    const char *name;
    const char *data;
    size_t len;
} jd_zip_entry;

/* A ZIP archive (PKWARE APPNOTE 6.3.10) of the entries, deflated, names marked UTF-8 (bit 11). Entries must be
   under 4 GiB each (no ZIP64). Returns 0, -1 when path cannot be written. */
int jd_zip_write(const char *path, const jd_zip_entry *e, int n);
