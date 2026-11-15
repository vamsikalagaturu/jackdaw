#include "core/pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#include "core/util.h"

/* A4 in points */
#define PAGE_W 595.28
#define PAGE_H 841.89

static void text_string(jd_buf *b, const char *s)
{
    jd_buf_adds(b, "(");
    for (; s && *s; s++) {
        if (*s == '(' || *s == ')' || *s == '\\') jd_buf_add(b, "\\", 1);
        if ((unsigned char)*s >= 32) jd_buf_add(b, s, 1);
    }
    jd_buf_adds(b, ")");
}

int jd_pdf_write(const char *path, unsigned char *const *pages, int n, int w, int h, const char *title)
{
    jd_buf b = {0};
    /* objects: 1 catalog, 2 page tree, 3 info, then per page: page, content, image */
    int nobj = 3 + 3 * n;
    long *at = jd_alloc(sizeof *at * (size_t)(nobj + 1));
    jd_buf_adds(&b, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");
    at[1] = (long)b.len;
    jd_buf_adds(&b, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    at[2] = (long)b.len;
    jd_buf_addf(&b, "2 0 obj\n<< /Type /Pages /Count %d /Kids [", n);
    for (int i = 0; i < n; i++) jd_buf_addf(&b, " %d 0 R", 4 + 3 * i);
    jd_buf_adds(&b, " ] >>\nendobj\n");
    at[3] = (long)b.len;
    jd_buf_adds(&b, "3 0 obj\n<< /Producer (jackdaw) /Title ");
    text_string(&b, title);
    jd_buf_adds(&b, " >>\nendobj\n");
    size_t raw = (size_t)w * (size_t)h * 3;
    uLongf cap = compressBound((uLong)raw);
    unsigned char *z = jd_alloc(cap);
    for (int i = 0; i < n; i++) {
        int page = 4 + 3 * i, content = page + 1, image = page + 2;
        at[page] = (long)b.len;
        jd_buf_addf(&b,
                    "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R "
                    "/Resources << /XObject << /Im %d 0 R >> >> >>\nendobj\n",
                    page, PAGE_W, PAGE_H, content, image);
        char ops[96];
        int len = snprintf(ops, sizeof ops, "q %.2f 0 0 %.2f 0 0 cm /Im Do Q\n", PAGE_W, PAGE_H);
        at[content] = (long)b.len;
        jd_buf_addf(&b, "%d 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", content, len, ops);
        uLongf zlen = cap;
        if (compress2(z, &zlen, pages[i], (uLong)raw, 6) != Z_OK) {
            free(z), free(at);
            jd_buf_free(&b);
            return -1;
        }
        at[image] = (long)b.len;
        jd_buf_addf(&b,
                    "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB "
                    "/BitsPerComponent 8 /Filter /FlateDecode /Length %lu >>\nstream\n",
                    image, w, h, (unsigned long)zlen);
        jd_buf_add(&b, (const char *)z, zlen);
        jd_buf_adds(&b, "\nendstream\nendobj\n");
    }
    free(z);
    long xref = (long)b.len;
    jd_buf_addf(&b, "xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int i = 1; i <= nobj; i++) jd_buf_addf(&b, "%010ld 00000 n \n", at[i]);
    jd_buf_addf(&b, "trailer\n<< /Size %d /Root 1 0 R /Info 3 0 R >>\nstartxref\n%ld\n%%%%EOF\n", nobj + 1, xref);
    free(at);
    int r = jd_write_file(path, b.s, b.len, 0600);
    jd_buf_free(&b);
    return r == 0 ? 0 : -1;
}
