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

void tabs_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    DrawRectangleRec(r, JD_NORD1);
    float x = r.x + 8, max_w = 240;
    for (int i = 0; i < app->ntabs; i++) {
        jd_tab *t = &app->tabs[i];
        const char *title = title_of(app, t);
        float tw = jd_text_width(ui, title, 13, 1);
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
        if (!app->dialog.kind && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (i && CheckCollisionPointRec(GetMousePosition(), close)) {
                tab_close(app, i);
                return;
            }
            app->tab = i;
        }
        if (!app->dialog.kind && i && hover && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            tab_close(app, i);
            return;
        }
        x += w + 4;
    }
}
