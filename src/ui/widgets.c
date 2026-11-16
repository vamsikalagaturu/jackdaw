#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "platform.h"

static const jd_fonts *role(jd_ui *ui)
{
    return ui->active ? ui->active : &ui->fonts;
}

static Font font(jd_ui *ui, int bold, float size)
{
    return jd_font_at(bold ? role(ui)->semibold : role(ui)->regular, size);
}

void jd_ui_use(jd_ui *ui, const jd_fonts *r)
{
    ui->active = r;
}

/* on a whole screen pixel, so glyphs rasterised for their size land texel on pixel */
static float snap(float v)
{
    float d = jd_text_density();
    return floorf(v * d + 0.5f) / d;
}

void jd_text(jd_ui *ui, const char *s, float x, float y, float size, Color c, int bold)
{
    size = jd_text_size(size);
    DrawTextEx(font(ui, bold, size), s, (Vector2){snap(x), snap(y)}, size, 0, c);
}

float jd_text_width(jd_ui *ui, const char *s, float size, int bold)
{
    size = jd_text_size(size);
    return MeasureTextEx(font(ui, bold, size), s, size, 0).x;
}

static int utf8_prev(const char *s, int i)
{
    do i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80);
    return i < 0 ? 0 : i;
}

static int utf8_next(const char *s, int len, int i)
{
    do i++;
    while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80);
    return i > len ? len : i;
}

void jd_text_fit(jd_ui *ui, const char *s, float x, float y, float width, float size, Color c, int bold)
{
    if (jd_text_width(ui, s, size, bold) <= width) {
        jd_text(ui, s, x, y, size, c, bold);
        return;
    }
    /* one pass over the advances: cutting a character at a time and measuring again cost a phone most of its frame */
    size = jd_text_size(size);
    Font f = font(ui, bold, size);
    float scale = size / (float)f.baseSize, room = width - jd_text_width(ui, "...", size, bold), w = 0;
    int n = (int)strlen(s), cut = 0;
    for (int i = 0; i < n;) {
        int len;
        int g = GetGlyphIndex(f, GetCodepointNext(s + i, &len));
        w += (f.glyphs[g].advanceX ? (float)f.glyphs[g].advanceX : f.recs[g].width) * scale;
        if (w > room) break;
        i += len;
        cut = i;
    }
    char buf[1024];
    if (cut > (int)sizeof buf - 4) cut = (int)sizeof buf - 4;
    memcpy(buf, s, (size_t)cut);
    memcpy(buf + cut, "...", 4);
    jd_text(ui, buf, x, y, size, c, bold);
}

void jd_field_set(jd_field *f, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n >= sizeof f->text) n = sizeof f->text - 1;
    memcpy(f->text, s ? s : "", n);
    f->text[n] = 0;
    f->len = f->cursor = (int)n;
}

static void insert(jd_field *f, const char *s, int n)
{
    if (f->len + n >= (int)sizeof f->text) return;
    memmove(f->text + f->cursor + n, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
    memcpy(f->text + f->cursor, s, (size_t)n);
    f->len += n;
    f->cursor += n;
}

static int pressed(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

void jd_field_input(jd_field *f)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    for (int c; (c = GetCharPressed());) {
        int n;
        const char *u = CodepointToUTF8(c, &n);
        if (c >= 0x20 && !ctrl) insert(f, u, n);
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        for (const char *p = clip; p && *p; p++)
            if ((unsigned char)*p >= 0x20) insert(f, p, 1);
    }
    if (pressed(KEY_BACKSPACE) && f->cursor > 0) {
        int from = ctrl ? 0 : utf8_prev(f->text, f->cursor);
        memmove(f->text + from, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
        f->len -= f->cursor - from;
        f->cursor = from;
    }
    if (pressed(KEY_DELETE) && f->cursor < f->len) {
        int to = utf8_next(f->text, f->len, f->cursor);
        memmove(f->text + f->cursor, f->text + to, (size_t)(f->len - to + 1));
        f->len -= to - f->cursor;
    }
    if (pressed(KEY_LEFT)) f->cursor = utf8_prev(f->text, f->cursor);
    if (pressed(KEY_RIGHT)) f->cursor = utf8_next(f->text, f->len, f->cursor);
    if (IsKeyPressed(KEY_HOME)) f->cursor = 0;
    if (IsKeyPressed(KEY_END)) f->cursor = f->len;
}

/* the one text field menu: its field must be drawn every frame it stays open, or it closes */
static struct {
    jd_field *f;
    int seen;
    jd_popup pop;
} field_menu;

static struct {
    const void *field; /* the last field that reported, to notice a new one */
    int pending, have;
    Rectangle at;
    float view_h;
} follow;

void jd_follow_field(const void *field, Rectangle r)
{
    if (field != follow.field) follow.field = field, follow.pending = 1;
    follow.have = 1;
    follow.at = r;
}

void jd_follow_begin(void) { follow.have = 0; }

void jd_follow_end(float *scroll, Rectangle view)
{
    if (follow.have && (follow.pending || view.height < follow.view_h - 1)) {
        const float margin = 12;
        if (follow.at.y + follow.at.height + margin > view.y + view.height)
            *scroll += follow.at.y + follow.at.height + margin - (view.y + view.height);
        else if (follow.at.y - margin < view.y)
            *scroll -= view.y - (follow.at.y - margin);
        if (*scroll < 0) *scroll = 0;
        follow.pending = 0;
    }
    follow.view_h = view.height;
    follow.have = 0;
}

int jd_field_draw(jd_ui *ui, jd_field *f, Rectangle r, int focused)
{
    const float size = 15, pad = f->bare ? 4 : 10;
    if (focused) jd_follow_field(f, r);
    if (!f->bare) {
        DrawRectangleRounded(r, 0.2f, 6, JD_NORD1);
        DrawRectangleRoundedLinesEx(r, 0.2f, 6, 1.5f, focused ? JD_NORD8 : JD_NORD2);
    }
    char shown[sizeof f->text * 3];
    const char *text = f->text;
    if (f->secret) {
        int n = 0;
        for (int i = 0; i < f->len; i = utf8_next(f->text, f->len, i)) shown[n++] = '*';
        shown[n] = 0;
        text = shown;
    }
    float ty = r.y + (r.height - size) / 2;
    jd_scissor(r.x, r.y, r.width, r.height);
    if (!f->len && f->placeholder) jd_text(ui, f->placeholder, r.x + pad, ty, size, JD_MUTED, 0);
    char before[sizeof shown];
    int cut = f->secret ? 0 : f->cursor;
    if (f->secret)
        for (int i = 0; i < f->cursor; i = utf8_next(f->text, f->len, i)) cut++;
    memcpy(before, text, (size_t)cut);
    before[cut] = 0;
    float cx = jd_text_width(ui, before, size, 0);
    float shift = cx > r.width - 2 * pad ? cx - (r.width - 2 * pad) : 0;
    jd_text(ui, text, r.x + pad - shift, ty, size, JD_NORD6, 0);
    if (focused && (int)(GetTime() * 2) % 2 == 0)
        DrawRectangle((int)(r.x + pad + cx - shift), (int)ty, 1, (int)size + 2, JD_NORD8);
    jd_scissor_end();
    int over = CheckCollisionPointRec(GetMousePosition(), r) &&
               !(field_menu.pop.open && CheckCollisionPointRec(GetMousePosition(), field_menu.pop.box));
    if (f == field_menu.f) field_menu.seen = 1;
    /* a phone's long press arrives as a right click */
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        field_menu.f = f, field_menu.seen = 1;
        jd_popup_open(&field_menu.pop);
        return 1;
    }
    return over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void jd_field_menu_draw(jd_ui *ui)
{
    jd_field *f = field_menu.f;
    if (!f || !field_menu.seen) {
        field_menu.f = NULL, field_menu.pop.open = 0;
        return;
    }
    field_menu.seen = 0;
    const char *clip = GetClipboardText();
    jd_popup_item items[] = {
        {"Paste", "clipboard", "Ctrl+V", !clip || !clip[0]},
        {"Copy", "copy", NULL, f->secret || !f->len},
        {"Cut", "scissors", NULL, f->secret || !f->len},
        {"Clear", "x", NULL, !f->len},
    };
    int k = jd_popup_draw(ui, &field_menu.pop, items, 4);
    if (k == 0)
        for (const char *p = clip; p && *p; p++)
            if ((unsigned char)*p >= 0x20) insert(f, p, 1);
    if (k == 1 || k == 2) SetClipboardText(f->text);
    if (k == 2 || k == 3) jd_field_set(f, "");
    if (!field_menu.pop.open) field_menu.f = NULL;
}

/* width of bytes [from, to), summed per glyph so a long line costs one pass */
static float span_width(jd_ui *ui, const char *s, int from, int to, float size)
{
    size = jd_text_size(size);
    Font f = font(ui, 0, size);
    float scale = size / (float)f.baseSize, w = 0;
    for (int i = from; i < to;) {
        int len;
        int cp = GetCodepointNext(s + i, &len);
        int g = GetGlyphIndex(f, cp);
        w += (f.glyphs[g].advanceX ? (float)f.glyphs[g].advanceX : f.recs[g].width) * scale;
        i += len;
    }
    return w;
}

int *jd_wrap(jd_ui *ui, const char *text, float width, float size, int *nlines)
{
    int len = (int)strlen(text), cap = 64, n = 0;
    int *starts = malloc(sizeof *starts * (size_t)cap);
    int start = 0;
    while (start <= len) {
        if (n == cap) starts = realloc(starts, sizeof *starts * (size_t)(cap *= 2));
        starts[n++] = start;
        int end = start;
        while (end < len && text[end] != '\n') end++;
        if (span_width(ui, text, start, end, size) <= width) {
            start = end + 1;
            continue;
        }
        int fit = start, last_space = -1;
        Font rf = font(ui, 0, size);
        float w = 0, scale = jd_text_size(size) / (float)rf.baseSize;
        while (fit < end) {
            int clen;
            int cp = GetCodepointNext(text + fit, &clen);
            int g = GetGlyphIndex(rf, cp);
            w += (rf.glyphs[g].advanceX ? (float)rf.glyphs[g].advanceX : rf.recs[g].width) * scale;
            if (w > width && fit > start) break;
            if (text[fit] == ' ') last_space = fit;
            fit += clen;
        }
        start = last_space > start ? last_space + 1 : fit;
    }
    *nlines = n;
    return starts;
}

void jd_text_range(jd_ui *ui, const char *text, int from, int to, float x, float y, float size, Color c, int bold)
{
    while (to > from && (text[to - 1] == '\n' || text[to - 1] == ' ' || text[to - 1] == '\r')) to--;
    if (to <= from) return;
    char stack[512], *buf = to - from < (int)sizeof stack ? stack : malloc((size_t)(to - from + 1));
    memcpy(buf, text + from, (size_t)(to - from));
    buf[to - from] = 0;
    jd_text(ui, buf, x, y, size, c, bold);
    if (buf != stack) free(buf);
}

enum { ED_SIZE = 15, ED_LINE = 21 };

void jd_editor_set(jd_editor *e, const char *s)
{
    e->len = e->cursor = 0;
    e->selecting = 0;
    if (e->text) e->text[0] = 0;
    e->wrapped_for = 0;
    if (s) jd_editor_insert(e, s, (int)strlen(s));
    e->cursor = 0;
    e->scroll = 0;
    e->changed = 0;
}

int jd_editor_selection(const jd_editor *e, int *from, int *to)
{
    if (!e->selecting || e->anchor == e->cursor) return 0;
    int a = e->anchor < e->len ? e->anchor : e->len;
    *from = a < e->cursor ? a : e->cursor;
    *to = a < e->cursor ? e->cursor : a;
    return 1;
}

void jd_editor_select_from(jd_editor *e, int anchor)
{
    e->selecting = 1;
    e->anchor = anchor;
}

void jd_editor_deselect(jd_editor *e)
{
    e->selecting = 0;
}

static void editor_delete(jd_editor *e, int from, int to);

void jd_editor_delete_range(jd_editor *e, int from, int to)
{
    e->selecting = 0;
    editor_delete(e, from, to);
}

void jd_editor_insert(jd_editor *e, const char *s, int n)
{
    int from, to;
    if (jd_editor_selection(e, &from, &to)) jd_editor_delete_range(e, from, to);
    e->selecting = 0;
    if (e->len + n + 1 > e->cap) {
        e->cap = (e->len + n + 1) * 2;
        e->text = realloc(e->text, (size_t)e->cap);
    }
    int k = 0;
    for (int i = 0; i < n; i++) k += s[i] != '\r';
    memmove(e->text + e->cursor + k, e->text + e->cursor, (size_t)(e->len - e->cursor));
    for (int i = 0, j = 0; i < n; i++)
        if (s[i] != '\r') e->text[e->cursor + j++] = s[i];
    e->len += k;
    e->cursor += k;
    e->text[e->len] = 0;
    e->wrapped_for = 0;
    e->changed = 1;
}

static void editor_delete(jd_editor *e, int from, int to)
{
    if (from >= to) return;
    memmove(e->text + from, e->text + to, (size_t)(e->len - to + 1));
    e->len -= to - from;
    e->cursor = from;
    e->wrapped_for = 0;
    e->changed = 1;
}

static int line_of(const jd_editor *e, int pos)
{
    int l = 0;
    while (l + 1 < e->nlines && e->starts[l + 1] <= pos) l++;
    return l;
}

static int line_end(const jd_editor *e, int l)
{
    int end = l + 1 < e->nlines ? e->starts[l + 1] : e->len;
    if (end > e->starts[l] && end <= e->len && (e->text[end - 1] == '\n' || (end < e->len && e->text[end - 1] == ' ')))
        end--;
    return end;
}

/* the byte in line l closest to x pixels from its start */
static int at_x(jd_ui *ui, const jd_editor *e, int l, float x)
{
    int i = e->starts[l], end = line_end(e, l);
    while (i < end) {
        int n = utf8_next(e->text, e->len, i);
        float w = span_width(ui, e->text, e->starts[l], n, ED_SIZE);
        if (w > x + (span_width(ui, e->text, i, n, ED_SIZE) / 2)) break;
        i = n;
    }
    return i;
}

int jd_editor_line_start(const jd_editor *e, int pos)
{
    while (pos > 0 && e->text[pos - 1] != '\n') pos--;
    return pos;
}

int jd_editor_line_stop(const jd_editor *e, int pos)
{
    while (pos < e->len && e->text[pos] != '\n') pos++;
    return pos;
}

static int word_char(char c)
{
    return (unsigned char)c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}

/* a cursor move; with select (Shift, or visual mode) the selection follows it, else the selection ends */
static void move_to(jd_editor *e, int pos, int select)
{
    if (select && !e->selecting) jd_editor_select_from(e, e->cursor);
    if (!select) e->selecting = 0;
    e->cursor = pos < 0 ? 0 : pos > e->len ? e->len : pos;
}

static int vertical(jd_ui *ui, jd_editor *e, int dir)
{
    if (!e->nlines) return e->cursor;
    int l = line_of(e, e->cursor), to = l + dir;
    if (to < 0 || to >= e->nlines) return e->cursor;
    return at_x(ui, e, to, span_width(ui, e->text, e->starts[l], e->cursor, ED_SIZE));
}

int jd_editor_motion(jd_ui *ui, jd_editor *e, int key, int shift, int select)
{
    if (!e->text) return 0;
    int pos = e->cursor;
    switch (key) {
    case KEY_H: case KEY_LEFT: pos = utf8_prev(e->text, pos); break;
    case KEY_L: case KEY_RIGHT: pos = utf8_next(e->text, e->len, pos); break;
    case KEY_J: case KEY_DOWN: pos = vertical(ui, e, 1); break;
    case KEY_K: case KEY_UP: pos = vertical(ui, e, -1); break;
    case KEY_W:
        while (pos < e->len && word_char(e->text[pos])) pos++;
        while (pos < e->len && !word_char(e->text[pos])) pos++;
        break;
    case KEY_B:
        while (pos > 0 && !word_char(e->text[pos - 1])) pos--;
        while (pos > 0 && word_char(e->text[pos - 1])) pos--;
        break;
    case KEY_ZERO: case KEY_HOME: pos = jd_editor_line_start(e, pos); break;
    case KEY_FOUR: /* $ */
        if (!shift) return 0;
        pos = jd_editor_line_stop(e, pos);
        break;
    case KEY_END: pos = jd_editor_line_stop(e, pos); break;
    case KEY_G: pos = shift ? e->len : 0; break;
    default: return 0;
    }
    move_to(e, pos, select);
    return 1;
}

void jd_editor_input(jd_ui *ui, jd_editor *e)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!e->text) jd_editor_insert(e, "", 0);
    for (int c; (c = GetCharPressed());) {
        int n;
        const char *u = CodepointToUTF8(c, &n);
        if (c >= 0x20 && !ctrl) jd_editor_insert(e, u, n);
    }
    int from, to, sel = jd_editor_selection(e, &from, &to);
    if (ctrl && IsKeyPressed(KEY_A)) {
        jd_editor_select_from(e, 0);
        e->cursor = e->len;
        return;
    }
    if (ctrl && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)) && sel) {
        char *copy = malloc((size_t)(to - from + 1));
        memcpy(copy, e->text + from, (size_t)(to - from));
        copy[to - from] = 0;
        SetClipboardText(copy);
        free(copy);
        if (IsKeyPressed(KEY_X)) jd_editor_delete_range(e, from, to);
        return;
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip) jd_editor_insert(e, clip, (int)strlen(clip));
    }
    if (pressed(KEY_ENTER) && !ctrl) jd_editor_insert(e, "\n", 1);
    if (pressed(KEY_BACKSPACE) || pressed(KEY_DELETE)) {
        if (sel) jd_editor_delete_range(e, from, to);
        else if (pressed(KEY_BACKSPACE) && e->cursor > 0) editor_delete(e, utf8_prev(e->text, e->cursor), e->cursor);
        else if (pressed(KEY_DELETE) && e->cursor < e->len) editor_delete(e, e->cursor, utf8_next(e->text, e->len, e->cursor));
    }
    int moves[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    for (int i = 0; i < 4; i++)
        if (pressed(moves[i])) {
            int pos = i == 0 ? utf8_prev(e->text, e->cursor) : i == 1 ? utf8_next(e->text, e->len, e->cursor)
                                                                     : vertical(ui, e, i == 2 ? -1 : 1);
            /* without Shift, Left and Right first close a selection at its edge */
            if (sel && !shift && i < 2) pos = i == 0 ? from : to;
            move_to(e, pos, shift);
        }
    if (IsKeyPressed(KEY_HOME) && e->nlines) move_to(e, ctrl ? 0 : e->starts[line_of(e, e->cursor)], shift);
    if (IsKeyPressed(KEY_END) && e->nlines) move_to(e, ctrl ? e->len : line_end(e, line_of(e, e->cursor)), shift);
}

int jd_editor_draw(jd_ui *ui, jd_editor *e, Rectangle r, int focused)
{
    const float pad = 10;
    if (!e->text) jd_editor_insert(e, "", 0), e->changed = 0;
    float w = r.width - 2 * pad;
    if (e->wrapped_for != w) {
        free(e->starts);
        e->starts = jd_wrap(ui, e->text, w, ED_SIZE, &e->nlines);
        e->wrapped_for = w;
    }
    int cl = line_of(e, e->cursor);
    float cy = (float)cl * ED_LINE;
    if (focused) {
        if (cy < e->scroll) e->scroll = cy;
        if (cy + ED_LINE > e->scroll + r.height - 2 * pad) e->scroll = cy + ED_LINE - (r.height - 2 * pad);
        jd_follow_field(e, (Rectangle){r.x, r.y + pad + cy - e->scroll, r.width, ED_LINE});
    }
    int hover = CheckCollisionPointRec(GetMousePosition(), r);
    if (hover) {
        e->scroll -= jd_wheel_y(ED_LINE * 3);
        float max = (float)e->nlines * ED_LINE - (r.height - 2 * pad);
        if (e->scroll > max) e->scroll = max;
        if (e->scroll < 0) e->scroll = 0;
    }
    DrawRectangleRounded(r, 0.02f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(r, 0.02f, 6, 1.5f, focused ? JD_NORD8 : JD_NORD2);
    jd_scissor(r.x, r.y, r.width, r.height);
    int sf, st, sel = jd_editor_selection(e, &sf, &st);
    for (int l = 0; l < e->nlines; l++) {
        float y = r.y + pad + (float)l * ED_LINE - e->scroll;
        if (y + ED_LINE < r.y || y > r.y + r.height) continue;
        int ls = e->starts[l], le = l + 1 < e->nlines ? e->starts[l + 1] : e->len;
        if (sel && sf < le + (l + 1 == e->nlines) && st > ls) {
            int a = sf > ls ? sf : ls, b = st < le ? st : le;
            float x0 = span_width(ui, e->text, ls, a, ED_SIZE), x1 = span_width(ui, e->text, ls, b, ED_SIZE);
            if (st > b || x1 - x0 < 4) x1 += 6; /* the selection runs on past the line's end */
            DrawRectangleV((Vector2){r.x + pad + x0, y + ED_SIZE * 0.5f - ED_SIZE * 0.625f}, (Vector2){x1 - x0, ED_SIZE * 1.25f},
                           ColorAlpha(JD_NORD10, 0.55f));
        }
        /* a quote wrapped onto more rows keeps the quote colour: what counts is where its line starts */
        int src = ls;
        while (src > 0 && e->text[src - 1] != '\n') src--;
        jd_text_range(ui, e->text, ls, le, r.x + pad, y, ED_SIZE, e->text[src] == '>' ? JD_NORD9 : JD_NORD6, 0);
    }
    if (focused && (int)(GetTime() * 2) % 2 == 0) {
        float cx = span_width(ui, e->text, e->starts[cl], e->cursor, ED_SIZE);
        DrawRectangle((int)(r.x + pad + cx), (int)(r.y + pad + cy - e->scroll), 1, ED_SIZE + 2, JD_NORD8);
    }
    jd_scissor_end();
    jd_scrollbar(r, (float)e->nlines * ED_LINE + 2 * pad, &e->scroll);
    int click = hover && !e->inert && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && GetMousePosition().x < r.x + r.width - 12;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && e->dragging) {
        e->dragging = 0;
        if (e->anchor == e->cursor) e->selecting = 0;
    }
    if ((click || e->dragging) && e->nlines) {
        /* a press puts the cursor (Shift+click selects up to it); dragging selects */
        Vector2 m = GetMousePosition();
        int l = (int)((m.y - r.y - pad + e->scroll) / ED_LINE);
        if (l < 0) l = 0;
        if (l >= e->nlines) l = e->nlines - 1;
        int pos = at_x(ui, e, l, m.x - r.x - pad);
        int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (click) {
            if (shift && !e->selecting) jd_editor_select_from(e, e->cursor);
            else if (!shift) jd_editor_select_from(e, pos);
            e->dragging = 1;
        }
        e->cursor = pos;
    }
    return click;
}

void jd_editor_free(jd_editor *e)
{
    free(e->text), free(e->starts);
    *e = (jd_editor){0};
}

static int touch_ui;

void jd_set_touch(int touch) { touch_ui = touch; }

float jd_wheel_y(float notch)
{
    float dx, dy;
    if (plat_scroll(&dx, &dy)) return dy;
    return GetMouseWheelMove() * notch;
}

float jd_wheel_x(float notch)
{
    float dx, dy;
    if (plat_scroll(&dx, &dy)) return dx;
    Vector2 m = GetMouseWheelMoveV();
    return (fabsf(m.x) > fabsf(m.y) ? m.x : m.y) * notch;
}

void jd_scrollbar(Rectangle view, float content, float *scroll)
{
    /* the scroll position being dragged, and where in the thumb it was grabbed */
    static float *dragging;
    static float grab;
    float max = content - view.height;
    if (max <= 0) {
        *scroll = 0;
        if (dragging == scroll) dragging = NULL;
        return;
    }
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
    Rectangle track = {view.x + view.width - 10, view.y + 2, 8, view.height - 4};
    float th = track.height * view.height / content;
    if (th < 32) th = 32;
    float ty = track.y + (track.height - th) * (*scroll / max);
    Rectangle thumb = {track.x, ty, track.width, th};
    Vector2 m = GetMousePosition();
    int near = CheckCollisionPointRec(m, (Rectangle){track.x - 4, track.y, track.width + 6, track.height});
    /* a touch screen's pointer stays where the last finger was: no hover, and a swipe starting at the edge must not
       page */
    int over = near && !touch_ui;
    if (near && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(m, thumb)) {
            dragging = scroll;
            grab = m.y - ty;
        } else if (!touch_ui) {
            *scroll += (m.y < ty ? -1 : 1) * view.height * 0.9f;
        }
    }
    if (dragging == scroll) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) *scroll = (m.y - grab - track.y) / (track.height - th) * max;
        else dragging = NULL;
    }
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
    ty = track.y + (track.height - th) * (*scroll / max);
    Color c = dragging == scroll ? JD_NORD8 : over ? JD_NORD4 : JD_NORD3;
    DrawRectangleRounded((Rectangle){track.x + (over || dragging == scroll ? 0 : 2), ty,
                                     over || dragging == scroll ? 8 : 4, th}, 1, 6, ColorAlpha(c, 0.85f));
}

int jd_switch(jd_ui *ui, float x, float y, int on)
{
    Rectangle r = {x, y, 42, 24};
    int hover = CheckCollisionPointRec(GetMousePosition(), r);
    DrawRectangleRounded(r, 1, 12, on ? JD_NORD10 : hover ? JD_NORD3 : JD_NORD2);
    DrawCircleV((Vector2){on ? x + 30 : x + 12, y + 12}, 9, on ? JD_ON_ACCENT : JD_NORD4);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

int jd_segmented(jd_ui *ui, float x, float y, const char *const *labels, int n, int selected)
{
    float total = 0, widths[8];
    for (int i = 0; i < n && i < 8; i++) total += widths[i] = jd_text_width(ui, labels[i], 13, 1) + 28;
    Rectangle all = {x, y, total + 4, 32};
    DrawRectangleRounded(all, 0.3f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(all, 0.3f, 6, 1, JD_NORD2);
    int picked = selected;
    float cx = x + 2;
    for (int i = 0; i < n && i < 8; i++) {
        Rectangle seg = {cx, y + 2, widths[i], 28};
        int hover = CheckCollisionPointRec(GetMousePosition(), seg);
        if (i == selected) DrawRectangleRounded(seg, 0.3f, 6, JD_NORD10);
        else if (hover) DrawRectangleRounded(seg, 0.3f, 6, JD_NORD2);
        jd_text(ui, labels[i], cx + 14, y + 9, 13, i == selected ? JD_ON_ACCENT : JD_NORD6, 1);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) picked = i;
        cx += widths[i];
    }
    return picked;
}

int jd_icon_button(jd_ui *ui, const char *icon, const char *label, Rectangle r, int primary)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), r);
    Color bg = primary ? JD_NORD10 : JD_NORD2;
    if (hover) bg = primary ? ColorBrightness(JD_NORD10, 0.12f) : JD_NORD3;
    DrawRectangleRounded(r, 0.2f, 6, bg);
    const float size = 15, isz = 16, gap = label && icon ? 8 : 0;
    Color fg = primary ? JD_ON_ACCENT : JD_NORD6;
    float w = (label ? jd_text_width(ui, label, size, 1) : 0) + (icon ? isz + gap : 0);
    float x = r.x + (r.width - w) / 2;
    if (icon) {
        jd_icon(icon, x, r.y + (r.height - isz) / 2, isz, fg);
        x += isz + gap;
    }
    if (label) jd_text(ui, label, x, r.y + (r.height - size) / 2, size, fg, 1);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

int jd_button(jd_ui *ui, const char *label, Rectangle r, int primary)
{
    return jd_icon_button(ui, NULL, label, r, primary);
}

int jd_key_went_down(void)
{
    for (int k = KEY_SPACE; k <= KEY_KP_EQUAL; k++) {
        if (k >= KEY_LEFT_SHIFT && k <= KEY_RIGHT_SUPER) continue;
        if (IsKeyPressed(k)) return 1;
    }
    return 0;
}

static char tip_text[256];
static Vector2 tip_at;

void jd_tooltip(const char *text, float x, float y)
{
    snprintf(tip_text, sizeof tip_text, "%s", text ? text : "");
    tip_at = (Vector2){x, y};
}

void jd_tooltip_draw(jd_ui *ui)
{
    if (!tip_text[0]) return;
    float tw = jd_text_width(ui, tip_text, 12, 0) + 16, x = tip_at.x;
    if (x + tw > jd_screen_w() - 4) x = jd_screen_w() - tw - 4;
    DrawRectangleRounded((Rectangle){x, tip_at.y, tw, 24}, 0.3f, 6, JD_NORD3);
    jd_text(ui, tip_text, x + 8, tip_at.y + 6, 12, JD_NORD6, 0);
    tip_text[0] = 0;
}

void jd_popup_open(jd_popup *p)
{
    p->open = 1;
    p->at = GetMousePosition();
    p->box = (Rectangle){0};
}

int jd_popup_draw(jd_ui *ui, jd_popup *p, const jd_popup_item *items, int n)
{
    if (!p->open) {
        p->box = (Rectangle){0};
        return -1;
    }
    float h = 8, w = 240;
    for (int i = 0; i < n; i++) h += items[i].label ? 30 : 9;
    float x = p->at.x, y = p->at.y;
    if (x + w > jd_screen_w() - 4) x = jd_screen_w() - w - 4;
    if (y + h > jd_screen_h() - 4) y = jd_screen_h() - h - 4;
    Rectangle box = {x, y, w, h};
    p->box = box;
    DrawRectangleRounded((Rectangle){x + 3, y + 4, w, h}, 0.04f, 6, ColorAlpha(BLACK, 0.25f));
    DrawRectangleRounded(box, 0.04f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.04f, 6, 1, JD_NORD2);
    float yy = y + 4;
    int chosen = -1;
    for (int i = 0; i < n; i++) {
        if (!items[i].label) {
            DrawRectangle((int)x + 8, (int)yy + 4, (int)w - 16, 1, JD_NORD2);
            yy += 9;
            continue;
        }
        Rectangle row = {x + 4, yy, w - 8, 28};
        int hover = !items[i].disabled && CheckCollisionPointRec(GetMousePosition(), row);
        Color fg = items[i].disabled ? JD_MUTED : JD_NORD6;
        if (hover) DrawRectangleRounded(row, 0.2f, 6, JD_NORD2);
        if (items[i].icon) jd_icon(items[i].icon, row.x + 8, yy + 6, 16, items[i].disabled ? JD_MUTED : hover ? JD_NORD8 : JD_NORD4);
        jd_text(ui, items[i].label, row.x + 34, yy + 7, 14, fg, 0);
        if (items[i].keys && !ui->touch) {
            float kw = jd_text_width(ui, items[i].keys, 12, 0);
            jd_text(ui, items[i].keys, row.x + row.width - kw - 8, yy + 8, 12, JD_MUTED, 0);
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) chosen = i;
        yy += 30;
    }
    int outside = !CheckCollisionPointRec(GetMousePosition(), box);
    if (chosen >= 0 || IsKeyPressed(KEY_ESCAPE) ||
        (outside && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))))
        p->open = 0;
    return chosen;
}
