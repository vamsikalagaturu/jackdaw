#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "ui/fx.h"
#include "ui/icons.h"

typedef enum {
    M_NONE, M_NOT_JUNK, M_OPEN_TAB, M_REPLY, M_REPLY_ALL, M_FORWARD, M_READ, M_FLAG, M_ARCHIVE, M_SNOOZE, M_MOVE, M_COPY, M_DELETE,
    M_F_NEW, M_F_RENAME, M_F_DELETE, M_F_ALL_READ, M_F_IN_NEW, M_SEP, M_BACK, M_SNOOZE_AT /* + snooze_items' index */
} menu_item;

typedef struct {
    menu_item item;
    const char *label, *icon, *keys;
} entry;

static const entry row_menu[] = {
    {M_NOT_JUNK, "Not junk", "inbox", NULL}, {M_SEP, NULL, NULL, NULL},
    {M_OPEN_TAB, "Open in a tab", "external-link", "Shift+Enter"}, {M_SEP, NULL, NULL, NULL},
    {M_REPLY, "Reply", "reply", "r"}, {M_REPLY_ALL, "Reply all", "reply-all", "R"},
    {M_FORWARD, "Forward", "forward", "F"}, {M_SEP, NULL, NULL, NULL},
    {M_READ, "Read / unread", "mail-open", "u"}, {M_FLAG, "Flag", "star", "s"},
    {M_ARCHIVE, "Archive", "archive", "e"}, {M_SNOOZE, "Snooze...", "bell", "b"}, {M_MOVE, "Move to...", "folder-input", "m"},
    {M_COPY, "Copy to...", "copy", "c"}, {M_DELETE, "Delete", "trash-2", "dd"}, {M_NONE, NULL, NULL, NULL},
};

static const entry folder_menu[] = {
    {M_F_ALL_READ, "Mark all as read", "mail-open", NULL}, {M_F_IN_NEW, "Leave out of Home", "house", NULL},
    {M_SEP, NULL, NULL, NULL},
    {M_F_NEW, "New folder inside...", "folder-plus", "a"}, {M_F_RENAME, "Rename...", "pencil", "Shift+R"},
    {M_F_DELETE, "Delete folder...", "trash-2", "Shift+D"}, {M_NONE, NULL, NULL, NULL},
};

static void run(jd_app *app, menu_item item)
{
    int64_t source = app->nrows ? app->rows[app->row].latest : 0;
    const jd_folder_row *f = app->nfolders ? &app->folders[app->folder] : NULL;
    switch (item) {
    case M_NOT_JUNK: act_not_junk(app); break;
    case M_OPEN_TAB: reader_open(app, 1); break;
    case M_REPLY: compose_open(app, COMPOSE_REPLY, source); break;
    case M_REPLY_ALL: compose_open(app, COMPOSE_REPLY_ALL, source); break;
    case M_FORWARD: compose_open(app, COMPOSE_FORWARD, source); break;
    case M_READ: act_toggle_read(app); break;
    case M_FLAG: act_toggle_flag(app); break;
    case M_ARCHIVE: act_archive(app); break;
    case M_SNOOZE:
    case M_BACK:
        app->menu.open = 1;
        app->menu.snooze = item == M_SNOOZE;
        app->menu.since = GetTime();
        break;
    case M_MOVE: dialog_open(app, DLG_PICK_FOLDER, ACT_MOVE, "Move to", NULL); break;
    case M_COPY: dialog_open(app, DLG_PICK_FOLDER, ACT_COPY, "Copy to", NULL); break;
    case M_DELETE: act_delete(app); break;
    case M_F_ALL_READ:
        if (f && app->sync) {
            jd_list_query q = {.folder = f->id, .unread = 1};
            int n;
            jd_thread_row *rows = jd_store_list(app->view, &q, &n);
            int64_t *ids = jd_alloc(sizeof *ids * (size_t)(n + 1));
            for (int i = 0; i < n; i++) ids[i] = rows[i].latest;
            jd_sync_set_flag(app->sync, ids, n, JD_SEEN, 1);
            free(ids);
            jd_thread_rows_free(rows, n);
        }
        break;
    case M_F_IN_NEW:
        if (f) folder_toggle_new(app, f->name);
        break;
    case M_F_NEW:
        if (f) {
            char *prefix = f->delim && strcasecmp(f->name, "INBOX") ? jd_printf("%s%c", f->path, f->delim) : jd_strdup("");
            dialog_open(app, DLG_PROMPT, ACT_CREATE_FOLDER, "New folder (full path)", prefix);
            free(prefix);
        }
        break;
    case M_F_RENAME:
        if (f && strcasecmp(f->name, "INBOX")) dialog_open(app, DLG_PROMPT, ACT_RENAME_FOLDER, "Rename folder (full path)", f->path);
        break;
    case M_F_DELETE:
        if (f && strcasecmp(f->name, "INBOX")) {
            char *title = jd_printf("Delete the folder %s and all %d messages in it, on the server?", f->path, f->total);
            dialog_open(app, DLG_CONFIRM, ACT_DELETE_FOLDER, title, NULL);
            free(title);
        }
        break;
    default:
        if (item >= M_SNOOZE_AT && item < M_SNOOZE_AT + 4) snooze_selection(app, (int)(item - M_SNOOZE_AT));
        break;
    }
}

void menu_draw(jd_app *app)
{
    jd_menu *m = &app->menu;
    if (!m->open) return;
    jd_ui *ui = &app->ui;
    const entry *items = m->on_folder ? folder_menu : row_menu;
    /* Not junk (and the line after it) only in a Junk folder */
    if (items == row_menu && !in_junk(app)) items += 2;
    /* New's item only for a folder New would show, saying what a click does */
    entry shown[24];
    int n = 0;
    const jd_folder_row *f = m->on_folder && m->index >= 0 && m->index < app->nfolders ? &app->folders[m->index] : NULL;
    for (int i = 0; items[i].item != M_NONE && n < 23; i++) {
        /* the Inbox's mail is always news: its notifications come with IDLE, before any list is read */
        if (items[i].item == M_F_IN_NEW && !(f && strcasecmp(f->name, "INBOX") && (f->gets_new || folder_left_out(app, f))))
            continue;
        shown[n] = items[i];
        if (items[i].item == M_F_IN_NEW && folder_left_out(app, f)) shown[n].label = "Show in Home";
        n++;
    }
    shown[n] = (entry){M_NONE, NULL, NULL, NULL};
    items = shown;
    float h = 8;
    for (int i = 0; i < n; i++) h += items[i].item == M_SEP ? 9 : 30;
    float w = 250, x = m->at.x, y = m->at.y;
    if (x + w > jd_screen_w() - 4) x = jd_screen_w() - w - 4;
    if (y + h > jd_screen_h() - 4) y = jd_screen_h() - h - 4;
    /* Snooze's times take the rows' place in the same box, under a row back to them */
    jd_popup_item times[4];
    char labels[3][80];
    entry sub[7];
    if (m->snooze && !m->on_folder) {
        snooze_items(times, labels);
        sub[0] = (entry){M_BACK, "Snooze", "chevron-left", NULL};
        sub[1] = (entry){M_SEP, NULL, NULL, NULL};
        for (int i = 0; i < 4; i++) sub[2 + i] = (entry){(menu_item)(M_SNOOZE_AT + i), times[i].label, times[i].icon, NULL};
        sub[6] = (entry){M_NONE, NULL, NULL, NULL};
        items = sub, n = 6, h = 8 + 5 * 30 + 9;
    }
    Rectangle box = {x, y, w, h};
    DrawRectangleRounded((Rectangle){x + 3, y + 4, w, h}, 0.04f, 6, ColorAlpha(BLACK, 0.25f));
    DrawRectangleRounded(box, 0.04f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.04f, 6, 1, JD_NORD2);
    /* the new rows slide in from the side they come from: the times from the right, back from the left */
    float u = m->since && fx_enabled() ? (float)(GetTime() - m->since) / 0.16f : 1;
    float slide = u < 1 ? (1 - (1 - u) * (1 - u) * (1 - u)) : 1;
    x += (1 - slide) * w * 0.35f * (m->snooze ? 1 : -1);
    jd_scissor(box.x, box.y, box.width, box.height);
    float yy = y + 4;
    for (int i = 0; i < n; i++) {
        if (items[i].item == M_SEP) {
            DrawRectangle((int)x + 8, (int)yy + 4, (int)w - 16, 1, JD_NORD2);
            yy += 9;
            continue;
        }
        Rectangle row = {x + 4, yy, w - 8, 28};
        int hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (hover) DrawRectangleRounded(row, 0.2f, 6, JD_NORD2);
        jd_icon(items[i].icon, row.x + 8, yy + 6, 16, hover ? JD_NORD8 : JD_NORD4);
        jd_text(ui, items[i].label, row.x + 34, yy + 7, 14, JD_NORD6, 0);
        if (items[i].keys && !ui->touch) {
            float kw = jd_text_width(ui, items[i].keys, 12, 0);
            jd_text(ui, items[i].keys, row.x + row.width - kw - 8, yy + 8, 12, JD_MUTED, 0);
        }
        if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            jd_scissor_end();
            m->open = 0;
            run(app, items[i].item);
            return;
        }
        yy += 30;
    }
    jd_scissor_end();
    int outside = !CheckCollisionPointRec(GetMousePosition(), box);
    if (IsKeyPressed(KEY_ESCAPE) && m->snooze) run(app, M_BACK);
    else if (IsKeyPressed(KEY_ESCAPE) || (outside && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))))
        m->open = 0;
}

static const char *help_lines[][2] = {
    {"Everywhere", NULL},
    {"Ctrl+Tab  Ctrl+1..9", "switch tabs"}, {"Ctrl+W", "close the tab"}, {"Ctrl+N", "new message"},
    {"Ctrl+,", "settings"}, {"Ctrl+R", "sync now"}, {"Ctrl+B", "sidebar full or icons only"}, {"?", "this help"},
    {"Ctrl + wheel", "magnify around the pointer, until Ctrl is let go"},
    {"Modes", NULL},
    {"v", "visual: j/k extend the selection, then an action (Esc leaves)"},
    {"Esc  i  a  o", "compose: normal mode, back to insert (I A O: line start, end, line above)"},
    {"h j k l  w b  0 $  gg G", "compose body, normal mode: move; Tab to the next field"},
    {"v  V", "compose body: select characters, whole lines; then d x y c, or Ctrl+B / I / K to format"},
    {"x  dd  yy  p", "compose body: delete a character, cut a line, copy a line, paste"},
    {"Shift+arrows  Ctrl+A", "compose, typing: select; Ctrl+C Ctrl+X Ctrl+V copy, cut, paste"},
    {"Enter", "on a list item: the next item; on an empty one: the list ends"},
    {"Mail", NULL},
    {"/  Ctrl+K", "the search bar, on any page: mail, folders, settings, events, people, actions as you type"},
    {"↑ ↓  Enter", "in the search: pick a result and open it; nothing picked, Enter searches the mail (Ctrl+Enter the server too)"},
    {"from:  is:  after:", "search operators become pills after a space; dates DD-MM-YYYY"},
    {"Backspace", "at the start of the search: the last pill back into the text"},
    {"Tab  h  l", "folders / list / reader"},
    {"j  k  gg  G", "move"},
    {"gi  gs  gd  ga  gt  gj", "go to Inbox, Sent, Drafts, Archive, Trash, Junk"}, {"gc", "the calendar"}, {"Enter  Shift+Enter", "open, open in a tab"}, {"x  Shift+j/k  Ctrl+A", "select"},
    {"t", "conversations or single messages"}, {"p", "reading pane right, bottom or off"},
    {"fu  ff  fa  fs  fc", "filter: unread, flagged, files, this sender, clear"},
    {"Messages", NULL},
    {"r  R  F", "reply, reply all, forward"}, {"u  s", "read/unread, flag"},
    {"e  dd  m  c", "archive, delete (d in visual mode, or Delete), move, copy"}, {"b", "snooze: later today, tomorrow, Monday or a time"}, {"z", "undo the last move or delete"},
    {"Shift+M", "make a rule from the search"}, {"Ctrl+Shift+L  Ctrl+Shift+F", "rules, server filters"},
    {"Reader", NULL},
    {"Space  j  k", "scroll"}, {"o  Shift+P", "expand all, plain text or HTML"}, {"Shift+L", "light or dark mail page"},
    {"v", "a caret: h j k l w b 0 $ gg G move (k above the text: From, To, Cc, In); v again selects, y copies, Esc leaves"},
    {"Drag  Ctrl+C", "select text (body, subject, sender, To, Cc) and copy it; right click for a menu"}, {"Shift+A", "save all attachments"},
    {"Ctrl+click", "a link opens it"}, {"Ctrl+=  Ctrl+-  Ctrl+0", "message text larger, smaller, as set (this session)"},
    {"Click  ↓", "an attachment opens it; its download icon saves it"},
    {"Ctrl+S  Ctrl+P", "save the message as .eml in Downloads, print"},
    {"Folders", NULL},
    {"Space  a  Shift+R  Shift+D", "collapse, new, rename, delete"},
    {"Calendar", NULL},
    {"gc", "open the calendar"}, {"1  2  3  4", "day, week, month, agenda (also w m a)"},
    {"h  l  Shift+h/l  t", "the day before, after (agenda: a week); the period before, after; today"},
    {"j  k  Enter", "the next, previous event; its details (Enter again edits)"},
    {"n  e  d  r", "new event, edit, delete, sync now"}, {"/", "search events: title, place, notes"},
    {"Tab  Ctrl+S  Esc", "in an event: the next field, save, cancel"},
};

/* a line is shown when the filter is empty, or found in its keys, its description or its section's name */
static int help_shown(int i, int section, const char *filter)
{
    if (!filter || !filter[0]) return 1;
    return strcasestr(help_lines[section][0], filter) || strcasestr(help_lines[i][0], filter) ||
           (help_lines[i][1] && strcasestr(help_lines[i][1], filter));
}

static int help_rows(const char *filter)
{
    int n = (int)(sizeof help_lines / sizeof *help_lines), rows = 0;
    for (int i = 0, section = 0; i < n; i++) {
        if (!help_lines[i][1]) {
            section = i;
            int any = 0;
            for (int k = i + 1; k < n && help_lines[k][1] && !any; k++) any = help_shown(k, section, filter);
            rows += any;
        } else rows += help_shown(i, section, filter);
    }
    return rows;
}

/* the same sizes as the rest of the interface: 15 for text, 13 for section names */
enum { HELP_ROW = 26 };

float help_content(jd_app *app, float x, float y, float w, const char *filter)
{
    jd_ui *ui = &app->ui;
    int n = (int)(sizeof help_lines / sizeof *help_lines);
    /* the descriptions start in one column, after the widest keys */
    float kw = 0;
    for (int i = 0; i < n; i++)
        if (help_lines[i][1]) {
            float t = jd_text_width(ui, help_lines[i][0], 15, 1);
            if (t > kw) kw = t;
        }
    if (kw > w * 0.45f) kw = w * 0.45f;
    float dx = x + 12 + kw + 24;
    for (int i = 0, section = 0; i < n; i++) {
        if (!help_lines[i][1]) {
            section = i;
            int any = 0;
            for (int k = i + 1; k < n && help_lines[k][1] && !any; k++) any = help_shown(k, section, filter);
            if (any) jd_text(ui, help_lines[i][0], x, y + 8, 13, JD_MUTED, 1), y += HELP_ROW + 4;
            continue;
        }
        if (!help_shown(i, section, filter)) continue;
        jd_text_fit(ui, help_lines[i][0], x + 12, y + 4, kw, 15, JD_NORD8, 1);
        jd_text_fit(ui, help_lines[i][1], dx, y + 4, x + w - dx, 15, JD_NORD6, 0);
        y += HELP_ROW;
    }
    if (!help_rows(filter)) {
        jd_text(ui, "No key matches.", x, y + 4, 15, JD_MUTED, 0);
        y += HELP_ROW;
    }
    return y;
}

/* The ? overlay: what is typed narrows it; Esc or a click outside closes it, the typed filter going with it */
void help_draw(jd_app *app)
{
    static jd_field filter;
    static int shown;
    if (!app->help) {
        jd_field_set(&filter, NULL);
        shown = 0;
        return;
    }
    /* opened by a press this frame (the sidebar's Keys): that press is not a click outside */
    int opening = !shown;
    shown = 1;
    if (IsKeyPressed(KEY_ESCAPE)) {
        app->help = 0;
        return;
    }
    jd_field_input(&filter);
    float sw = jd_screen_w(), sh = jd_screen_h();
    fx_backdrop(ColorAlpha(BLACK, 0.45f));
    float w = sw - 80 < 820 ? sw - 80 : 820;
    float h = 0;
    {
        /* measured as drawn: sections and rows the filter keeps */
        int n = (int)(sizeof help_lines / sizeof *help_lines);
        for (int i = 0, section = 0; i < n; i++) {
            if (!help_lines[i][1]) {
                section = i;
                int any = 0;
                for (int k = i + 1; k < n && help_lines[k][1] && !any; k++) any = help_shown(k, section, jd_field_text(&filter));
                h += any ? HELP_ROW + 4 : 0;
            } else if (help_shown(i, section, jd_field_text(&filter))) h += HELP_ROW;
        }
        if (!help_rows(jd_field_text(&filter))) h += HELP_ROW;
    }
    float content = h;
    h += 124;
    if (h > sh - 40) h = sh - 40;
    Rectangle box = {(sw - w) / 2, (sh - h) / 2 > 20 ? (sh - h) / 2 : 20, w, h};
    if (!opening && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), box)) {
        app->help = 0;
        return;
    }
    fx_shadow(box, 0.03f, 1, 1);
    DrawRectangleRounded(box, 0.03f, 8, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.03f, 8, 1, JD_NORD2);
    jd_icon("keyboard", box.x + 24, box.y + 22, 20, JD_NORD8);
    jd_text(&app->ui, "Keyboard", box.x + 54, box.y + 22, 18, JD_NORD6, 1);
    filter.placeholder = "Type to find a key or an action";
    jd_field_draw(&app->ui, &filter, (Rectangle){box.x + 24, box.y + 56, w - 48, 34}, 1);
    /* taller than the window: the list scrolls under the search field */
    static float scroll;
    Rectangle list = {box.x, box.y + 100, w, h - 112};
    if (CheckCollisionPointRec(GetMousePosition(), list)) scroll -= jd_wheel_y(60);
    if (scroll > content - list.height) scroll = content - list.height;
    if (scroll < 0) scroll = 0;
    jd_scissor(list.x, list.y, list.width, list.height);
    help_content(app, box.x + 24, list.y + 4 - scroll, w - 48, jd_field_text(&filter));
    jd_scissor_end();
    jd_scrollbar(list, content + 8, &scroll);
}
