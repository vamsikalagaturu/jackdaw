#pragma once
#include "raylib.h"

/* The palette, by role, named after Nord (https://www.nordtheme.com/docs/colors-and-palettes) as the dark theme
   uses it. The light theme swaps in Snow Storm backgrounds and darker accents with the same roles:
   0 app background, 1 panels, 2 hover/selected, 3 strong hover and rules, 4 secondary text, 6 primary text,
   8 accent text, 9 quotes, 10 buttons, 11 red, 13 warnings, 14 green, 15 purple, 16 orange (Nord Aurora). */
#define JD_PALETTE_SIZE 17
extern Color jd_palette[JD_PALETTE_SIZE];
#define JD_NORD0 (jd_palette[0])
#define JD_NORD1 (jd_palette[1])
#define JD_NORD2 (jd_palette[2])
#define JD_NORD3 (jd_palette[3])
#define JD_NORD4 (jd_palette[4])
#define JD_NORD6 (jd_palette[6])
#define JD_NORD8 (jd_palette[8])
#define JD_NORD9 (jd_palette[9])
#define JD_NORD10 (jd_palette[10])
#define JD_NORD11 (jd_palette[11])
#define JD_NORD13 (jd_palette[13])
#define JD_GREEN (jd_palette[14])
#define JD_PURPLE (jd_palette[15])
#define JD_ORANGE (jd_palette[16])
/* secondary text (placeholders, hints), at least 4.5:1 on backgrounds 0 and 1 (WCAG 2.1 AA) in both themes */
#define JD_MUTED (jd_palette[5])
/* text on buttons and chips (color 10) */
#define JD_ON_ACCENT (jd_palette[7])
/* the page HTML mail is drawn on */
#define JD_PAPER (jd_palette[12])

void jd_theme_set(int light);

/* Interface scale: everything is drawn in logical pixels, scaled when it reaches the screen. */
extern float jd_scale;
void jd_scale_begin(void);
/* screen pixels covered by the system at the top and bottom (phones: status bar, cutout, soft keyboard) */
void jd_set_insets(float top, float bottom);
void jd_scale_end(void);
float jd_screen_w(void);
float jd_screen_h(void);
/* clipping in logical pixels; nests (each is cut to the one around it), every jd_scissor needs a jd_scissor_end */
void jd_scissor(float x, float y, float w, float h);
void jd_scissor_end(void);

/* a font file, drawn through jd_font_at */
typedef struct jd_face jd_face;
typedef struct {
    jd_face *regular;
    jd_face *semibold;
} jd_fonts;

/* f rasterised for size logical pixels at the current scale, cached per pixel size; draw and measure a text
   with the same one */
Font jd_font_at(jd_face *f, float size);
/* screen pixels per logical pixel of text: interface scale x display HiDPI x jd_text_zoom */
float jd_text_density(void);
void jd_text_zoom(float zoom);
/* size rounded to whole screen pixels, so the atlas maps texel to pixel */
float jd_text_size(float size);
void jd_face_free(jd_face *f);
/* cached; NULL when the reader's own face fits (sans-serif, families not installed) */
jd_face *jd_face_css(const char *families, int bold, int italic);

void jd_fonts_load(jd_fonts *f);
/* family as fontconfig names it; NULL, "" or a family it does not have gives the built-in Inter */
void jd_fonts_load_family(jd_fonts *f, const char *family);
jd_face *jd_font_mono(const char *family);
void jd_fonts_unload(jd_fonts *f);
/* installed families raylib can read (TrueType/OpenType), sorted; mono: monospaced only */
char **jd_font_families(int mono, int *n);
