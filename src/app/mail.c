#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "rlgl.h"
#include "ui/fx.h"
#include "ui/icons.h"

enum { TAB_H = 34, BAR_H = 28, SPLIT = 5, RAIL_W = 60 };

/* The phone's drawer grows out of the rail; drawer_k is how far it is open (0 to 1). What it grows over as it opens
   and the drawer itself as it closes cannot be drawn live (the page would take the taps, and by then the sidebar
   draws as the rail), so drawer_shot holds the screen of the frame it opened or closed in. */
static float drawer_k;
static Texture2D drawer_shot;
static int drawer_captured;
/* the page as the drawer opened over it, frosted; kept while it closes, when drawer_shot holds the drawer */
static RenderTexture2D drawer_blur;

static void drawer_capture(int opening)
{
    if (drawer_shot.id) UnloadTexture(drawer_shot);
    /* the frame's pending draws first: the screen is read as it stands */
    rlDrawRenderBatchActive();
    Image im = LoadImageFromScreen();
    drawer_shot = LoadTextureFromImage(im);
    UnloadImage(im);
    if (opening) fx_blur(drawer_shot, 0, &drawer_blur);
    drawer_captured = 1;
}

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
    settings_write(app);
    share_changed(app);
}

static void unload_fonts(jd_ui *ui)
{
    jd_fonts_unload(&ui->fonts);
    jd_fonts_unload(&ui->list);
    jd_fonts_unload(&ui->reader);
    jd_face_free(ui->mono.regular);
    ui->mono.regular = ui->mono.semibold = NULL;
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
    app->prefs.light = app->prefs.scheme == 2 ? jd_theme_desktop_light() == 1 : app->prefs.scheme == 1;
    jd_theme_set(app->prefs.light, app->prefs.theme_roles);
    jd_scale = plat_density() * (float)(app->prefs.scale ? app->prefs.scale : 100) / 100.0f;
    /* a phone has no room for a reading pane beside or under the list */
    /* and keeps its tabs in a bar on top, the sidebar being hidden behind what is open */
    if (plat_touch()) app->prefs.reader_pos = 2, app->prefs.tabs_side = 0;
    app->grouped = app->prefs.conversations;
    app->side_w = (float)app->prefs.sidebar_width;
    app->list_w = (float)app->prefs.list_width;
    app->list_h = (float)app->prefs.list_height;
    plat_push(app->prefs.notify && app->prefs.push);
    fx_enable(app->prefs.effects);
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

void mail_view_reset(jd_app *app)
{
    reader_close(app);
    jd_folder_rows_free(app->folders, app->nfolders);
    jd_thread_rows_free(app->rows, app->nrows);
    app->folders = NULL;
    app->rows = NULL;
    app->nfolders = app->folder = app->nrows = app->row = 0;
    free(app->status), free(app->error), free(app->query), free(app->server_hits), free(app->filter_from);
    app->status = app->error = app->query = app->filter_from = NULL;
    app->server_hits = NULL;
    app->nserver_hits = 0;
    free(app->picked);
    app->picked = NULL;
    app->npicked = 0;
    app->list_scroll = app->side_scroll = 0;
    free(app->dialog.ids);
    jd_field_free(&app->dialog.field);
    app->dialog = (jd_dialog){0};
}

void mail_reload(jd_app *app)
{
    if (!app->view) return;
    int64_t keep = app->nfolders ? app->folders[app->folder].id : 0;
    jd_folder_rows_free(app->folders, app->nfolders);
    app->folders = jd_store_folders(app->view, &app->nfolders);
    folders_tree_order(app);
    share_check(app);
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

void mail_open_message(jd_app *app, int64_t id)
{
    mail_select_inbox(app);
    int64_t thread = app->view ? jd_message_thread(app->view, id) : 0;
    /* a test notification names no message */
    if (!thread) return;
    for (int k = 0; k < app->nrows; k++)
        if (app->grouped ? app->rows[k].thread == thread : app->rows[k].latest == id) {
            app->row = k;
            app->pane = PANE_LIST;
            reader_open(app, app->prefs.reader_pos == 2);
            return;
        }
    /* filtered out of the list */
    jd_tab *t = tab_add(app, TAB_CONV);
    conv_open(app, &t->conv, thread, app->grouped ? 0 : id);
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
/* *pos changes only while dragged, so a window shown small for a moment does not shrink the saved size */
static void splitter(jd_app *app, int id, Rectangle r, int vertical, float origin, float *pos, float min, float max)
{
    Vector2 m = GetMousePosition();
    Rectangle grab = vertical ? (Rectangle){r.x - 3, r.y, r.width + 6, r.height} : (Rectangle){r.x, r.y - 3, r.width, r.height + 6};
    int hover = CheckCollisionPointRec(m, grab) && !app->dialog.kind && !app->menu.open;
    if (hover || app->dragging == id) SetMouseCursor(vertical ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS);
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app->dragging = id;
    DrawRectangleRec(r, app->dragging == id ? JD_NORD3 : hover ? JD_NORD2 : JD_NORD1);
    if (app->dragging != id) return;
    float p = (vertical ? m.x : m.y) - origin;
    *pos = p < min ? min : p > max ? max : p;
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        app->dragging = 0;
        save_prefs(app);
    }
}

/* reply, forward and the message actions above the preview */
enum { TB_NOT_JUNK, TB_REPLY, TB_REPLY_ALL, TB_FORWARD, TB_ARCHIVE, TB_DELETE, TB_MOVE, TB_UNREAD, TB_FLAG, TB_PLAIN, TB_PAGE,
       TB_ZOOM_OUT, TB_ZOOM_IN, TB_ZOOM_RESET, TB_TAB, TB_SAVE, TB_PRINT, TB_COUNT };

/* those that do not fit go into a menu behind the last; returns 1 when a tab's message left it (archived, deleted) */
static int toolbar_buttons(jd_app *app, jd_conv *c, Rectangle r, int in_tab)
{
    static jd_popup more;
    static int more_ids[TB_COUNT], nmore;
    /* drawn once a frame: the calls count frames */
    static unsigned calls, opened_at;
    calls++;
    int64_t last = c->reading[c->n - 1].row.id;
    /* the zoom's tips say where it stands; a phone's menu has no keys to name */
    char zoom_out_tip[48], zoom_in_tip[48], zoom_reset_tip[48];
    int touch = plat_touch(), pct = reader_zoom_percent();
    snprintf(zoom_out_tip, sizeof zoom_out_tip, touch ? "Zoom out (%d%%)" : "Zoom out (Ctrl+-), %d%%", pct);
    snprintf(zoom_in_tip, sizeof zoom_in_tip, touch ? "Zoom in (%d%%)" : "Zoom in (Ctrl+=), %d%%", pct);
    snprintf(zoom_reset_tip, sizeof zoom_reset_tip, touch ? "Zoom 100%%" : "Zoom 100%% (Ctrl+0)");
    struct {
        const char *icon, *tip;
        int gap;
    } all[TB_COUNT] = {
        [TB_NOT_JUNK] = {"inbox", "Not junk: back to the Inbox", 0},
        [TB_REPLY] = {"reply", "Reply (r)", 0}, [TB_REPLY_ALL] = {"reply-all", "Reply all (R)", 0},
        [TB_FORWARD] = {"forward", "Forward (F)", 0}, [TB_ARCHIVE] = {"archive", "Archive (e)", 1},
        [TB_DELETE] = {"trash-2", "Delete (dd)", 0}, [TB_MOVE] = {"folder-input", "Move (m)", 0},
        [TB_UNREAD] = {"mail", "Mark unread (u)", 0}, [TB_FLAG] = {"star", "Flag (s)", 0},
        [TB_PLAIN] = {c->plain ? "file-image" : "file-text", c->plain ? "Show HTML (Shift+P)" : "Show plain text (Shift+P)", 1},
        [TB_PAGE] = {mail_page_dark(app) ? "sun" : "moon", mail_page_dark(app) ? "Light mail page (Shift+L)" : "Dark mail page (Shift+L)", 0},
        [TB_ZOOM_OUT] = {"zoom-out", zoom_out_tip, 1}, [TB_ZOOM_IN] = {"zoom-in", zoom_in_tip, 0},
        [TB_ZOOM_RESET] = {"search", zoom_reset_tip, 0},
        [TB_TAB] = {"external-link", "Open in a tab (Shift+Enter)", 1}, [TB_SAVE] = {"download", "Save as .eml (Ctrl+S)", 0},
        [TB_PRINT] = {"printer", "Print (Ctrl+P)", 0},
    };
    static const int desk[] = {TB_REPLY, TB_REPLY_ALL, TB_FORWARD, TB_ARCHIVE, TB_DELETE, TB_MOVE, TB_UNREAD,
                               TB_FLAG, TB_PLAIN, TB_PAGE, TB_ZOOM_OUT, TB_ZOOM_IN, TB_ZOOM_RESET, TB_TAB, TB_SAVE, TB_PRINT};
    static const int tab[] = {TB_REPLY, TB_REPLY_ALL, TB_FORWARD, TB_ARCHIVE, TB_DELETE, TB_MOVE, TB_UNREAD,
                              TB_FLAG, TB_PLAIN, TB_PAGE, TB_ZOOM_OUT, TB_ZOOM_IN, TB_ZOOM_RESET, TB_SAVE, TB_PRINT};
    static const int phone[] = {TB_ARCHIVE, TB_DELETE, TB_UNREAD, TB_FLAG, TB_REPLY, TB_MOVE, TB_REPLY_ALL,
                                TB_FORWARD, TB_PLAIN, TB_PAGE, TB_ZOOM_IN, TB_ZOOM_OUT, TB_ZOOM_RESET, TB_SAVE, TB_PRINT};
    const int *base = !in_tab ? desk : plat_touch() ? phone : tab;
    int nbase = !in_tab ? (int)(sizeof desk / sizeof *desk)
                        : plat_touch() ? (int)(sizeof phone / sizeof *phone) : (int)(sizeof tab / sizeof *tab);
    /* a conversation with a message in Junk (or Spam) has Not junk first */
    int junk = 0, order[TB_COUNT], n = 0;
    for (int k = 0; k < c->n; k++) junk |= jd_streq(c->reading[k].row.special_use, "Junk");
    if (junk) order[n++] = TB_NOT_JUNK;
    for (int k = 0; k < nbase; k++) order[n++] = base[k];
    float need = 0;
    for (int k = 0; k < n; k++) need += 36 + (all[order[k]].gap && k ? 12 : 0);
    int shown = n;
    if (need > r.width - 8) {
        float w = 36;
        for (shown = 0; shown < n && w + 36 + (all[order[shown]].gap && shown ? 12 : 0) <= r.width - 8; shown++)
            w += 36 + (all[order[shown]].gap && shown ? 12 : 0);
    }
    /* 48 dp: Material's minimum touch target */
    int spread = in_tab && plat_touch();
    if (spread) {
        int slots = (int)(r.width / 48);
        shown = n <= slots ? n : slots - 1;
    }
    float step = spread ? r.width / (float)(shown + (shown < n)) : 36;
    float x = r.x;
    const char *tip = NULL;
    float tip_x = 0;
    int chosen = -1, free_mouse = !app->dialog.kind && !app->menu.open && !more.open && !on_floating();
    for (int k = 0; k < shown; k++) {
        int id = order[k];
        if (spread) x = r.x + step * (float)k + (step - 32) / 2;
        else if (all[id].gap && k) {
            DrawRectangle((int)x + 4, (int)r.y + 12, 1, 20, JD_NORD2);
            x += 12;
        }
        Rectangle b = {x, r.y + 6, 32, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && free_mouse;
        jd_tap_icon(all[id].icon, b, 18, jd_tap_face(b, hover, 0, 0.3f), JD_NORD4, JD_NORD8);
        if (hover && !app->ui.touch) tip = all[id].tip, tip_x = x;
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) chosen = id;
        x += 36;
    }
    if (shown < n) {
        if (spread) x = r.x + step * (float)shown + (step - 32) / 2;
        Rectangle b = {x, r.y + 6, 32, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && free_mouse;
        jd_tap_icon("ellipsis-vertical", b, 18, jd_tap_face(b, hover, more.open, 0.3f), JD_NORD4, JD_NORD8);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            jd_popup_open(&more);
            nmore = 0;
            for (int k = shown; k < n; k++) more_ids[nmore++] = order[k];
            /* jd_popup_draw's height is 8 plus 30 a row */
            float h = 8 + 30 * (float)nmore;
            more.at = (Vector2){x + 32 - 240, spread ? r.y - h - 4 : r.y + 40};
            opened_at = calls;
        }
    }
    /* drawn from the next frame: the press that opened it is outside it, and would close it at once */
    if (more.open && calls > opened_at) {
        jd_popup_item items[TB_COUNT];
        for (int k = 0; k < nmore; k++) {
            const char *t = all[more_ids[k]].tip;
            items[k] = (jd_popup_item){.label = t, .icon = all[more_ids[k]].icon};
        }
        char labels[TB_COUNT][48];
        for (int k = 0; k < nmore; k++) {
            snprintf(labels[k], sizeof labels[k], "%s", items[k].label);
            char *paren = strstr(labels[k], " (");
            if (paren) *paren = 0;
            items[k].label = labels[k];
        }
        int k = jd_popup_draw(&app->ui, &more, items, nmore);
        if (k >= 0) chosen = more_ids[k];
    }
    if (tip) jd_tooltip(tip, tip_x, r.y + 42);
    /* a tab's message may not be the list's selection: the actions on the list's messages are pointed at it */
    int on_list = chosen == TB_ARCHIVE || chosen == TB_DELETE || chosen == TB_MOVE || chosen == TB_UNREAD || chosen == TB_FLAG ||
                  chosen == TB_NOT_JUNK;
    if (in_tab && on_list && !list_select_conv(app, c)) return 0;
    switch (chosen) {
    case TB_REPLY: compose_open(app, COMPOSE_REPLY, last); break;
    case TB_REPLY_ALL: compose_open(app, COMPOSE_REPLY_ALL, last); break;
    case TB_FORWARD: compose_open(app, COMPOSE_FORWARD, last); break;
    case TB_NOT_JUNK: act_not_junk(app); return in_tab;
    case TB_ARCHIVE: act_archive(app); return in_tab;
    case TB_DELETE: act_delete(app); return in_tab;
    case TB_MOVE: dialog_open(app, DLG_PICK_FOLDER, ACT_MOVE, "Move to", NULL); break;
    case TB_UNREAD: act_toggle_read(app); break;
    case TB_FLAG: act_toggle_flag(app); break;
    case TB_PLAIN: c->plain = !c->plain; break;
    case TB_PAGE: app->page_flip = !app->page_flip; break;
    case TB_ZOOM_OUT: reader_zoom(app, -1); break;
    case TB_ZOOM_IN: reader_zoom(app, 1); break;
    case TB_ZOOM_RESET: reader_zoom(app, 0); break;
    case TB_TAB: reader_open(app, 1); break;
    case TB_SAVE: conv_save_message(app, c); break;
    case TB_PRINT: app->print_tab = in_tab ? app->tab + 1 : 1; break;
    }
    return 0;
}

static void toolbar(jd_app *app, Rectangle r)
{
    DrawRectangleRec((Rectangle){r.x, r.y, r.width, 44}, JD_NORD0);
    DrawRectangle((int)r.x, (int)r.y + 43, (int)r.width, 1, JD_NORD1);
    toolbar_buttons(app, preview(app), (Rectangle){r.x + 16, r.y, r.width - 16, 44}, 0);
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
    if (app->tab == 0 && app->pane == pane && !plat_touch()) DrawRectangleLinesEx(r, 2, JD_NORD8);
}

/* the list and the reading pane, right of the sidebar */
/* the reading pane fades up from the background when another message comes into it */
static void reader_arrives(jd_app *app, Rectangle r)
{
    static int64_t seen;
    static double at = -10;
    int64_t shown = preview(app)->thread * 31 + preview(app)->message;
    if (shown != seen) {
        if (seen && shown && fx_enabled()) at = GetTime();
        seen = shown;
    }
    float u = (float)(GetTime() - at) / 0.18f;
    if (u < 1) DrawRectangleRec(r, ColorAlpha(JD_NORD0, 0.85f * (1 - u) * (1 - u)));
}

static void mail_draw(jd_app *app, Rectangle r)
{
    float w = r.width, h = r.height;
    if (!app->list_w) app->list_w = w > 1300 ? 500 : w * 0.44f;
    if (!app->list_h) app->list_h = h * 0.45f;
    /* fitted to this window without changing the saved sizes */
    float lh = fmaxf(160, fminf(app->list_h, h - 140)), lw = fmaxf(300, fminf(app->list_w, w - 300));
    switch (app->prefs.reader_pos) {
    case 1:
        splitter(app, 2, (Rectangle){r.x, r.y + lh, w, SPLIT}, 0, r.y, &app->list_h, 160, h - 140);
        lh = fmaxf(160, fminf(app->list_h, h - 140));
        list_draw(app, (Rectangle){r.x, r.y, w, lh});
        preview_draw(app, (Rectangle){r.x, r.y + lh + SPLIT, w, h - lh - SPLIT});
        reader_arrives(app, (Rectangle){r.x, r.y + lh + SPLIT, w, h - lh - SPLIT});
        focus_line(app, PANE_LIST, (Rectangle){r.x, r.y, w, lh});
        focus_line(app, PANE_READER, (Rectangle){r.x, r.y + lh + SPLIT, w, h - lh - SPLIT});
        break;
    case 2:
        list_draw(app, r);
        focus_line(app, PANE_LIST, r);
        break;
    default:
        splitter(app, 2, (Rectangle){r.x + lw, r.y, SPLIT, h}, 1, r.x, &app->list_w, 300, w - 300);
        lw = fmaxf(300, fminf(app->list_w, w - 300));
        list_draw(app, (Rectangle){r.x, r.y, lw, h});
        preview_draw(app, (Rectangle){r.x + lw + SPLIT, r.y, w - lw - SPLIT, h});
        reader_arrives(app, (Rectangle){r.x + lw + SPLIT, r.y, w - lw - SPLIT, h});
        focus_line(app, PANE_LIST, (Rectangle){r.x, r.y, lw, h});
        focus_line(app, PANE_READER, (Rectangle){r.x + lw + SPLIT, r.y, w - lw - SPLIT, h});
        break;
    }
}

/* "Archived 2 conversations  Undo" while a move waits */
void floating_add(Rectangle r) { jd_cover(r); }
int on_floating(void) { return jd_covered(); }

static void toast(jd_app *app, float sh)
{
    jd_pending *p = &app->pending;
    if (!p->ids || p->due < 0) return;
    jd_ui *ui = &app->ui;
    float tw = jd_text_width(ui, p->label, 14, 0);
    Rectangle box = {(jd_screen_w() - tw - 130) / 2, sh - BAR_H - 62, tw + 130, 44};
    floating_add(box);
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
        (t->kind == TAB_FILTERS && app->filters.state == 1) || (t->kind == TAB_CALENDAR && calendar_typing()) ||
        app->show_form)
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
    /* the theme's accent, magenta and green, with the text buttons on the accent use */
    Color colors[] = {JD_NORD10, JD_PURPLE, JD_GREEN};
    jd_mode m = current_mode(app);
    float x = 12;
    /* the mode and the pane with the keys mean nothing on a touch screen */
    if (!plat_touch()) {
        float mw = jd_text_width(ui, names[m], 12, 1) + 20;
        DrawRectangleV((Vector2){0, y}, (Vector2){mw, BAR_H}, colors[m]);
        jd_text(ui, names[m], 10, y + 8, 12, JD_ON_ACCENT, 1);
        x = mw + 12;
    }
    /* which pane has the keys, beside the mode */
    if (app->tab == 0 && !plat_touch()) {
        static const char *panes[] = {"Folders", "List", "Reader"};
        const char *pane = panes[app->pane % 3];
        jd_text(ui, pane, x, y + 7, 14, JD_NORD8, 1);
        x += jd_text_width(ui, pane, 14, 1) + 16;
    }
    const char *chord = app->pending_g ? "g  (g top, i Inbox, s Sent, d Drafts, a Archive, t Trash, j Junk, c Calendar)"
                        : app->pending_d ? "d  (d again deletes)"
                        : reader_caret() ? "Caret: h j k l w b move, v starts selecting, Esc leaves"
                        : reader_visual() ? "y copies, v or Esc stops selecting"
                        : app->pending_f ? "f  (u unread, f flagged, a files, s sender, c clear)"
                                         : NULL;
    if (chord) {
        jd_text(ui, chord, x, y + 7, 14, JD_NORD8, 1);
        x += jd_text_width(ui, chord, 14, 1) + 16;
    }
    int noting = app->notice[0] && GetTime() < app->notice_until;
    /* the calendar's tab shows its own worker */
    int cal = app->tabs[app->tab].kind == TAB_CALENDAR;
    const char *error = cal ? calendar_error() : app->error, *status = cal ? calendar_status() : app->status;
    const char *line = noting && cal ? app->notice : error ? error : status ? status : noting ? app->notice
                       : cal ? (calendar_connected() ? "Calendars up to date" : "No calendar connected") : "Up to date";
    jd_icon(error && !(noting && cal) ? "triangle-alert" : status ? "refresh-cw" : noting ? "check" : "circle-check", x, y + 6, 15,
            error && !(noting && cal) ? JD_NORD13 : noting ? JD_NORD8 : JD_NORD4);
    char counts[200] = "";
    if (app->tab == 0 && app->nfolders) {
        const jd_folder_row *f = &app->folders[app->folder];
        int n = snprintf(counts, sizeof counts, "%s", app->query ? "Search" : folder_label(f));
        if (app->npicked) n += snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d selected", app->npicked);
        n += snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d %s", app->nrows,
                      app->grouped ? (app->nrows == 1 ? "conversation" : "conversations") : (app->nrows == 1 ? "message" : "messages"));
        if (!app->query && f->unread) snprintf(counts + n, sizeof counts - (size_t)n, "  ·  %d unread", f->unread);
    }
    float cw = jd_text_width(ui, counts, 14, 0);
    jd_text_fit(ui, line, x + 22, y + 7, sw - x - 40 - cw - 24, 14, line == error ? JD_NORD13 : JD_NORD4, 0);
    jd_text(ui, counts, sw - cw - 14, y + 7, 14, JD_NORD4, 0);
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
        else if (IsKeyPressed(KEY_B) && last) message_snooze(app, last);
        else if (IsKeyPressed(KEY_F) && shift) compose_open(app, COMPOSE_FORWARD, last);
    } else if (t->kind == TAB_FILTERS) filters_keys(app);
    else if (t->kind == TAB_SETTINGS) settings_keys(app);
    else if (t->kind == TAB_CALENDAR) calendar_keys(app);
    else compose_keys(app, t->compose);
}

void mail_frame(jd_app *app)
{
    float sw = jd_screen_w(), sh = jd_screen_h();
    jd_cover_frame();
    pending_tick(app, 0);
    compose_held_tick(app);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int free_keys = !app->dialog.kind && !app->menu.open && !app->help && !snooze_open_now() && !accounts_menu_open();
    /* Ctrl+Shift+1..9: that account */
    if (free_keys && ctrl && shift && current_mode(app) != MODE_INSERT)
        for (int k = 0; k < 9; k++)
            if (IsKeyPressed(KEY_ONE + k)) account_switch(app, k);
    jd_tab *now = &app->tabs[app->tab];
    /* the search bar shows over every page; a phone has room for it over the mail list only */
    int bar_shown = !plat_touch() || now->kind == TAB_MAIL;
    if (!bar_shown) app->searching = 0;
    if (free_keys && app->searching) {
        search_bar_keys(app);
    } else if (free_keys && current_mode(app) != MODE_INSERT && IsKeyPressed(KEY_SLASH) && shift) {
        app->help = 1;
        GetCharPressed();
    } else if (free_keys && bar_shown && current_mode(app) != MODE_INSERT &&
               ((IsKeyPressed(KEY_SLASH) && !shift) || (ctrl && IsKeyPressed(KEY_K) && now->kind != TAB_COMPOSE))) {
        /* / or Ctrl+K: the search bar, from any page */
        search_focus(app);
        GetCharPressed();
    } else if (free_keys && ctrl && current_mode(app) != MODE_INSERT &&
               (app->tabs[app->tab].kind == TAB_CONV || (app->tabs[app->tab].kind == TAB_MAIL && preview(app)->n)) &&
               (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD) || IsKeyPressed(KEY_MINUS) ||
                IsKeyPressed(KEY_KP_SUBTRACT) || IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0))) {
        /* the reader's zoom, while a message shows */
        reader_zoom(app, IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0) ? 0
                         : IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) ? -1 : 1);
    } else if (free_keys && ctrl && IsKeyPressed(KEY_B) && current_mode(app) != MODE_INSERT) {
        app->prefs.sidebar_rail = !app->prefs.sidebar_rail;
        save_prefs(app);
    } else if (free_keys && !tabs_keys(app)) {
        tab_keys(app);
    }

    /* the tab bar shows only when there is more than the mail view, and not when tabs live in the sidebar */
    float top = (!app->prefs.tabs_side && app->ntabs > 1) ? TAB_H : 0;
    if (top) tabs_draw(app, (Rectangle){0, 0, sw, TAB_H});
    float side = app->prefs.sidebar_rail ? RAIL_W : fmaxf(170, fminf(app->side_w ? app->side_w : 230, sw * 0.4f));
    float body_h = sh - top - BAR_H;
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    /* On a phone the full sidebar is a drawer over the page, which is too narrow to share: the page shows behind it
       as it was when the drawer opened (so it takes no taps), and a tap on the dimmed rest closes it, as choosing a
       folder does. */
    const float DRAWER_S = 0.25f;
    float dw = sw * 0.8f < 300 ? sw * 0.8f : 300;
    int drawer = plat_touch() && !app->prefs.sidebar_rail && app->tabs[app->tab].kind == TAB_MAIL;
    /* the frame after a capture took its readback's time (16-23 ms on a phone), and any other slow frame is a
       stall: counted, the drawer jumped a quarter of the way at once. A step is at most a 60 Hz frame. */
    float dt = drawer_captured ? 0 : fminf(GetFrameTime(), 1.0f / 60);
    drawer_captured = 0;
    /* both ways it moves at once and slows as it arrives (cubic ease-out): opening as 1 - (1 - k)^3, closing as
       k^3. Turning back midway, k is moved to where the other curve stands at the same opening. */
    static int opening = 1;
    if (drawer != opening) {
        float at = opening ? 1 - powf(1 - drawer_k, 3) : powf(drawer_k, 3);
        drawer_k = drawer ? 1 - cbrtf(1 - at) : cbrtf(at);
        opening = drawer;
    }
    drawer_k = drawer ? fminf(1, drawer_k + dt / DRAWER_S) : fmaxf(0, drawer_k - dt / DRAWER_S);
    float out = drawer ? 1 - powf(1 - drawer_k, 3) : powf(drawer_k, 3);    /* the drawer is uncovered from the rail's edge outwards, crossfading with the rail over the rail's width */
    float shown = RAIL_W + (dw - RAIL_W) * out;
    if (drawer) {
        if (drawer_shot.id) {
            jd_scale_end();
            DrawTexture(drawer_shot, 0, 0, WHITE);
            jd_scale_begin();
        }
        fx_draw_blurred(drawer_blur, out);
        DrawRectangleRec((Rectangle){0, top, sw, body_h}, ColorAlpha(BLACK, 0.45f * out));
        Rectangle rest = {dw, top, sw - dw, body_h};
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), rest)) {
            app->prefs.sidebar_rail = 1;
            save_prefs(app);
        }
        jd_scissor(0, top, shown, body_h);
        sidebar_draw(app, (Rectangle){0, top, dw, body_h});
        jd_scissor_end();
        if (drawer_shot.id && out < 1) {
            jd_scissor(0, top, RAIL_W, body_h);
            jd_scale_end();
            DrawTexture(drawer_shot, 0, 0, ColorAlpha(WHITE, 1 - out));
            jd_scale_begin();
            jd_scissor_end();
        }
        footer(app, sw, sh);
        jd_tooltip_draw(&app->ui);
        menu_draw(app);
        accounts_menu_draw(app);
        dialog_draw(app);
        if (app->prefs.sidebar_rail) drawer_capture(0);
        return;
    }
    /* a phone gives what is open (a message, compose, settings) the whole width; the folders are the list's */
    int bare = plat_touch() && app->tabs[app->tab].kind != TAB_MAIL;
    if (bare) side = 0;
    else sidebar_draw(app, (Rectangle){0, top, side, body_h});
    focus_line(app, PANE_FOLDERS, (Rectangle){0, top, side, body_h});
    float bx = side;
    if (!app->prefs.sidebar_rail && !bare) {
        if (!app->side_w) app->side_w = side;
        splitter(app, 1, (Rectangle){side, top, SPLIT, body_h}, 1, 0, &app->side_w, 170, sw * 0.4f);
        bx = side + SPLIT;
    }
    Rectangle page = {bx, top, sw - bx, body_h};
    if (bar_shown) {
        search_bar(app, (Rectangle){page.x, page.y, page.width, 52});
        page.y += 52, page.height -= 52;
    }
    jd_tab *t = &app->tabs[app->tab];
    /* a phone has no Esc to close what is open: a back bar does it, to the folder it came from */
    Rectangle actions = {0};
    if (plat_touch() && (t->kind == TAB_CONV || t->kind == TAB_COMPOSE || t->kind == TAB_FILTERS || t->kind == TAB_CALENDAR)) {
        const char *to = app->nfolders ? folder_label(&app->folders[app->folder]) : "Back";
        Rectangle back = {page.x, page.y, page.width, 44};
        int hover = CheckCollisionPointRec(GetMousePosition(), back) && !app->dialog.kind && !app->menu.open;
        DrawRectangleRec(back, hover ? JD_NORD2 : JD_NORD1);
        jd_icon("chevron-left", back.x + 14, back.y + 14, 16, JD_NORD8);
        jd_text_fit(&app->ui, to, back.x + 38, back.y + 14, page.width - 52, 15, JD_NORD8, 1);
        page.y += 44, page.height -= 44;
        if (t->kind == TAB_CONV && t->conv.n) {
            page.height -= 48;
            actions = (Rectangle){page.x, page.y + page.height, page.width, 48};
            DrawRectangleRec(actions, JD_NORD1);
            DrawRectangle((int)actions.x, (int)actions.y, (int)actions.width, 1, JD_NORD2);
            actions.y += 2;
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            /* the page under the bar is not drawn this frame: the tap would land on it (the search box) too */
            tab_close(app, app->tab);
            footer(app, sw, sh);
            return;
        }
    } else if (t->kind == TAB_CONV && t->conv.n) {
        DrawRectangleRec((Rectangle){page.x, page.y, page.width, 44}, JD_NORD0);
        DrawRectangle((int)page.x, (int)page.y + 43, (int)page.width, 1, JD_NORD1);
        actions = (Rectangle){page.x + 16, page.y, page.width - 16, 44};
        page.y += 44, page.height -= 44;
    }
    /* another tab, page or account comes in from a little to the right, fading up from the background */
    static int seen_tab = -1, seen_cur = -1;
    static tab_kind seen_kind;
    static double page_at = -10;
    if (app->tab != seen_tab || app->cur != seen_cur || t->kind != seen_kind) {
        if (seen_tab >= 0 && fx_enabled()) page_at = GetTime();
        seen_tab = app->tab, seen_cur = app->cur, seen_kind = t->kind;
    }
    float pu = fminf(1, (float)(GetTime() - page_at) / 0.22f), pe = 1 - (1 - pu) * (1 - pu) * (1 - pu);
    Rectangle arriving = page;
    if (pu < 1) page.x += 14 * (1 - pe);
    if (t->kind == TAB_MAIL) mail_draw(app, page);
    else if (t->kind == TAB_CONV) {
        /* a swipe pages through the list; the finger to the left is the next message */
        static struct {
            int tab, owned;      /* owned: the gesture began on the page */
            double at;           /* settling start; < 0 none, 0 starts with the next frame */
            float from, last;    /* settling start offset; last frame's drag */
            int leaving;         /* `out` holds the message sliding away, `dir` the side it goes to */
            float dir;
            jd_conv out;
        } pg = {.at = -1};
        const double SLIDE_S = 0.3;
        if (pg.tab != app->tab) {
            if (pg.leaving) conv_close(&pg.out);
            pg = (typeof(pg)){.tab = app->tab, .at = -1};
        }
        int free_page = !app->dialog.kind && !app->menu.open && !app->help;
        float drag = plat_drag_x();
        int swipe = plat_swipe(); /* -1 or 1 pages, 2 springs back */
        if (drag && !pg.last && !swipe) {
            /* the pointer stays at the finger's first point while it drags */
            pg.owned = free_page && CheckCollisionPointRec(GetMousePosition(), page);
            if (pg.owned && pg.leaving) conv_close(&pg.out), pg.leaving = 0;
            if (pg.owned) pg.at = -1;
        }
        if (!pg.owned) drag = 0;
        if (swipe && pg.owned) {
            int stepped = (swipe == 1 || swipe == -1) && free_page && reader_step(app, &t->conv, -swipe, &pg.out);
            pg.from = stepped ? drag - (float)swipe * page.width : drag;
            pg.leaving = stepped, pg.dir = (float)swipe;
            /* the clock starts once the new message is drawn: its layout takes a frame or two */
            pg.at = 0;
            drag = 0;
        }
        if (swipe) pg.owned = 0;
        pg.last = drag;
        float shift = drag;
        if (!drag && pg.at >= 0) {
            float k = pg.at == 0 ? 0 : (float)((GetTime() - pg.at) / SLIDE_S);
            if (k < 1) shift = pg.from * (1 - k) * (1 - k) * (1 - k) * (1 - k);
            else {
                pg.at = -1;
                if (pg.leaving) conv_close(&pg.out), pg.leaving = 0;
            }
        }
        jd_scissor(page.x, page.y, page.width, page.height);
        if (pg.leaving && pg.at >= 0)
            conv_draw(app, &pg.out, (Rectangle){page.x + shift + pg.dir * page.width, page.y, page.width, page.height}, 0);
        /* a reply or forward from the conversation opens a tab, and tab_add may move the tabs: t is looked up again */
        t = &app->tabs[pg.tab];
        conv_draw(app, &t->conv, (Rectangle){page.x + shift, page.y, page.width, page.height}, 1);
        t = &app->tabs[pg.tab];
        if (pg.at == 0 && !drag) pg.at = GetTime();
        /* the next message is named from the list: opening it early would mark it read */
        if (drag && (shift > 1 || shift < -1)) {
            int dir = shift < 0 ? 1 : -1, at = -1;
            for (int k = 0; k < app->nrows && at < 0; k++)
                if (app->grouped ? app->rows[k].thread == t->conv.thread : app->rows[k].latest == t->conv.message) at = k;
            Rectangle gap = shift < 0 ? (Rectangle){page.x + page.width + shift, page.y, -shift, page.height}
                                      : (Rectangle){page.x, page.y, shift, page.height};
            DrawRectangleRec(gap, JD_NORD0);
            int to = at + dir;
            float tx = shift < 0 ? gap.x + 20 : gap.x + gap.width - page.width + 20, tw = page.width - 40;
            if (at >= 0 && to >= 0 && to < app->nrows) {
                const jd_thread_row *n = &app->rows[to];
                jd_icon(dir > 0 ? "chevron-right" : "chevron-left", shift < 0 ? gap.x + 8 : gap.x + gap.width - 24,
                        page.y + 60, 16, JD_NORD8);
                jd_text_fit(&app->ui, n->senders, tx + 12, page.y + 40, tw - 24, 14, JD_NORD4, 0);
                jd_text_fit(&app->ui, n->subject, tx + 12, page.y + 62, tw - 24, 17, JD_NORD6, 1);
            } else {
                jd_text_fit(&app->ui, dir > 0 ? "No newer message" : "No earlier message", tx + 12, page.y + 50,
                            tw - 24, 14, JD_MUTED, 0);
            }
        }
        jd_scissor_end();
        if (actions.width > 0 && t->conv.n && toolbar_buttons(app, &t->conv, actions, 1)) {
            tab_close(app, app->tab);
            footer(app, sw, sh);
            return;
        }
    }
    else if (t->kind == TAB_FILTERS) filters_draw(app, page);
    else if (t->kind == TAB_SETTINGS) settings_draw(app, page);
    else if (t->kind == TAB_CALENDAR) calendar_draw(app, page);
    else compose_draw(app, t->compose, page);
    if (pu < 1) DrawRectangleRec(arriving, ColorAlpha(JD_NORD0, 0.9f * (1 - pe)));
    if (bar_shown) search_bar_overlay(app);

    if (drawer_k > 0 && drawer_shot.id) {
        fx_draw_blurred(drawer_blur, out);
        DrawRectangleRec((Rectangle){0, top, sw, body_h}, ColorAlpha(BLACK, 0.45f * out));
        jd_scissor(0, top, shown, body_h);
        jd_scale_end();
        DrawTexture(drawer_shot, 0, 0, ColorAlpha(WHITE, out));
        jd_scale_begin();
        jd_scissor_end();
    } else if (drawer_shot.id) {
        UnloadTexture(drawer_shot);
        drawer_shot = (Texture2D){0};
        fx_blur_free(&drawer_blur);
    }
    footer(app, sw, sh);
    jd_tooltip_draw(&app->ui);
    toast(app, sh);
    /* above the undo toast when that shows too */
    compose_held_draw(app, app->pending.ids && app->pending.due >= 0 ? sh - BAR_H - 62 - 4 : sh - BAR_H - 10);
    menu_draw(app);
    snooze_draw(app);
    accounts_menu_draw(app);
    dialog_draw(app);
    help_draw(app);
    if (plat_touch() && !app->prefs.sidebar_rail && app->tabs[app->tab].kind == TAB_MAIL) drawer_capture(1);
}
