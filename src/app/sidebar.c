#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "ui/icons.h"

const char *folder_label(const jd_folder_row *f)
{
    if (strcasecmp(f->name, "INBOX") == 0) return "Inbox";
    const char *leaf = f->delim ? strrchr(f->name, f->delim) : NULL;
    return leaf ? leaf + 1 : f->name;
}

static const char *folder_icon(const jd_folder_row *f)
{
    static const struct {
        const char *use, *icon;
    } icons[] = {{"Drafts", "file-pen"}, {"Sent", "send"}, {"Archive", "archive"}, {"Junk", "shield-alert"},
                 {"Trash", "trash-2"}, {"Flagged", "star"}, {"All", "mail"}, {NULL, NULL}};
    if (strcasecmp(f->name, "INBOX") == 0) return "inbox";
    for (int i = 0; icons[i].use; i++)
        if (jd_streq(f->special_use, icons[i].use)) return icons[i].icon;
    return "folder";
}

static int depth_of(const jd_folder_row *f)
{
    int d = 0;
    for (const char *c = f->name; f->delim && *c; c++) d += *c == f->delim;
    return d;
}

/* a child's name is its parent's, the delimiter, then more */
static int is_under(const jd_folder_row *child, const jd_folder_row *parent)
{
    size_t n = strlen(parent->name);
    return parent->delim && strncmp(child->name, parent->name, n) == 0 && child->name[n] == parent->delim;
}

static int has_children(jd_app *app, int i)
{
    for (int j = 0; j < app->nfolders; j++)
        if (j != i && is_under(&app->folders[j], &app->folders[i])) return 1;
    return 0;
}

int folder_hidden(jd_app *app, int i)
{
    for (int j = 0; j < app->nfolders; j++)
        if (j != i && is_under(&app->folders[i], &app->folders[j]) && app->folders[j].collapsed) return 1;
    return 0;
}

static void toggle_collapsed(jd_app *app, int i)
{
    jd_folder_row *f = &app->folders[i];
    f->collapsed = !f->collapsed;
    jd_folder_set_collapsed(app->view, f->id, f->collapsed);
}

/* on a phone the full sidebar is a drawer (mail_frame): whatever is chosen in it closes it */
static void drawer_done(jd_app *app)
{
    if (!plat_touch() || app->prefs.sidebar_rail) return;
    app->prefs.sidebar_rail = 1;
    save_prefs(app);
}

static void select_folder(jd_app *app, int i)
{
    drawer_done(app);
    app->tab = 0;
    if (i == app->folder) return;
    app->folder = i;
    app->row = 0;
    app->list_scroll = 0;
    reader_close(app);
    list_reload(app);
}

/* the folder with this special use (RFC 6154), else one whose own name is it ("Sent" under "INBOX." too);
   "INBOX" is the Inbox. Shows it and returns 1, or 0 when there is none. */
int folder_go(jd_app *app, const char *use)
{
    int found = -1;
    for (int i = 0; i < app->nfolders && found < 0; i++) {
        const jd_folder_row *f = &app->folders[i];
        if (!strcasecmp(use, "INBOX") ? !strcasecmp(f->name, "INBOX") : jd_streq(f->special_use, use)) found = i;
    }
    for (int i = 0; i < app->nfolders && found < 0; i++)
        if (!strcasecmp(folder_label(&app->folders[i]), use)) found = i;
    if (found < 0) return 0;
    app->pane = PANE_LIST;
    select_folder(app, found);
    return 1;
}

static void step(jd_app *app, int dir)
{
    for (int i = app->folder + dir; i >= 0 && i < app->nfolders; i += dir)
        if (!folder_hidden(app, i)) {
            select_folder(app, i);
            return;
        }
}

int sidebar_keys(jd_app *app)
{
    if (!app->nfolders) return 0;
    const jd_folder_row *f = &app->folders[app->folder];
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) step(app, 1);
    else if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) step(app, -1);
    else if (IsKeyPressed(KEY_SPACE) && has_children(app, app->folder)) toggle_collapsed(app, app->folder);
    else if (IsKeyPressed(KEY_A) && !shift) {
        char *prefix = f->delim && !jd_streq(f->name, "INBOX") ? jd_printf("%s%c", f->name, f->delim) : jd_strdup("");
        dialog_open(app, DLG_PROMPT, ACT_CREATE_FOLDER, "New folder (full path)", prefix);
        free(prefix);
    } else if (IsKeyPressed(KEY_R) && shift && !jd_streq(f->name, "INBOX")) {
        dialog_open(app, DLG_PROMPT, ACT_RENAME_FOLDER, "Rename folder (full path)", f->name);
    } else if (IsKeyPressed(KEY_D) && shift && !jd_streq(f->name, "INBOX")) {
        char *title = jd_printf("Delete the folder %s and all %d messages in it, on the server?", f->name, f->total);
        dialog_open(app, DLG_CONFIRM, ACT_DELETE_FOLDER, title, NULL);
        free(title);
    } else {
        return 0;
    }
    return 1;
}

static void tooltip(jd_app *app, const char *text, float x, float y)
{
    (void)app;
    jd_tooltip(text, x, y);
}

/* an icon button; returns 1 when clicked, shows tip on hover */
static int icon_button(jd_app *app, const char *icon, Rectangle b, const char *tip, Color tint, const char **show_tip,
                       Vector2 *tip_at)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open;
    if (hover) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
    jd_icon(icon, b.x + (b.width - 18) / 2, b.y + (b.height - 18) / 2, 18, hover ? JD_NORD8 : tint);
    if (hover && tip) *show_tip = tip, *tip_at = (Vector2){b.x + b.width + 6, b.y + (b.height - 24) / 2};
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/* the open tabs as rows (tabs set to the sidebar); returns the y below them */
static float tab_rows(jd_app *app, Rectangle r, float y, int rail)
{
    jd_ui *ui = &app->ui;
    static const char *icons[] = {"inbox", "mail", "pencil", "flag", "settings"};
    for (int i = 0; i < app->ntabs; i++) {
        Rectangle box = {r.x + 8, y, r.width - 16, 30};
        int hover = CheckCollisionPointRec(GetMousePosition(), box) && !app->dialog.kind && !app->menu.open;
        if (i == app->tab) DrawRectangleRounded(box, 0.25f, 6, JD_NORD2);
        else if (hover) DrawRectangleRounded(box, 0.25f, 6, ColorAlpha(JD_NORD2, 0.5f));
        jd_icon(icons[app->tabs[i].kind], box.x + (rail ? (box.width - 16) / 2 : 12), y + 7, 16,
                i == app->tab ? JD_NORD8 : JD_NORD4);
        Rectangle close = {box.x + box.width - 24, y + 6, 18, 18};
        if (!rail) {
            jd_text_fit(ui, tab_title(app, i), box.x + 36, y + 8, box.width - 66, 14, JD_NORD6, i == app->tab);
            if (i && hover) jd_icon("x", close.x + 2, close.y + 2, 14, CheckCollisionPointRec(GetMousePosition(), close) ? JD_NORD11 : JD_MUTED);
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (i && !rail && CheckCollisionPointRec(GetMousePosition(), close)) {
                tab_close(app, i);
                return y;
            }
            app->tab = i;
        }
        if (hover && i && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            tab_close(app, i);
            return y;
        }
        y += 32;
    }
    DrawRectangle((int)r.x + 12, (int)y + 4, (int)r.width - 24, 1, JD_NORD2);
    return y + 12;
}

/* icons only: compose, tabs, folders with unread badges, settings; tooltips name them */
static void rail_draw(jd_app *app, Rectangle r)
{
    const char *tip = NULL;
    Vector2 tip_at = {0};
    float cx = r.x + (r.width - 40) / 2;
    /* in line with the list's header: the mark with the search field (the panel starts at its top), the toggle
       with the folder's name, the pencil with the filter chips (list_draw) */
    Rectangle mark = {cx, r.y + 14, 40, 34};
    jd_icon("bird", cx + 9, r.y + 20, 22, JD_NORD8);
    if (CheckCollisionPointRec(GetMousePosition(), mark))
        tip = "jackdaw " JD_VERSION ", built " JD_BUILD_DATE, tip_at = (Vector2){mark.x + 46, mark.y + 6};
    if (icon_button(app, "panel-left-open", (Rectangle){cx, r.y + 49, 40, 36}, "Show the sidebar (Ctrl+B)", JD_NORD4, &tip, &tip_at)) {
        app->prefs.sidebar_rail = 0;
        save_prefs(app);
    }
    Rectangle write = {cx, r.y + 85, 40, 36};
    int hover_write = CheckCollisionPointRec(GetMousePosition(), write) && !app->dialog.kind && !app->menu.open;
    DrawRectangleRounded(write, 0.3f, 6, hover_write ? ColorBrightness(JD_NORD10, 0.12f) : JD_NORD10);
    jd_icon("pencil", write.x + 11, write.y + 9, 18, JD_ON_ACCENT);
    if (hover_write) tip = "New message (Ctrl+N)", tip_at = (Vector2){write.x + 46, write.y + 6};
    if (hover_write && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) compose_open(app, COMPOSE_NEW, 0);
    float y = r.y + 134;
    if (app->prefs.tabs_side && app->ntabs > 1) y = tab_rows(app, r, y, 1);
    float bottom = r.y + r.height - 50;
    for (int i = 0; i < app->nfolders && y < bottom - 40; i++) {
        if (folder_hidden(app, i) || depth_of(&app->folders[i]) > 0) continue;
        const jd_folder_row *f = &app->folders[i];
        Rectangle b = {cx, y, 40, 36};
        int active = i == app->folder && app->tab == 0;
        if (active) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
        if (icon_button(app, folder_icon(f), b, folder_label(f), active ? JD_NORD8 : JD_NORD4, &tip, &tip_at))
            select_folder(app, i);
        if (f->unread) {
            char count[16];
            snprintf(count, sizeof count, "%d", f->unread > 99 ? 99 : f->unread);
            float cw = jd_text_width(&app->ui, count, 10, 1) + 8;
            Rectangle badge = {b.x + b.width - cw + 2, b.y + 1, cw, 15};
            DrawRectangleRounded(badge, 0.5f, 6, JD_NORD10);
            jd_text(&app->ui, count, badge.x + 4, badge.y + 2, 10, JD_ON_ACCENT, 1);
        }
        y += 40;
    }
    DrawRectangle((int)r.x + 10, (int)bottom, (int)r.width - 20, 1, JD_NORD2);
    if (icon_button(app, "settings", (Rectangle){cx, bottom + 8, 40, 34}, "Settings (Ctrl+,)", JD_NORD4, &tip, &tip_at))
        settings_open(app);
    if (tip) tooltip(app, tip, tip_at.x, tip_at.y);
}

void sidebar_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    const float row = 30;
    int free_input = !app->dialog.kind && !app->menu.open;
    if (app->prefs.sidebar_rail) {
        /* the rail's panel starts level with the list's search field, its top corner rounded */
        Rectangle panel = {r.x, r.y + 14, r.width, r.height - 14};
        DrawRectangleRounded((Rectangle){panel.x, panel.y, panel.width, 40}, 0.3f, 6, JD_NORD1);
        DrawRectangleRec((Rectangle){panel.x, panel.y + 12, panel.width, panel.height - 12}, JD_NORD1);
        DrawRectangleRec((Rectangle){panel.x, panel.y, 12, 12}, JD_NORD1);
        rail_draw(app, r);
        return;
    }
    DrawRectangleRec(r, JD_NORD1);
    /* the app's name and mark, then the account */
    jd_icon("bird", r.x + 16, r.y + 16, 22, JD_NORD8);
    jd_text(ui, "jackdaw", r.x + 46, r.y + 17, 18, JD_NORD6, 1);
    const char *tip = NULL;
    Vector2 tip_at = {0};
    if (icon_button(app, "panel-left-close", (Rectangle){r.x + r.width - 42, r.y + 10, 34, 34}, "Icons only (Ctrl+B)", JD_MUTED,
                    &tip, &tip_at)) {
        app->prefs.sidebar_rail = 1;
        save_prefs(app);
    }
    tip_at.x = r.x + r.width - 160;
    tip_at.y = r.y + 48;
    char initial[8] = "?";
    if (app->account.address) snprintf(initial, sizeof initial, "%c", toupper((unsigned char)app->account.address[0]));
    DrawCircleV((Vector2){r.x + 30, r.y + 74}, 14, JD_NORD10);
    jd_text(ui, initial, r.x + 30 - jd_text_width(ui, initial, 14, 1) / 2, r.y + 67, 14, JD_ON_ACCENT, 1);
    jd_text_fit(ui, app->account.name ? app->account.name : "", r.x + 52, r.y + 58, r.width - 64, 15, JD_NORD6, 1);
    jd_text_fit(ui, app->account.address ? app->account.address : "", r.x + 52, r.y + 76, r.width - 64, 12, JD_MUTED, 0);

    Rectangle write = {r.x + 12, r.y + 102, r.width - 24, 38};
    int hover_write = CheckCollisionPointRec(GetMousePosition(), write) && free_input;
    DrawRectangleRounded(write, 0.3f, 6, hover_write ? ColorBrightness(JD_NORD10, 0.12f) : JD_NORD10);
    jd_icon("pencil", write.x + 14, write.y + 11, 16, JD_ON_ACCENT);
    jd_text(ui, "New message", write.x + 40, write.y + 11, 14, JD_ON_ACCENT, 1);
    if (hover_write && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) drawer_done(app), compose_open(app, COMPOSE_NEW, 0);

    float y = r.y + 154, bottom = r.y + r.height - 82;
    if (app->prefs.tabs_side && app->ntabs > 1) y = tab_rows(app, r, y, 0);
    for (int i = 0; i < app->nfolders && y < bottom - row; i++) {
        if (folder_hidden(app, i)) continue;
        const jd_folder_row *f = &app->folders[i];
        Rectangle box = {r.x + 8, y, r.width - 16, row - 2};
        int hover = CheckCollisionPointRec(GetMousePosition(), box) && free_input;
        if (i == app->folder && app->tab == 0) {
            DrawRectangleRounded(box, 0.25f, 6, JD_NORD2);
            if (app->pane == PANE_FOLDERS && !plat_touch()) DrawRectangleRoundedLinesEx(box, 0.25f, 6, 1.5f, JD_NORD8);
        }
        else if (hover) DrawRectangleRounded(box, 0.25f, 6, ColorAlpha(JD_NORD2, 0.5f));
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app->pane = PANE_FOLDERS;
            select_folder(app, i);
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            select_folder(app, i);
            app->menu = (jd_menu){.open = 1, .at = GetMousePosition(), .on_folder = 1, .index = i};
        }
        float indent = 22 + 16 * (float)depth_of(f);
        if (has_children(app, i)) {
            Rectangle arrow = {box.x + indent - 20, y, 18, row - 2};
            jd_icon(f->collapsed ? "chevron-right" : "chevron-down", arrow.x + 2, y + 7, 14, JD_NORD4);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), arrow))
                toggle_collapsed(app, i);
        }
        jd_icon(folder_icon(f), box.x + indent, y + 6, 16, i == app->folder ? JD_NORD8 : JD_NORD4);
        indent += 24;
        char count[16] = "";
        if (f->unread) snprintf(count, sizeof count, "%d", f->unread);
        float cw = jd_text_width(ui, count, 14, 1);
        jd_text_fit(ui, folder_label(f), box.x + indent, y + 7, box.width - indent - cw - 20, 15, JD_NORD6, f->unread > 0);
        if (f->unread) jd_text(ui, count, box.x + box.width - cw - 10, y + 7, 14, JD_NORD8, 1);
        y += row;
    }

    if (tip) tooltip(app, tip, tip_at.x, tip_at.y);
    DrawRectangle((int)r.x + 12, (int)bottom, (int)r.width - 24, 1, JD_NORD2);
    struct {
        const char *icon, *label;
    } foot[] = {{"settings", "Settings"}, {"circle-help", "Keys (?)"}};
    float fx = r.x + 12;
    for (int k = 0; k < 2; k++) {
        float fw = jd_text_width(ui, foot[k].label, 13, 0) + 40;
        Rectangle b = {fx, bottom + 8, fw, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && free_input;
        if (hover) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
        jd_icon(foot[k].icon, fx + 10, bottom + 16, 16, hover ? JD_NORD8 : JD_NORD4);
        jd_text(ui, foot[k].label, fx + 32, bottom + 17, 13, JD_NORD4, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            drawer_done(app);
            if (k == 0) settings_open(app);
            else app->help = 1;
        }
        fx += fw + 4;
    }
    /* two lines, so a narrow sidebar does not cut the build date off */
    jd_text_fit(ui, "jackdaw " JD_VERSION, r.x + 22, bottom + 46, r.width - 34, 11, JD_MUTED, 0);
    jd_text_fit(ui, "built " JD_BUILD_DATE, r.x + 22, bottom + 61, r.width - 34, 11, JD_MUTED, 0);
}
