/* A query typed as in the search box, for rules: a complete "key:value" operator becomes a pill when a space
   follows it, Backspace at the start takes the last pill back to edit, and the hints the search box shows (operators,
   contacts, folders, flags, dates) open under the field. The search box keeps its own copy of this behaviour because
   its scope and quick filters are pills kept in other state (list.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "ui/icons.h"

static const char *const keys[] = {"from", "to", "subject", "in", "is", "has", "after", "before"};

/* "key:value" with a known key and a value, quotes closed */
static int is_operator(const char *tok, int n)
{
    const char *colon = memchr(tok, ':', (size_t)n);
    if (!colon || colon == tok || colon + 1 >= tok + n) return 0;
    int known = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys && !known; i++)
        known = strlen(keys[i]) == (size_t)(colon - tok) && !strncasecmp(tok, keys[i], (size_t)(colon - tok));
    int quotes = 0;
    for (int i = 0; i < n; i++) quotes += tok[i] == '"';
    return known && quotes % 2 == 0;
}

void qfield_clear(jd_qfield *q)
{
    for (int i = 0; i < q->npills; i++) free(q->pills[i]);
    free(q->pills);
    q->pills = NULL;
    q->npills = 0;
    jd_field_set(&q->text, NULL);
    q->nhints = 0;
    q->sel = -1;
    snprintf(q->hints_for, sizeof q->hints_for, "\x01");
}

char *qfield_query(const jd_qfield *q)
{
    jd_buf b = {0};
    for (int i = 0; i < q->npills; i++) jd_buf_addf(&b, "%s%s", b.len ? " " : "", q->pills[i]);
    const char *t = jd_field_text(&q->text);
    while (*t == ' ') t++;
    if (*t) jd_buf_addf(&b, "%s%s", b.len ? " " : "", t);
    return b.s;
}

/* complete operators (followed by a space, or every one when all) out of the text and into pills */
static void absorb(jd_qfield *q, int all)
{
    const char *t = jd_field_text(&q->text);
    int len = q->text.len, i = 0, took = 0;
    jd_buf keep = {0};
    while (i < len) {
        while (i < len && t[i] == ' ') i++;
        int start = i, quoted = 0;
        while (i < len && (quoted || t[i] != ' ')) {
            if (t[i] == '"') quoted = !quoted;
            i++;
        }
        if (i == start) break;
        int complete = (i < len && !quoted) || all;
        if (complete && is_operator(t + start, i - start)) {
            q->pills = jd_realloc(q->pills, sizeof *q->pills * (size_t)(q->npills + 1));
            q->pills[q->npills++] = jd_strndup(t + start, (size_t)(i - start));
            took = 1;
            continue;
        }
        jd_buf_addf(&keep, "%s%.*s", keep.len ? " " : "", i - start, t + start);
    }
    if (took) {
        if (!all && len && t[len - 1] == ' ' && keep.len) jd_buf_adds(&keep, " ");
        jd_field_set(&q->text, keep.s);
    }
    jd_buf_free(&keep);
}

static const char *typed_word(const jd_qfield *q)
{
    if (q->text.cursor != q->text.len) return NULL;
    const char *word = jd_field_text(&q->text);
    int quoted = 0;
    for (const char *p = jd_field_text(&q->text); *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == ' ' && !quoted) word = p + 1;
    }
    return word;
}

static void hints_update(jd_app *app, jd_qfield *q, int focused)
{
    const char *word = focused ? typed_word(q) : NULL;
    char now[300];
    snprintf(now, sizeof now, "%s", word ? word : "\x01");
    if (!strcmp(now, q->hints_for)) return;
    snprintf(q->hints_for, sizeof q->hints_for, "%s", now);
    q->sel = -1;
    q->nhints = word ? query_hints(app, word, q->hints, (int)(sizeof q->hints / sizeof *q->hints)) : 0;
}

static void hint_accept(jd_qfield *q, int i)
{
    const char *word = typed_word(q);
    if (!word || i < 0 || i >= q->nhints) return;
    char *text = jd_printf("%.*s%s", (int)(word - jd_field_text(&q->text)), jd_field_text(&q->text), q->hints[i].insert);
    jd_field_set(&q->text, text);
    free(text);
    absorb(q, 0);
    snprintf(q->hints_for, sizeof q->hints_for, "\x01");
}

int qfield_keys(jd_app *app, jd_qfield *q)
{
    hints_update(app, q, 1);
    if (q->nhints) {
        if (IsKeyPressed(KEY_DOWN)) {
            q->sel = (q->sel + 1) % q->nhints;
            return 0;
        }
        if (IsKeyPressed(KEY_UP)) {
            q->sel = q->sel <= 0 ? q->nhints - 1 : q->sel - 1;
            return 0;
        }
        if (IsKeyPressed(KEY_TAB) || ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && q->sel >= 0)) {
            hint_accept(q, q->sel < 0 ? 0 : q->sel);
            return 0;
        }
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        absorb(q, 1);
        return 1;
    }
    if (IsKeyPressed(KEY_BACKSPACE) && q->text.cursor == 0 && !q->text.selecting && q->npills) {
        char *last = q->pills[--q->npills];
        char *back = jd_printf("%s%s%s", last, q->text.len ? " " : "", jd_field_text(&q->text));
        jd_field_set(&q->text, back);
        q->text.cursor = (int)strlen(last);
        free(back), free(last);
        return 0;
    }
    int before = q->text.len;
    jd_field_input(&q->text);
    if (q->text.len != before && q->text.cursor == q->text.len) absorb(q, 0);
    return 0;
}

/* one pill: the filter's icon in its colour, the key muted, the value; with an x when removable. Its width. */
static float pill(jd_app *app, const char *tok, int n, float x, float y, float max, int removable, int *remove)
{
    jd_ui *ui = &app->ui;
    char text[300];
    snprintf(text, sizeof text, "%.*s", n, tok);
    const char *colon = strchr(text, ':');
    int klen = colon ? (int)(colon - text) + 1 : 0;
    char key[32];
    snprintf(key, sizeof key, "%.*s", klen < 31 ? klen : 31, text);
    const char *icon;
    Color tint;
    filter_kind(text, &icon, &tint);
    float kw = jd_text_width(ui, key, 13, 0), vw = jd_text_width(ui, text + klen, 13, 1);
    float extra = removable ? 54 : 36;
    if (kw + vw + extra > max) vw = max - kw - extra;
    if (vw < 20) vw = 20;
    Rectangle chip = {x, y, kw + vw + extra, 24};
    Vector2 m = GetMousePosition();
    int hover = CheckCollisionPointRec(m, chip) && !app->dialog.kind;
    DrawRectangleRounded(chip, 0.45f, 6, ColorAlpha(tint, hover ? 0.38f : 0.24f));
    jd_icon(icon, x + 8, y + 5, 14, tint);
    jd_text(ui, key, x + 28, y + 5, 13, JD_NORD4, 0);
    jd_text_fit(ui, text + klen, x + 28 + kw, y + 5, vw, 13, JD_NORD6, 1);
    if (removable) {
        Rectangle xb = {chip.x + chip.width - 22, y + 4, 16, 16};
        int on_x = CheckCollisionPointRec(m, xb) && !app->dialog.kind;
        jd_icon("x", xb.x + 2, xb.y + 2, 12, on_x ? JD_NORD11 : JD_MUTED);
        if (on_x && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) *remove = 1;
    }
    return chip.width;
}

float query_pills_draw(jd_app *app, const char *query, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    float px = x;
    const char *t = query ? query : "";
    int len = (int)strlen(t), i = 0, dummy = 0;
    while (i < len) {
        while (i < len && t[i] == ' ') i++;
        int start = i, quoted = 0;
        while (i < len && (quoted || t[i] != ' ')) {
            if (t[i] == '"') quoted = !quoted;
            i++;
        }
        if (i == start || px > x + w - 30) break;
        if (is_operator(t + start, i - start)) {
            px += pill(app, t + start, i - start, px, y, x + w - px, 0, &dummy) + 6;
        } else {
            char word[256];
            snprintf(word, sizeof word, "%.*s", i - start, t + start);
            jd_text_fit(ui, word, px, y + 5, x + w - px, 14, JD_NORD6, 0);
            px += jd_text_width(ui, word, 14, 0) + 6;
        }
    }
    return px - x;
}

int qfield_draw(jd_app *app, jd_qfield *q, Rectangle box, int focused)
{
    Vector2 m = GetMousePosition();
    DrawRectangleRounded(box, 0.2f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.2f, 6, 1.5f, focused ? JD_NORD8 : JD_NORD2);
    float px = box.x + 6, end = box.x + box.width - 6;
    int on_pill = 0, removed = -1;
    jd_scissor(box.x, box.y, box.width, box.height);
    for (int i = 0; i < q->npills; i++) {
        if (px > end - 80) break;
        int remove = 0;
        Rectangle at = {px, box.y + 5, end - 80 - px, 24};
        float w = pill(app, q->pills[i], (int)strlen(q->pills[i]), px, box.y + (box.height - 24) / 2, end - 80 - px, 1, &remove);
        at.width = w;
        on_pill |= CheckCollisionPointRec(m, at);
        if (remove) removed = i;
        px += w + 6;
    }
    jd_scissor_end();
    q->text.bare = 1;
    q->text.placeholder = q->npills ? NULL : "from:news.example subject:digest";
    int clicked = jd_field_draw(&app->ui, &q->text, (Rectangle){px, box.y, end - px, box.height}, focused);
    clicked |= !on_pill && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, box) && !app->dialog.kind;
    if (removed >= 0) {
        free(q->pills[removed]);
        memmove(q->pills + removed, q->pills + removed + 1, sizeof *q->pills * (size_t)(q->npills - removed - 1));
        q->npills--;
        clicked = 0;
    }
    /* where the hints go; their size stays last frame's, so what lies under them can leave them the clicks */
    q->hints_box.x = box.x, q->hints_box.y = box.y + box.height + 2;
    return clicked;
}

void qfield_hints_draw(jd_app *app, jd_qfield *q, int focused)
{
    hints_update(app, q, focused);
    if (!focused || !q->nhints) {
        q->hints_box.width = q->hints_box.height = 0;
        return;
    }
    jd_ui *ui = &app->ui;
    float x = q->hints_box.x, y = q->hints_box.y, bw = 420;
    if (x + bw > jd_screen_w() - 8) bw = jd_screen_w() - 8 - x;
    Rectangle box = {x, y, bw, (float)q->nhints * 30 + 8};
    DrawRectangleRounded((Rectangle){x + 2, y + 3, bw, box.height}, 0.06f, 6, ColorAlpha(BLACK, 0.25f));
    DrawRectangleRounded(box, 0.06f, 6, JD_NORD2);
    float lw = 0;
    for (int i = 0; i < q->nhints; i++) {
        float t = jd_text_width(ui, q->hints[i].label, 14, 1);
        if (t > lw) lw = t;
    }
    if (lw > bw * 0.6f - 40) lw = bw * 0.6f - 40;
    for (int i = 0; i < q->nhints; i++) {
        Rectangle row = {x + 4, y + 4 + (float)i * 30, bw - 8, 28};
        int hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (i == q->sel || hover) DrawRectangleRounded(row, 0.25f, 6, JD_NORD3);
        const char *icon;
        Color tint;
        filter_kind(q->hints[i].label, &icon, &tint);
        jd_icon(icon, row.x + 10, row.y + 7, 14, tint);
        jd_text_fit(ui, q->hints[i].label, row.x + 32, row.y + 7, lw, 14, JD_NORD6, 1);
        if (q->hints[i].note[0])
            jd_text_fit(ui, q->hints[i].note, row.x + lw + 48, row.y + 7, row.width - lw - 58, 13, JD_MUTED, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            hint_accept(q, i);
            break;
        }
    }
    q->hints_box.width = bw, q->hints_box.height = box.height;
}
