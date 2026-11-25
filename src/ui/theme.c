#include "theme.h"
#include "mail/account.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGB(h) ((Color){(h) >> 16 & 0xff, (h) >> 8 & 0xff, (h) & 0xff, 0xff})

Color jd_palette[JD_PALETTE_SIZE];
float jd_scale = 1.0f;

/* in the order of jd_theme_roles */
enum { R_ACCENT, R_BACKGROUND, R_DARK_BACKGROUND, R_LIGHTER_BACKGROUND, R_FOREGROUND, R_BRIGHT_FOREGROUND, R_RED,
       R_YELLOW, R_GREEN, R_BLUE, R_MAGENTA, R_CYAN, R_ORANGE };

/* a moved t of the way to b */
static Color mix(Color a, Color b, float t)
{
    return (Color){(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                   (unsigned char)(a.b + (b.b - a.b) * t), 255};
}

/* Rec. 709 luma of the gamma-encoded channels: enough to tell a light colour from a dark one */
static float luma(Color c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

static int parse_hex(const char *s, Color *out)
{
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    if (*s == '#') s++;
    unsigned v;
    if (sscanf(s, "%6x", &v) != 1) return 0;
    *out = RGB(v);
    return 1;
}

/* the roles the desktop's colors.toml sets into c and set[]; 0 when there is none */
static int read_desktop(Color *c, int *set)
{
#if defined(__linux__) && !defined(__ANDROID__)
    const char *home = getenv("HOME");
    if (!home) return 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/.local/state/omarchy/current/theme/colors.toml", home);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        char *end = eq;
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
        for (int i = 0; i < JD_THEME_ROLES; i++)
            if ((size_t)(end - line) == strlen(jd_theme_roles[i]) &&
                !strncmp(line, jd_theme_roles[i], (size_t)(end - line)))
                set[i] |= parse_hex(eq + 1, &c[i]);
    }
    fclose(f);
    return 1;
#else
    (void)c, (void)set;
    return 0;
#endif
}

int jd_theme_desktop_light(void)
{
    Color c[JD_THEME_ROLES] = {0};
    int set[JD_THEME_ROLES] = {0};
    if (read_desktop(c, set) && set[R_BACKGROUND]) return luma(c[R_BACKGROUND]) > 0.5f;
    return -1;
}

void jd_theme_set(int light, char *const *overrides)
{
    /* Nord, as ferret and cuttle: the light accent is Frost's nord10, since nord8 on Snow Storm is too faint */
    Color c[JD_THEME_ROLES] = {[R_ACCENT] = RGB(0x88c0d0),     [R_BACKGROUND] = RGB(0x2e3440),
                               [R_DARK_BACKGROUND] = RGB(0x242933), [R_LIGHTER_BACKGROUND] = RGB(0x3b4252),
                               [R_FOREGROUND] = RGB(0xd8dee9), [R_BRIGHT_FOREGROUND] = RGB(0xeceff4),
                               [R_RED] = RGB(0xbf616a),        [R_YELLOW] = RGB(0xebcb8b),
                               [R_GREEN] = RGB(0xa3be8c),      [R_BLUE] = RGB(0x81a1c1),
                               [R_MAGENTA] = RGB(0xb48ead),    [R_CYAN] = RGB(0x8fbcbb),
                               [R_ORANGE] = RGB(0xd08770)};
    if (light) {
        c[R_ACCENT] = RGB(0x5e81ac);
        c[R_BACKGROUND] = RGB(0xeceff4);
        c[R_DARK_BACKGROUND] = RGB(0xe5e9f0);
        c[R_LIGHTER_BACKGROUND] = RGB(0xf8f9fb);
        c[R_FOREGROUND] = RGB(0x434c5e);
        c[R_BRIGHT_FOREGROUND] = RGB(0x2e3440);
        c[R_BLUE] = RGB(0x5e81ac);
    }
    Color d[JD_THEME_ROLES];
    int set[JD_THEME_ROLES] = {0};
    memcpy(d, c, sizeof d);
    if (read_desktop(d, set) && (!set[R_BACKGROUND] || (luma(d[R_BACKGROUND]) > 0.5f) == !!light)) memcpy(c, d, sizeof c);
    for (int i = 0; overrides && i < JD_THEME_ROLES; i++)
        if (overrides[i] && overrides[i][0]) parse_hex(overrides[i], &c[i]);

    Color bg = c[R_BACKGROUND], fg = c[R_FOREGROUND], bright = c[R_BRIGHT_FOREGROUND];
    /* the panels: raised above the background when dark, a shade below it when light */
    Color panel = light ? c[R_DARK_BACKGROUND] : c[R_LIGHTER_BACKGROUND];
    /* Aurora is made for dark backgrounds: on light ones it takes some of the text colour to keep its contrast */
    float deepen = light ? 0.3f : 0;
    jd_palette[0] = bg;
    jd_palette[1] = panel;
    jd_palette[2] = mix(panel, fg, light ? 0.08f : 0.06f);
    jd_palette[3] = mix(panel, fg, light ? 0.18f : 0.12f);
    jd_palette[4] = fg;
    /* secondary text, about 4.5:1 on backgrounds 0 and 1 with Nord */
    jd_palette[5] = mix(fg, bg, light ? 0.20f : 0.27f);
    jd_palette[6] = bright;
    jd_palette[8] = light ? mix(c[R_ACCENT], bright, 0.3f) : c[R_ACCENT];
    jd_palette[9] = light ? mix(c[R_BLUE], bright, 0.2f) : c[R_BLUE];
    /* buttons and chips: a dark theme's accent is a light colour made for text, too loud as a fill, so they take
       the theme's blue (Nord: Frost nord9) with dark text, as cuttle and ferret */
    jd_palette[10] = light ? c[R_ACCENT] : c[R_BLUE];
    jd_palette[7] = luma(jd_palette[10]) > 0.55f ? (light ? bright : bg) : RGB(0xffffff);
    jd_palette[11] = mix(c[R_RED], bright, deepen);
    jd_palette[12] = light ? RGB(0xffffff) : bright;
    jd_palette[13] = mix(c[R_YELLOW], bright, light ? 0.45f : 0);
    jd_palette[14] = mix(c[R_GREEN], bright, light ? 0.4f : 0);
    jd_palette[15] = mix(c[R_MAGENTA], bright, deepen);
    jd_palette[16] = mix(c[R_ORANGE], bright, deepen);
}

static Camera2D camera;

/* screen pixels the system covers at the top (status bar, camera cutout) and bottom (navigation, soft keyboard) */
static float inset_top, inset_bottom;

void jd_set_insets(float top, float bottom)
{
    inset_top = top, inset_bottom = bottom;
}

/* Magnification: the view enlarged around the pointer, the layout as it was. at is the logical point that lies under
   the pointer unmagnified; it is drawn under the pointer, everything else spread around it, so moving the pointer
   moves the enlarged view across the page. */
static float magnify = 1;
static Vector2 magnify_at;

void jd_magnify(float m) { magnify = m > 1 ? m : 1; }

float jd_magnified(Vector2 *at)
{
    *at = magnify_at;
    return magnify;
}

void jd_scale_begin(void)
{
#ifdef __ANDROID__
    float dpi = 1; /* the screen density is part of jd_scale there */
#else
    /* BeginMode2D loads identity, dropping raylib's HiDPI screen scale, so the camera carries it */
    float dpi = GetWindowScaleDPI().x;
#endif
    float s = jd_scale, m = magnify;
    /* the window point under the pointer, undoing last frame's mapping with the whole-pixel offset raylib was given:
       deriving it from the unrounded offset fed the rounding back into the view, which shook by up to m-1 pixels */
    static int ofs_x, ofs_y;
    if (camera.zoom) {
        Vector2 p = GetMousePosition();
        float was = camera.zoom / dpi;
        magnify_at = (Vector2){(p.x * was - ofs_x) / s, (p.y * was - ofs_y - inset_top) / s};
    }
    if (m == 1) magnify_at = (Vector2){0};
    camera = (Camera2D){.zoom = s * dpi * m, .target = magnify_at,
                        .offset = {magnify_at.x * s * dpi, magnify_at.y * s * dpi + inset_top}};
    ofs_x = (int)(magnify_at.x * s * (m - 1)), ofs_y = (int)(-inset_top + magnify_at.y * s * (m - 1));
    SetMouseScale(1 / (s * m), 1 / (s * m));
    SetMouseOffset(ofs_x, ofs_y);
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
    return ((float)GetScreenHeight() - inset_top - inset_bottom) / jd_scale;
}

float jd_pixel_scale(void)
{
    return camera.zoom ? camera.zoom : jd_scale;
}

Rectangle jd_to_pixels(Rectangle r)
{
    if (!camera.zoom) return (Rectangle){r.x * jd_scale, r.y * jd_scale + inset_top, r.width * jd_scale, r.height * jd_scale};
    float z = camera.zoom;
    return (Rectangle){(r.x - camera.target.x) * z + camera.offset.x, (r.y - camera.target.y) * z + camera.offset.y,
                       r.width * z, r.height * z};
}

/* raylib's scissor does not nest: a stack of logical rectangles, each cut to the one around it */
static Rectangle clips[16];
static int nclips;

/* in window coordinates (raylib applies the HiDPI scale), through the magnification */
static void apply(Rectangle c)
{
    float s = jd_scale, m = magnify;
    float x = (c.x - magnify_at.x) * s * m + magnify_at.x * s, y = (c.y - magnify_at.y) * s * m + magnify_at.y * s + inset_top;
    BeginScissorMode((int)x, (int)y, (int)(c.width * s * m + 0.5f), (int)(c.height * s * m + 0.5f));
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
