#include "theme.h"

#define RGB(h) ((Color){(h) >> 16 & 0xff, (h) >> 8 & 0xff, (h) & 0xff, 0xff})

Color jd_palette[JD_PALETTE_SIZE];
float jd_scale = 1.0f;

void jd_theme_set(int light)
{
    static const unsigned dark[JD_PALETTE_SIZE] = {0x2e3440, 0x3b4252, 0x434c5e, 0x4c566a, 0xd8dee9, 0xa8b1c2,
                                                   0xeceff4, 0xffffff, 0x88c0d0, 0x81a1c1, 0x5e81ac, 0xbf616a,
                                                   0xeceff4, 0xebcb8b, 0xa3be8c, 0xb48ead, 0xd08770};
    /* Snow Storm surfaces, Polar Night text; accents darkened until text on them stays above 4.5:1 */
    static const unsigned bright[JD_PALETTE_SIZE] = {0xeceff4, 0xe5e9f0, 0xd8dee9, 0xc5cddb, 0x434c5e, 0x5c6577,
                                                     0x2e3440, 0xffffff, 0x3b6a99, 0x4c6a8e, 0x5e81ac, 0xa5424c,
                                                     0xffffff, 0x8a6416, 0x4f7a3a, 0x845382, 0xa0522d};
    for (int i = 0; i < JD_PALETTE_SIZE; i++) jd_palette[i] = RGB(light ? bright[i] : dark[i]);
}

static Camera2D camera;

void jd_scale_begin(void)
{
    camera = (Camera2D){.zoom = jd_scale};
    SetMouseScale(1 / jd_scale, 1 / jd_scale);
    BeginMode2D(camera);
}

void jd_scale_end(void)
{
    EndMode2D();
}

float jd_screen_w(void)
{
    return (float)GetScreenWidth() / jd_scale;
}

float jd_screen_h(void)
{
    return (float)GetScreenHeight() / jd_scale;
}

/* raylib's scissor does not nest: a stack of logical rectangles, each cut to the one around it */
static Rectangle clips[16];
static int nclips;

static void apply(Rectangle c)
{
    BeginScissorMode((int)(c.x * jd_scale), (int)(c.y * jd_scale), (int)(c.width * jd_scale + 0.5f),
                     (int)(c.height * jd_scale + 0.5f));
}

void jd_scissor(float x, float y, float w, float h)
{
    Rectangle c = {x, y, w, h};
    if (nclips) {
        Rectangle o = clips[nclips - 1];
        float x1 = c.x > o.x ? c.x : o.x, y1 = c.y > o.y ? c.y : o.y;
        float x2 = c.x + c.width < o.x + o.width ? c.x + c.width : o.x + o.width;
        float y2 = c.y + c.height < o.y + o.height ? c.y + c.height : o.y + o.height;
        c = (Rectangle){x1, y1, x2 > x1 ? x2 - x1 : 0, y2 > y1 ? y2 - y1 : 0};
    }
    if (nclips < 16) clips[nclips++] = c;
    apply(c);
}

void jd_scissor_end(void)
{
    if (nclips) nclips--;
    if (nclips) apply(clips[nclips - 1]);
    else EndScissorMode();
}
