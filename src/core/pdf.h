#pragma once

/* A PDF (ISO 32000-1) of A4 pages, each one RGB image of w x h pixels filling the page, Flate-compressed.
   Returns 0, -1 when path cannot be written. */
int jd_pdf_write(const char *path, unsigned char *const *pages, int n, int w, int h, const char *title);
