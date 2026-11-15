#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/util.h"
#include "ui/icons.h"

static const char *title_of(jd_app *app, jd_tab *t);

jd_tab *tab_add(jd_app *app, tab_kind kind)
{
    app->tabs = jd_realloc(app->tabs, sizeof *app->tabs * (size_t)(app->ntabs + 1));
    app->tabs[app->ntabs] = (jd_tab){.kind = kind};
    app->tab = app->ntabs++;
    return &app->tabs[app->tab];
}

void tab_close(jd_app *app, int i)
{
    if (i <= 0 || i >= app->ntabs) return;
    jd_tab *t = &app->tabs[i];
    if (t->kind == TAB_COMPOSE && !compose_may_close(app, t->compose, i)) return;
    conv_close(&t->conv);
    compose_free(t->compose);
    memmove(app->tabs + i, app->tabs + i + 1, sizeof *app->tabs * (size_t)(app->ntabs - i - 1));
    app->ntabs--;
    if (app->tab >= i && app->tab > 0) app->tab--;
}

const char *tab_title(jd_app *app, int i)
{
    return title_of(app, &app->tabs[i]);
}

static const char *title_of(jd_app *app, jd_tab *t)
{
    if (t->kind == TAB_MAIL) return "Mail";
    if (t->kind == TAB_COMPOSE) return compose_title(t->compose);
    if (t->kind == TAB_FILTERS) return "Server filters";
    if (t->kind == TAB_SETTINGS) return "Settings";
    const char *s = t->conv.n ? t->conv.reading[t->conv.n - 1].row.subject : NULL;
    return s && s[0] ? s : "(no subject)";
}

int tabs_keys(jd_app *app)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!ctrl) return 0;
    if (IsKeyPressed(KEY_TAB)) app->tab = (app->tab + (shift ? app->ntabs - 1 : 1)) % app->ntabs;
    else if (IsKeyPressed(KEY_W)) tab_close(app, app->tab);
    else if (IsKeyPressed(KEY_R) && !shift && app->sync) jd_sync_now(app->sync);
    else {
        for (int k = KEY_ONE; k <= KEY_NINE; k++)
            if (IsKeyPressed(k) && k - KEY_ONE < app->ntabs) {
                app->tab = k - KEY_ONE;
                return 1;
            }
        return 0;
    }
    return 1;
}

static float tab_width(jd_app *app, int i)
{
    float tw = jd_text_width(&app->ui, title_of(app, &app->tabs[i]), 13, 1), max_w = 240;
    return (tw > max_w ? max_w : tw) + (i ? 62 : 40);
}

/* Tabs that do not fit scroll sideways (a sideways swipe or the wheel over the bar), with a scrollbar under
   them; the active tab is brought into view when it changes. */
void tabs_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    DrawRectangleRec(r, JD_NORD1);
    static float scroll;
    static int shown_tab = -1;
    float total = 8, active_x = 0, active_w = 0;
    for (int i = 0; i < app->ntabs; i++) {
        if (i == app->tab) active_x = total, active_w = tab_width(app, i);
        total += tab_width(app, i) + 4;
    }
    total += 4;
    if (CheckCollisionPointRec(GetMousePosition(), r)) scroll -= GetMouseWheelMove() * 40;
    if (app->tab != shown_tab) {
        if (active_x < scroll) scroll = active_x - 8;
        if (active_x + active_w > scroll + r.width) scroll = active_x + active_w - r.width + 8;
        shown_tab = app->tab;
    }
    if (scroll > total - r.width) scroll = total - r.width;
    if (scroll < 0) scroll = 0;
    jd_scissor(r.x, r.y, r.width, r.height);
    float x = r.x + 8 - scroll;
    for (int i = 0; i < app->ntabs; i++) {
        jd_tab *t = &app->tabs[i];
        const char *title = title_of(app, t);
        float tw = jd_text_width(ui, title, 13, 1), max_w = 240;
        float w = (tw > max_w ? max_w : tw) + (i ? 62 : 40);
        Rectangle tab = {x, r.y + 4, w, r.height - 4};
        int hover = CheckCollisionPointRec(GetMousePosition(), tab);
        if (i == app->tab) DrawRectangleRounded(tab, 0.25f, 6, JD_NORD0);
        else if (hover) DrawRectangleRounded(tab, 0.25f, 6, JD_NORD2);
        static const char *icons[] = {"inbox", "mail", "pencil", "flag", "settings"};
        const char *icon = icons[t->kind];
        jd_icon(icon, x + 10, r.y + 12, 14, i == app->tab ? JD_NORD8 : JD_NORD4);
        jd_text_fit(ui, title, x + 30, r.y + 12, w - (i ? 62 : 40), 13, i == app->tab ? JD_NORD6 : JD_NORD4, 1);
        Rectangle close = {x + w - 26, r.y + 9, 18, 18};
        if (i) jd_icon("x", close.x + 2, close.y + 2, 14, CheckCollisionPointRec(GetMousePosition(), close) ? JD_NORD11 : JD_NORD3);
        int closing = -1;
        if (!app->dialog.kind && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (i && CheckCollisionPointRec(GetMousePosition(), close)) closing = i;
            else app->tab = i;
        }
        if (!app->dialog.kind && i && hover && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) closing = i;
        if (closing >= 0) {
            tab_close(app, closing);
            break;
        }
        x += w + 4;
    }
    jd_scissor_end();
    if (total <= r.width) return;
    /* the scrollbar: its thumb drags, a press on the track pages */
    static int dragging;
    static float grab;
    Rectangle track = {r.x, r.y + r.height - 4, r.width, 4};
    float thumb_w = r.width * r.width / total, thumb_x = r.x + scroll / total * r.width;
    Rectangle thumb = {thumb_x, track.y, thumb_w, 4}, hit = {track.x, track.y - 6, track.width, 10};
    Vector2 m = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, hit) && !app->dialog.kind) {
        if (m.x >= thumb.x && m.x <= thumb.x + thumb.width) dragging = 1, grab = m.x - thumb.x;
        else scroll += m.x < thumb.x ? -r.width : r.width;
    }
    if (dragging) {
        scroll = (m.x - grab - r.x) / r.width * total;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragging = 0;
    }
    DrawRectangleRounded(thumb, 1, 4, dragging || CheckCollisionPointRec(m, hit) ? JD_NORD8 : JD_NORD3);
}
