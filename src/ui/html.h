#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "theme.h"

/* An HTML mail body laid out by litehtml and drawn with raylib on a light page. Remote content (images, style
   sheets) is never fetched. */
typedef struct jd_html jd_html;

/* size: the text size mail without its own font sizes gets */
jd_html *jd_html_new(const char *html, const jd_fonts *fonts, float size);
/* an image for src (a "cid:<content id>" the mail carries, or a fetched remote URL); layout again after it */
void jd_html_add_image(jd_html *h, const char *src, const char *data, size_t len);
/* the http(s) images the mail refers to, none of which are fetched by jackdaw unless asked */
int jd_html_remote_count(jd_html *h);
const char *jd_html_remote_url(jd_html *h, int i);
int jd_html_has_image(jd_html *h, const char *src);
/* lays out for width; returns the height */
float jd_html_layout(jd_html *h, float width);
/* draws with its top left at (x, y); only the part between clip_top and clip_bottom (screen y) */
void jd_html_draw(jd_html *h, float x, float y, float clip_top, float clip_bottom);
/* the mail's colours lightness-inverted for the dark page; images unchanged */
void jd_html_set_dark(jd_html *h, int dark);
Color jd_html_page(int dark);
/* Selecting text: from (ax, ay) to (bx, by), relative to the top left; laying out again drops the selection */
void jd_html_select(jd_html *h, float ax, float ay, float bx, float by);
void jd_html_select_all(jd_html *h);
/* the word under (x, y), without the punctuation around it; 0 when no text is there */
int jd_html_select_word(jd_html *h, float x, float y);
/* the line of text under (x, y), as laid out; 0 when no text is there */
int jd_html_select_line(jd_html *h, float x, float y);
/* where the selection starts and ends: the x and the top of each end's line, and the end line's height */
int jd_html_selection_ends(jd_html *h, float *ax, float *ay, float *bx, float *by, float *line_h);
void jd_html_select_none(jd_html *h);
int jd_html_has_selection(jd_html *h);
/* the selected text, or NULL; free() it */
char *jd_html_selection(jd_html *h);
/* Visual mode: a caret from the first line at or below y (relative to the top), moved by vim motions
   ('h' 'l' 'w' 'b' 'j' 'k' '0' '$' 'g' 'G'), selecting from where it started */
void jd_html_caret_begin(jd_html *h, float y);
void jd_html_caret_move(jd_html *h, int motion);
void jd_html_caret_end(jd_html *h);
/* the selection starts again where the caret is */
void jd_html_caret_anchor(jd_html *h);
float jd_html_caret_y(jd_html *h);
/* the link under (x, y), relative to the top left, or NULL; free() it */
char *jd_html_link_at(jd_html *h, float x, float y);
void jd_html_free(jd_html *h);

#ifdef __cplusplus
}
#endif
