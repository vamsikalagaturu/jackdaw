#include <fontconfig/fontconfig.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "theme.h"

extern const unsigned char jd_font_regular[], jd_font_semibold[];
extern const int jd_font_regular_size, jd_font_semibold_size;

/* rasterised at three times the usual drawing size, so text stays sharp up to about 48 px on screen (40 px text at
   120%); beyond that glyphs are scaled up from the atlas and soften */
#define ATLAS_SIZE 48

/* Latin, Greek, Cyrillic and common punctuation and symbols */
static const int ranges[][2] = {
    {0x20, 0x7e}, {0xa0, 0x24f}, {0x370, 0x3ff}, {0x400, 0x4ff}, {0x2010, 0x2027}, {0x2030, 0x203a}, {0x20ac, 0x20ac},
    {0x2122, 0x2122}, {0x2190, 0x2193}, {0x2500, 0x257f}, {0xfffd, 0xfffd},
};

static int *codepoints(int *n)
{
    *n = 0;
    for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++) *n += ranges[i][1] - ranges[i][0] + 1;
    int *cps = malloc(sizeof *cps * (size_t)*n), k = 0;
    for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++)
        for (int c = ranges[i][0]; c <= ranges[i][1]; c++) cps[k++] = c;
    return cps;
}

static Font finish(Font font)
{
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

static Font embedded(int bold)
{
    int n;
    int *cps = codepoints(&n);
    Font f = bold ? LoadFontFromMemory(".ttf", jd_font_semibold, jd_font_semibold_size, ATLAS_SIZE, cps, n)
                  : LoadFontFromMemory(".ttf", jd_font_regular, jd_font_regular_size, ATLAS_SIZE, cps, n);
    free(cps);
    return finish(f);
}

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

static Font from_family(const char *family, int bold, int mono)
{
    if (!family || !family[0]) return embedded(bold);
    char *path = lookup(family, bold ? FC_WEIGHT_DEMIBOLD : FC_WEIGHT_REGULAR, mono);
    if (!path) return embedded(bold);
    int n;
    int *cps = codepoints(&n);
    Font f = LoadFontEx(path, ATLAS_SIZE, cps, n);
    free(cps), free(path);
    if (!f.texture.id || f.glyphCount == 0) return embedded(bold);
    return finish(f);
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

Font jd_font_mono(const char *family)
{
    return from_family(family && family[0] ? family : "DejaVu Sans Mono", 0, 1);
}

void jd_fonts_unload(jd_fonts *f)
{
    UnloadFont(f->regular);
    UnloadFont(f->semibold);
}

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
