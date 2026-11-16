#ifndef __ANDROID__
#include <fontconfig/fontconfig.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "theme.h"

extern const unsigned char jd_font_regular[], jd_font_semibold[];
extern const int jd_font_regular_size, jd_font_semibold_size;

/* Latin, Greek, Cyrillic and common punctuation and symbols */
static const int ranges[][2] = {
    {0x20, 0x7e}, {0xa0, 0x24f}, {0x370, 0x3ff}, {0x400, 0x4ff}, {0x2010, 0x2027}, {0x2030, 0x203a}, {0x20ac, 0x20ac},
    {0x2122, 0x2122}, {0x2190, 0x2193}, {0x2500, 0x257f}, {0xfffd, 0xfffd},
};

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

static float text_zoom = 1;

void jd_text_zoom(float zoom) { text_zoom = zoom > 0 ? zoom : 1; }

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

Font jd_font_at(jd_face *f, float size)
{
    int px = (int)(size * jd_text_density() + 0.5f);
    if (px < 4) px = 4;
    if (px > 200) px = 200;
    for (int i = 0; i < f->nsizes; i++)
        if (f->sizes[i].px == px) return f->sizes[i].font;
    int n;
    int *cps = codepoints(&n);
    Font font = LoadFontFromMemory(f->ext, f->bytes, f->len, px, cps, n);
    if (!font.texture.id || !font.glyphCount) return GetFontDefault();
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    f->sizes = realloc(f->sizes, sizeof *f->sizes * (size_t)(f->nsizes + 1));
    f->sizes[f->nsizes].px = px;
    f->sizes[f->nsizes++].font = font;
    return font;
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
