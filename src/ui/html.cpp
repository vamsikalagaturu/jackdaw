#include "html.h"

#include <litehtml.h>

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

/* the page mail is drawn on, and its default text: mail written for white paper stays readable */
std::string user_css(float size)
{
    return "html, body { background: transparent; color: #2e3440; font-family: Inter; font-size: " +
           std::to_string((int)size) + "px; }";
}

struct font_handle {
    Font font;
    float size;
    int decoration;
};

Color to_color(const litehtml::web_color &c)
{
    return Color{c.red, c.green, c.blue, c.alpha};
}

class container : public litehtml::document_container {
public:
    container(const jd_fonts *fonts, float size) : size(size), fonts(*fonts) {}

    float size;
    float width = 600;
    std::string clicked;

    /* The text as laid out, in drawing (reading) order, for selecting it: recorded by a drawing pass that
       draws nothing, after each layout. Positions are relative to the document's top left. */
    struct run {
        float x, y, w, h;
        std::string text;
        Font font;
        float size;
    };
    std::vector<run> runs;
    bool collecting = false;

    litehtml::uint_ptr create_font(const litehtml::font_description &d, const litehtml::document *,
                                   litehtml::font_metrics *fm) override
    {
        auto *f = new font_handle{d.weight >= 600 ? fonts.semibold : fonts.regular, d.size > 0 ? d.size : size,
                                  d.decoration_line};
        if (fm) {
            fm->font_size = f->size;
            fm->ascent = f->size * ASCENT;
            fm->descent = f->size * DESCENT;
            fm->height = fm->ascent + fm->descent;
            fm->x_height = f->size * 0.53f;
            fm->ch_width = MeasureTextEx(f->font, "0", f->size, 0).x;
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
        return MeasureTextEx(f->font, text, f->size, 0).x;
    }

    void draw_text(litehtml::uint_ptr, const char *text, litehtml::uint_ptr h, litehtml::web_color color,
                   const litehtml::position &pos) override
    {
        auto *f = reinterpret_cast<font_handle *>(h);
        /* raylib draws from the line's top; litehtml's box bottom sits DESCENT below the baseline */
        float top = pos.y + pos.height - f->size * (ASCENT + DESCENT);
        if (collecting) {
            if (text && text[0])
                runs.push_back(run{pos.x, top, MeasureTextEx(f->font, text, f->size, 0).x, f->size * (ASCENT + DESCENT),
                                   text, f->font, f->size});
            return;
        }
        Color c = to_color(color);
        DrawTextEx(f->font, text, Vector2{pos.x, top}, f->size, 0, c);
        float w = MeasureTextEx(f->font, text, f->size, 0).x, t = std::max(1.0f, f->size / 14);
        if (f->decoration & litehtml::text_decoration_line_underline)
            DrawRectangleV(Vector2{pos.x, top + f->size * ASCENT + t}, Vector2{w, t}, c);
        if (f->decoration & litehtml::text_decoration_line_line_through)
            DrawRectangleV(Vector2{pos.x, top + f->size * ASCENT * 0.65f}, Vector2{w, t}, c);
    }

    litehtml::pixel_t pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    litehtml::pixel_t get_default_font_size() const override { return size; }
    const char *get_default_font_name() const override { return "Inter"; }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &m) override
    {
        if (collecting) return;
        Color c = to_color(m.color);
        float cx = m.pos.x + m.pos.width / 2, cy = m.pos.y + m.pos.height / 2, r = m.pos.width / 2;
        switch (m.marker_type) {
        case litehtml::list_style_type_circle: DrawCircleLinesV(Vector2{cx, cy}, r, c); break;
        case litehtml::list_style_type_square: DrawRectangleV(Vector2{m.pos.x, m.pos.y}, Vector2{m.pos.width, m.pos.height}, c); break;
        case litehtml::list_style_type_disc: DrawCircleV(Vector2{cx, cy}, r, c); break;
        default: {
            auto *f = reinterpret_cast<font_handle *>(m.font);
            std::string n = std::to_string(m.index) + ".";
            float w = f ? MeasureTextEx(f->font, n.c_str(), f->size, 0).x : 0;
            if (f) DrawTextEx(f->font, n.c_str(), Vector2{m.pos.x + m.pos.width - w, m.pos.y - f->size * 0.1f}, f->size, 0, c);
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
        auto it = images.find(url);
        if (collecting || it == images.end()) return;
        const Texture2D &t = it->second;
        const auto &b = layer.origin_box;
        DrawTexturePro(t, Rectangle{0, 0, (float)t.width, (float)t.height}, Rectangle{b.x, b.y, b.width, b.height},
                       Vector2{0, 0}, 0, WHITE);
    }

    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer &layer, const litehtml::web_color &color) override
    {
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
        if (collecting || root) return;
        auto side = [](const litehtml::border &s) { return s.width > 0 && s.style > litehtml::border_style_hidden; };
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
        v.height = 100000;
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
        m.height = m.device_height = 100000;
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

struct jd_html {
    container box;
    litehtml::document::ptr doc;
    float height = 0;
    bool selected = false;
    text_pos sel_a, sel_b;
    /* the keyboard caret of visual mode, and where its selection started */
    bool caret_on = false;
    text_pos caret, anchor;
    jd_html(const jd_fonts *f, float size) : box(f, size) {}
};

namespace {

/* the byte in run r closest to x (relative to the document) */
int offset_in(const container::run &r, float x)
{
    int i = 0, n = (int)r.text.size();
    while (i < n) {
        int next = i + 1;
        while (next < n && ((unsigned char)r.text[next] & 0xc0) == 0x80) next++;
        float w_next = MeasureTextEx(r.font, r.text.substr(0, (size_t)next).c_str(), r.size, 0).x;
        float w_here = i ? MeasureTextEx(r.font, r.text.substr(0, (size_t)i).c_str(), r.size, 0).x : 0;
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
    h->sel_a = hit(h->box.runs, ax, ay);
    h->sel_b = hit(h->box.runs, bx, by);
    if (h->sel_b < h->sel_a) std::swap(h->sel_a, h->sel_b);
    h->selected = h->sel_a < h->sel_b;
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
    return r.x + (at ? MeasureTextEx(r.font, r.text.substr(0, (size_t)at).c_str(), r.size, 0).x : 0);
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
    return h->box.runs[(size_t)h->caret.run].y;
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
        float x0 = from ? MeasureTextEx(r.font, r.text.substr(0, (size_t)from).c_str(), r.size, 0).x : 0;
        float x1 = MeasureTextEx(r.font, r.text.substr(0, (size_t)to).c_str(), r.size, 0).x;
        /* the gap to the next run on the line is selected too, so a selected phrase reads as one band */
        if (i != h->sel_b.run && i + 1 < (int)runs.size() && runs[(size_t)i + 1].y < r.y + r.h * 0.6f)
            x1 = runs[(size_t)i + 1].x - r.x;
        /* centred on the letters, about half the size below the top raylib draws from */
        DrawRectangleV(Vector2{x + r.x + x0, y + r.y + r.size * 0.5f - r.size * 0.625f}, Vector2{x1 - x0, r.size * 1.25f},
                       Color{0x88, 0xc0, 0xd0, 0x80});
    }
}

extern "C" jd_html *jd_html_new(const char *html, const jd_fonts *fonts, float size)
{
    auto *h = new jd_html(fonts, size);
    h->doc = litehtml::document::createFromString(html ? html : "", &h->box, litehtml::master_css,
                                                  user_css(size).c_str());
    if (!h->doc) {
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

extern "C" float jd_html_layout(jd_html *h, float width)
{
    h->box.width = width;
    h->doc->render(width);
    h->height = h->doc->height();
    h->box.runs.clear();
    h->selected = false;
    h->caret_on = false;
    h->box.collecting = true;
    litehtml::position all(0, 0, width + 100000, h->height + 100000);
    h->doc->draw(0, 0, 0, &all);
    h->box.collecting = false;
    return h->height;
}

extern "C" void jd_html_draw(jd_html *h, float x, float y, float clip_top, float clip_bottom)
{
    draw_selection(h, x, y);
    /* litehtml culls against drawn (screen) positions */
    litehtml::position clip(x, clip_top, h->box.width, clip_bottom - clip_top);
    h->doc->draw(0, x, y, &clip);
}

extern "C" char *jd_html_link_at(jd_html *h, float x, float y)
{
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
