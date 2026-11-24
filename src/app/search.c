/* The search bar over every page, and what it finds while text is typed in it: mail, folders, Settings, calendar
   events, people and actions, a few of each. Enter with nothing picked searches the mail into the list, as the
   list's own search box did: operators become pills, and their hints come before the results. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app.h"
#include "mail/query.h"
#include "platform.h"
#include "ui/fx.h"
#include "ui/icons.h"

enum { G_MAIL, G_FOLDERS, G_SETTINGS, G_EVENTS, G_PEOPLE, G_ACTIONS, G_COUNT };
static const char *const group_names[G_COUNT] = {"Mail", "Folders", "Settings", "Calendar", "People", "Actions"};
enum { PER_GROUP = 4, MAX_FOUND = G_COUNT * PER_GROUP, ROW_H = 32, HEAD_H = 26 };

typedef struct {
    int group, index;
    int64_t id;
    const char *icon;
    char title[200], note[200];
} found;

static found hits[MAX_FOUND];
static int nhits, sel = -1; /* sel -1: Enter searches the mail */
static char hits_for[300] = "\x01";
static Rectangle bar;

static void new_message(jd_app *app) { compose_open(app, COMPOSE_NEW, 0); }
static void keys_help(jd_app *app) { app->help = 1; }
static void sidebar_toggle(jd_app *app)
{
    app->prefs.sidebar_rail = !app->prefs.sidebar_rail;
    save_prefs(app);
}
static void open_rules(jd_app *app)
{
    int i;
    if (settings_find("Rules", &i, 1)) settings_goto(app, i);
}
static void zoom_in(jd_app *app) { reader_zoom(app, 1); }
static void zoom_out(jd_app *app) { reader_zoom(app, -1); }
static void zoom_reset(jd_app *app) { reader_zoom(app, 0); }

/* commands by name, with their keys; the ones on the selection act on the list's, as their keys do */
static const struct {
    const char *label, *keys, *icon;
    void (*run)(jd_app *);
} actions[] = {
    {"New message", "Ctrl+N", "pencil", new_message},
    {"Check for new mail", "Ctrl+R", "refresh-cw", list_refresh},
    {"Archive", "e", "archive", act_archive},
    {"Delete", "dd", "trash-2", act_delete},
    {"Mark read or unread", "u", "mail-open", act_toggle_read},
    {"Flag", "s", "star", act_toggle_flag},
    {"Snooze", "b", "bell", act_snooze},
    {"Not junk", NULL, "inbox", act_not_junk},
    {"Calendar", "g c", "calendar", calendar_open},
    {"Settings", "Ctrl+,", "settings", settings_open},
    {"Rules", "Ctrl+Shift+L", "list", open_rules},
    {"Server filters", "Ctrl+Shift+F", "flag", filters_open},
    {"Reading pane: right, bottom or off", "p", "panel-right", cycle_reader},
    {"Sidebar: full or icons only", "Ctrl+B", "panel-left-close", sidebar_toggle},
    {"Zoom in (message text)", "Ctrl+=", "zoom-in", zoom_in},
    {"Zoom out (message text)", "Ctrl+-", "zoom-out", zoom_out},
    {"Zoom 100% (message text)", "Ctrl+0", "search", zoom_reset},
    {"Keyboard shortcuts", "?", "keyboard", keys_help},
};

static found *add(int group, const char *icon)
{
    if (nhits >= MAX_FOUND) return NULL;
    found *f = &hits[nhits++];
    *f = (found){.group = group, .index = -1, .icon = icon};
    return f;
}

/* Found again once typing pauses (150 ms): a short word matches much of a mailbox, and the mail query took up to
   0.9 s on one by conversation (each row's count asked apart), 44 ms by message, stalling every key. So the mail is
   asked for by message, from two letters on; the found list a pause behind is kept meanwhile, nothing picked. */
static void refresh(jd_app *app)
{
    static char typing[300];
    static double typed_at;
    const char *typed = jd_field_text(&app->search);
    while (*typed == ' ') typed++;
    if (strcmp(typed, typing)) {
        snprintf(typing, sizeof typing, "%s", typed);
        typed_at = GetTime();
        sel = -1;
    }
    if (!strcmp(typed, hits_for) || (typed[0] && GetTime() - typed_at < 0.15)) return;
    snprintf(hits_for, sizeof hits_for, "%s", typed);
    nhits = 0, sel = -1;
    if (!typed[0]) return;

    if (app->view && strlen(typed) >= 2) {
        jd_search s;
        jd_search_parse(typed, &s);
        if (s.folder_name) s.q.folder = jd_store_folder_by_name(app->view, s.folder_name);
        s.q.grouped = 0;
        int n;
        jd_thread_row *rows = jd_store_list(app->view, &s.q, &n);
        for (int i = 0; i < n && i < PER_GROUP; i++) {
            found *f = add(G_MAIL, "mail");
            if (!f) break;
            f->id = rows[i].latest;
            snprintf(f->title, sizeof f->title, "%s", rows[i].subject && rows[i].subject[0] ? rows[i].subject : "(no subject)");
            snprintf(f->note, sizeof f->note, "%s", rows[i].senders ? rows[i].senders : "");
        }
        jd_thread_rows_free(rows, n);
        jd_search_free(&s);
    }
    for (int i = 0, k = 0; i < app->nfolders && k < PER_GROUP; i++) {
        const jd_folder_row *fr = &app->folders[i];
        if (!strcasestr(fr->path, typed) && !strcasestr(folder_label(fr), typed)) continue;
        found *f = add(G_FOLDERS, folder_icon(fr));
        if (!f) break;
        f->index = i, k++;
        snprintf(f->title, sizeof f->title, "%s", folder_label(fr));
        /* the folders it is in, as the folder picker shows them */
        const char *leaf = folder_label(fr);
        if (fr->delim && strcasecmp(fr->name, "INBOX") && leaf != fr->path) {
            int m = 0;
            for (const char *c = fr->path; c < leaf - 1 && m < (int)sizeof f->note - 4; c++) {
                if (*c == fr->delim) f->note[m++] = ' ', f->note[m++] = '/', f->note[m++] = ' ';
                else f->note[m++] = *c;
            }
            f->note[m] = 0;
        }
    }
    int ids[PER_GROUP];
    int ns = settings_find(typed, ids, PER_GROUP);
    for (int i = 0; i < ns; i++) {
        found *f = add(G_SETTINGS, "settings");
        if (!f) break;
        const char *section;
        snprintf(f->title, sizeof f->title, "%s", settings_entry(ids[i], &section));
        snprintf(f->note, sizeof f->note, "%s", section);
        f->index = ids[i];
    }
    cal_found ev[PER_GROUP];
    int ne = calendar_find(typed, ev, PER_GROUP);
    for (int i = 0; i < ne; i++) {
        found *f = add(G_EVENTS, "calendar");
        if (!f) break;
        f->id = ev[i].id;
        snprintf(f->title, sizeof f->title, "%s", ev[i].title);
        char when[64];
        time_t t = (time_t)ev[i].start;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, ev[i].all_day ? "%a %d %b %Y" : "%a %d %b %Y, %H:%M", &tm);
        snprintf(f->note, sizeof f->note, "%s%s%s", when, ev[i].place[0] ? "  ·  " : "", ev[i].place);
    }
    if (app->view) {
        int np;
        jd_contact *c = jd_store_contacts(app->view, typed, PER_GROUP, &np);
        for (int i = 0; i < np; i++) {
            found *f = add(G_PEOPLE, "user");
            if (!f) break;
            snprintf(f->title, sizeof f->title, "%s", c[i].name && c[i].name[0] ? c[i].name : c[i].addr);
            snprintf(f->note, sizeof f->note, "%s", c[i].addr);
        }
        jd_contacts_free(c, np);
    }
    for (int i = 0, k = 0; i < (int)(sizeof actions / sizeof *actions) && k < PER_GROUP; i++) {
        if (!strcasestr(actions[i].label, typed)) continue;
        found *f = add(G_ACTIONS, actions[i].icon);
        if (!f) break;
        f->index = i, k++;
        snprintf(f->title, sizeof f->title, "%s", actions[i].label);
        snprintf(f->note, sizeof f->note, "%s", actions[i].keys && !plat_touch() ? actions[i].keys : "");
    }
}

/* what a found thing does; the typed text goes (the pills of a mail search stay) */
static void run(jd_app *app, int i)
{
    found f = hits[i];
    char typed[300];
    snprintf(typed, sizeof typed, "%s", hits_for);
    app->searching = 0;
    jd_field_set(&app->search, NULL);
    snprintf(hits_for, sizeof hits_for, "\x01");
    nhits = 0, sel = -1;
    switch (f.group) {
    case G_MAIL: mail_open_message(app, f.id); break;
    case G_FOLDERS: folder_select(app, f.index); break;
    case G_SETTINGS: settings_goto(app, f.index); break;
    case G_EVENTS: calendar_show_event(app, typed, f.id); break;
    case G_PEOPLE:
        compose_open(app, COMPOSE_NEW, 0);
        if (app->tabs[app->tab].kind == TAB_COMPOSE) compose_set_to(app->tabs[app->tab].compose, f.note);
        break;
    case G_ACTIONS: actions[f.index].run(app); break;
    }
}

void search_bar(jd_app *app, Rectangle r)
{
    bar = r;
    DrawRectangleRec(r, JD_NORD0);
    search_box_draw(app, r.x + 16, r.y + 9, r.width - 32);
    DrawRectangle((int)r.x, (int)(r.y + r.height - 1), (int)r.width, 1, JD_NORD1);
}

void search_bar_keys(jd_app *app)
{
    if (!search_hints_open(app)) {
        refresh(app);
        if (nhits && IsKeyPressed(KEY_DOWN)) {
            if (sel + 1 < nhits) sel++;
            return;
        }
        if (nhits && IsKeyPressed(KEY_UP)) {
            if (sel >= 0) sel--;
            return;
        }
        if (sel >= 0 && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
            run(app, sel);
            return;
        }
        if (sel >= 0 && IsKeyPressed(KEY_ESCAPE)) {
            sel = -1;
            return;
        }
    }
    search_box_keys(app);
}

/* Under the bar, over the page: the operator hints while one is being typed, else what the text finds, by group;
   the first row says Enter searches the mail. */
void search_bar_overlay(jd_app *app)
{
    search_hints_forget();
    if (!app->searching) return;
    float x = bar.x + 16, y = bar.y + 47, w = fminf(bar.width - 32, 680);
    if (search_hints_open(app)) {
        search_hints_draw(app, x, y, w);
        return;
    }
    refresh(app);
    const char *typed = jd_field_text(&app->search);
    while (*typed == ' ') typed++;
    if (!typed[0]) return;
    if (hits_for[0] == '\x01') nhits = 0;
    jd_ui *ui = &app->ui;
    float h = 8 + ROW_H;
    for (int i = 0; i < nhits; i++) h += ROW_H + (i == 0 || hits[i].group != hits[i - 1].group ? HEAD_H : 0);
    float room = jd_screen_h() - y - 16;
    if (h > room) h = room;
    Rectangle panel = {x, y, w, h};
    jd_cover(panel);
    fx_shadow(panel, 0.04f, 0.6f, 1);
    DrawRectangleRounded(panel, 0.04f, 8, JD_NORD1);
    DrawRectangleRoundedLinesEx(panel, 0.04f, 8, 1, JD_NORD2);
    jd_scissor(panel.x, panel.y, panel.width, panel.height);
    Vector2 m = GetMousePosition();
    float ry = y + 4;
    for (int i = -1; i < nhits; i++) {
        if (i >= 0 && (i == 0 || hits[i].group != hits[i - 1].group)) {
            jd_text(ui, group_names[hits[i].group], x + 14, ry + 8, 12, JD_MUTED, 1);
            ry += HEAD_H;
        }
        Rectangle row = {x + 4, ry, w - 8, ROW_H - 2};
        int hover = CheckCollisionPointRec(m, row) && CheckCollisionPointRec(m, panel);
        if (i == sel || hover) DrawRectangleRounded(row, 0.25f, 6, JD_NORD2);
        if (i < 0) {
            char line[340];
            snprintf(line, sizeof line, "Search mail for \"%s\"", typed);
            jd_icon("search", row.x + 10, row.y + 8, 15, JD_NORD8);
            jd_text_fit(ui, line, row.x + 34, row.y + 8, row.width - 120, 14, JD_NORD6, 1);
            if (!plat_touch()) jd_text(ui, "Enter", row.x + row.width - jd_text_width(ui, "Enter", 12, 0) - 10, row.y + 9, 12, JD_MUTED, 0);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                jd_scissor_end();
                search_run_mail(app);
                return;
            }
        } else {
            found *f = &hits[i];
            jd_icon(f->icon, row.x + 10, row.y + 8, 15, i == sel || hover ? JD_NORD8 : JD_NORD4);
            float tw = fminf(jd_text_width(ui, f->title, 14, 0), row.width * 0.55f);
            jd_text_fit(ui, f->title, row.x + 34, row.y + 8, tw, 14, JD_NORD6, 0);
            if (f->note[0]) jd_text_fit(ui, f->note, row.x + 34 + tw + 14, row.y + 9, row.width - tw - 58, 12, JD_MUTED, 0);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                jd_scissor_end();
                run(app, i);
                return;
            }
        }
        ry += ROW_H;
        if (ry > panel.y + panel.height) break;
    }
    jd_scissor_end();
}
