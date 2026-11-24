#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fx.h"
#include "rlgl.h"
#include "theme.h"

/* ---- shaders: one body, the header of the GLSL the context speaks (raylib's default vertex shader is paired) ---- */

static const char head_330[] = "#version 330\n#define IN in\n#define TEX texture\nout vec4 jd_out;\n#define OUT jd_out\n";
static const char head_300es[] = "#version 300 es\nprecision mediump float;\n#define IN in\n#define TEX texture\n"
                                 "out vec4 jd_out;\n#define OUT jd_out\n";
static const char head_100[] = "#version 100\nprecision mediump float;\n#define IN varying\n#define TEX texture2D\n"
                               "#define OUT gl_FragColor\n";
static const char head_120[] = "#version 120\n#define IN varying\n#define TEX texture2D\n#define OUT gl_FragColor\n";

/* 9 Gaussian taps in 5 bilinear fetches: D. Rákos, "Efficient Gaussian blur with linear sampling" (2010) */
static const char blur_fs[] =
    "IN vec2 fragTexCoord;\nIN vec4 fragColor;\nuniform sampler2D texture0;\nuniform vec4 colDiffuse;\nuniform vec2 dir;\n"
    "void main() {\n"
    "  vec4 c = TEX(texture0, fragTexCoord) * 0.2270270270;\n"
    "  c += (TEX(texture0, fragTexCoord + dir * 1.3846153846) + TEX(texture0, fragTexCoord - dir * 1.3846153846)) * 0.3162162162;\n"
    "  c += (TEX(texture0, fragTexCoord + dir * 3.2307692308) + TEX(texture0, fragTexCoord - dir * 3.2307692308)) * 0.0702702703;\n"
    "  OUT = c * colDiffuse * fragColor;\n"
    "}\n";

/* the message turning into paper: its picture fades to matte paper with a faint grain and fibres, the text left
   showing through as ink would; the vertex colour carries the facet's shade */
static const char paper_fs[] =
    "IN vec2 fragTexCoord;\nIN vec4 fragColor;\nuniform sampler2D texture0;\nuniform vec4 colDiffuse;\n"
    "uniform float paper;\nuniform vec4 paperColor;\n"
    "void main() {\n"
    "  vec4 t = TEX(texture0, fragTexCoord);\n"
    "  vec2 cell = mod(floor(gl_FragCoord.xy), 61.0);\n"
    "  float grain = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "  float fibre = fract(sin(dot(floor(fragTexCoord * vec2(90.0, 23.0)), vec2(39.3468, 11.135))) * 24634.6345);\n"
    "  vec3 sheet = paperColor.rgb * (0.965 + 0.025 * grain + 0.015 * fibre);\n"
    /* ink is whatever differs from the page's own colour, taken at its corner: in a dark or a light theme */
    "  vec3 bg = TEX(texture0, vec2(0.004, 0.004)).rgb;\n"
    "  float ink = clamp(length(t.rgb - bg) * 2.2, 0.0, 1.0);\n"
    "  vec3 inked = sheet * (1.0 - 0.3 * ink);\n"
    "  vec3 c = mix(t.rgb, inked, paper);\n"
    "  OUT = vec4(c, t.a) * colDiffuse * fragColor;\n"
    "}\n";

/* Burning paper, outward from a point. front() is where the fire stands at a point of the row (row texture
   coordinates): the distance from the ignition roughened by value noise, negative where it has passed.
   Hash without sine (D. Hoskins, "Hash without Sine", 2014): stable at mediump, as a phone's GPU gives. */
#define BURN_COMMON                                                                                                  \
    "IN vec2 fragTexCoord;\nIN vec4 fragColor;\nuniform sampler2D texture0;\nuniform vec4 colDiffuse;\n"              \
    "uniform float progress;\nuniform vec2 size;\nuniform vec2 ignite;\nuniform float seed;\nuniform float time;\n"    \
    NOISE_COMMON                                                                                                     \
    "float front(vec2 uv) {\n"                                                                                       \
    "  float n = fbm(uv * size / 38.0 + seed);\n"                                                                    \
    "  float aspect = size.x / size.y;\n"                                                                            \
    "  float dist = length((uv - ignite) * vec2(aspect, 1.0)) / aspect;\n"                                           \
    "  float reach = max(ignite.x, 1.0 - ignite.x) + 0.1;\n"                                                         \
    "  return dist * 0.75 + n * 0.3 - progress * (reach * 0.75 + 0.36);\n"                                           \
    "}\n"

#define NOISE_COMMON                                                                                                 \
    "float hash(vec2 p) {\n"                                                                                         \
    "  vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"                                                                     \
    "  p3 += dot(p3, p3.yzx + 33.33);\n"                                                                             \
    "  return fract((p3.x + p3.y) * p3.z);\n"                                                                        \
    "}\n"                                                                                                            \
    "float noise(vec2 p) {\n"                                                                                        \
    "  vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);\n"                                              \
    "  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);\n" \
    "}\n"                                                                                                            \
    /* five octaves, each turned against the last so the lattice of value noise does not show */                \
    "float fbm(vec2 p) {\n"                                                                                          \
    "  float v = 0.0, a = 0.5;\n"                                                                                    \
    "  for (int i = 0; i < 5; i++) {\n"                                                                              \
    "    v += a * noise(p);\n"                                                                                       \
    "    p = mat2(0.8, -0.6, 0.6, 0.8) * p * 2.03 + 17.0;\n"                                                         \
    "    a *= 0.5;\n"                                                                                                \
    "  }\n"                                                                                                          \
    "  return v;\n"                                                                                                  \
    "}\n"

/* Inbox zero's sky: soft clouds of the panel colour drifting slowly over the background, fading out at the top
   and bottom so they sit in the page rather than on it */
static const char clouds_fs[] =
    "IN vec2 fragTexCoord;\nIN vec4 fragColor;\nuniform sampler2D texture0;\nuniform vec4 colDiffuse;\n"
    "uniform float time;\nuniform vec2 size;\nuniform vec4 sky;\nuniform vec4 cloud;\n" NOISE_COMMON
    "void main() {\n"
    "  vec2 p = fragTexCoord * size / 240.0 + vec2(time * 0.02, 0.0);\n"
    "  float c = fbm(p) * 0.7 + fbm(p * 1.8 + vec2(time * 0.035, 3.0)) * 0.3;\n"
    "  float m = smoothstep(0.46, 0.74, c);\n"
    "  float edge = smoothstep(0.0, 0.3, fragTexCoord.y) * (1.0 - smoothstep(0.65, 1.0, fragTexCoord.y))\n"
    "             * smoothstep(0.0, 0.12, fragTexCoord.x) * (1.0 - smoothstep(0.88, 1.0, fragTexCoord.x));\n"
    "  OUT = vec4(mix(sky.rgb, cloud.rgb, m * edge * 0.9), 1.0);\n"
    "}\n";

/* The row as paper held to a flame, from the unburnt side in: the paper browning over a wide band, a charred
   black rim, and at the hole's edge a thin ember line that flickers and breaks; past it nothing is left. */
static const char burn_fs[] = BURN_COMMON
    "void main() {\n"
    "  vec2 px = fragTexCoord * size;\n"
    "  float e = front(fragTexCoord);\n"
    "  if (e < -0.03) discard;\n"
    "  float flick = noise(px / 5.0 + vec2(seed, time * 7.0));\n"
    "  vec4 t = TEX(texture0, fragTexCoord);\n"
    "  vec3 c;\n"
    "  if (e < -0.017) {\n"
    "    float k = (e + 0.03) / 0.013;\n"
    "    vec3 hot = mix(vec3(1.0, 0.88, 0.5), vec3(0.95, 0.32, 0.04), k);\n"
    "    c = mix(vec3(0.12, 0.03, 0.0), hot, 0.35 + 0.65 * flick);\n"
    "  } else if (e < 0.014) {\n"
    "    float seep = (1.0 - smoothstep(-0.017, 0.006, e)) * flick;\n"
    "    c = vec3(0.055, 0.04, 0.03) + vec3(0.35, 0.09, 0.0) * seep * 0.5;\n"
    "  } else {\n"
    "    float s = 1.0 - smoothstep(0.014, 0.13, e);\n"
    "    c = mix(t.rgb, vec3(0.52, 0.33, 0.15), smoothstep(0.0, 0.6, s) * 0.6);\n"
    "    c = mix(c, vec3(0.16, 0.09, 0.04), smoothstep(0.7, 1.0, s));\n"
    "  }\n"
    "  OUT = vec4(c, 1.0) * colDiffuse * fragColor;\n"
    "}\n";

/* Small flames along the ember line only, added on as light over the row and a little room above it (lift
   pixels): narrow flickering tongues, low, dying with height. The ramp runs dark red, orange, yellow with the
   heat, as a black body's does. */
static const char flame_fs[] = BURN_COMMON
    "uniform float lift;\n"
    "void main() {\n"
    "  float total = size.y + lift;\n"
    "  vec2 q = vec2(fragTexCoord.x * size.x, fragTexCoord.y * total - lift);\n"
    "  float e = front(vec2(fragTexCoord.x, clamp(q.y / size.y, 0.0, 1.0)));\n"
    "  float act = 1.0 - smoothstep(0.0, 0.022, abs(e + 0.024));\n"
    "  float above = max(0.0, -q.y) / lift;\n"
    "  float n = noise(vec2(q.x / 6.0 + seed, q.y / 14.0 + time * 4.5)) * 0.6\n"
    "          + noise(vec2(q.x / 2.6 - seed, q.y / 6.0 + time * 8.0)) * 0.4;\n"
    "  float heat = clamp(act * (n * 1.5 - above * 1.3 - 0.4), 0.0, 1.0);\n"
    "  vec3 c = vec3(0.85, 0.18, 0.02) * smoothstep(0.0, 0.2, heat)\n"
    "         + vec3(0.15, 0.45, 0.08) * smoothstep(0.15, 0.45, heat)\n"
    "         + vec3(0.0, 0.25, 0.3) * smoothstep(0.4, 0.8, heat);\n"
    "  OUT = vec4(c, clamp(heat * 2.0, 0.0, 1.0)) * colDiffuse * fragColor;\n"
    "}\n";

/* warm white, the same in both themes: a sheet of printer paper */
static const Color PAPER = {0xf7, 0xf4, 0xec, 0xff};

static Shader blur_sh, paper_sh, burn_sh, flame_sh, clouds_sh;
static int loaded, blur_ok, paper_ok, burn_ok, flame_ok, clouds_ok, dir_loc, paper_loc, color_loc;
static int clouds_time_loc, clouds_size_loc, clouds_sky_loc, clouds_cloud_loc;
/* the uniforms both burn shaders share, by shader: progress, size, ignite, seed, time */
enum { U_PROGRESS, U_SIZE, U_IGNITE, U_SEED, U_TIME, U_COUNT };
static int burn_loc[U_COUNT], flame_loc[U_COUNT], lift_loc;

static Shader compile(const char *body, int *ok)
{
    int v = rlGetVersion();
    const char *head = v == RL_OPENGL_ES_20 ? head_100 : v == RL_OPENGL_ES_30 ? head_300es : v == RL_OPENGL_21 ? head_120 : head_330;
    char src[8192];
    snprintf(src, sizeof src, "%s%s", head, body);
    Shader s = LoadShaderFromMemory(NULL, src);
    *ok = s.id > 0 && s.id != rlGetShaderIdDefault();
    return s;
}

static void load(void)
{
    if (loaded) return;
    loaded = 1;
    blur_sh = compile(blur_fs, &blur_ok);
    paper_sh = compile(paper_fs, &paper_ok);
    burn_sh = compile(burn_fs, &burn_ok);
    flame_sh = compile(flame_fs, &flame_ok);
    if (blur_ok) dir_loc = GetShaderLocation(blur_sh, "dir");
    static const char *names[U_COUNT] = {"progress", "size", "ignite", "seed", "time"};
    for (int u = 0; u < U_COUNT; u++) {
        if (burn_ok) burn_loc[u] = GetShaderLocation(burn_sh, names[u]);
        if (flame_ok) flame_loc[u] = GetShaderLocation(flame_sh, names[u]);
    }
    if (flame_ok) lift_loc = GetShaderLocation(flame_sh, "lift");
    clouds_sh = compile(clouds_fs, &clouds_ok);
    if (clouds_ok) {
        clouds_time_loc = GetShaderLocation(clouds_sh, "time");
        clouds_size_loc = GetShaderLocation(clouds_sh, "size");
        clouds_sky_loc = GetShaderLocation(clouds_sh, "sky");
        clouds_cloud_loc = GetShaderLocation(clouds_sh, "cloud");
    }
    if (paper_ok) {
        paper_loc = GetShaderLocation(paper_sh, "paper");
        color_loc = GetShaderLocation(paper_sh, "paperColor");
    }
}

static int enabled = 1;

void fx_enable(int on) { enabled = on; }
int fx_enabled(void) { return enabled; }

/* drawing in framebuffer pixels, between the interface's scaled drawing */
static void pixels_begin(void)
{
    jd_scale_end();
    BeginMode2D((Camera2D){.zoom = 1});
}

static void pixels_end(void)
{
    EndMode2D();
    jd_scale_begin();
}

static Image screen_image(void)
{
    rlDrawRenderBatchActive();
    return LoadImageFromScreen();
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float x)
{
    x = clamp01(x);
    return x * x * (3 - 2 * x);
}
static float lerp(float a, float b, float k) { return a + (b - a) * k; }
static float lerp_angle(float a, float b, float k)
{
    float d = fmodf(b - a + 3 * PI, 2 * PI) - PI;
    return a + d * k;
}

/* ---- blur ---- */

static RenderTexture2D blur_tmp;

static void ensure(RenderTexture2D *rt, int w, int h)
{
    if (rt->id && rt->texture.width == w && rt->texture.height == h) return;
    if (rt->id) UnloadRenderTexture(*rt);
    *rt = LoadRenderTexture(w, h);
    SetTextureFilter(rt->texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(rt->texture, TEXTURE_WRAP_CLAMP);
}

static void pass(Texture2D src, int flipped, RenderTexture2D *dst, Vector2 dir)
{
    BeginTextureMode(*dst);
    if (dir.x || dir.y) {
        BeginShaderMode(blur_sh);
        SetShaderValue(blur_sh, dir_loc, &dir, SHADER_UNIFORM_VEC2);
    }
    DrawTexturePro(src, (Rectangle){0, 0, (float)src.width, flipped ? -(float)src.height : (float)src.height},
                   (Rectangle){0, 0, (float)dst->texture.width, (float)dst->texture.height}, (Vector2){0}, 0, WHITE);
    if (dir.x || dir.y) EndShaderMode();
    EndTextureMode();
}

void fx_blur(Texture2D src, int flipped, RenderTexture2D *out)
{
    load();
    if (!enabled || !blur_ok || !src.id) return;
    int w = src.width / 4 > 0 ? src.width / 4 : 1, h = src.height / 4 > 0 ? src.height / 4 : 1;
    jd_scale_end();
    ensure(out, w, h);
    ensure(&blur_tmp, w, h);
    pass(src, flipped, out, (Vector2){0});
    for (int i = 0; i < 2; i++) {
        pass(out->texture, 1, &blur_tmp, (Vector2){1.0f / (float)w, 0});
        pass(blur_tmp.texture, 1, out, (Vector2){0, 1.0f / (float)h});
    }
    jd_scale_begin();
}

void fx_blur_free(RenderTexture2D *out)
{
    if (out->id) UnloadRenderTexture(*out);
    *out = (RenderTexture2D){0};
}

void fx_draw_blurred(RenderTexture2D blurred, float alpha)
{
    if (!enabled || !blurred.id || alpha <= 0) return;
    Texture2D t = blurred.texture;
    pixels_begin();
    DrawTexturePro(t, (Rectangle){0, 0, (float)t.width, -(float)t.height},
                   (Rectangle){0, 0, (float)GetRenderWidth(), (float)GetRenderHeight()}, (Vector2){0}, 0,
                   ColorAlpha(WHITE, alpha));
    pixels_end();
}

/* stacked translucent outlines, each wider and fainter: the falloff of a blurred shadow without a blur pass */
void fx_shadow(Rectangle box, float roundness, float lift, float strength)
{
    enum { LAYERS = 16 };
    float spread = 6 + 28 * lift, drop = 3 + 14 * lift;
    float px = fminf(box.width, box.height) * roundness / 2;
    for (int i = LAYERS; i >= 1; i--) {
        float k = (float)i / LAYERS, g = spread * k;
        Rectangle r = {box.x - g, box.y - g + drop * (0.4f + 0.6f * k), box.width + 2 * g, box.height + 2 * g};
        float rr = (px + g) * 2 / fminf(r.width, r.height);
        DrawRectangleRounded(r, fminf(1, rr), 8, ColorAlpha(BLACK, strength * (0.03f + 0.03f * lift) * (1 - k) * (1 - k) * 2.2f));
    }
}

/* ---- backdrop ---- */

static long frame;
static RenderTexture2D backdrop;
static long backdrop_frame = -2;
static int backdrop_w, backdrop_h;
static double backdrop_at;

void fx_backdrop(Color tint)
{
    if (!enabled) {
        DrawRectangleV((Vector2){0, 0}, (Vector2){jd_screen_w(), jd_screen_h()}, tint);
        return;
    }
    if (backdrop_frame < frame - 1 || backdrop_w != GetRenderWidth() || backdrop_h != GetRenderHeight()) {
        Image im = screen_image();
        Texture2D shot = LoadTextureFromImage(im);
        UnloadImage(im);
        SetTextureFilter(shot, TEXTURE_FILTER_BILINEAR);
        fx_blur(shot, 0, &backdrop);
        UnloadTexture(shot);
        backdrop_w = GetRenderWidth(), backdrop_h = GetRenderHeight();
        if (backdrop_frame < frame - 1) backdrop_at = GetTime();
    }
    backdrop_frame = frame;
    if (blur_ok) fx_draw_blurred(backdrop, smooth((float)(GetTime() - backdrop_at) / 0.18f));
    DrawRectangleV((Vector2){0, 0}, (Vector2){jd_screen_w(), jd_screen_h()}, tint);
}

/* ---- paper planes ---- */

enum { MAX_PLANES = 8, NU = 14, NV = 10, TRAIL = 64 };
enum { FLYING, AWAY, BACK };
/* the air it leaves: one streak from the tail, a thin one from each wingtip */
enum { STREAK_TAIL, STREAK_NEAR, STREAK_FAR, STREAKS };

typedef struct {
    Vector2 p[TRAIL];
    double t[TRAIL];
    int n;
} streak;

typedef struct {
    Vector2 p;
    float angle, scale, fold, card, bank, lift;
    float roll; /* 1 upright, -1 mirrored across its length: heading left it rolls over to keep the keel below */
} pose;

typedef struct {
    int used, id, state;
    Texture2D tex;
    /* coming back: what the screen showed where the message reopens, hiding it until the paper lands there */
    Texture2D cover;
    Rectangle home; /* framebuffer pixels */
    double t0, go_at, since;
    pose from;
    float phase;
    streak streaks[STREAKS];
} plane;

static plane planes[MAX_PLANES];

static const double FOLD_END = 0.75, TAKEOFF_END = 1.4, AWAY_S = 1.1, BACK_S = 1.2;

static float card_w(const plane *pl)
{
    float z = jd_pixel_scale();
    return fminf(pl->home.width * 0.5f, 300 * z);
}

static float card_h(const plane *pl)
{
    return card_w(pl) * fminf(pl->home.height / pl->home.width, 0.72f);
}

/* the lazy figure of eight it circles above the Undo toast */
static pose loiter(const plane *pl, double t)
{
    float z = jd_pixel_scale(), W = (float)GetRenderWidth(), H = (float)GetRenderHeight();
    const float w = 2 * PI / 4.6f;
    float tau = (float)t + pl->phase;
    float ax = fminf(W * 0.3f, 230 * z), ay = 42 * z;
    Vector2 c = {W * 0.5f, H - 190 * z};
    pose ps = {0};
    ps.p = (Vector2){c.x + ax * sinf(w * tau), c.y + ay * sinf(2 * w * tau) + 5 * z * sinf(4.3f * tau)};
    float dx = ax * w * cosf(w * tau), dy = 2 * ay * w * cosf(2 * w * tau);
    float ddx = -ax * w * w * sinf(w * tau), ddy = -4 * ay * w * w * sinf(2 * w * tau);
    ps.angle = atan2f(dy, dx);
    float turn = (dx * ddy - dy * ddx) / (dx * dx + dy * dy + 1e-3f);
    ps.bank = fmaxf(-1, fminf(1, turn / 2.5f));
    ps.scale = 110 * z / (card_w(pl) * 1.05f);
    ps.fold = 1, ps.card = 1, ps.lift = 1;
    return ps;
}

static pose at_home(const plane *pl)
{
    return (pose){.p = {pl->home.x + pl->home.width / 2, pl->home.y + pl->home.height / 2}, .scale = 1};
}

static pose flying(const plane *pl, double t)
{
    float z = jd_pixel_scale();
    pose home = at_home(pl), air = loiter(pl, t);
    float m = smooth((float)t / 0.35f), k = smooth((float)((t - FOLD_END) / (TAKEOFF_END - FOLD_END)));
    pose ps;
    ps.card = m;
    ps.fold = smooth((float)(t - 0.2) / 0.55f);
    ps.p = (Vector2){lerp(home.p.x, air.p.x, k), lerp(home.p.y - 10 * z * m, air.p.y, k)};
    ps.angle = lerp_angle(0, air.angle, k);
    ps.scale = lerp(1, air.scale, k);
    ps.bank = air.bank * k;
    ps.lift = k;
    return ps;
}

static pose away(const plane *pl, double now)
{
    float z = jd_pixel_scale(), W = (float)GetRenderWidth();
    float q = clamp01((float)((now - pl->since) / AWAY_S));
    q = q * q;
    pose f = pl->from;
    Vector2 e = {cosf(f.angle) >= 0 ? W + 220 * z : -220 * z, -220 * z};
    Vector2 c = {f.p.x + cosf(f.angle) * 280 * z, f.p.y + sinf(f.angle) * 280 * z - 60 * z};
    float a = (1 - q) * (1 - q), b = 2 * (1 - q) * q, d = q * q;
    pose ps = f;
    ps.p = (Vector2){a * f.p.x + b * c.x + d * e.x, a * f.p.y + b * c.y + d * e.y};
    Vector2 dir = {2 * (1 - q) * (c.x - f.p.x) + 2 * q * (e.x - c.x), 2 * (1 - q) * (c.y - f.p.y) + 2 * q * (e.y - c.y)};
    ps.angle = atan2f(dir.y, dir.x);
    ps.scale = f.scale * (1 - 0.3f * q);
    ps.bank = lerp(f.bank, cosf(f.angle) >= 0 ? -0.4f : 0.4f, q);
    return ps;
}

/* Undo: it banks round on its own heading and glides home (0 to 0.5), touches down with a small settle, then
   unfolds and grows to the page; the last frame is the page as it was sent, where it now reopens */
static pose back(const plane *pl, double now)
{
    float z = jd_pixel_scale();
    float e = clamp01((float)((now - pl->since) / BACK_S));
    float a = clamp01(e / 0.5f), q = smooth(a);
    pose f = pl->from, h = at_home(pl), ps;
    float reach = f.lift > 0.5f ? 240 * z : 0;
    Vector2 c = {f.p.x + cosf(f.angle) * reach, f.p.y + sinf(f.angle) * reach - (f.lift > 0.5f ? 70 * z : 0)};
    float b0 = (1 - q) * (1 - q), b1 = 2 * (1 - q) * q, b2 = q * q;
    ps.p = (Vector2){b0 * f.p.x + b1 * c.x + b2 * h.p.x, b0 * f.p.y + b1 * c.y + b2 * h.p.y};
    Vector2 dir = {2 * (1 - q) * (c.x - f.p.x) + 2 * q * (h.p.x - c.x), 2 * (1 - q) * (c.y - f.p.y) + 2 * q * (h.p.y - c.y)};
    float heading = dir.x * dir.x + dir.y * dir.y > 1 ? atan2f(dir.y, dir.x) : f.angle;
    ps.angle = lerp_angle(heading, 0, smooth((a - 0.55f) / 0.45f));
    float settle = e > 0.5f && e < 0.64f ? 0.05f * sinf(PI * (e - 0.5f) / 0.14f) : 0;
    ps.scale = lerp(f.scale, 1, smooth(a * 1.15f)) * (1 + settle);
    ps.bank = f.bank * (1 - q) + (a < 1 ? 0.5f * sinf(PI * a) : 0);
    ps.lift = f.lift * (1 - q);
    ps.fold = f.fold * (1 - smooth((e - 0.56f) / 0.26f));
    ps.card = f.card * (1 - smooth((e - 0.72f) / 0.28f));
    return ps;
}

static pose current(const plane *pl, double now)
{
    pose ps = pl->state == AWAY ? away(pl, now) : pl->state == BACK ? back(pl, now) : flying(pl, now - pl->t0);
    ps.roll = fmaxf(-1, fminf(1, cosf(ps.angle) / 0.3f));
    return ps;
}

static Vector2 place(pose ps, Vector2 local)
{
    float c = cosf(ps.angle), s = sinf(ps.angle);
    local.x *= ps.scale, local.y *= ps.scale * ps.roll;
    return (Vector2){ps.p.x + local.x * c - local.y * s, ps.p.y + local.x * s + local.y * c};
}

/* Seen from above and a little to the side, as a dart folds: the sheet's top half is the near wing, open; then
   the keel, the two halves folded down under the centre line; then the far wing, foreshortened (more or less
   with the bank). The tail is notched where the keel ends. Rows of the grid by v: */
enum { KEEL_TOP = NV / 2, KEEL_BOTTOM = NV / 2 + 2 };

static void shape(const plane *pl, pose ps, Vector2 *pos, float *fold)
{
    float pw = pl->home.width, ph = pl->home.height, cw = card_w(pl), ch = card_h(pl);
    float sx = lerp(pw, cw, ps.card), sy = lerp(ph, ch, ps.card);
    float L = cw * 1.05f, S = ch * 0.5f;
    for (int i = 0; i <= NU; i++)
        for (int j = 0; j <= NV; j++) {
            float u = (float)i / NU, v = (float)j / NV, s = 2 * v - 1;
            float fu = smooth(ps.fold * 1.6f - (1 - u) * 0.6f);
            float taper = powf(1 - u, 0.95f), y;
            if (j <= KEEL_TOP) y = s * S * (1.0f - 0.2f * ps.bank);
            else if (j <= KEEL_BOTTOM) y = s / (2.0f * (KEEL_BOTTOM - KEEL_TOP) / NV) * S * 0.16f;
            else y = (S * 0.16f + (s - 2.0f * (KEEL_BOTTOM - KEEL_TOP) / NV) * S * (0.42f + 0.25f * ps.bank));
            Vector2 flat = {(u - 0.5f) * sx, (v - 0.5f) * sy};
            Vector2 dart = {(u - 0.5f) * L + L * 0.16f * (1 - u) * (1 - fminf(1, fabsf(s) * 2.5f)), y * taper};
            pos[i * (NV + 1) + j] = (Vector2){lerp(flat.x, dart.x, fu), lerp(flat.y, dart.y, fu)};
            fold[i * (NV + 1) + j] = fu;
        }
}

/* flat facets, as folded paper lit from the top left: the near wing brightest, the keel in its shadow */
static float facet(int j)
{
    if (j < KEEL_TOP) return 1.0f;
    if (j < KEEL_BOTTOM) return 0.74f;
    return 0.87f;
}

static void mesh(const plane *pl, pose ps, const Vector2 *pos, const float *fold, Vector2 off, Color tint)
{
    rlCheckRenderBatchLimit(NU * NV * 4);
    rlSetTexture(pl->tex.id);
    rlBegin(RL_QUADS);
    static const int corner[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    for (int i = 0; i < NU; i++)
        for (int j = 0; j < NV; j++) {
            float f = (fold[i * (NV + 1) + j] + fold[(i + 1) * (NV + 1) + j + 1]) / 2;
            float sh = 1 - f * (1 - facet(j));
            Color c = {(unsigned char)(tint.r * sh), (unsigned char)(tint.g * sh), (unsigned char)(tint.b * sh), tint.a};
            for (int k = 0; k < 4; k++) {
                int ii = i + corner[k][0], jj = j + corner[k][1];
                Vector2 w = place(ps, pos[ii * (NV + 1) + jj]);
                rlColor4ub(c.r, c.g, c.b, c.a);
                rlTexCoord2f((float)ii / NU, (float)jj / NV);
                rlVertex2f(w.x + off.x, w.y + off.y);
            }
        }
    rlEnd();
    rlSetTexture(0);
}

/* the folds and edges, drawn as paper's are seen: thin and a shade darker than the sheet */
static void creases(pose ps, const Vector2 *pos, float k, float alpha)
{
    if (k <= 0.02f) return;
    float z = jd_pixel_scale();
    Color line = ColorAlpha((Color){0x8a, 0x84, 0x78, 0xff}, 0.75f * k * alpha);
    static const int rows[] = {0, KEEL_TOP, KEEL_BOTTOM, NV};
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < NU; i++)
            DrawLineEx(place(ps, pos[i * (NV + 1) + rows[r]]), place(ps, pos[(i + 1) * (NV + 1) + rows[r]]), 1.1f * z, line);
    for (int j = 0; j < NV; j++) DrawLineEx(place(ps, pos[j]), place(ps, pos[j + 1]), 1.1f * z, line);
}

/* sampled at 60 Hz whatever the frame rate, so a streak's length and smoothness stay the same */
static void streak_add(streak *s, Vector2 p, double now)
{
    if (s->n && now - s->t[0] < 1.0 / 60) return;
    memmove(s->p + 1, s->p, sizeof *s->p * (TRAIL - 1));
    memmove(s->t + 1, s->t, sizeof *s->t * (TRAIL - 1));
    s->p[0] = p, s->t[0] = now;
    if (s->n < TRAIL) s->n++;
}

static float catmull(float a, float b, float c, float d, float t)
{
    return 0.5f * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t * t + (3 * b - a - 3 * c + d) * t * t * t);
}

/* A ribbon through the samples, curved between them (Catmull-Rom), narrowing and fading with age and fuller the
   faster the air moved: faint while it circles, a long streak as it leaves. head: where it starts now, or NULL
   (on the ground) to draw only what is left. */
static void streak_draw(const streak *s, const Vector2 *head, double now, float width, float alpha, double life,
                        Color color)
{
    enum { SUB = 3, MAXP = TRAIL * SUB + 2 };
    float z = jd_pixel_scale();
    Vector2 key[TRAIL + 1];
    double kt[TRAIL + 1];
    int n = 0;
    if (head) key[n] = *head, kt[n++] = now;
    for (int i = 0; i < s->n && now - s->t[i] < life; i++) key[n] = s->p[i], kt[n++] = s->t[i];
    if (n < 3) return;
    Vector2 pt[MAXP];
    float age[MAXP], fast[MAXP];
    int m = 0;
    for (int i = 0; i + 1 < n; i++) {
        Vector2 a = key[i > 0 ? i - 1 : 0], b = key[i], c = key[i + 1], d = key[i + 2 < n ? i + 2 : n - 1];
        double dt = kt[i] - kt[i + 1] > 1e-3 ? kt[i] - kt[i + 1] : 1.0 / 60;
        float speed = hypotf(c.x - b.x, c.y - b.y) / (float)dt;
        for (int k = 0; k < SUB; k++) {
            float t = (float)k / SUB;
            pt[m] = (Vector2){catmull(a.x, b.x, c.x, d.x, t), catmull(a.y, b.y, c.y, d.y, t)};
            age[m] = clamp01((float)((now - (kt[i] + (kt[i + 1] - kt[i]) * t)) / life));
            fast[m] = clamp01(speed / (520 * z));
            m++;
        }
    }
    pt[m] = key[n - 1], age[m] = 1, fast[m] = fast[m - 1], m++;

    rlCheckRenderBatchLimit((m - 1) * 6 * 2);
    rlSetTexture(0);
    for (int layer = 0; layer < 2; layer++) {
        float lw = layer ? width : width * 2.6f, la = layer ? alpha : alpha * 0.3f;
        rlBegin(RL_TRIANGLES);
        for (int k = 0; k + 1 < m; k++) {
            Vector2 side[2][2];
            unsigned char op[2];
            for (int e = 0; e < 2; e++) {
                int q = k + e, a = q + 1 < m ? q : q - 1;
                float dx = pt[a + 1].x - pt[a].x, dy = pt[a + 1].y - pt[a].y, len = hypotf(dx, dy);
                if (len < 1e-3f) len = 1e-3f;
                float life_left = 1 - age[q], strength = 0.25f + 0.75f * fast[q];
                float hw = lw * powf(life_left, 1.3f) * (0.45f + 0.55f * fast[q]);
                side[e][0] = (Vector2){pt[q].x - dy / len * hw, pt[q].y + dx / len * hw};
                side[e][1] = (Vector2){pt[q].x + dy / len * hw, pt[q].y - dx / len * hw};
                op[e] = (unsigned char)(255 * la * life_left * life_left * strength);
            }
            rlColor4ub(color.r, color.g, color.b, op[0]), rlVertex2f(side[0][0].x, side[0][0].y);
            rlColor4ub(color.r, color.g, color.b, op[0]), rlVertex2f(side[0][1].x, side[0][1].y);
            rlColor4ub(color.r, color.g, color.b, op[1]), rlVertex2f(side[1][0].x, side[1][0].y);
            rlColor4ub(color.r, color.g, color.b, op[0]), rlVertex2f(side[0][1].x, side[0][1].y);
            rlColor4ub(color.r, color.g, color.b, op[1]), rlVertex2f(side[1][1].x, side[1][1].y);
            rlColor4ub(color.r, color.g, color.b, op[1]), rlVertex2f(side[1][0].x, side[1][0].y);
        }
        rlEnd();
    }
}

static void draw_streaks(plane *pl, pose ps, const Vector2 *pos, double now)
{
    float z = jd_pixel_scale();
    Vector2 at[STREAKS] = {place(ps, (Vector2){-card_w(pl) * 0.45f, 0}), place(ps, pos[0]), place(ps, pos[NV])};
    int flying = ps.lift > 0.05f;
    rlDisableBackfaceCulling();
    for (int k = 0; k < STREAKS; k++) {
        if (flying) streak_add(&pl->streaks[k], at[k], now);
        int tail = k == STREAK_TAIL;
        streak_draw(&pl->streaks[k], flying ? &at[k] : NULL, now, (tail ? 2.4f : 0.8f) * z, tail ? 0.5f : 0.32f,
                    tail ? 0.9 : 0.45, JD_NORD6);
    }
    rlEnableBackfaceCulling();
}

static void plane_draw(plane *pl, double now)
{
    pose ps = current(pl, now);
    float z = jd_pixel_scale();
    Vector2 pos[(NU + 1) * (NV + 1)];
    float fold[(NU + 1) * (NV + 1)];
    shape(pl, ps, pos, fold);
    if (pl->state == BACK && pl->cover.id)
        DrawTexturePro(pl->cover, (Rectangle){0, 0, (float)pl->cover.width, (float)pl->cover.height}, pl->home,
                       (Vector2){0}, 0, WHITE);
    draw_streaks(pl, ps, pos, now);

    rlDisableBackfaceCulling();
    float lift = ps.lift;
    Vector2 off = {(3 + 9 * lift) * z, (5 + 16 * lift) * z};
    mesh(pl, ps, pos, fold, off, ColorAlpha(BLACK, 0.2f - 0.08f * lift));
    float paper = 0.92f * smooth(ps.fold * 3);
    if (paper_ok) {
        Vector4 pc = ColorNormalize(PAPER);
        BeginShaderMode(paper_sh);
        SetShaderValue(paper_sh, paper_loc, &paper, SHADER_UNIFORM_FLOAT);
        SetShaderValue(paper_sh, color_loc, &pc, SHADER_UNIFORM_VEC4);
    }
    mesh(pl, ps, pos, fold, (Vector2){0}, WHITE);
    if (paper_ok) EndShaderMode();
    rlEnableBackfaceCulling();
    creases(ps, pos, smooth(ps.fold * 1.4f - 0.3f), 1);
}

static plane *find(int id)
{
    for (int i = 0; i < MAX_PLANES; i++)
        if (planes[i].used && planes[i].id == id && planes[i].state == FLYING) return &planes[i];
    return NULL;
}

static Texture2D grab(Rectangle px)
{
    Image im = screen_image();
    ImageCrop(&im, px);
    Texture2D tex = LoadTextureFromImage(im);
    UnloadImage(im);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
    return tex;
}

static void plane_free(plane *pl)
{
    if (pl->tex.id) UnloadTexture(pl->tex);
    if (pl->cover.id) UnloadTexture(pl->cover);
    *pl = (plane){0};
}

void fx_plane_launch(int id, Rectangle page)
{
    if (!enabled) return;
    load();
    plane *pl = NULL;
    for (int i = 0; i < MAX_PLANES && !pl; i++)
        if (!planes[i].used) pl = &planes[i];
    if (!pl) return;
    Rectangle r = jd_to_pixels(page);
    float W = (float)GetRenderWidth(), H = (float)GetRenderHeight();
    r.x = fmaxf(0, floorf(r.x)), r.y = fmaxf(0, floorf(r.y));
    r.width = fminf(W - r.x, floorf(r.width)), r.height = fminf(H - r.y, floorf(r.height));
    if (r.width < 8 || r.height < 8) return;
    Texture2D tex = grab(r);
    int others = 0;
    for (int i = 0; i < MAX_PLANES; i++) others += planes[i].used;
    *pl = (plane){.used = 1, .id = id, .state = FLYING, .tex = tex, .home = r, .t0 = GetTime(), .phase = 1.3f * (float)others};
}

void fx_plane_release(int id)
{
    plane *pl = find(id);
    if (pl) pl->go_at = fmax(GetTime(), pl->t0 + TAKEOFF_END);
}

void fx_plane_recall(int id)
{
    plane *pl = find(id);
    if (!pl) return;
    double now = GetTime();
    pl->from = current(pl, now);
    pl->state = BACK, pl->since = now;
    for (int k = 0; k < STREAKS; k++) pl->streaks[k].n = 0;
    pl->cover = grab(pl->home);
}

/* ---- burning rows ---- */

enum { MAX_BURNS = 32, EMBERS = 16, SMOKE = 8 };
static const double BURN_S = 1.4;

typedef struct {
    Texture2D tex;
    Rectangle px;
    double t0;
    float seed;
    Vector2 ignite; /* texture coordinates */
} burn;

static burn burns[MAX_BURNS];

static float hash1(float seed, int k)
{
    float v = sinf(seed * 12.9898f + (float)k * 78.233f) * 43758.5453f;
    return v - floorf(v);
}

double fx_burn(Rectangle box, double delay)
{
    if (!enabled) return 0;
    load();
    burn *b = NULL;
    for (int i = 0; i < MAX_BURNS && !b; i++)
        if (!burns[i].tex.id) b = &burns[i];
    Rectangle r = jd_to_pixels(box);
    float W = (float)GetRenderWidth(), H = (float)GetRenderHeight();
    r.x = fmaxf(0, floorf(r.x)), r.y = fmaxf(0, floorf(r.y));
    r.width = fminf(W - r.x, floorf(r.width)), r.height = fminf(H - r.y, floorf(r.height));
    if (!b || !burn_ok || r.width < 4 || r.height < 4) return 0;
    float seed = (float)GetRandomValue(1, 1000) / 37.0f;
    *b = (burn){.tex = grab(r), .px = r, .t0 = GetTime() + delay, .seed = seed,
                .ignite = {0.1f + 0.8f * hash1(seed, 1), 1.05f}};
    return delay + BURN_S;
}

/* where the shader's front reaches a point, in seconds from the start, without its noise */
static float front_at(const burn *b, Vector2 uv)
{
    float aspect = b->px.width / b->px.height;
    float dist = hypotf((uv.x - b->ignite.x) * aspect, uv.y - b->ignite.y) / aspect;
    float reach = fmaxf(b->ignite.x, 1 - b->ignite.x) + 0.1f;
    return (float)BURN_S * (dist * 0.75f + 0.14f) / (reach * 0.75f + 0.36f);
}

static void burn_uniforms(Shader s, const int *loc, const burn *b, float p, double now)
{
    Vector2 size = {b->px.width, b->px.height};
    float time = (float)(now - b->t0);
    SetShaderValue(s, loc[U_PROGRESS], &p, SHADER_UNIFORM_FLOAT);
    SetShaderValue(s, loc[U_SIZE], &size, SHADER_UNIFORM_VEC2);
    SetShaderValue(s, loc[U_IGNITE], &b->ignite, SHADER_UNIFORM_VEC2);
    SetShaderValue(s, loc[U_SEED], &b->seed, SHADER_UNIFORM_FLOAT);
    SetShaderValue(s, loc[U_TIME], &time, SHADER_UNIFORM_FLOAT);
}

static void burn_draw(burn *b, double now)
{
    float z = jd_pixel_scale();
    float p = (float)((now - b->t0) / BURN_S);
    if (p < 0) {
        DrawTexturePro(b->tex, (Rectangle){0, 0, (float)b->tex.width, (float)b->tex.height}, b->px, (Vector2){0}, 0, WHITE);
        return;
    }
    Rectangle src = {0, 0, (float)b->tex.width, (float)b->tex.height};
    burn_uniforms(burn_sh, burn_loc, b, p, now);
    BeginShaderMode(burn_sh);
    DrawTexturePro(b->tex, src, b->px, (Vector2){0}, 0, WHITE);
    EndShaderMode();

    /* smoke first, under the flames: grey puffs that leave behind the front, rise slower, spread and thin out */
    const float SMOKE_LIFE = 1.1f;
    for (int k = 0; k < SMOKE; k++) {
        Vector2 uv = {hash1(b->seed, 200 + k), 0.2f + 0.6f * hash1(b->seed, 230 + k)};
        float age = (float)(now - b->t0) - front_at(b, uv) - 0.08f;
        if (age < 0 || age > SMOKE_LIFE) continue;
        float a = age / SMOKE_LIFE;
        Vector2 at = {b->px.x + uv.x * b->px.width + sinf(age * 2.5f + (float)k) * 8 * z,
                      b->px.y + uv.y * b->px.height - (22 + 26 * hash1(b->seed, 260 + k)) * z * age * 1.6f};
        float rad = (5 + 13 * a) * z * (0.8f + 0.5f * hash1(b->seed, 290 + k));
        float in = fminf(1, a / 0.15f);
        DrawCircleV(at, rad, ColorAlpha((Color){0x5a, 0x56, 0x54, 0xff}, 0.1f * in * (1 - a)));
    }

    /* the flames rise out of the row into the room above it, added on as light */
    float lift = b->px.height * 0.6f;
    if (flame_ok && p < 1.1f) {
        burn_uniforms(flame_sh, flame_loc, b, p, now);
        SetShaderValue(flame_sh, lift_loc, &lift, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(flame_sh);
        BeginBlendMode(BLEND_ADDITIVE);
        DrawTexturePro(b->tex, src, (Rectangle){b->px.x, b->px.y - lift, b->px.width, b->px.height + lift},
                       (Vector2){0}, 0, WHITE);
        EndBlendMode();
        EndShaderMode();
    }

    /* embers leave the front as it passes, rise, drift, cool from orange to smoke */
    const float LIFE = 0.7f;
    for (int k = 0; k < EMBERS; k++) {
        Vector2 uv = {hash1(b->seed, 10 + k), hash1(b->seed, 40 + k)};
        float age = (float)(now - b->t0) - front_at(b, uv);
        if (age < 0 || age > LIFE) continue;
        float a = age / LIFE, rise = (28 + 60 * hash1(b->seed, 70 + k)) * z;
        Vector2 at = {b->px.x + uv.x * b->px.width + sinf(age * 7 + (float)k) * 4 * z,
                      b->px.y + uv.y * b->px.height - rise * age - 30 * z * age * age};
        Color hot = {0xff, (unsigned char)(0xb0 - 0x70 * a), (unsigned char)(0x40 * (1 - a)), 0xff};
        Color c = a < 0.5f ? hot : ColorLerp(hot, (Color){0x70, 0x6a, 0x66, 0xff}, (a - 0.5f) * 2);
        float rad = (1.4f + 2.2f * hash1(b->seed, 100 + k)) * z * (1 - 0.4f * a);
        DrawCircleV(at, rad * 1.4f, ColorAlpha(c, 0.15f * (1 - a)));
        DrawCircleV(at, rad * 0.55f, ColorAlpha(c, 0.95f * (1 - a)));
    }
}

/* ---- a small paper plane drawn as three flat facets, for places too small for the folded page ---- */

void fx_mini_plane(Vector2 at, float angle, float size, float roll, float alpha)
{
    float L = size, S = size * 0.42f;
    Vector2 local[5] = {{L / 2, 0}, {-L / 2, -S}, {-L / 2 + L * 0.2f, 0}, {-L / 2 + L * 0.08f, S * 0.2f}, {-L / 2, S * 0.55f}};
    Vector2 w[5];
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 5; i++) {
        float y = local[i].y * roll;
        w[i] = (Vector2){at.x + local[i].x * c - y * s, at.y + local[i].x * s + y * c};
    }
    rlDisableBackfaceCulling();
    Color near = ColorAlpha(PAPER, alpha), keel = ColorAlpha(ColorBrightness(PAPER, -0.26f), alpha);
    Color far = ColorAlpha(ColorBrightness(PAPER, -0.13f), alpha);
    DrawTriangle(w[0], w[3], w[4], far);
    DrawTriangle(w[0], w[2], w[3], keel);
    DrawTriangle(w[0], w[1], w[2], near);
    rlEnableBackfaceCulling();
    Color line = ColorAlpha((Color){0x8a, 0x84, 0x78, 0xff}, 0.7f * alpha);
    float t = fmaxf(1, size / 40);
    DrawLineEx(w[0], w[2], t, line);
    DrawLineEx(w[0], w[1], t, line);
    DrawLineEx(w[1], w[2], t, line);
    DrawLineEx(w[0], w[4], t, line);
}

/* ---- short-lived effects drawn over everything: sparkles, a launched plane, an envelope ---- */

enum { MAX_SPARKS = 16, MAX_FLINGS = 4, MAX_ENVELOPES = 4 };

typedef struct {
    Vector2 at; /* framebuffer pixels */
    Color color;
    double t0;
    float seed;
} sparkle;

static sparkle sparks[MAX_SPARKS];

static void sparkle_px(Vector2 px, Color color)
{
    for (int i = 0; i < MAX_SPARKS; i++)
        if (!sparks[i].t0) {
            sparks[i] = (sparkle){px, color, GetTime(), (float)GetRandomValue(1, 1000) / 91.0f};
            return;
        }
}

void fx_sparkle(Vector2 at, Color color)
{
    if (!enabled) return;
    Rectangle r = jd_to_pixels((Rectangle){at.x, at.y, 0, 0});
    sparkle_px((Vector2){r.x, r.y}, color);
}

/* a ring that widens and fades, and rays that shoot out and slow, as a flag being set pops */
static int sparkle_draw(sparkle *s, double now)
{
    const float LIFE = 0.55f;
    float u = (float)(now - s->t0) / LIFE, z = jd_pixel_scale();
    if (u >= 1) return 0;
    float out = 1 - (1 - u) * (1 - u) * (1 - u);
    float r = (6 + 18 * out) * z;
    DrawCircleV(s->at, 9 * z * (1 - out), ColorAlpha(s->color, 0.55f * (1 - u)));
    DrawRing(s->at, r - 2.6f * z * (1 - u), r, 0, 360, 32, ColorAlpha(s->color, 0.85f * (1 - u)));
    for (int k = 0; k < 9; k++) {
        float a = (float)k * 2 * PI / 9 + s->seed, reach = (10 + 24 * out) * z * (0.75f + 0.25f * sinf(s->seed + (float)k * 3));
        Vector2 d = {cosf(a), sinf(a)};
        float len = 7 * z * (1 - u);
        Vector2 p0 = {s->at.x + d.x * (reach - len), s->at.y + d.y * (reach - len)};
        Vector2 p1 = {s->at.x + d.x * reach, s->at.y + d.y * reach};
        DrawLineEx(p0, p1, 2.6f * z * (1 - u * 0.5f), ColorAlpha(s->color, 1 - u * u));
    }
    return 1;
}

typedef struct {
    Vector2 from; /* framebuffer pixels */
    double t0;
} fling;

static fling flings[MAX_FLINGS];

void fx_fling(Vector2 from)
{
    if (!enabled) return;
    for (int i = 0; i < MAX_FLINGS; i++)
        if (!flings[i].t0) {
            Rectangle r = jd_to_pixels((Rectangle){from.x, from.y, 0, 0});
            flings[i] = (fling){{r.x, r.y}, GetTime()};
            return;
        }
}

/* let go of the slingshot: the plane shoots up and off the top, a streak fading behind it */
static int fling_draw(fling *f, double now)
{
    const float LIFE = 0.6f;
    float u = (float)(now - f->t0) / LIFE, z = jd_pixel_scale();
    if (u >= 1) return 0;
    float rise = (f->from.y + 80 * z) * u * u;
    Vector2 at = {f->from.x + sinf(u * 5) * 6 * z, f->from.y - rise};
    for (int k = 1; k <= 8; k++) {
        float uk = fmaxf(0, u - 0.025f * (float)k);
        Vector2 p = {f->from.x + sinf(uk * 5) * 6 * z, f->from.y - (f->from.y + 80 * z) * uk * uk};
        DrawCircleV(p, (2.2f - 0.2f * (float)k) * z, ColorAlpha(JD_NORD6, 0.3f * (1 - (float)k / 9) * (1 - u)));
    }
    fx_mini_plane(at, -PI / 2 + sinf(u * 5) * 0.15f, 30 * z * (1 - 0.3f * u), 1, 1);
    return 1;
}

/* Send later: the page shrinks to a card, an envelope closes over it with a clock on its corner, and it flies to
   the Drafts folder (or up and away when Drafts is not in sight) */
typedef struct {
    Texture2D tex;
    Rectangle home; /* framebuffer pixels */
    Vector2 target;
    int has_target;
    double t0;
} envelope;

static envelope envelopes[MAX_ENVELOPES];

void fx_envelope(Rectangle page, Vector2 target, int has_target)
{
    if (!enabled) return;
    load();
    envelope *e = NULL;
    for (int i = 0; i < MAX_ENVELOPES && !e; i++)
        if (!envelopes[i].tex.id) e = &envelopes[i];
    Rectangle r = jd_to_pixels(page);
    float W = (float)GetRenderWidth(), H = (float)GetRenderHeight();
    r.x = fmaxf(0, floorf(r.x)), r.y = fmaxf(0, floorf(r.y));
    r.width = fminf(W - r.x, floorf(r.width)), r.height = fminf(H - r.y, floorf(r.height));
    if (!e || r.width < 8 || r.height < 8) return;
    Rectangle t = jd_to_pixels((Rectangle){target.x, target.y, 0, 0});
    *e = (envelope){grab(r), r, {t.x, t.y}, has_target, GetTime()};
}

static const float ENVELOPE_S = 1.45f;

static void clock_badge(Vector2 c, float rad, float spin)
{
    DrawCircleV(c, rad, JD_NORD10);
    DrawRing(c, rad * 0.78f, rad * 0.9f, 0, 360, 24, JD_ON_ACCENT);
    float a = -PI / 2 + spin * 2 * PI, b = -PI / 2 + spin * PI / 6 + PI / 3;
    DrawLineEx(c, (Vector2){c.x + cosf(a) * rad * 0.6f, c.y + sinf(a) * rad * 0.6f}, rad * 0.14f, JD_ON_ACCENT);
    DrawLineEx(c, (Vector2){c.x + cosf(b) * rad * 0.42f, c.y + sinf(b) * rad * 0.42f}, rad * 0.16f, JD_ON_ACCENT);
}

static int envelope_draw(envelope *e, double now)
{
    float t = (float)(now - e->t0), z = jd_pixel_scale();
    if (t >= ENVELOPE_S) {
        if (e->has_target) sparkle_px(e->target, JD_NORD8);
        return 0;
    }
    float cw = fminf(e->home.width * 0.42f, 260 * z), ch = cw * 0.62f;
    float m = smooth(t / 0.35f), close = smooth((t - 0.3f) / 0.3f), fly = smooth((t - 0.68f) / (ENVELOPE_S - 0.68f));
    float w = lerp(e->home.width, cw, m), h = lerp(e->home.height, ch, m);
    Vector2 home = {e->home.x + e->home.width / 2, e->home.y + e->home.height / 2 - 12 * z * m};
    Vector2 dest = e->has_target ? e->target : (Vector2){home.x - 120 * z, -ch};
    Vector2 ctl = {(home.x + dest.x) / 2, fminf(home.y, dest.y) - 160 * z};
    float a = (1 - fly) * (1 - fly), b = 2 * (1 - fly) * fly, d = fly * fly;
    Vector2 at = {a * home.x + b * ctl.x + d * dest.x, a * home.y + b * ctl.y + d * dest.y};
    float scale = lerp(1, e->has_target ? 0.12f : 0.4f, fly), spin = -0.25f * sinf(PI * fly);
    float alpha = e->has_target ? 1 - smooth((fly - 0.85f) / 0.15f) : 1 - smooth((fly - 0.6f) / 0.4f);

    rlPushMatrix();
    rlTranslatef(at.x, at.y, 0);
    rlRotatef(spin * RAD2DEG, 0, 0, 1);
    rlScalef(scale, scale, 1);
    Rectangle card = {-w / 2, -h / 2, w, h};
    DrawRectangleRec((Rectangle){card.x + 4 * z, card.y + 7 * z, w, h}, ColorAlpha(BLACK, 0.18f * alpha));
    float paper = 0.9f * close;
    if (paper_ok) {
        Vector4 pc = ColorNormalize(PAPER);
        BeginShaderMode(paper_sh);
        SetShaderValue(paper_sh, paper_loc, &paper, SHADER_UNIFORM_FLOAT);
        SetShaderValue(paper_sh, color_loc, &pc, SHADER_UNIFORM_VEC4);
    }
    DrawTexturePro(e->tex, (Rectangle){0, 0, (float)e->tex.width, (float)e->tex.height}, card, (Vector2){0}, 0,
                   ColorAlpha(WHITE, alpha));
    if (paper_ok) EndShaderMode();
    if (close > 0) {
        Color fold = ColorAlpha((Color){0x8a, 0x84, 0x78, 0xff}, 0.8f * close * alpha);
        Color flap = ColorAlpha(ColorBrightness(PAPER, -0.06f), alpha);
        Vector2 tl = {card.x, card.y}, tr = {card.x + w, card.y}, bl = {card.x, card.y + h}, br = {card.x + w, card.y + h};
        Vector2 mid = {0, card.y + h * 0.55f};
        DrawLineEx(bl, mid, 1.2f * z, fold);
        DrawLineEx(br, mid, 1.2f * z, fold);
        /* the flap swings down from above the card to its middle */
        Vector2 apex = {0, lerp(card.y - h * 0.5f, card.y + h * 0.58f, close)};
        rlDisableBackfaceCulling();
        DrawTriangle(tl, apex, tr, flap);
        rlEnableBackfaceCulling();
        DrawLineEx(tl, apex, 1.2f * z, fold);
        DrawLineEx(apex, tr, 1.2f * z, fold);
        DrawRectangleLinesEx(card, 1.2f * z, fold);
        float pop = smooth((t - 0.45f) / 0.2f);
        if (pop > 0) {
            float rad = h * 0.2f * (pop + 0.25f * sinf(PI * pop));
            clock_badge((Vector2){card.x + w - h * 0.24f, card.y + h - h * 0.24f}, rad, t * 1.2f);
        }
    }
    rlPopMatrix();
    return 1;
}

/* ---- inbox zero ---- */

void fx_inbox_zero(Rectangle r, Color sky, Color cloud)
{
    double t = GetTime();
    float z = jd_pixel_scale();
    load();
    if (clouds_ok) {
        Vector2 size = {r.width * z, r.height * z};
        Vector4 s = ColorNormalize(sky), c = ColorNormalize(cloud);
        float tf = (float)fmod(t, 10000.0);
        SetShaderValue(clouds_sh, clouds_time_loc, &tf, SHADER_UNIFORM_FLOAT);
        SetShaderValue(clouds_sh, clouds_size_loc, &size, SHADER_UNIFORM_VEC2);
        SetShaderValue(clouds_sh, clouds_sky_loc, &s, SHADER_UNIFORM_VEC4);
        SetShaderValue(clouds_sh, clouds_cloud_loc, &c, SHADER_UNIFORM_VEC4);
        BeginShaderMode(clouds_sh);
        Texture2D white = {rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        DrawTexturePro(white, (Rectangle){0, 0, 1, 1}, r, (Vector2){0}, 0, WHITE);
        EndShaderMode();
    }
    /* the plane glides a slow figure over the middle, a dotted trail behind */
    float cx = r.x + r.width / 2, cy = r.y + r.height * 0.36f, ax = fminf(r.width * 0.24f, 220), ay = 14;
#define GLIDE_AT(tt) ((Vector2){cx + sinf((float)(tt) * 0.35f) * ax, cy + sinf((float)(tt) * 0.7f) * ay + sinf((float)(tt) * 1.9f) * 3})
    for (int k = 1; k <= 26; k += 2) {
        Vector2 p0 = GLIDE_AT(t - 0.07 * k), p1 = GLIDE_AT(t - 0.07 * (k + 1));
        DrawLineEx(p0, p1, 1.4f, ColorAlpha(JD_NORD4, 0.35f * (1 - (float)k / 27)));
    }
    Vector2 at = GLIDE_AT(t), ahead = GLIDE_AT(t + 0.05);
#undef GLIDE_AT
    float angle = atan2f(ahead.y - at.y, ahead.x - at.x);
    fx_mini_plane(at, angle, 46, fmaxf(-1, fminf(1, cosf(angle) / 0.3f)), 1);
}

void fx_frame(void)
{
    frame++;
    double now = GetTime();
    int any = 0;
    for (int i = 0; i < MAX_BURNS; i++) {
        burn *b = &burns[i];
        if (!b->tex.id) continue;
        if (now - b->t0 > BURN_S + 1.3) {
            UnloadTexture(b->tex);
            *b = (burn){0};
            continue;
        }
        if (!any) pixels_begin();
        any = 1;
        burn_draw(b, now);
    }
    for (int i = 0; i < MAX_PLANES; i++) {
        plane *pl = &planes[i];
        if (!pl->used) continue;
        /* Undo waits 30 s at most: one never released is let go */
        if (pl->state == FLYING && !pl->go_at && now - pl->t0 > 120) pl->go_at = now;
        if (pl->state == FLYING && pl->go_at > 0 && now >= pl->go_at) {
            pl->from = current(pl, now);
            pl->state = AWAY, pl->since = now;
        }
        if ((pl->state == AWAY && now - pl->since >= AWAY_S + 0.6) || (pl->state == BACK && now - pl->since >= BACK_S)) {
            plane_free(pl);
            continue;
        }
        if (!any) pixels_begin();
        any = 1;
        plane_draw(pl, now);
    }
    for (int i = 0; i < MAX_ENVELOPES; i++) {
        envelope *e = &envelopes[i];
        if (!e->tex.id) continue;
        if (!any) pixels_begin();
        any = 1;
        if (!envelope_draw(e, now)) {
            UnloadTexture(e->tex);
            *e = (envelope){0};
        }
    }
    for (int i = 0; i < MAX_FLINGS; i++) {
        if (!flings[i].t0) continue;
        if (!any) pixels_begin();
        any = 1;
        if (!fling_draw(&flings[i], now)) flings[i] = (fling){0};
    }
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!sparks[i].t0) continue;
        if (!any) pixels_begin();
        any = 1;
        if (!sparkle_draw(&sparks[i], now)) sparks[i] = (sparkle){0};
    }
    if (any) pixels_end();
}
