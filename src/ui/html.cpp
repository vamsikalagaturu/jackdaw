#include "html.h"

#include <litehtml.h>
#include <rlgl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

/* Inter's vertical metrics as fractions of the em (hhea ascender 0.969, descender 0.242) */
constexpr float ASCENT = 0.969f, DESCENT = 0.242f;
/* The viewport height. litehtml 0.10 resolves a % height against it even inside an auto-height parent (CSS 2.1
   10.5 says auto; fixed upstream after 0.10), and a real window's height would cut such mail off there: until
   0.11 it stays this tall, which leaves those mails long instead */
constexpr float VIEW_H = 100000;

/* the page mail is drawn on, and its default text: mail written for white paper stays readable */
std::string user_css(float size)
{
    return "html, body { background: transparent; color: #2e3440; font-family: Inter; font-size: " +
           std::to_string((int)size) + "px; }"
           /* litehtml turns a cell's align into an inherited text-align; browsers centre only the nested table */
           " table { text-align: left; }"
           " td[align=center] > table, th[align=center] > table, td[align=middle] > table, center > table,"
           " div[align=center] > table { margin-left: auto; margin-right: auto; }";
}

struct font_handle {
    jd_face *face;
    float size;
    int decoration;
    Font font() const { return jd_font_at(face, size); }
};

/* on a whole screen pixel, so glyphs rasterised for their size land texel on pixel */
static float snap(float v)
{
    float d = jd_text_density();
    return std::floor(v * d + 0.5f) / d;
}

/* set while a mail is drawn on the dark page */
bool dark_page = false;

/* HSL lightness inverted into lo..hi, so white lands on jd_html_page and black on a soft white */
Color invert(const litehtml::web_color &c)
{
    float r = c.red / 255.0f, g = c.green / 255.0f, b = c.blue / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (mx + mn) / 2, d = mx - mn;
    float h = 0, s = 0;
    if (d > 0) {
        s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
        h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
    }
    const float lo = 0.19f, hi = 0.90f;
    l = lo + (1 - l) * (hi - lo);
    /* chroma is kept, not saturation: a barely tinted white has a high HSL saturation, strong when dark */
    float room = 1 - std::fabs(2 * l - 1);
    s = room > 0 ? std::min(1.0f, d / room) : 0;
    auto hue = [](float p, float q, float t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6) return p + (q - p) * 6 * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
        return p;
    };
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    auto byte = [](float v) { return (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
    if (s == 0) return Color{byte(l), byte(l), byte(l), c.alpha};
    return Color{byte(hue(p, q, h + 1.0f / 3)), byte(hue(p, q, h)), byte(hue(p, q, h - 1.0f / 3)), c.alpha};
}

float luminance(Color c)
{
    auto lin = [](unsigned char v) {
        float s = v / 255.0f;
        return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

/* On the dark page only what would glare or vanish changes, as Apple Mail and Gmail darken mail: light
   backgrounds go dark and dark text goes light; a mail's own dark sections (a navy footer, a blue box with white
   text) are already right and stay. 0.2 is about the relative luminance of middle grey. */
const float MID_LUMINANCE = 0.2f;

Color original(const litehtml::web_color &c) { return Color{c.red, c.green, c.blue, c.alpha}; }

/* backgrounds and borders */
Color to_color(const litehtml::web_color &c)
{
    Color o = original(c);
    return dark_page && luminance(o) > MID_LUMINANCE ? invert(c) : o;
}

/* Text on the dark page, then lightened towards white, hue kept, until it has WCAG 2.2's 4.5:1 against the page
   (SC 1.4.3): HSL lightness says little about how light a colour looks (inverted pure blue stays dark) */
Color text_color(const litehtml::web_color &web)
{
    Color c = original(web);
    if (!dark_page) return c;
    if (luminance(c) < MID_LUMINANCE) c = invert(web);
    const float page = 0.028f, need = 4.5f * (page + 0.05f) - 0.05f;
    Color out = c;
    for (float t = 0.1f; luminance(out) < need && t <= 1.0f; t += 0.1f)
        out = Color{(unsigned char)(c.r + (255 - c.r) * t), (unsigned char)(c.g + (255 - c.g) * t),
                    (unsigned char)(c.b + (255 - c.b) * t), c.a};
    return out;
}

class container : public litehtml::document_container {
public:
    container(const jd_fonts *fonts, float size) : size(size), fonts(*fonts) {}

    float size;
    float width = 600;
    std::string clicked;

    /* Where the drawn content ends, from the collecting pass: a box as tall as the viewport is litehtml 0.10's
       height:100% (see VIEW_H) and does not count, nor does what is too small to see (a 1x1 tracking pixel
       placed after it). */
    float content_bottom = 0;
    void reach(const litehtml::position &p)
    {
        if (p.height < VIEW_H / 2 && p.width > 2 && p.height > 2)
            content_bottom = std::max(content_bottom, (float)(p.y + p.height));
    }

    /* The text as laid out, in drawing (reading) order, for selecting it: recorded by a drawing pass that
       draws nothing, after each layout. Positions are relative to the document's top left. */
    struct run {
        float x, y, w, h;
        std::string text;
        jd_face *face;
        float size;
        Font font() const { return jd_font_at(face, size); }
    };
    std::vector<run> runs;
    bool collecting = false;

    litehtml::uint_ptr create_font(const litehtml::font_description &d, const litehtml::document *,
                                   litehtml::font_metrics *fm) override
    {
        int bold = d.weight >= 600;
        jd_face *face = jd_face_css(d.family.c_str(), bold, d.style == litehtml::font_style_italic);
        auto *f = new font_handle{face ? face : bold ? fonts.semibold : fonts.regular,
                                  jd_text_size(d.size > 0 ? d.size : size), d.decoration_line};
        if (fm) {
            fm->font_size = f->size;
            fm->ascent = f->size * ASCENT;
            fm->descent = f->size * DESCENT;
            fm->height = fm->ascent + fm->descent;
            fm->x_height = f->size * 0.53f;
            fm->ch_width = MeasureTextEx(f->font(), "0", f->size, 0).x;
            fm->draw_spaces = false;
            fm->sub_shift = f->size * 0.2f;
            fm->super_shift = f->size * 0.35f;
        }
        return reinterpret_cast<litehtml::uint_ptr>(f);
    }

    void delete_font(litehtml::uint_ptr h) override { delete reinterpret_cast<font_handle *>(h); }

    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr h) override
    {
        auto *f = reinterpret_cast<font_handle *>(h);
        return MeasureTextEx(f->font(), text, f->size, 0).x;
    }

    void draw_text(litehtml::uint_ptr, const char *text, litehtml::uint_ptr h, litehtml::web_color color,
                   const litehtml::position &pos) override
    {
        auto *f = reinterpret_cast<font_handle *>(h);
        /* raylib draws from the line's top; litehtml's box bottom sits DESCENT below the baseline */
        float top = pos.y + pos.height - f->size * (ASCENT + DESCENT);
        if (collecting) {
            content_bottom = std::max(content_bottom, (float)(pos.y + pos.height));
            if (text && text[0])
                runs.push_back(run{pos.x, top, MeasureTextEx(f->font(), text, f->size, 0).x, f->size * (ASCENT + DESCENT),
                                   text, f->face, f->size});
            return;
        }
        Color c = text_color(color);
        Font font = f->font();
        DrawTextEx(font, text, Vector2{snap(pos.x), snap(top)}, f->size, 0, c);
        float w = MeasureTextEx(font, text, f->size, 0).x, t = std::max(1.0f, f->size / 14);
        if (f->decoration & litehtml::text_decoration_line_underline)
            DrawRectangleV(Vector2{pos.x, top + f->size * ASCENT + t}, Vector2{w, t}, c);
        if (f->decoration & litehtml::text_decoration_line_line_through)
            DrawRectangleV(Vector2{pos.x, top + f->size * ASCENT * 0.65f}, Vector2{w, t}, c);
    }

    /* litehtml breaks lines only between these words. Besides spaces and CJK characters (as litehtml's own), a word
       ends after a hyphen between letters or digits (UAX #14 HY), as browsers break, so long links wrap. */
    void split_text(const char *text, const std::function<void(const char *)> &on_word,
                    const std::function<void(const char *)> &on_space) override
    {
        std::string word;
        const unsigned char *s = (const unsigned char *)text;
        auto flush = [&] {
            if (!word.empty()) on_word(word.c_str()), word.clear();
        };
        for (size_t i = 0; s[i];) {
            unsigned c = s[i];
            size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            for (size_t k = 1; k < len; k++)
                if (!s[i + k]) len = k;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                flush();
                std::string sp(1, (char)c);
                on_space(sp.c_str());
            } else if (len == 3 && c >= 0xe4 && c <= 0xe9) {
                flush();
                on_word(std::string((const char *)s + i, len).c_str());
            } else {
                word.append((const char *)s + i, len);
                if (c == '-' && word.size() > 1 && std::isalnum(s[i + 1]) && std::isalnum((unsigned char)word[word.size() - 2]))
                    flush();
                /* and after a URL's / (UAX #14 SY) and ? & =, as browsers allow in links */
                else if ((c == '/' || c == '?' || c == '&' || c == '=') && word.size() > 1 && s[i + 1] &&
                         s[i + 1] != '/' && s[i + 1] != ' ' && !std::isdigit(s[i + 1]))
                    flush();
            }
            i += len;
        }
        flush();
    }

    litehtml::pixel_t pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    litehtml::pixel_t get_default_font_size() const override { return size; }
    const char *get_default_font_name() const override { return "Inter"; }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &m) override
    {
        if (collecting) return;
        Color c = text_color(m.color);
        float cx = m.pos.x + m.pos.width / 2, cy = m.pos.y + m.pos.height / 2, r = m.pos.width / 2;
        switch (m.marker_type) {
        case litehtml::list_style_type_circle: DrawCircleLinesV(Vector2{cx, cy}, r, c); break;
        case litehtml::list_style_type_square: DrawRectangleV(Vector2{m.pos.x, m.pos.y}, Vector2{m.pos.width, m.pos.height}, c); break;
        case litehtml::list_style_type_disc: DrawCircleV(Vector2{cx, cy}, r, c); break;
        default: {
            auto *f = reinterpret_cast<font_handle *>(m.font);
            std::string n = std::to_string(m.index) + ".";
            float w = f ? MeasureTextEx(f->font(), n.c_str(), f->size, 0).x : 0;
            if (f) DrawTextEx(f->font(), n.c_str(), Vector2{snap(m.pos.x + m.pos.width - w), snap(m.pos.y - f->size * 0.1f)}, f->size, 0, c);
        }
        }
    }

    /* cid: images come with the mail; remote ones are never fetched and take no room, so tracking pixels and
       layout images stay out */
    std::map<std::string, Texture2D> images;
    std::vector<std::string> remote;

    /* only noted: fetching is the reader's decision */
    void load_image(const char *src, const char *, bool) override
    {
        if (!src || (strncmp(src, "http://", 7) != 0 && strncmp(src, "https://", 8) != 0)) return;
        if (std::find(remote.begin(), remote.end(), src) == remote.end()) remote.push_back(src);
    }

    void get_image_size(const char *src, const char *, litehtml::size &sz) override
    {
        auto it = images.find(src ? src : "");
        sz.width = it == images.end() ? 0 : (float)it->second.width;
        sz.height = it == images.end() ? 0 : (float)it->second.height;
    }

    void draw_image(litehtml::uint_ptr, const litehtml::background_layer &layer, const std::string &url,
                    const std::string &) override
    {
        if (collecting) reach(layer.origin_box);
        auto it = images.find(url);
        if (collecting || it == images.end()) return;
        const Texture2D &t = it->second;
        const auto &b = layer.origin_box;
        DrawTexturePro(t, Rectangle{0, 0, (float)t.width, (float)t.height}, Rectangle{b.x, b.y, b.width, b.height},
                       Vector2{0, 0}, 0, WHITE);
    }

    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer &layer, const litehtml::web_color &color) override
    {
        if (collecting && !layer.is_root && color.alpha) reach(layer.border_box);
        if (collecting || layer.is_root || color.alpha == 0) return;
        const auto &b = layer.border_box;
        DrawRectangleV(Vector2{b.x, b.y}, Vector2{b.width, b.height}, to_color(color));
    }

    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                              const litehtml::background_layer::linear_gradient &) override {}
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                              const litehtml::background_layer::radial_gradient &) override {}
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer &,
                             const litehtml::background_layer::conic_gradient &) override {}

    void draw_borders(litehtml::uint_ptr, const litehtml::borders &b, const litehtml::position &p, bool root) override
    {
        if (collecting && !root) reach(p);
        if (collecting || root) return;
        auto side =[](const litehtml::border &s) { return s.width > 0 && s.style > litehtml::border_style_hidden; };
        if (side(b.top)) DrawRectangleV(Vector2{p.x, p.y}, Vector2{p.width, b.top.width}, to_color(b.top.color));
        if (side(b.bottom))
            DrawRectangleV(Vector2{p.x, p.y + p.height - b.bottom.width}, Vector2{p.width, b.bottom.width}, to_color(b.bottom.color));
        if (side(b.left)) DrawRectangleV(Vector2{p.x, p.y}, Vector2{b.left.width, p.height}, to_color(b.left.color));
        if (side(b.right))
            DrawRectangleV(Vector2{p.x + p.width - b.right.width, p.y}, Vector2{b.right.width, p.height}, to_color(b.right.color));
    }

    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    void on_anchor_click(const char *url, const litehtml::element::ptr &) override { clicked = url ? url : ""; }
    void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
    void set_cursor(const char *) override {}

    void transform_text(litehtml::string &text, litehtml::text_transform tt) override
    {
        if (tt == litehtml::text_transform_uppercase)
            for (auto &c : text) c = (char)std::toupper((unsigned char)c);
        else if (tt == litehtml::text_transform_lowercase)
            for (auto &c : text) c = (char)std::tolower((unsigned char)c);
    }

    /* @import of remote style sheets is a fetch too */
    void import_css(litehtml::string &, const litehtml::string &, litehtml::string &) override {}
    void set_clip(const litehtml::position &, const litehtml::border_radiuses &) override {}
    void del_clip() override {}

    void get_viewport(litehtml::position &v) const override
    {
        v.x = v.y = 0;
        v.width = width;
        v.height = VIEW_H;
    }

    litehtml::element::ptr create_element(const char *, const litehtml::string_map &,
                                          const std::shared_ptr<litehtml::document> &) override
    {
        return nullptr;
    }

    void get_media_features(litehtml::media_features &m) const override
    {
        m.type = litehtml::media_type_screen;
        m.width = m.device_width = width;
        m.height = m.device_height = VIEW_H;
        m.color = 8;
        m.monochrome = 0;
        m.color_index = 256;
        m.resolution = 96;
    }

    void get_language(litehtml::string &language, litehtml::string &culture) const override
    {
        language = "en";
        culture = "";
    }

private:
    jd_fonts fonts;
};

} // namespace

/* a place in the laid-out text: a run and a byte offset in it */
struct text_pos {
    int run = 0, at = 0;
    bool operator<(const text_pos &o) const { return run < o.run || (run == o.run && at < o.at); }
};

/* The message text size zooms the whole mail, as a browser does: a mail's CSS sizes in px and pt never see a
   default font size. Positions the API takes and gives are unzoomed (the reader's); the runs' and litehtml's are
   the document's. */
static const float BASE_SIZE = 15;

struct zoom_scope {
    explicit zoom_scope(float z) { jd_text_zoom(z); }
    ~zoom_scope() { jd_text_zoom(1); }
};

struct jd_html {
    container box;
    litehtml::document::ptr doc;
    std::string source;
    float base_zoom = 1; /* from the message text size */
    float zoom = 1;      /* base_zoom, or less where the mail is wider than the page (jd_html_layout) */
    bool dark = false;
    bool has_media = false; /* the source has @media rules: their match depends on the width */
    float media_w = -1;     /* the width they were matched for */
    float height = 0;
    bool selected = false;
    text_pos sel_a, sel_b;
    /* the keyboard caret of visual mode, and where its selection started */
    bool caret_on = false;
    text_pos caret, anchor;
    jd_html(const jd_fonts *f, float size) : box(f, BASE_SIZE), base_zoom(size > 0 ? size / BASE_SIZE : 1), zoom(base_zoom) {}
};

namespace {

/* the byte in run r closest to x (relative to the document) */
int offset_in(const container::run &r, float x)
{
    int i = 0, n = (int)r.text.size();
    while (i < n) {
        int next = i + 1;
        while (next < n && ((unsigned char)r.text[next] & 0xc0) == 0x80) next++;
        float w_next = MeasureTextEx(r.font(), r.text.substr(0, (size_t)next).c_str(), r.size, 0).x;
        float w_here = i ? MeasureTextEx(r.font(), r.text.substr(0, (size_t)i).c_str(), r.size, 0).x : 0;
        if (r.x + (w_here + w_next) / 2 > x) break;
        i = next;
    }
    return i;
}

/* the text position under (x, y): on that line the run under x, or the line's start or end; between lines the
   start of the next one */
text_pos hit(const std::vector<container::run> &runs, float x, float y)
{
    int n = (int)runs.size();
    if (!n) return {};
    int line_first = -1, line_last = -1;
    for (int i = 0; i < n; i++)
        if (y >= runs[i].y && y < runs[i].y + runs[i].h) {
            if (line_first < 0) line_first = i;
            line_last = i;
            if (x < runs[i].x + runs[i].w) return {i, x <= runs[i].x ? 0 : offset_in(runs[i], x)};
        }
    if (line_last >= 0) return {line_last, (int)runs[line_last].text.size()};
    for (int i = 0; i < n; i++)
        if (runs[i].y > y) return {i, 0};
    return {n - 1, (int)runs[n - 1].text.size()};
}

} // namespace

extern "C" void jd_html_select(jd_html *h, float ax, float ay, float bx, float by)
{
    if (h->box.runs.empty()) return;
    zoom_scope zs(h->zoom);
    float z = h->zoom;
    h->sel_a = hit(h->box.runs, ax / z, ay / z);
    h->sel_b = hit(h->box.runs, bx / z, by / z);
    if (h->sel_b < h->sel_a) std::swap(h->sel_a, h->sel_b);
    h->selected = h->sel_a < h->sel_b;
}

extern "C" int jd_html_select_word(jd_html *h, float x, float y)
{
    const auto &runs = h->box.runs;
    float z = h->zoom;
    x /= z, y /= z;
    for (int i = 0; i < (int)runs.size(); i++) {
        const auto &r = runs[(size_t)i];
        if (y < r.y || y >= r.y + r.h || x < r.x || x >= r.x + r.w) continue;
        int a = 0, b = (int)r.text.size();
        while (a < b && strchr(" \t([{\"'<", r.text[(size_t)a])) a++;
        while (b > a && strchr(" \t.,;:!?)]}\"'>", r.text[(size_t)b - 1])) b--;
        if (a == b) a = 0, b = (int)r.text.size();
        h->sel_a = {i, a}, h->sel_b = {i, b};
        h->selected = a < b;
        h->caret_on = false;
        return h->selected;
    }
    return 0;
}

extern "C" int jd_html_selection_ends(jd_html *h, float *ax, float *ay, float *bx, float *by, float *line_h)
{
    if (!h->selected) return 0;
    zoom_scope zs(h->zoom);
    const auto &runs = h->box.runs;
    const auto &a = runs[(size_t)h->sel_a.run], &b = runs[(size_t)h->sel_b.run];
    float z = h->zoom;
    auto x_at = [](const container::run &r, int at) {
        return r.x + (at ? MeasureTextEx(r.font(), r.text.substr(0, (size_t)at).c_str(), r.size, 0).x : 0);
    };
    *ax = x_at(a, h->sel_a.at) * z, *ay = (a.y + a.size * 0.5f - a.size * 0.625f) * z;
    *bx = x_at(b, h->sel_b.at) * z, *by = (b.y + b.size * 0.5f - b.size * 0.625f) * z;
    *line_h = b.size * 1.25f * z;
    return 1;
}

extern "C" void jd_html_select_all(jd_html *h)
{
    if (h->box.runs.empty()) return;
    h->sel_a = {};
    h->sel_b = {(int)h->box.runs.size() - 1, (int)h->box.runs.back().text.size()};
    h->selected = true;
}

extern "C" void jd_html_select_none(jd_html *h)
{
    h->selected = false;
}

extern "C" int jd_html_has_selection(jd_html *h)
{
    return h->selected;
}

/* the selected text, runs on one line joined by a space where there is a gap, lines by a newline */
extern "C" char *jd_html_selection(jd_html *h)
{
    if (!h->selected) return nullptr;
    const auto &runs = h->box.runs;
    std::string out;
    for (int i = h->sel_a.run; i <= h->sel_b.run && i < (int)runs.size(); i++) {
        int from = i == h->sel_a.run ? h->sel_a.at : 0, to = i == h->sel_b.run ? h->sel_b.at : (int)runs[i].text.size();
        out += runs[(size_t)i].text.substr((size_t)from, (size_t)(to - from));
        if (i == h->sel_b.run || i + 1 >= (int)runs.size()) break;
        const auto &a = runs[(size_t)i], &b = runs[(size_t)i + 1];
        if (b.y >= a.y + a.h * 0.6f) out += b.y > a.y + a.h * 1.8f ? "\n\n" : "\n";
        else if (b.x > a.x + a.w + 0.5f) out += " ";
    }
    return strdup(out.c_str());
}

namespace {

bool same_line(const container::run &a, const container::run &b)
{
    return std::fabs(a.y - b.y) < std::min(a.h, b.h) * 0.5f;
}

float caret_x(const container::run &r, int at)
{
    return r.x + (at ? MeasureTextEx(r.font(), r.text.substr(0, (size_t)at).c_str(), r.size, 0).x : 0);
}

/* in the line holding run i, the place nearest x */
text_pos on_line(const std::vector<container::run> &runs, int i, float x)
{
    int first = i, last = i, n = (int)runs.size();
    while (first > 0 && same_line(runs[(size_t)first - 1], runs[(size_t)i])) first--;
    while (last + 1 < n && same_line(runs[(size_t)last + 1], runs[(size_t)i])) last++;
    for (int k = first; k <= last; k++)
        if (x < runs[(size_t)k].x + runs[(size_t)k].w) return {k, x <= runs[(size_t)k].x ? 0 : offset_in(runs[(size_t)k], x)};
    return {last, (int)runs[(size_t)last].text.size()};
}

} // namespace

extern "C" void jd_html_caret_begin(jd_html *h, float y)
{
    const auto &runs = h->box.runs;
    if (runs.empty()) return;
    y /= h->zoom;
    int i = 0;
    while (i + 1 < (int)runs.size() && runs[(size_t)i].y + runs[(size_t)i].h <= y) i++;
    h->caret = h->anchor = {i, 0};
    h->caret_on = true;
    h->selected = false;
}

extern "C" void jd_html_caret_end(jd_html *h)
{
    h->caret_on = false;
}

/* a vim motion of the caret: h l (character) w b (word) j k (line) 0 $ (line ends) g G (start, end); the
   selection runs from where visual mode started to the caret */
extern "C" void jd_html_caret_move(jd_html *h, int motion)
{
    const auto &runs = h->box.runs;
    int n = (int)runs.size();
    if (!n || !h->caret_on) return;
    zoom_scope zs(h->zoom);
    text_pos &c = h->caret;
    const auto &r = runs[(size_t)c.run];
    int len = (int)r.text.size();
    switch (motion) {
    case 'h':
        if (c.at > 0) {
            c.at--;
            while (c.at > 0 && ((unsigned char)r.text[(size_t)c.at] & 0xc0) == 0x80) c.at--;
        } else if (c.run > 0) c = {c.run - 1, (int)runs[(size_t)c.run - 1].text.size()};
        break;
    case 'l':
        if (c.at < len) {
            c.at++;
            while (c.at < len && ((unsigned char)r.text[(size_t)c.at] & 0xc0) == 0x80) c.at++;
        } else if (c.run + 1 < n) c = {c.run + 1, 0};
        break;
    case 'w':
        if (c.run + 1 < n) c = {c.run + 1, 0};
        else c.at = len;
        break;
    case 'b':
        if (c.at > 0) c.at = 0;
        else if (c.run > 0) c = {c.run - 1, 0};
        break;
    case '0': c = on_line(runs, c.run, -1e9f); break;
    case '$': c = on_line(runs, c.run, 1e9f); break;
    case 'g': c = {0, 0}; break;
    case 'G': c = {n - 1, (int)runs[(size_t)n - 1].text.size()}; break;
    case 'j': case 'k': {
        float x = caret_x(r, c.at);
        int k = c.run;
        if (motion == 'j') {
            while (k < n && same_line(runs[(size_t)k], r)) k++;
            if (k < n) c = on_line(runs, k, x);
        } else {
            while (k >= 0 && same_line(runs[(size_t)k], r)) k--;
            if (k >= 0) c = on_line(runs, k, x);
        }
        break;
    }
    default: break;
    }
    h->sel_a = std::min(h->anchor, h->caret);
    h->sel_b = std::max(h->anchor, h->caret);
    h->selected = h->sel_a < h->sel_b;
}

/* the selection starts again at the caret: nothing selected until it moves */
extern "C" void jd_html_caret_anchor(jd_html *h)
{
    h->anchor = h->caret;
    h->selected = false;
}

/* the caret's top, relative to the document */
extern "C" float jd_html_caret_y(jd_html *h)
{
    if (h->box.runs.empty()) return 0;
    return h->box.runs[(size_t)h->caret.run].y * h->zoom;
}

/* the selection's highlight, under the text, and the caret */
static void draw_selection(jd_html *h, float x, float y)
{
    if (h->caret_on && !h->box.runs.empty()) {
        const auto &r = h->box.runs[(size_t)h->caret.run];
        DrawRectangleV(Vector2{x + caret_x(r, h->caret.at) - 1, y + r.y + r.size * 0.5f - r.size * 0.625f},
                       Vector2{2, r.size * 1.25f}, Color{0x5e, 0x81, 0xac, 0xff});
    }
    if (!h->selected) return;
    const auto &runs = h->box.runs;
    for (int i = h->sel_a.run; i <= h->sel_b.run && i < (int)runs.size(); i++) {
        const auto &r = runs[(size_t)i];
        int from = i == h->sel_a.run ? h->sel_a.at : 0, to = i == h->sel_b.run ? h->sel_b.at : (int)r.text.size();
        float x0 = from ? MeasureTextEx(r.font(), r.text.substr(0, (size_t)from).c_str(), r.size, 0).x : 0;
        float x1 = MeasureTextEx(r.font(), r.text.substr(0, (size_t)to).c_str(), r.size, 0).x;
        /* the gap to the next run on the line is selected too, so a selected phrase reads as one band */
        if (i != h->sel_b.run && i + 1 < (int)runs.size() && runs[(size_t)i + 1].y < r.y + r.h * 0.6f)
            x1 = runs[(size_t)i + 1].x - r.x;
        /* centred on the letters, about half the size below the top raylib draws from */
        DrawRectangleV(Vector2{x + r.x + x0, y + r.y + r.size * 0.5f - r.size * 0.625f}, Vector2{x1 - x0, r.size * 1.25f},
                       Color{0x88, 0xc0, 0xd0, 0x80});
    }
}

/* litehtml measures each word while parsing, so a new zoom parses again: words drawn wider than measured close up
   the spaces between them */
static bool parse(jd_html *h, float z)
{
    h->zoom = z;
    zoom_scope zs(z);
    /* the MIME layer already decoded the part to UTF-8: a <meta charset> in it must not decode it again */
    litehtml::estring text(h->source, litehtml::encoding::utf_8, litehtml::confidence::certain);
    auto doc = litehtml::document::createFromString(text, &h->box, litehtml::master_css, user_css(BASE_SIZE).c_str());
    if (!doc) return false;
    h->doc = doc;
    return true;
}

extern "C" jd_html *jd_html_new(const char *html, const jd_fonts *fonts, float size)
{
    auto *h = new jd_html(fonts, size);
    h->source = html ? html : "";
    h->has_media = h->source.find("@media") != std::string::npos;
    if (!parse(h, h->base_zoom)) {
        delete h;
        return nullptr;
    }
    return h;
}

extern "C" void jd_html_add_image(jd_html *h, const char *src, const char *data, size_t len)
{
    /* stb_image tells PNG, JPEG, GIF and BMP apart by their bytes; the extension only gets past raylib's check */
    Image im = LoadImageFromMemory(".png", (const unsigned char *)data, (int)len);
    if (!im.data) return;
    Texture2D t = LoadTextureFromImage(im);
    UnloadImage(im);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    auto old = h->box.images.find(src);
    if (old != h->box.images.end()) UnloadTexture(old->second);
    h->box.images[src] = t;
}

extern "C" int jd_html_remote_count(jd_html *h)
{
    return (int)h->box.remote.size();
}

extern "C" const char *jd_html_remote_url(jd_html *h, int i)
{
    return h->box.remote[(size_t)i].c_str();
}

extern "C" int jd_html_has_image(jd_html *h, const char *src)
{
    return h->box.images.count(src) > 0;
}

/* fixed-width mail wider than the page shrinks to fit, as phone mail apps do, down to FIT_MIN */
static const float FIT_MIN = 0.45f;

extern "C" float jd_html_layout(jd_html *h, float width)
{
    /* @media rules are matched against box.width when parsing (a mail's "max-width: 600px" phone styles must not
       apply on a wide page), so mail with them is parsed again for a new width. litehtml 0.10's media_changed()
       would avoid that, but its rebuilt render tree lays out nothing. */
    bool media_stale = h->has_media && h->media_w != width;
    if (h->zoom != h->base_zoom || media_stale) {
        h->box.width = width / h->base_zoom;
        parse(h, h->base_zoom);
        h->media_w = width;
    }
    h->box.width = width / h->zoom;
    {
        zoom_scope zs(h->zoom);
        h->doc->render(width / h->zoom);
    }
    /* a smaller zoom gives the mail a wider page, which some of it fills again: a few rounds settle it */
    for (int round = 0; round < 4; round++) {
        float wide = h->doc->width() * h->zoom;
        if (wide <= width + 1 || h->zoom <= h->base_zoom * FIT_MIN + 0.001f) break;
        float z = std::max(h->base_zoom * FIT_MIN, h->zoom * width / wide * 0.99f);
        h->box.width = width / z;
        if (!parse(h, z)) break;
        zoom_scope zs(z);
        h->doc->render(width / z);
    }
    zoom_scope zs(h->zoom);
    float w = width / h->zoom;
    h->height = h->doc->height();
    h->box.runs.clear();
    h->selected = false;
    h->caret_on = false;
    h->box.collecting = true;
    h->box.content_bottom = 0;
    litehtml::position all(0, 0, w + 100000, h->height + 100000);
    h->doc->draw(0, 0, 0, &all);
    h->box.collecting = false;
    /* a page stretched to the viewport by litehtml 0.10's height:100% ends where its content does */
    if (h->height >= VIEW_H / 2 && h->box.content_bottom > 0) h->height = std::min(h->height, h->box.content_bottom);
    return h->height * h->zoom;
}

extern "C" Color jd_html_page(int dark)
{
    return dark ? Color{0x2b, 0x30, 0x3b, 0xff} : Color{0xff, 0xff, 0xff, 0xff};
}

extern "C" void jd_html_set_dark(jd_html *h, int dark)
{
    h->dark = dark != 0;
}

extern "C" void jd_html_draw(jd_html *h, float x, float y, float clip_top, float clip_bottom)
{
    struct dark_scope {
        explicit dark_scope(bool d) { dark_page = d; }
        ~dark_scope() { dark_page = false; }
    } ds(h->dark);
    /* the origin on a whole screen pixel (snap() inside works in the document's units) */
    x = snap(x), y = snap(y);
    zoom_scope zs(h->zoom);
    float z = h->zoom;
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlTranslatef(x, y, 0);
    rlScalef(z, z, 1);
    /* litehtml culls against drawn positions, here the document's own */
    litehtml::position clip(0, (clip_top - y) / z, h->box.width, (clip_bottom - clip_top) / z);
    h->doc->draw(0, 0, 0, &clip);
    /* over the mail, translucent: drawn under it, the backgrounds a mail paints (tables, cells) hid it */
    draw_selection(h, 0, 0);
    rlDrawRenderBatchActive();
    rlPopMatrix();
}

extern "C" char *jd_html_link_at(jd_html *h, float x, float y)
{
    x /= h->zoom, y /= h->zoom;
    litehtml::position::vector boxes;
    h->box.clicked.clear();
    h->doc->on_lbutton_down(x, y, x, y, boxes);
    h->doc->on_lbutton_up(x, y, x, y, boxes);
    return h->box.clicked.empty() ? nullptr : strdup(h->box.clicked.c_str());
}

extern "C" void jd_html_free(jd_html *h)
{
    if (!h) return;
    for (auto &kv : h->box.images) UnloadTexture(kv.second);
    delete h;
}
