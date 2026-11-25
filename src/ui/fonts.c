#ifndef __ANDROID__
#include <fontconfig/fontconfig.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "theme.h"
#include "rlgl.h"

extern const unsigned char jd_font_regular[], jd_font_semibold[];
extern const int jd_font_regular_size, jd_font_semibold_size;

/* Latin, Greek, Cyrillic and common punctuation and symbols */
static const int ranges[][2] = {
    {0x20, 0x7e}, {0xa0, 0x24f}, {0x370, 0x3ff}, {0x400, 0x4ff}, {0x2000, 0x203a}, {0x205f, 0x2064},
    {0x20ac, 0x20ac}, {0x2122, 0x2122}, {0x2190, 0x2193}, {0x2500, 0x257f}, {0xfeff, 0xfeff}, {0xfffd, 0xfffd},
};

/* Unicode's spaces (Zs) other than U+0020, and its invisible format characters (Cf): mail pads its preview text
   with them (U+2007, U+FEFF...), and a font without their glyphs had them drawn as raylib's fallback '?' */
static int is_space(int cp)
{
    return (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f;
}

static int is_invisible(int cp)
{
    return (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x2064) ||
           cp == 0xfeff;
}

/* those glyphs as a space's width without ink, and as nothing */
static void blank_glyphs(Font *font)
{
    int space = GetGlyphIndex(*font, ' ');
    for (int i = 0; i < font->glyphCount; i++) {
        int cp = font->glyphs[i].value;
        if (!is_space(cp) && !is_invisible(cp)) continue;
        font->glyphs[i].advanceX = is_space(cp) ? font->glyphs[space].advanceX : 0;
        font->glyphs[i].offsetX = font->glyphs[i].offsetY = 0;
        font->recs[i].width = font->recs[i].height = 0;
    }
}

static int *codepoints(int *n)
{
    static int *cps, count;
    if (!cps) {
        for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++) count += ranges[i][1] - ranges[i][0] + 1;
        cps = malloc(sizeof *cps * (size_t)count);
        int k = 0;
        for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++)
            for (int c = ranges[i][0]; c <= ranges[i][1]; c++) cps[k++] = c;
    }
    *n = count;
    return cps;
}

/* rasterised per pixel size: glyphs scaled from one large atlas came out thin and soft at reading sizes */
struct jd_face {
    unsigned char *data; /* the font file; NULL for the built-in Inter, whose bytes are embedded */
    const unsigned char *bytes;
    int len;
    char ext[8];
    struct {
        int px;
        Font font;
    } *sizes;
    int nsizes;
};

/* Sizes, widths and positions come from the font rasterised without the magnification, so text stays where it was
   laid out; a magnification only draws each glyph rasterised for it, at those same places. */
static float text_zoom = 1, text_magnify = 1;

void jd_text_zoom(float zoom) { text_zoom = zoom > 0 ? zoom : 1; }

/* The magnifier's glyphs, rasterised one at a time as they are first drawn, into one atlas dropped when the
   magnification ends: a whole font per zoom step took up to 16 ms to build and stayed in memory. */
enum { ATLAS_W = 4096, ATLAS_H = 2048, SLOTS = 1 << 13 };
static struct zglyph {
    jd_face *face;
    int px, cp;
    Rectangle rec; /* in the atlas, with a clear texel around the ink so bilinear sampling does not bleed */
    float ox, oy;
} *slots;
static int nslots, shelf_x, shelf_y, shelf_h;
static Texture2D atlas;

static void zoom_glyphs_clear(void)
{
    rlDrawRenderBatchActive(); /* what is queued still samples the atlas as it was */
    memset(slots, 0, sizeof *slots * SLOTS);
    nslots = shelf_x = shelf_y = shelf_h = 0;
}

static void zoom_glyphs_drop(void)
{
    rlDrawRenderBatchActive();
    if (atlas.id) UnloadTexture(atlas);
    atlas = (Texture2D){0};
    free(slots);
    slots = NULL;
    nslots = shelf_x = shelf_y = shelf_h = 0;
}

static struct zglyph *slot_of(jd_face *f, int px, int cp)
{
    unsigned h = (unsigned)((uintptr_t)f >> 4) * 2654435761u ^ (unsigned)px * 40503u ^ (unsigned)cp * 2246822519u;
    unsigned i = h & (SLOTS - 1);
    while (slots[i].face && !(slots[i].face == f && slots[i].px == px && slots[i].cp == cp)) i = (i + 1) & (SLOTS - 1);
    return &slots[i];
}

static const struct zglyph *zoom_glyph(jd_face *f, int px, int cp)
{
    if (!slots) {
        slots = calloc(SLOTS, sizeof *slots);
        Image im = {calloc(ATLAS_W * ATLAS_H, 2), ATLAS_W, ATLAS_H, 1, PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA};
        atlas = LoadTextureFromImage(im);
        free(im.data);
        SetTextureFilter(atlas, TEXTURE_FILTER_BILINEAR);
    }
    struct zglyph *z = slot_of(f, px, cp);
    if (z->face) return z;
    if (nslots > SLOTS * 3 / 4) zoom_glyphs_clear();
    struct zglyph g = {.face = f, .px = px, .cp = cp};
    int got = 0;
    GlyphInfo *info = LoadFontData(f->bytes, f->len, px, &cp, 1, FONT_DEFAULT, &got);
    if (info && got == 1) {
        Image im = info->image;
        int w = im.width + 2, h = im.height + 2;
        if (im.data && im.width > 0 && im.height > 0 && w <= ATLAS_W && h <= ATLAS_H) {
            if (shelf_x + w > ATLAS_W) shelf_y += shelf_h, shelf_x = shelf_h = 0;
            if (shelf_y + h > ATLAS_H) zoom_glyphs_clear();
            unsigned char *texels = calloc((size_t)(w * h), 2);
            const unsigned char *ink = im.data;
            for (int y = 0; y < im.height; y++)
                for (int x = 0; x < im.width; x++) {
                    unsigned char *p = texels + ((y + 1) * w + x + 1) * 2;
                    p[0] = 255, p[1] = ink[y * im.width + x];
                }
            g.rec = (Rectangle){(float)shelf_x, (float)shelf_y, (float)w, (float)h};
            UpdateTextureRec(atlas, g.rec, texels);
            free(texels);
            shelf_x += w;
            if (h > shelf_h) shelf_h = h;
            g.ox = (float)info->offsetX - 1, g.oy = (float)info->offsetY - 1;
        }
    }
    if (info) UnloadFontData(info, got);
    z = slot_of(f, px, cp);
    *z = g;
    nslots++;
    return z;
}

void jd_text_magnify(float m)
{
    text_magnify = m > 1 ? m : 1;
    if (text_magnify == 1) zoom_glyphs_drop();
}

float jd_text_density(void)
{
#ifdef __ANDROID__
    return jd_scale * text_zoom; /* the screen density is part of jd_scale there */
#else
    return jd_scale * text_zoom * GetWindowScaleDPI().x;
#endif
}

float jd_text_size(float size)
{
    float d = jd_text_density();
    float px = floorf(size * d + 0.5f);
    return (px < 4 ? 4 : px) / d;
}

static Font at_px(jd_face *f, int px)
{
    if (px < 4) px = 4;
    if (px > 200) px = 200;
    for (int i = 0; i < f->nsizes; i++)
        if (f->sizes[i].px == px) return f->sizes[i].font;
    int n;
    int *cps = codepoints(&n);
    Font font = LoadFontFromMemory(f->ext, f->bytes, f->len, px, cps, n);
    if (!font.texture.id || !font.glyphCount) return GetFontDefault();
    blank_glyphs(&font);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    f->sizes = realloc(f->sizes, sizeof *f->sizes * (size_t)(f->nsizes + 1));
    f->sizes[f->nsizes].px = px;
    f->sizes[f->nsizes++].font = font;
    return font;
}

Font jd_font_at(jd_face *f, float size)
{
    return at_px(f, (int)(size * jd_text_density() + 0.5f));
}

void jd_text_draw(jd_face *f, const char *s, Vector2 at, float size, Color c)
{
    Font base = jd_font_at(f, size);
    if (text_magnify == 1) {
        DrawTextEx(base, s, at, size, 0, c);
        return;
    }
    int px = (int)(size * jd_text_density() * text_magnify + 0.5f);
    float scale = size / (float)base.baseSize, zs = size / (float)px, x = 0, y = 0;
    for (int i = 0; s[i];) {
        int n = 0, cp = GetCodepointNext(s + i, &n);
        i += n > 0 ? n : 1;
        if (cp == '\n') {
            y += size + 2; /* raylib's default line spacing, as DrawTextEx */
            x = 0;
            continue;
        }
        int g = GetGlyphIndex(base, cp);
        /* the base font's glyph, which may be its stand-in for what it lacks or blank for an invisible character */
        if (cp != ' ' && cp != '\t' && base.recs[g].width > 0) {
            const struct zglyph *z = zoom_glyph(f, px, base.glyphs[g].value);
            if (z->rec.width > 0)
                DrawTexturePro(atlas, z->rec,
                               (Rectangle){at.x + x + z->ox * zs, at.y + y + z->oy * zs, z->rec.width * zs, z->rec.height * zs},
                               (Vector2){0}, 0, c);
        }
        x +=(base.glyphs[g].advanceX ? (float)base.glyphs[g].advanceX : base.recs[g].width) * scale;
    }
}

void jd_face_free(jd_face *f)
{
    if (!f) return;
    for (int i = 0; i < f->nsizes; i++) UnloadFont(f->sizes[i].font);
    free(f->sizes);
    if (f->data) UnloadFileData(f->data);
    free(f);
}

static jd_face *embedded(int bold)
{
    jd_face *f = calloc(1, sizeof *f);
    f->bytes = bold ? jd_font_semibold : jd_font_regular;
    f->len = bold ? jd_font_semibold_size : jd_font_regular_size;
    strcpy(f->ext, ".ttf");
    return f;
}

#ifdef __ANDROID__
/* Android has no fontconfig: its fonts are files in /system/fonts named Family-Style.ttf (NotoSerif-Bold.ttf,
   DroidSansMono.ttf), and a family is that name's part before the dash. */
#include <dirent.h>

#define SYSTEM_FONTS "/system/fonts"
enum { FC_WEIGHT_REGULAR = 80, FC_WEIGHT_DEMIBOLD = 180 };

static char *lookup(const char *family, int weight, int mono)
{
    (void)mono;
    static const char *const regular[] = {"-Regular", "", NULL}, *const bold[] = {"-SemiBold", "-Medium", "-Bold", NULL};
    const char *const *styles = weight >= FC_WEIGHT_DEMIBOLD ? bold : regular;
    char path[512];
    for (int i = 0; styles[i]; i++)
        for (int ext = 0; ext < 2; ext++) {
            snprintf(path, sizeof path, SYSTEM_FONTS "/%s%s.%s", family, styles[i], ext ? "otf" : "ttf");
            if (access(path, R_OK) == 0) return strdup(path);
        }
    /* a family with no bold of its own draws its bold in the regular face */
    return weight >= FC_WEIGHT_DEMIBOLD ? lookup(family, FC_WEIGHT_REGULAR, mono) : NULL;
}
#else
/* the file fontconfig picks for family at weight; NULL when it has nothing of that family (fontconfig always
   answers with some font, so the family it gives is compared) */
static char *lookup(const char *family, int weight, int mono)
{
    if (!FcInit()) return NULL;
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);
    if (mono) FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    char *path = NULL;
    FcChar8 *file = NULL, *got = NULL;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetString(match, FC_FAMILY, 0, &got) == FcResultMatch && strcasecmp((const char *)got, family) == 0) {
        const char *ext = strrchr((const char *)file, '.');
        if (ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf"))) path = strdup((const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return path;
}
#endif

/* the font file at path, NULL when raylib cannot read it */
static jd_face *from_file(const char *path)
{
    int len = 0;
    unsigned char *data = LoadFileData(path, &len);
    if (!data) return NULL;
    const char *ext = strrchr(path, '.');
    jd_face *f = calloc(1, sizeof *f);
    f->data = data, f->bytes = data, f->len = len;
    snprintf(f->ext, sizeof f->ext, "%s", ext ? ext : ".ttf");
    Font probe = LoadFontFromMemory(f->ext, data, len, 16, NULL, 95);
    int good = probe.texture.id && probe.glyphCount;
    if (probe.texture.id) UnloadFont(probe);
    if (!good) jd_face_free(f), f = NULL;
    return f;
}

static jd_face *from_family(const char *family, int bold, int mono)
{
    if (!family || !family[0]) return embedded(bold);
    char *path = lookup(family, bold ? FC_WEIGHT_DEMIBOLD : FC_WEIGHT_REGULAR, mono);
    jd_face *f = path ? from_file(path) : NULL;
    free(path);
    return f ? f : embedded(bold);
}

#ifdef __ANDROID__
/* Android's serif is Noto Serif, its files named by style */
static char *lookup_css(const char *family, int bold, int italic, int generic)
{
    if (!generic || strcasecmp(family, "serif")) return NULL;
    static const char *const names[] = {"Regular", "Italic", "Bold", "BoldItalic"};
    char path[256];
    snprintf(path, sizeof path, SYSTEM_FONTS "/NotoSerif-%s.ttf", names[bold * 2 + italic]);
    return access(path, R_OK) == 0 ? strdup(path) : NULL;
}
#else
/* a named family only when installed under that name; an italic only when the match is one */
static char *lookup_css(const char *family, int bold, int italic, int generic)
{
    if (!FcInit()) return NULL;
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    char *path = NULL;
    FcChar8 *file = NULL, *got = NULL;
    int slant = FC_SLANT_ROMAN;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetString(match, FC_FAMILY, 0, &got) == FcResultMatch &&
        (generic || strcasecmp((const char *)got, family) == 0)) {
        FcPatternGetInteger(match, FC_SLANT, 0, &slant);
        const char *ext = strrchr((const char *)file, '.');
        if ((!italic || slant != FC_SLANT_ROMAN) && ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf")))
            path = strdup((const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return path;
}
#endif

/* faces asked for by mail CSS, kept while the program runs: mail uses the same few families */
static struct {
    char *key;
    jd_face *face;
} *css_faces;
static int ncss_faces;

jd_face *jd_face_css(const char *families, int bold, int italic)
{
    char key[512];
    snprintf(key, sizeof key, "%d%d%s", bold, italic, families ? families : "");
    for (int i = 0; i < ncss_faces; i++)
        if (!strcmp(css_faces[i].key, key)) return css_faces[i].face;
    jd_face *face = NULL;
    int done = 0;
    for (const char *p = families ? families : ""; *p && !face && !done;) {
        while (*p == ' ' || *p == ',' || *p == '"' || *p == '\'') p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        const char *last = end;
        while (last > p && (last[-1] == ' ' || last[-1] == '"' || last[-1] == '\'')) last--;
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)(last - p), p);
        p = end;
        if (!name[0]) continue;
        /* the reader's own face stands for the sans families: a mail's Arial or Helvetica reads as the rest */
        if (!strcasecmp(name, "sans-serif") || !strcasecmp(name, "system-ui") || !strcasecmp(name, "-apple-system") ||
            !strcasecmp(name, "inter")) {
            done = 1;
            break;
        }
        int generic = !strcasecmp(name, "serif") || !strcasecmp(name, "monospace");
        char *path = lookup_css(name, bold, italic, generic);
        if (path) face = from_file(path);
        free(path);
        if (generic) done = 1;
    }
    if (!face && italic) {
        char *path = lookup_css("sans-serif", bold, 1, 1);
        if (path) face = from_file(path);
        free(path);
    }
    css_faces = realloc(css_faces, sizeof *css_faces * (size_t)(ncss_faces + 1));
    css_faces[ncss_faces].key = strdup(key);
    css_faces[ncss_faces++].face = face;
    return face;
}

void jd_fonts_load(jd_fonts *f)
{
    jd_fonts_load_family(f, NULL);
}

void jd_fonts_load_family(jd_fonts *f, const char *family)
{
    f->regular = from_family(family, 0, 0);
    f->semibold = from_family(family, 1, 0);
}

jd_face *jd_font_mono(const char *family)
{
#ifdef __ANDROID__
    return from_family(family && family[0] ? family : "DroidSansMono", 0, 1);
#else
    return from_family(family && family[0] ? family : "DejaVu Sans Mono", 0, 1);
#endif
}

void jd_fonts_unload(jd_fonts *f)
{
    jd_face_free(f->regular);
    if (f->semibold != f->regular) jd_face_free(f->semibold);
    f->regular = f->semibold = NULL;
}

#ifdef __ANDROID__
/* the families of /system/fonts, by file name; mono ones are the few named so */
char **jd_font_families(int mono, int *n)
{
    *n = 0;
    DIR *d = opendir(SYSTEM_FONTS);
    if (!d) return NULL;
    char **out = NULL;
    for (struct dirent *e; (e = readdir(d));) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf"))) continue;
        const char *dash = strchr(e->d_name, '-');
        int len = (int)((dash && dash < dot ? dash : dot) - e->d_name);
        char fam[128];
        snprintf(fam, sizeof fam, "%.*s", len, e->d_name);
        if (mono != (strcasestr(fam, "mono") != NULL)) continue;
        int dup = 0;
        for (int k = 0; k < *n && !dup; k++) dup = !strcmp(out[k], fam);
        if (dup) continue;
        out = realloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = strdup(fam);
    }
    closedir(d);
    for (int i = 1; i < *n; i++)
        for (int j = i; j > 0 && strcasecmp(out[j - 1], out[j]) > 0; j--) {
            char *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return out;
}
#else
char **jd_font_families(int mono, int *n)
{
    *n = 0;
    if (!FcInit()) return NULL;
    FcPattern *pat = mono ? FcNameParse((const FcChar8 *)":spacing=mono") : FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_FONTFORMAT, NULL);
    FcFontSet *set = FcFontList(NULL, pat, os);
    char **out = NULL;
    for (int i = 0; set && i < set->nfont; i++) {
        FcChar8 *fam = NULL, *format = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch) continue;
        /* raylib reads TrueType and OpenType outlines, not bitmap or Type 1 fonts */
        if (FcPatternGetString(set->fonts[i], FC_FONTFORMAT, 0, &format) == FcResultMatch &&
            strcmp((const char *)format, "TrueType") != 0 && strcmp((const char *)format, "CFF") != 0)
            continue;
        int dup = 0;
        for (int k = 0; k < *n && !dup; k++) dup = !strcmp(out[k], (const char *)fam);
        if (dup) continue;
        out = realloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = strdup((const char *)fam);
    }
    if (set) FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    for (int i = 1; i < *n; i++)
        for (int j = i; j > 0 && strcasecmp(out[j - 1], out[j]) > 0; j--) {
            char *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return out;
}
#endif
