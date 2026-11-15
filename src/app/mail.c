#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "ui/icons.h"

enum { TAB_H = 34, BAR_H = 28, SPLIT = 5 };

void notify_user(jd_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->notice, sizeof app->notice, fmt, ap);
    va_end(ap);
    app->notice_until = GetTime() + 6;
}

void save_prefs(jd_app *app)
{
    app->prefs.conversations = app->grouped;
    app->prefs.sidebar_width = (int)app->side_w;
    app->prefs.list_width = (int)app->list_w;
    app->prefs.list_height = (int)app->list_h;
    jd_settings_save(app->have_account ? &app->account : NULL, &app->prefs);
}

static void unload_fonts(jd_ui *ui)
{
    jd_fonts_unload(&ui->fonts);
    jd_fonts_unload(&ui->list);
    jd_fonts_unload(&ui->reader);
    UnloadFont(ui->mono.regular);
}

/* the font families of prefs, loaded when they differ from what is loaded */
static void load_fonts(jd_app *app)
{
    jd_prefs *p = &app->prefs;
    char *want = jd_printf("%s|%s|%s|%s", p->font_ui ? p->font_ui : "", p->font_list ? p->font_list : "",
                           p->font_reader ? p->font_reader : "", p->font_mono ? p->font_mono : "");
    if (app->fonts_loaded && strcmp(app->fonts_loaded, want) == 0) {
        free(want);
        return;
    }
    if (app->fonts_loaded) unload_fonts(&app->ui);
    free(app->fonts_loaded);
    app->fonts_loaded = want;
    jd_fonts_load_family(&app->ui.fonts, p->font_ui);
    jd_fonts_load_family(&app->ui.list, p->font_list);
    jd_fonts_load_family(&app->ui.reader, p->font_reader);
    app->ui.mono.regular = app->ui.mono.semibold = jd_font_mono(p->font_mono);
    /* open messages hold the old fonts in their HTML layout */
    for (int t = 0; t < app->ntabs; t++)
        if (app->tabs[t].conv.thread) conv_open(app, &app->tabs[t].conv, app->tabs[t].conv.thread, app->tabs[t].conv.message);
}

void fonts_release(jd_app *app)
{
    if (app->fonts_loaded) unload_fonts(&app->ui);
    free(app->fonts_loaded);
    app->fonts_loaded = NULL;
}

void apply_prefs(jd_app *app)
{
    jd_theme_set(app->prefs.light);
    jd_scale = (float)(app->prefs.scale ? app->prefs.scale : 100) / 100.0f;
    app->grouped = app->prefs.conversations;
    app->side_w = (float)app->prefs.sidebar_width;
    app->list_w = (float)app->prefs.list_width;
    app->list_h = (float)app->prefs.list_height;
    load_fonts(app);
    /* HTML is laid out with the message text size as its default: open messages are read again for a new one */
    static int laid_out_size;
    if (laid_out_size && laid_out_size != app->prefs.reader_text)
        for (int t = 0; t < app->ntabs; t++)
            if (app->tabs[t].conv.thread) conv_open(app, &app->tabs[t].conv, app->tabs[t].conv.thread, app->tabs[t].conv.message);
    laid_out_size = app->prefs.reader_text;
    for (int t = 0; t < app->ntabs; t++) {
        jd_conv *c = &app->tabs[t].conv;
        c->plain = app->prefs.prefer_plain;
        for (int i = 0; i < c->n; i++) c->reading[i].wrapped_for = c->reading[i].html_w = 0;
    }
}

void mail_reload(jd_app *app)
{
    if (!app->view) return;
    int64_t keep = app->nfolders ? app->folders[app->folder].id : 0;
    jd_folder_rows_free(app->folders, app->nfolders);
    app->folders = jd_store_folders(app->view, &app->nfolders);
    app->folder = 0;
    for (int i = 0; i < app->nfolders; i++)
        if (app->folders[i].id == keep) app->folder = i;
    list_reload(app);
    for (int t = 0; t < app->ntabs; t++) {
        jd_conv *c = &app->tabs[t].conv;
        if (!c->thread) continue;
        conv_refresh(app, c);
        if (!c->n && t == 0) reader_close(app);
    }
}

void mail_body_ready(jd_app *app, int64_t id)
{
    for (int t = 0; t < app->ntabs; t++) conv_body_ready(app, &app->tabs[t].conv, id);
}

void mail_select_inbox(jd_app *app)
{
    app->tab = 0;
    for (int i = 0; i < app->nfolders; i++)
        if (strcasecmp(app->folders[i].name, "INBOX") == 0 && app->folder != i) {
            app->folder = i;
            reader_close(app);
            list_reload(app);
        }
}

void cycle_reader(jd_app *app)
{
    app->prefs.reader_pos = (app->prefs.reader_pos + 1) % 3;
    if (app->prefs.reader_pos == 2) reader_close(app);
    save_prefs(app);
}

static void mail_keys(jd_app *app)
{
    if (app->pane == PANE_LIST && app->searching) {
        list_keys(app);
        return;
    }
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_TAB) && !ctrl) {
        int n = preview(app)->n ? 3 : 2;
        app->pane = (app->pane + (shift ? n - 1 : 1)) % n;
        return;
    }
    if (IsKeyPressed(KEY_P) && !ctrl && !shift) {
        cycle_reader(app);
        return;
    }
    /* the reader's caret uses g itself (gg, G) */
    if (!(app->pane == PANE_READER && (reader_caret() || reader_visual())) && go_keys(app)) return;
    switch (app->pane) {
    case PANE_FOLDERS:
        if (sidebar_keys(app)) return;
        if (IsKeyPressed(KEY_L) || IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_ENTER)) app->pane = PANE_LIST;
        else list_keys(app);
        break;
    case PANE_LIST:
        if (list_keys(app)) return;
        if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_LEFT)) app->pane = PANE_FOLDERS;
        break;
    case PANE_READER:
        if (conv_keys(app, preview(app))) return;
        if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_LEFT)) app->pane = PANE_LIST;
        else list_keys(app);
        break;
    }
}

/* a splitter the mouse drags: pos is measured from origin along the drag axis */
static float splitter(jd_app *app, int id, Rectangle r, int vertical, float origin, float pos, float min, float max)
{
    Vector2 m = GetMousePosition();
    Rectangle grab = vertical ? (Rectangle){r.x - 3, r.y, r.width + 6, r.height} : (Rectangle){r.x, r.y - 3, r.width, r.height + 6};
    int hover = CheckCollisionPointRec(m, grab) && !app->dialog.kind && !app->menu.open;
    if (hover || app->dragging == id) SetMouseCursor(vertical ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS);
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app->dragging = id;
    if (app->dragging == id) {
        pos = (vertical ? m.x : m.y) - origin;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            app->dragging = 0;
            save_prefs(app);
        }
    }
    DrawRectangleRec(r, app->dragging == id ? JD_NORD3 : hover ? JD_NORD2 : JD_NORD1);
    return pos < min ? min : pos > max ? max : pos;
}

/* reply, forward and the message actions above the preview */
static void toolbar(jd_app *app, Rectangle r)
{
    jd_conv *c = preview(app);
    DrawRectangleRec((Rectangle){r.x, r.y, r.width, 44}, JD_NORD0);
    DrawRectangle((int)r.x, (int)r.y + 43, (int)r.width, 1, JD_NORD1);
    int64_t last = c->reading[c->n - 1].row.id;
    struct {
        const char *icon, *tip;
        int gap;
    } buttons[] = {{"reply", "Reply (r)", 0}, {"reply-all", "Reply all (R)", 0}, {"forward", "Forward (F)", 0},
                   {"archive", "Archive (e)", 1}, {"trash-2", "Delete (dd)", 0}, {"folder-input", "Move (m)", 0},
                   {"mail", "Mark unread (u)", 0}, {"star", "Flag (s)", 0},
                   {c->plain ? "file-image" : "file-text", c->plain ? "Show HTML (Shift+P)" : "Show plain text (Shift+P)", 1},
                   {"external-link", "Open in a tab (Shift+Enter)", 0}, {"download", "Save as .eml (Ctrl+S)", 0}, {"printer", "Print (Ctrl+P)", 0}};
    float x = r.x + 16;
    const char *tip = NULL;
    float tip_x = 0;
    for (int i = 0; i < (int)(sizeof buttons / sizeof *buttons); i++) {
        /* buttons that would not fit the pane are left out; their keys still work */
        if (x + (buttons[i].gap ? 12 : 0) + 32 > r.x + r.width - 8) break;
        if (buttons[i].gap) {
            DrawRectangle((int)x + 4, (int)r.y + 12, 1, 20, JD_NORD2);
            x += 12;
        }
        Rectangle b = {x, r.y + 6, 32, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open;
        if (hover) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
        jd_icon(buttons[i].icon, x + 7, r.y + 13, 18, hover ? JD_NORD8 : JD_NORD4);
        if (hover) tip = buttons[i].tip, tip_x = x;
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            switch (i) {
            case 0: compose_open(app, COMPOSE_REPLY, last); break;
            case 1: compose_open(app, COMPOSE_REPLY_ALL, last); break;
            case 2: compose_open(app, COMPOSE_FORWARD, last); break;
            case 3: act_archive(app); break;
            case 4: act_delete(app); break;
            case 5: dialog_open(app, DLG_PICK_FOLDER, ACT_MOVE, "Move to", NULL); break;
            case 6: act_toggle_read(app); break;
            case 7: act_toggle_flag(app); break;
            case 8: c->plain = !c->plain; break;
            case 9: reader_open(app, 1); break;
            case 10: conv_save_message(app, c); break;
            case 11: app->print_tab = 1; break;
            }
            return;
        }
        x += 36;
    }
    if (tip) jd_tooltip(tip, tip_x, r.y + 42);
}

/* the reader first, then its toolbar above it, so the toolbar's tooltip lands on top */
static void preview_draw(jd_app *app, Rectangle r)
{
    float bar = preview(app)->n ? 44 : 0;
    conv_draw(app, preview(app), (Rectangle){r.x, r.y + bar, r.width, r.height - bar}, app->pane == PANE_READER);
    if (bar) toolbar(app, r);
}

/* the pane with the keys is framed in the accent colour */
static void focus_line(jd_app *app, int pane, Rectangle r)
{
    if (app->tab == 0 && app->pane == pane) DrawRectangleLinesEx(r, 2, JD_NORD8);
}

/* the list and the reading pane, right of the sidebar */
static void mail_draw(jd_app *app, Rectangle r)
{
    float w = r.width, h = r.height;
    if (!app->list_w) app->list_w = w > 1300 ? 500 : w * 0.44f;
    if (!app->list_h) app->list_h = h * 0.45f;
    switch (app->prefs.reader_pos) {
    case 1:
        app->list_h = splitter(app, 2, (Rectangle){r.x, r.y + app->list_h, w, SPLIT}, 0, r.y, app->list_h, 160, h - 140);
        list_draw(app, (Rectangle){r.x, r.y, w, app->list_h});
        preview_draw(app, (Rectangle){r.x, r.y + app->list_h + SPLIT, w, h - app->list_h - SPLIT});
        focus_line(app, PANE_LIST, (Rectangle){r.x, r.y, w, app->list_h});
        focus_line(app, PANE_READER, (Rectangle){r.x, r.y + app->list_h + SPLIT, w, h - app->list_h - SPLIT});
        break;
    case 2:
        list_draw(app, r);
        focus_line(app, PANE_LIST, r);
        break;
    default:
        app->list_w = splitter(app, 2, (Rectangle){r.x + app->list_w, r.y, SPLIT, h}, 1, r.x, app->list_w, 300, w - 300);
        list_draw(app, (Rectangle){r.x, r.y, app->list_w, h});
        preview_draw(app, (Rectangle){r.x + app->list_w + SPLIT, r.y, w - app->list_w - SPLIT, h});
        focus_line(app, PANE_LIST, (Rectangle){r.x, r.y, app->list_w, h});
        focus_line(app, PANE_READER, (Rectangle){r.x + app->list_w + SPLIT, r.y, w - app->list_w - SPLIT, h});
        break;
    }
}

/* "Archived 2 conversations  Undo" while a move waits */
static void toast(jd_app *app, float sh)
{
    jd_pending *p = &app->pending;
    if (!p->ids || p->due < 0) return;
    jd_ui *ui = &app->ui;
    float tw = jd_text_width(ui, p->label, 14, 0);
    Rectangle box = {(jd_screen_w() - tw - 130) / 2, sh - BAR_H - 62, tw + 130, 44};
    DrawRectangleRounded((Rectangle){box.x + 2, box.y + 3, box.width, box.height}, 0.3f, 6, ColorAlpha(BLACK, 0.3f));
    DrawRectangleRounded(box, 0.3f, 6, JD_NORD3);
    jd_text(ui, p->label, box.x + 16, box.y + 14, 14, JD_NORD6, 0);
    Rectangle undo = {box.x + tw + 30, box.y + 7, 86, 30};
    int hover = CheckCollisionPointRec(GetMousePosition(), undo);
    DrawRectangleRounded(undo, 0.3f, 6, hover ? JD_NORD2 : JD_NORD3);
    jd_icon("undo-2", undo.x + 8, undo.y + 7, 16, JD_NORD8);
    jd_text(ui, "Undo", undo.x + 30, undo.y + 7, 14, JD_NORD8, 1);
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pending_undo(app);
    float left = (float)(p->due - GetTime()) / 5.0f;
    DrawRectangleV((Vector2){box.x + 10, box.y + box.height - 3}, (Vector2){(box.width - 20) * (left > 0 ? left : 0), 2}, JD_NORD8);
}

/* NORMAL, VISUAL or INSERT, as vim's status line shows it */
jd_mode current_mode(jd_app *app)
{
    jd_tab *t = &app->tabs[app->tab];
    if (app->searching || (app->dialog.kind == DLG_PROMPT || app->dialog.kind == DLG_PICK_FOLDER) ||
        (t->kind == TAB_COMPOSE && compose_inserting(t->compose)) || (t->kind == TAB_SETTINGS && app->settings_focus >= 0) ||
        (t->kind == TAB_FILTERS && app->filters.state == 1) || app->show_form)
        return MODE_INSERT;
    return app->visual || reader_visual() || (t->kind == TAB_COMPOSE && compose_visual(t->compose)) ? MODE_VISUAL
                                                                                                   : MODE_NORMAL;
}

/* mode, what the worker does (or the last notice), and the counts of the folder on show */
static void footer(jd_app *app, float sw, float sh)
{
    jd_ui *ui = &app->ui;
    float y = sh - BAR_H;
    DrawRectangleV((Vector2){0, y}, (Vector2){sw, BAR_H}, JD_NORD1);
    static const char *names[] = {"NORMAL", "VISUAL", "INSERT"};
    Color colors[] = {JD_NORD10, (Color){0xb4, 0x8e, 0xad, 0xff}, (Color){0x8f, 0xa8, 0x76, 0xff}};
    jd_mode m = current_mode(app);
    float mw = jd_text_width(ui, names[m], 12, 1) + 20;
    DrawRectangleV((Vector2){0, y}, (Vector2){mw, BAR_H}, colors[m]);
    jd_text(ui, names[m], 10, y + 8, 12, (Color){0xff, 0xff, 0xff, 0xff}, 1);
    float x = mw + 12;
    /* which pane has the keys, beside the mode */
    if (app->tab == 0) {
        static const char *panes[] = {"Folders", "List", "Reader"};
        const char *pane = panes[app->pane % 3];
        jd_text(ui, pane, x, y + 7, 13, JD_NORD8, 1);
        x += jd_text_width(ui, pane, 13, 1) + 16;
    }
    const char *chord = app->pending_g ? "g  (g top, i Inbox, s Sent, d Drafts, a Archive, t Trash, j Junk)"
                        : app->pending_d ? "d  (d again deletes)"
                        : reader_caret() ? "Caret: h j k l w b move, v starts selecting, Esc leaves"
                        : reader_visual() ? "y copies, v or Esc stops selecting"
                        : app->pending_f ? "f  (u unread, f flagged, a files, s sender, c clear)"
                                         : NULL;
    if (chord) {
        jd_text(ui, chord, x, y + 7, 13, JD_NORD8, 1);
        x += jd_text_width(ui, chord, 13, 1) + 16;
    }
    int noting = app->notice[0] && GetTime() < app->notice_until;
    const char *line = app->error ? app->error : app->status ? app->status : noting ? app->notice : "Up to date";
    jd_icon(app->error ? "triangle-alert" : app->status ? "refresh-cw" : noting ? "check" : "circle-check", x, y + 6, 15,
            app->error ? JD_NORD13 : noting ? JD_NORD8 : JD_NORD4);
    char counts[200] = "";
    if (app->tab == 0 && app->nfolders) {
        const jd_folder_row *f = &app->folders[app->folder];
        int n = snprintf(counts, sizeof counts, "%s", app->query ? "Search" : folder_label(f));
        if (app->npicked) n += snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d selected", app->npicked);
        n += snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d %s", app->nrows,
                      app->grouped ? (app->nrows == 1 ? "conversation" : "conversations") : (app->nrows == 1 ? "message" : "messages"));
        if (!app->query && f->unread) snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d unread", f->unread);
    }
    float cw = jd_text_width(ui, counts, 13, 0);
    jd_text_fit(ui, line, x + 22, y + 7, sw - x - 40 - cw - 24, 13, app->error ? JD_NORD13 : JD_NORD4, 0);
    jd_text(ui, counts, sw - cw - 14, y + 7, 13, JD_MUTED, 0);
}

static void tab_keys(jd_app *app)
{
    jd_tab *t = &app->tabs[app->tab];
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (t->kind == TAB_MAIL) mail_keys(app);
    else if (t->kind == TAB_CONV) {
        int64_t last = t->conv.n ? t->conv.reading[t->conv.n - 1].row.id : 0;
        if (conv_keys(app, &t->conv)) {
        } else if (IsKeyPressed(KEY_ESCAPE)) tab_close(app, app->tab);
        else if (IsKeyPressed(KEY_R)) compose_open(app, shift ? COMPOSE_REPLY_ALL : COMPOSE_REPLY, last);
        else if (IsKeyPressed(KEY_F) && shift) compose_open(app, COMPOSE_FORWARD, last);
    } else if (t->kind == TAB_FILTERS) filters_keys(app);
    else if (t->kind == TAB_SETTINGS) settings_keys(app);
    else compose_keys(app, t->compose);
}

void mail_frame(jd_app *app)
{
    float sw = jd_screen_w(), sh = jd_screen_h();
    pending_tick(app, 0);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int free_keys = !app->dialog.kind && !app->menu.open && !app->help;
    if (free_keys && current_mode(app) != MODE_INSERT && IsKeyPressed(KEY_SLASH) && shift) {
        app->help = 1;
        GetCharPressed();
    } else if (free_keys && ctrl && IsKeyPressed(KEY_B) && current_mode(app) != MODE_INSERT) {
        app->prefs.sidebar_rail = !app->prefs.sidebar_rail;
        save_prefs(app);
    } else if (free_keys && !tabs_keys(app)) {
        tab_keys(app);
    }

    /* the tab bar shows only when there is more than the mail view, and not when tabs live in the sidebar */
    float top = (!app->prefs.tabs_side && app->ntabs > 1) ? TAB_H : 0;
    if (top) tabs_draw(app, (Rectangle){0, 0, sw, TAB_H});
    float side = app->prefs.sidebar_rail ? 60 : (app->side_w ? app->side_w : 230);
    float body_h = sh - top - BAR_H;
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    sidebar_draw(app, (Rectangle){0, top, side, body_h});
    focus_line(app, PANE_FOLDERS, (Rectangle){0, top, side, body_h});
    float bx = side;
    if (!app->prefs.sidebar_rail) {
        app->side_w = splitter(app, 1, (Rectangle){side, top, SPLIT, body_h}, 1, 0, side, 170, sw * 0.4f);
        bx = side + SPLIT;
    }
    Rectangle page = {bx, top, sw - bx, body_h};
    jd_tab *t = &app->tabs[app->tab];
    if (t->kind == TAB_MAIL) mail_draw(app, page);
    else if (t->kind == TAB_CONV) conv_draw(app, &t->conv, page, 1);
    else if (t->kind == TAB_FILTERS) filters_draw(app, page);
    else if (t->kind == TAB_SETTINGS) settings_draw(app, page);
    else compose_draw(app, t->compose, page);

    footer(app, sw, sh);
    jd_tooltip_draw(&app->ui);
    toast(app, sh);
    menu_draw(app);
    dialog_draw(app);
    help_draw(app);
}
