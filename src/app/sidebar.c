#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "ui/fx.h"
#include "ui/icons.h"

/* the accounts list under the sidebar's account (accounts_menu_draw draws it over everything) */
static jd_popup accounts_menu;

/* on a phone the full sidebar is a drawer (mail_frame): whatever is chosen in it closes it */
static void drawer_done(jd_app *app)
{
    if (!plat_touch() || app->prefs.sidebar_rail) return;
    app->prefs.sidebar_rail = 1;
    save_prefs(app);
}

int accounts_menu_open(void)
{
    return accounts_menu.open;
}

void accounts_menu_draw(jd_app *app)
{
    if (!accounts_menu.open) return;
    int n = accounts_count(app);
    if (n > 9) n = 9;
    jd_popup_item items[12];
    char labels[9][300], counts[9][48];
    int folder_unread = 0;
    for (int i = 0; i < app->nfolders; i++)
        if (strcasecmp(app->folders[i].name, "INBOX") == 0) folder_unread = app->folders[i].unread;
    for (int i = 0; i < n; i++) {
        const jd_account *a = account_at(app, i);
        const jd_slot *s = app->nslots ? &app->slots[i] : NULL;
        int unread = i == app->cur ? folder_unread : s ? s->unread : 0;
        snprintf(labels[i], sizeof labels[i], "%s%s", a->address ? a->address : "?", s && s->refused ? "  (sign in)" : "");
        if (unread && !app->ui.touch) snprintf(counts[i], sizeof counts[i], "%d  ·  Ctrl+Shift+%d", unread, i + 1);
        else if (unread) snprintf(counts[i], sizeof counts[i], "%d", unread);
        else if (!app->ui.touch) snprintf(counts[i], sizeof counts[i], "Ctrl+Shift+%d", i + 1);
        else counts[i][0] = 0;
        items[i] = (jd_popup_item){labels[i], i == app->cur ? "check" : "mail", counts[i][0] ? counts[i] : NULL, 0};
    }
    items[n] = (jd_popup_item){NULL, NULL, NULL, 0};
    items[n + 1] = (jd_popup_item){"Add an account...", "user", NULL, 0};
    int k = jd_popup_draw(&app->ui, &accounts_menu, items, n + 2);
    if (k >= 0) drawer_done(app);
    if (k >= 0 && k < n) account_switch(app, k);
    else if (k == n + 1) account_add_open(app);
}

const char *folder_label(const jd_folder_row *f)
{
    if (strcasecmp(f->name, "INBOX") == 0) return "Inbox";
    const char *leaf = f->delim ? strrchr(f->path, f->delim) : NULL;
    return leaf ? leaf + 1 : f->path;
}

const char *folder_icon(const jd_folder_row *f)
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
    for (const char *c = f->path; f->delim && *c; c++) d += *c == f->delim;
    return d;
}

/* a child's path is its parent's, the delimiter, then more */
static int is_under(const jd_folder_row *child, const jd_folder_row *parent)
{
    size_t n = strlen(parent->path);
    return parent->delim && strncmp(child->path, parent->path, n) == 0 && child->path[n] == parent->delim;
}

static int has_children(jd_app *app, int i)
{
    for (int j = 0; j < app->nfolders; j++)
        if (j != i && is_under(&app->folders[j], &app->folders[i])) return 1;
    return 0;
}

int folder_depth(const jd_folder_row *f) { return depth_of(f); }
int folder_has_children(jd_app *app, int i) { return has_children(app, i); }

/* Each folder followed by the folders inside it, at every level in the order the store gives (special folders
   first, then by name): a special folder's subfolders (Archives' own) stay under it, not among the others. A
   folder whose parent is not listed goes under its nearest listed ancestor. */
static void tree_emit(jd_app *app, const int *parent, int i, jd_folder_row *out, int *n)
{
    out[(*n)++] = app->folders[i];
    for (int j = 0; j < app->nfolders; j++)
        if (parent[j] == i) tree_emit(app, parent, j, out, n);
}

void folders_tree_order(jd_app *app)
{
    int nf = app->nfolders;
    if (nf < 2) return;
    int *parent = jd_alloc(sizeof *parent * (size_t)nf);
    for (int i = 0; i < nf; i++) {
        parent[i] = -1;
        for (int j = 0; j < nf; j++)
            if (j != i && is_under(&app->folders[i], &app->folders[j]) &&
                (parent[i] < 0 || depth_of(&app->folders[j]) > depth_of(&app->folders[parent[i]])))
                parent[i] = j;
    }
    jd_folder_row *out = jd_alloc(sizeof *out * (size_t)nf);
    int n = 0;
    for (int i = 0; i < nf; i++)
        if (parent[i] < 0) tree_emit(app, parent, i, out, &n);
    memcpy(app->folders, out, sizeof *out * (size_t)nf);
    free(out), free(parent);
}

int folder_hidden(jd_app *app, int i)
{
    for (int j = 0; j < app->nfolders; j++)
        if (j != i && is_under(&app->folders[i], &app->folders[j]) && app->folders[j].collapsed) return 1;
    return 0;
}

void toggle_collapsed(jd_app *app, int i)
{
    jd_folder_row *f = &app->folders[i];
    f->collapsed = !f->collapsed;
    jd_folder_set_collapsed(app->view, f->id, f->collapsed);
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

void folder_select(jd_app *app, int i) { select_folder(app, i); }

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
        char *prefix = f->delim && !jd_streq(f->name, "INBOX") ? jd_printf("%s%c", f->path, f->delim) : jd_strdup("");
        dialog_open(app, DLG_PROMPT, ACT_CREATE_FOLDER, "New folder (full path)", prefix);
        free(prefix);
    } else if (IsKeyPressed(KEY_R) && shift && !jd_streq(f->name, "INBOX")) {
        dialog_open(app, DLG_PROMPT, ACT_RENAME_FOLDER, "Rename folder (full path)", f->path);
    } else if (IsKeyPressed(KEY_D) && shift && !jd_streq(f->name, "INBOX")) {
        char *title = jd_printf("Delete the folder %s and all %d messages in it, on the server?", f->path, f->total);
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
    /* a touch screen's pointer sits where a finger scrolling the sidebar is: no hover there */
    int lit = hover && !plat_touch();
    if (lit) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
    jd_icon(icon, b.x + (b.width - 18) / 2, b.y + (b.height - 18) / 2, 18, lit ? JD_NORD8 : tint);
    if (lit && tip) *show_tip = tip, *tip_at = (Vector2){b.x + b.width + 6, b.y + (b.height - 24) / 2};
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/* the open tabs as rows (tabs set to the sidebar); returns the y below them */
static float tab_rows(jd_app *app, Rectangle r, float y, int rail)
{
    jd_ui *ui = &app->ui;
    static const char *icons[] = {"inbox", "mail", "pencil", "flag", "settings", "calendar"};
    for (int i = 0; i < app->ntabs; i++) {
        Rectangle box = {r.x + 8, y, r.width - 16, 30};
        int hover = CheckCollisionPointRec(GetMousePosition(), box) && !app->dialog.kind && !app->menu.open;
        if (i == app->tab) DrawRectangleRounded(box, 0.25f, 6, ColorAlpha(JD_NORD10, 0.45f));
        else if (hover && !plat_touch()) DrawRectangleRounded(box, 0.25f, 6, JD_NORD3);
        jd_icon(icons[app->tabs[i].kind], box.x + (rail ? (box.width - 16) / 2 : 12), y + 7, 16,
                i == app->tab ? JD_NORD8 : JD_NORD4);
        Rectangle close = {box.x + box.width - 24, y + 6, 18, 18};
        if (!rail) {
            jd_text_fit(ui, tab_title(app, i), box.x + 36, y + 8, box.width - 66, 14, JD_NORD6, i == app->tab);
            /* a touch screen has no hover to show it by: always there */
            if (i && (hover || plat_touch()))
                jd_icon("x", close.x + 2, close.y + 2, 14,
                        hover && !plat_touch() && CheckCollisionPointRec(GetMousePosition(), close) ? JD_NORD11 : JD_MUTED);
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

/* sel: the selected row's offset in the content, -1 when it is not shown */
static void folder_scroll(jd_app *app, Rectangle view, float content, float sel, float row)
{
    if (CheckCollisionPointRec(GetMousePosition(), view) && !app->dialog.kind && !app->menu.open)
        app->side_scroll -= jd_wheel_y(row * 2);
    if (app->folder != app->side_followed && sel >= 0) {
        app->side_followed = app->folder;
        if (sel < app->side_scroll) app->side_scroll = sel;
        else if (sel + row > app->side_scroll + view.height) app->side_scroll = sel + row - view.height;
    }
    float max = content - view.height;
    if (app->side_scroll > max) app->side_scroll = max;
    if (app->side_scroll < 0) app->side_scroll = 0;
}

static int in_view(Rectangle view, Rectangle b) { return b.y + b.height > view.y && b.y < view.y + view.height; }

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
    /* a tap leaves no hover behind: on a touch screen it shows the version for a while */
    static double shown_until;
    int on_mark = CheckCollisionPointRec(GetMousePosition(), mark);
    if (app->ui.touch && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) shown_until = on_mark ? GetTime() + 4 : 0;
    static char version[96];
    if (!version[0]) snprintf(version, sizeof version, "jackdaw %s", jd_version);
    if ((on_mark && !app->ui.touch) || GetTime() < shown_until) tip = version, tip_at = (Vector2){mark.x + 46, mark.y + 6};
    if (icon_button(app, "panel-left-open", (Rectangle){cx, r.y + 49, 40, 36}, "Show the sidebar (Ctrl+B)", JD_NORD4, &tip, &tip_at)) {
        app->prefs.sidebar_rail = 0;
        save_prefs(app);
    }
    Rectangle write = {cx, r.y + 85, 40, 36};
    int hover_write = CheckCollisionPointRec(GetMousePosition(), write) && !app->dialog.kind && !app->menu.open;
    int lit_write = hover_write && !plat_touch();
    DrawRectangleRounded(write, 0.3f, 6, lit_write ? ColorBrightness(JD_NORD10, 0.12f) : JD_NORD10);
    jd_icon("pencil", write.x + 11, write.y + 9, 18, JD_ON_ACCENT);
    if (lit_write) tip ="New message (Ctrl+N)", tip_at = (Vector2){write.x + 46, write.y + 6};
    if (hover_write && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) compose_open(app, COMPOSE_NEW, 0);
    float y = r.y + 134;
    if (app->prefs.tabs_side && app->ntabs > 1) y = tab_rows(app, r, y, 1);
    float bottom = r.y + r.height - 86;
    Rectangle view = {r.x, y, r.width, bottom - 4 - y};
    float content = 0, sel = -1;
    for (int i = 0; i < app->nfolders; i++) {
        if (folder_hidden(app, i) || depth_of(&app->folders[i]) > 0) continue;
        if (i == app->folder) sel = content;
        content += 40;
    }
    folder_scroll(app, view, content, sel, 40);
    jd_scissor(view.x, view.y, view.width, view.height);
    y = view.y - app->side_scroll;
    int over = CheckCollisionPointRec(GetMousePosition(), view);
    for (int i = 0; i < app->nfolders; i++) {
        if (folder_hidden(app, i) || depth_of(&app->folders[i]) > 0) continue;
        const jd_folder_row *f = &app->folders[i];
        Rectangle b = {cx, y, 40, 36};
        if (!in_view(view, b)) {
            y += 40;
            continue;
        }
        int active = i == app->folder && app->tab == 0;
        if (active) DrawRectangleRounded(b, 0.3f, 6, ColorAlpha(JD_NORD10, 0.45f));
        if (icon_button(app, folder_icon(f), b, folder_label(f), active ? JD_NORD8 : JD_NORD4, &tip, &tip_at) && over)
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
    jd_scissor_end();
    jd_scrollbar(view, content, &app->side_scroll);
    DrawRectangle((int)r.x + 10, (int)bottom, (int)r.width - 20, 1, JD_NORD2);
    if (icon_button(app, "calendar", (Rectangle){cx, bottom + 8, 40, 34}, "Calendar (g c)", JD_NORD4, &tip, &tip_at))
        calendar_open(app);
    if (icon_button(app, "settings", (Rectangle){cx, bottom + 44, 40, 34}, "Settings (Ctrl+,)", JD_NORD4, &tip, &tip_at))
        settings_open(app);
    if (tip) tooltip(app, tip, tip_at.x, tip_at.y);
}

/* A folder's unread count rolls when it changes, as an odometer's digits do: the old number rises out of its place
   as the new one comes up from below (down for a count going down). By folder, for the folders on show. */
static void rolling_count(jd_app *app, int64_t folder, int unread, Rectangle box, float y)
{
    static struct {
        int64_t folder;
        int now, was;
        double at;
    } counts[64];
    int slot = -1, free_slot = 0;
    for (int i = 0; i < 64 && slot < 0; i++) {
        if (counts[i].folder == folder) slot = i;
        else if (!counts[i].folder) free_slot = i;
    }
    if (slot < 0) slot = free_slot, counts[slot] = (typeof(counts[0])){folder, unread, unread, 0};
    if (counts[slot].now != unread) counts[slot].was = counts[slot].now, counts[slot].now = unread, counts[slot].at = GetTime();
    float u = fx_enabled() ? (float)(GetTime() - counts[slot].at) / 0.32f : 1;
    char text[16];
    if (u >= 1) {
        if (!unread) return;
        snprintf(text, sizeof text, "%d", unread);
        jd_text(&app->ui, text, box.x + box.width - jd_text_width(&app->ui, text, 14, 1) - 10, y, 14, JD_NORD8, 1);
        return;
    }
    float e = 1 - (1 - u) * (1 - u) * (1 - u), dir = counts[slot].now > counts[slot].was ? 1 : -1;
    jd_scissor(box.x, box.y, box.width, box.height);
    int values[2] = {counts[slot].was, unread};
    for (int k = 0; k < 2; k++) {
        if (!values[k]) continue;
        snprintf(text, sizeof text, "%d", values[k]);
        float shift = (k ? 1 - e : -e) * 14 * dir;
        jd_text(&app->ui, text, box.x + box.width - jd_text_width(&app->ui, text, 14, 1) - 10, y + shift, 14,
                ColorAlpha(JD_NORD8, k ? e : 1 - e), 1);
    }
    jd_scissor_end();
}

/* where the folders with a special use were drawn last, for effects flying to one (send later to Drafts) */
static struct {
    char use[16];
    Rectangle box;
    double at;
} specials[8];

static void special_seen(const char *use, Rectangle box)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(specials[i].use, use)) {
            slot = i;
            break;
        }
        if (specials[i].at < specials[slot].at) slot = i;
    }
    snprintf(specials[slot].use, sizeof specials[slot].use, "%s", use);
    specials[slot].box = box, specials[slot].at = GetTime();
}

int sidebar_special_at(const char *use, Vector2 *at)
{
    for (int i = 0; i < 8; i++)
        if (!strcmp(specials[i].use, use) && GetTime() - specials[i].at < 0.2) {
            *at = (Vector2){specials[i].box.x + 30, specials[i].box.y + specials[i].box.height / 2};
            return 1;
        }
    return 0;
}

void sidebar_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    const float row = 30;
    /* the accounts list lies over the rows: a tap on it is not theirs */
    int free_input = !app->dialog.kind && !app->menu.open && !accounts_menu.open;
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
    /* the open account; a click lists them all, to switch or add one */
    Rectangle who = {r.x + 8, r.y + 54, r.width - 16, 42};
    int hover_who = CheckCollisionPointRec(GetMousePosition(), who) && free_input && !accounts_menu.open;
    if ((hover_who && !plat_touch()) || accounts_menu.open) DrawRectangleRounded(who, 0.25f, 6, JD_NORD2);
    char initial[8] = "?";
    if (app->account.address) snprintf(initial, sizeof initial, "%c", toupper((unsigned char)app->account.address[0]));
    DrawCircleV((Vector2){r.x + 30, r.y + 74}, 14, JD_NORD10);
    jd_text(ui, initial, r.x + 30 - jd_text_width(ui, initial, 14, 1) / 2, r.y + 67, 14, JD_ON_ACCENT, 1);
    jd_text_fit(ui, app->account.name ? app->account.name : "", r.x + 52, r.y + 58, r.width - 84, 15, JD_NORD6, 1);
    jd_text_fit(ui, app->account.address ? app->account.address : "", r.x + 52, r.y + 76, r.width - 84, 12, JD_MUTED, 0);
    /* another account's unread mail, beside the arrow */
    int elsewhere = 0;
    for (int i = 0; i < app->nslots; i++)
        if (i != app->cur) elsewhere += app->slots[i].unread;
    jd_icon(accounts_menu.open ? "chevron-down" : "chevron-right", who.x + who.width - 22, who.y + 13, 16, JD_NORD4);
    if (elsewhere) DrawCircleV((Vector2){who.x + who.width - 28, who.y + 12}, 4, JD_NORD8);
    /* on release: the press that opens it would otherwise count as a click outside it, and shut it. The press
       must have been on it too: the tap that opens the phone's drawer lifts over the account as it slides out. */
    static int who_pressed;
    if (hover_who && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) who_pressed = 1;
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (who_pressed && hover_who) {
            jd_popup_open(&accounts_menu);
            accounts_menu.at = (Vector2){who.x, who.y + who.height + 4};
        }
        who_pressed = 0;
    }

    Rectangle write = {r.x + 12, r.y + 102, r.width - 24, 38};
    if (jd_icon_button(ui, "pencil", "New message", write, 1) && free_input) drawer_done(app), compose_open(app, COMPOSE_NEW, 0);

    float y = r.y + 154, bottom = r.y + r.height - 124;
    if (app->prefs.tabs_side && app->ntabs > 1) y = tab_rows(app, r, y, 0);
    Rectangle view = {r.x, y, r.width, bottom - 4 - y};
    float content = 0, sel = -1;
    for (int i = 0; i < app->nfolders; i++) {
        if (folder_hidden(app, i)) continue;
        if (i == app->folder) sel = content;
        content += row;
    }
    folder_scroll(app, view, content, sel, row);
    /* room for the scrollbar beside the counts */
    float bar = content > view.height ? 10 : 0;
    jd_scissor(view.x, view.y, view.width, view.height);
    y = view.y - app->side_scroll;
    int over = CheckCollisionPointRec(GetMousePosition(), view);
    for (int i = 0; i < app->nfolders; i++) {
        if (folder_hidden(app, i)) continue;
        const jd_folder_row *f = &app->folders[i];
        Rectangle box = {r.x + 8, y, r.width - 16 - bar, row - 2};
        if (!in_view(view, box)) {
            y += row;
            continue;
        }
        int hover = CheckCollisionPointRec(GetMousePosition(), box) && over && free_input;
        float indent = 22 + 16 * (float)depth_of(f);
        Rectangle arrow = {box.x + indent - 20, y, 18, row - 2};
        int expander = has_children(app, i), on_arrow = expander && hover && CheckCollisionPointRec(GetMousePosition(), arrow);
        if (i == app->folder && app->tab == 0) {
            DrawRectangleRounded(box, 0.25f, 6, ColorAlpha(JD_NORD10, 0.45f));
            if (app->pane == PANE_FOLDERS && !plat_touch()) DrawRectangleRoundedLinesEx(box, 0.25f, 6, 1.5f, JD_NORD8);
        }
        else if (hover && !plat_touch()) DrawRectangleRounded(box, 0.25f, 6, JD_NORD3);
        /* the arrow only opens or closes the folder: choosing it would also shut the phone's drawer */
        if (on_arrow && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) toggle_collapsed(app, i);
        else if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app->pane = PANE_FOLDERS;
            select_folder(app, i);
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            select_folder(app, i);
            app->menu = (jd_menu){.open = 1, .at = GetMousePosition(), .on_folder = 1, .index = i};
        }
        if (expander) jd_icon(f->collapsed ? "chevron-right" : "chevron-down", arrow.x + 2, y + 7, 14, JD_NORD4);
        jd_icon(folder_icon(f), box.x + indent, y + 6, 16, i == app->folder ? JD_NORD8 : JD_NORD4);
        indent += 24;
        char count[16] = "";
        if (f->unread) snprintf(count, sizeof count, "%d", f->unread);
        float cw = jd_text_width(ui, count, 14, 1);
        jd_text_fit(ui, folder_label(f), box.x + indent, y + 7, box.width - indent - cw - 20, 15, JD_NORD6, f->unread > 0);
        rolling_count(app, f->id, f->unread, box, y + 7);
        if (f->special_use) special_seen(f->special_use, box);
        y += row;
    }
    jd_scissor_end();
    jd_scrollbar(view, content, &app->side_scroll);

    if (tip) tooltip(app, tip, tip_at.x, tip_at.y);
    DrawRectangle((int)r.x + 12, (int)bottom, (int)r.width - 24, 1, JD_NORD2);
    struct {
        const char *icon, *label;
    } foot[] = {{"settings", "Settings"}, {"circle-help", "Keys (?)"}};
    /* the calendar as a button of its own, the width of the sidebar; Settings and Keys under it */
    float fx = r.x + 12, fy = bottom + 8;
    {
        Rectangle b = {r.x + 12, fy, r.width - 24, 36};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && free_input;
        DrawRectangleRounded(b, 0.3f, 6, ColorAlpha(JD_NORD10, 0.16f));
        jd_tap_face(b, hover && !plat_touch(), 0, 0.3f);
        jd_tap_icon("calendar", (Rectangle){b.x + 4, b.y + 2, 32, 32}, 16, 1, JD_NORD8, JD_NORD8);
        jd_text(ui, "Calendar", b.x + 38, b.y + 10, 14, JD_NORD6, 1);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) drawer_done(app), calendar_open(app);
        fy += 42;
    }
    /* a phone has no keyboard to show the keys of */
    for (int k = 0; k < (plat_touch() ? 1 : 2); k++) {
        float fw = jd_text_width(ui, foot[k].label, 13, 0) + 40;
        Rectangle b = {fx, fy, fw, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && free_input;
        float lit = jd_tap_face(b, hover && !plat_touch(), 0, 0.3f);
        jd_tap_icon(foot[k].icon, (Rectangle){fx + 2, fy, 32, 32}, 16, lit, JD_NORD4, JD_NORD8);
        jd_text(ui, foot[k].label, fx + 32, fy + 9, 13, JD_NORD4, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            drawer_done(app);
            if (k == 0) settings_open(app);
            else app->help = 1;
        }
        fx += fw + 4;
    }
    char version[96];
    snprintf(version, sizeof version, "jackdaw %s", jd_version);
    jd_text_fit(ui, version, r.x + 22, fy + 42, r.width - 34, 12, JD_NORD4, 0);
}
