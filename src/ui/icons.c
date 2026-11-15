#include "icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

/* Rasterised white once per name and pixel size, then tinted when drawn: Lucide draws in currentColor. */
typedef struct {
    const char *name;
    int px;
    Texture2D tex;
} cached;

static cached cache[128];
static int ncache;

static Texture2D rasterise(const jd_icon_file *f, int px)
{
    char *svg = malloc((size_t)*f->size + 1);
    memcpy(svg, f->data, (size_t)*f->size);
    svg[*f->size] = 0;
    for (char *c = svg; (c = strstr(c, "currentColor"));) memcpy(c, "#ffffff     ", 12);
    NSVGimage *img = nsvgParse(svg, "px", 96);
    free(svg);
    Texture2D tex = {0};
    if (!img) return tex;
    unsigned char *pixels = calloc((size_t)px * (size_t)px, 4);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    nsvgRasterize(r, img, 0, 0, (float)px / img->width, pixels, px, px, px * 4);
    nsvgDeleteRasterizer(r);
    nsvgDelete(img);
    Image im = {.data = pixels, .width = px, .height = px, .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    tex = LoadTextureFromImage(im);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    free(pixels);
    return tex;
}

void jd_icon(const char *name, float x, float y, float size, Color color)
{
    /* twice the drawn size, for HiDPI screens */
    int px = (int)(size * 2 + 0.5f);
    Texture2D *tex = NULL;
    for (int i = 0; i < ncache && !tex; i++)
        if (cache[i].px == px && strcmp(cache[i].name, name) == 0) tex = &cache[i].tex;
    if (!tex) {
        const jd_icon_file *f = jd_icon_files;
        while (f->name && strcmp(f->name, name) != 0) f++;
        if (!f->name || ncache == (int)(sizeof cache / sizeof *cache)) return;
        cache[ncache] = (cached){f->name, px, rasterise(f, px)};
        tex = &cache[ncache++].tex;
    }
    DrawTexturePro(*tex, (Rectangle){0, 0, (float)tex->width, (float)tex->height}, (Rectangle){x, y, size, size},
                   (Vector2){0, 0}, 0, color);
}

void jd_icons_unload(void)
{
    for (int i = 0; i < ncache; i++) UnloadTexture(cache[i].tex);
    ncache = 0;
}
