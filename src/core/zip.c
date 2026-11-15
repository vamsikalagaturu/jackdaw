#include "core/zip.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "core/util.h"

static void u16(jd_buf *b, unsigned v)
{
    unsigned char c[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    jd_buf_add(b, (const char *)c, 2);
}

static void u32(jd_buf *b, uint32_t v)
{
    unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    jd_buf_add(b, (const char *)c, 4);
}

/* raw deflate (no zlib header), as ZIP method 8 wants; NULL when it does not shrink the data */
static unsigned char *deflated(const char *data, size_t len, size_t *out)
{
    z_stream z = {0};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t cap = deflateBound(&z, (uLong)len);
    unsigned char *buf = jd_alloc(cap ? cap : 1);
    z.next_in = (Bytef *)data;
    z.avail_in = (uInt)len;
    z.next_out = buf;
    z.avail_out = (uInt)cap;
    int r = deflate(&z, Z_FINISH);
    *out = z.total_out;
    deflateEnd(&z);
    if (r != Z_STREAM_END || *out >= len) {
        free(buf);
        return NULL;
    }
    return buf;
}

int jd_zip_write(const char *path, const jd_zip_entry *e, int n)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    unsigned dos_time = (unsigned)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    unsigned dos_date = (unsigned)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    jd_buf b = {0}, dir = {0};
    for (int i = 0; i < n; i++) {
        uint32_t crc = (uint32_t)crc32(0, (const Bytef *)e[i].data, (uInt)e[i].len);
        size_t clen;
        unsigned char *z = deflated(e[i].data, e[i].len, &clen);
        unsigned method = z ? 8 : 0;
        if (!z) clen = e[i].len;
        size_t name_len = strlen(e[i].name);
        uint32_t offset = (uint32_t)b.len;
        for (int central = 0; central < 2; central++) {
            jd_buf *o = central ? &dir : &b;
            u32(o, central ? 0x02014b50 : 0x04034b50);
            if (central) u16(o, 3 << 8 | 20); /* made by Unix, version 2.0 */
            u16(o, 20);
            u16(o, 1 << 11);
            u16(o, method);
            u16(o, dos_time);
            u16(o, dos_date);
            u32(o, crc);
            u32(o, (uint32_t)clen);
            u32(o, (uint32_t)e[i].len);
            u16(o, (unsigned)name_len);
            u16(o, 0);
            if (central) {
                u16(o, 0), u16(o, 0), u16(o, 0);
                u32(o, (uint32_t)0100644 << 16);
                u32(o, offset);
            }
            jd_buf_add(o, e[i].name, name_len);
        }
        jd_buf_add(&b, z ? (const char *)z : e[i].data, clen);
        free(z);
    }
    uint32_t dir_at = (uint32_t)b.len;
    jd_buf_add(&b, dir.s ? dir.s : "", dir.len);
    u32(&b, 0x06054b50);
    u16(&b, 0), u16(&b, 0);
    u16(&b, (unsigned)n), u16(&b, (unsigned)n);
    u32(&b, (uint32_t)dir.len);
    u32(&b, dir_at);
    u16(&b, 0);
    int r = jd_write_file(path, b.s, b.len, 0600);
    jd_buf_free(&b), jd_buf_free(&dir);
    return r == 0 ? 0 : -1;
}
