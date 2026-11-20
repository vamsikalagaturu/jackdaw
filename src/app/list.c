#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "ui/icons.h"

enum { HEADER_H = 28, UNDO_SECONDS = 5 };

/* the mouse is over the search hints, which lie on top of the list: what is under them takes no clicks */
static int under_hints;
/* where the search hints were drawn last frame (empty when hidden) */
static Rectangle hints_box;

static float row_h(jd_app *app)
{
    float k = (float)(app->prefs.list_text ? app->prefs.list_text : 15) / 15.0f;
    return (float)(int)((app->prefs.compact ? 40 : app->prefs.snippets ? 80 : 60) * k);
}

/* "folder" chips for a row, right-aligned at right; measure only returns their width */
static float folder_chips(jd_app *app, const char *folders, float right, float y, int measure, float k)
{
    if (!folders || !folders[0]) return 0;
    jd_ui *ui = &app->ui;
    char *copy = jd_strdup(folders), *save = NULL;
    char *names[8];
    int n = 0;
    for (char *f = strtok_r(copy, ",", &save); f && n < 8; f = strtok_r(NULL, ",", &save)) names[n++] = f;
    float total = 0, x = right;
    for (int i = n - 1; i >= 0; i--) {
        const char *leaf = strcasecmp(names[i], "INBOX") == 0 ? "Inbox" : names[i];
        for (int j = 0; j < app->nfolders; j++)
            if (!strcmp(app->folders[j].name, names[i])) leaf = folder_label(&app->folders[j]);
        float w = jd_text_width(ui, leaf, 11 * k, 1) + 30;
        x -= w;
        total += w + 4;
        if (!measure) {
            Rectangle chip = {x, y, w, 20 * k};
            DrawRectangleRounded(chip, 0.5f, 6, JD_NORD2);
            jd_icon(strcasecmp(names[i], "INBOX") == 0 ? "inbox" : "folder", x + 7, y + 4 * k, 12, JD_NORD8);
            jd_text(ui, leaf, x + 23, y + 4 * k, 11 * k, JD_NORD6, 1);
        }
        x -= 4;
    }
    free(copy);
    return total;
}

void format_date(int64_t t, char *out, int len)
{
    time_t when = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&when, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, (size_t)len, "%H:%M", &a);
    else if (now - when < 6 * 86400 && when < now) strftime(out, (size_t)len, "%a %H:%M", &a);
    /* a scheduled draft's time ahead */
    else if (when > now && when - now < 6 * 86400) strftime(out, (size_t)len, "%a %H:%M", &a);
    else if (when > now) strftime(out, (size_t)len, "%b %d %H:%M", &a);
    else if (a.tm_year == b.tm_year) strftime(out, (size_t)len, "%b %d", &a);
    else strftime(out, (size_t)len, "%Y-%m-%d", &a);
}

/* the heading a date falls under: Today, Yesterday, This week, then months */
static void date_group(int64_t t, char *out, int len)
{
    time_t when = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&when, &a);
    localtime_r(&now, &b);
    struct tm midnight = b;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    time_t today = mktime(&midnight);
    if (when >= today) snprintf(out, (size_t)len, "Today");
    else if (when >= today - 86400) snprintf(out, (size_t)len, "Yesterday");
    else if (when >= today - 6 * 86400) snprintf(out, (size_t)len, "This week");
    else if (a.tm_year == b.tm_year) strftime(out, (size_t)len, "%B", &a);
    else strftime(out, (size_t)len, "%B %Y", &a);
}

static int64_t current_folder(jd_app *app)
{
    return app->nfolders ? app->folders[app->folder].id : 0;
}

static int64_t row_key(jd_app *app, int i)
{
    return app->grouped ? app->rows[i].thread : app->rows[i].latest;
}

static int is_picked(jd_app *app, int64_t key)
{
    for (int i = 0; i < app->npicked; i++)
        if (app->picked[i] == key) return 1;
    return 0;
}

static void toggle_pick(jd_app *app, int i)
{
    int64_t key = row_key(app, i);
    for (int k = 0; k < app->npicked; k++)
        if (app->picked[k] == key) {
            app->picked[k] = app->picked[--app->npicked];
            return;
        }
    app->picked = jd_realloc(app->picked, sizeof *app->picked * (size_t)(app->npicked + 1));
    app->picked[app->npicked++] = key;
}

static void clear_picks(jd_app *app)
{
    free(app->picked);
    app->picked = NULL;
    app->npicked = 0;
}

static void pick_range(jd_app *app, int from, int to)
{
    if (from > to) {
        int t = from;
        from = to;
        to = t;
    }
    for (int i = from; i <= to && i < app->nrows; i++)
        if (!is_picked(app, row_key(app, i))) toggle_pick(app, i);
}

/* rows a pending (undoable) move or delete takes away are hidden until it is sent */
static int hidden(jd_app *app, const jd_thread_row *r)
{
    for (int i = 0; i < app->pending.n; i++) {
        if (app->pending.ids[i] == r->latest) return 1;
        if (app->grouped && app->view && jd_message_thread(app->view, app->pending.ids[i]) == r->thread) return 1;
    }
    return 0;
}

void list_reload(jd_app *app)
{
    if (!app->view) return;
    int64_t keep = app->nrows ? app->rows[app->row].latest : 0, keep_thread = app->nrows ? app->rows[app->row].thread : 0;
    jd_thread_rows_free(app->rows, app->nrows);
    app->rows = NULL;
    app->nrows = 0;

    jd_search parsed = {0};
    if (app->query) jd_search_parse(app->query, &parsed);
    jd_list_query q = parsed.q;
    q.grouped = app->grouped;
    q.folder = app->query && !app->search_here ? 0 : current_folder(app);
    if (parsed.folder_name) {
        q.folder = jd_store_folder_by_name(app->view, parsed.folder_name);
        if (!q.folder) q.folder = -1;
    }
    q.unread |= app->filter_unread;
    q.flagged |= app->filter_flagged;
    q.attachment |= app->filter_attachment;
    if (app->filter_from && !q.from) q.from = app->filter_from;
    if (app->query) {
        q.extra = app->server_hits;
        q.nextra = app->nserver_hits;
    }
    app->rows = jd_store_list(app->view, &q, &app->nrows);
    jd_search_free(&parsed);

    if (app->pending.n) {
        int k = 0;
        for (int i = 0; i < app->nrows; i++) {
            if (hidden(app, &app->rows[i])) {
                free(app->rows[i].subject), free(app->rows[i].senders), free(app->rows[i].snippet), free(app->rows[i].sender_addr);
                free(app->rows[i].folders);
                continue;
            }
            app->rows[k++] = app->rows[i];
        }
        app->nrows = k;
    }
    int was = app->row;
    if (app->row >= app->nrows) app->row = app->nrows ? app->nrows - 1 : 0;
    for (int i = 0; i < app->nrows; i++)
        if ((app->grouped && app->rows[i].thread == keep_thread) || (!app->grouped && app->rows[i].latest == keep))
            app->row = i;
    /* the same message pushed down by new mail is no move of the selection: no follow-scroll */
    if (app->followed_row == was) app->followed_row = app->row;
}

static int64_t *ids_for_row(jd_app *app, int i, int *n)
{
    const jd_thread_row *r = &app->rows[i];
    if (!app->grouped) {
        int64_t *one = jd_alloc(sizeof *one);
        one[0] = r->latest;
        *n = 1;
        return one;
    }
    return jd_store_thread_ids(app->view, r->thread, app->query ? 0 : current_folder(app), n);
}

/* the picked rows, else the current one */
int64_t *selection_ids(jd_app *app, int *n)
{
    *n = 0;
    if (!app->nrows) return NULL;
    if (!app->npicked) return ids_for_row(app, app->row, n);
    int64_t *all = NULL;
    for (int i = 0; i < app->nrows; i++) {
        if (!is_picked(app, row_key(app, i))) continue;
        int k;
        int64_t *ids = ids_for_row(app, i, &k);
        all = jd_realloc(all, sizeof *all * (size_t)(*n + k));
        memcpy(all + *n, ids, sizeof *ids * (size_t)k);
        *n += k;
        free(ids);
    }
    return all;
}

static int selected_rows(jd_app *app)
{
    return app->npicked ? app->npicked : app->nrows ? 1 : 0;
}

static const jd_folder_row *special(jd_app *app, const char *use)
{
    for (int i = 0; i < app->nfolders; i++)
        if (jd_streq(app->folders[i].special_use, use)) return &app->folders[i];
    return NULL;
}

void pending_tick(jd_app *app, int force)
{
    jd_pending *p = &app->pending;
    if (!p->ids || p->due < 0 || (!force && GetTime() < p->due)) return;
    if (app->sync) {
        if (p->to) jd_sync_move(app->sync, p->ids, p->n, p->to, 0);
        else jd_sync_destroy(app->sync, p->ids, p->n);
    }
    /* stays hidden until the store has the change (the next reload after JD_EV_CHANGED clears it) */
    p->due = -1;
}

static void pending_clear(jd_app *app)
{
    free(app->pending.ids), free(app->pending.to);
    app->pending = (jd_pending){0};
}

void pending_undo(jd_app *app)
{
    if (!app->pending.ids || app->pending.due < 0) return;
    pending_clear(app);
    list_reload(app);
    notify_user(app, "Undone.");
}

/* called after the store changed: a sent pending change is in it now */
void pending_settled(jd_app *app)
{
    if (app->pending.ids && app->pending.due < 0) pending_clear(app);
}

/* single: ids is one message of a conversation (a reader card), not the list's selection */
static void schedule(jd_app *app, int64_t *ids, int n, const char *to, const char *verb, int single)
{
    if (!n || !app->sync) {
        free(ids);
        return;
    }
    pending_tick(app, 1);
    pending_clear(app);
    int rows = single ? 1 : selected_rows(app);
    app->pending = (jd_pending){.ids = ids, .n = n, .to = jd_strdup(to), .due = GetTime() + UNDO_SECONDS};
    snprintf(app->pending.label, sizeof app->pending.label, "%s %d %s", verb, rows,
             app->grouped && !single ? (rows == 1 ? "conversation" : "conversations") : (rows == 1 ? "message" : "messages"));
    clear_picks(app);
    list_reload(app);
    conv_refresh(app, preview(app));
}

void list_refresh(jd_app *app)
{
    if (!app->sync) return;
    jd_sync_now(app->sync);
    notify_user(app, "Checking for new mail...");
}

int list_select_conv(jd_app *app, const jd_conv *c)
{
    for (int k = 0; k < app->nrows; k++)
        if (app->grouped ? app->rows[k].thread == c->thread : app->rows[k].latest == c->message) {
            clear_picks(app);
            app->row = k;
            return 1;
        }
    notify_user(app, "This message is not in the list any more: open it from the list to act on it.");
    return 0;
}

void act_move_ids(jd_app *app, int64_t *ids, int n, const char *folder, int copy, int single)
{
    if (copy) {
        if (app->sync && n) jd_sync_move(app->sync, ids, n, folder, 1);
        free(ids);
        notify_user(app, "Copied to %s.", folder);
        return;
    }
    char verb[200];
    snprintf(verb, sizeof verb, "Moved to %s:", folder);
    schedule(app, ids, n, folder, verb, single);
}

void act_move_to(jd_app *app, const char *folder, int copy)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    act_move_ids(app, ids, n, folder, copy, 0);
}

static void archive_ids(jd_app *app, int64_t *ids, int n, int single)
{
    const jd_folder_row *archive = special(app, "Archive");
    if (!archive) {
        free(ids);
        free(app->error);
        app->error = jd_strdup("The server has no Archive folder.");
        return;
    }
    schedule(app, ids, n, archive->name, "Archived", single);
}

/* to Trash; inside Trash (or without one) for good, after asking */
static void delete_ids(jd_app *app, int64_t *ids, int n, int64_t folder, int single)
{
    const jd_folder_row *trash = special(app, "Trash");
    if (!trash || folder == trash->id) {
        char *title = jd_printf("Delete %d message%s for good?", n, n == 1 ? "" : "s");
        dialog_open(app, DLG_CONFIRM, ACT_DESTROY, title, NULL);
        free(title);
        app->dialog.ids = ids;
        app->dialog.nids = n;
        return;
    }
    schedule(app, ids, n, trash->name, "Deleted", single);
}

void act_archive(jd_app *app)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    archive_ids(app, ids, n, 0);
}

void act_delete(jd_app *app)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    delete_ids(app, ids, n, app->query ? 0 : current_folder(app), 0);
}

static int64_t *one_id(int64_t id)
{
    int64_t *ids = jd_alloc(sizeof *ids);
    ids[0] = id;
    return ids;
}

void message_archive(jd_app *app, int64_t id) { archive_ids(app, one_id(id), 1, 1); }

/* ---- snooze: the messages' times menu, then out of sight until the time chosen ---- */

static jd_popup snooze_pop;
static int64_t *snooze_ids;
static int nsnooze;

static void snooze_open(jd_app *app, int64_t *ids, int n)
{
    free(snooze_ids);
    snooze_ids = ids, nsnooze = n;
    if (!n) return;
    jd_popup_open(&snooze_pop);
    /* under the mouse when that asked, else in the middle of the list */
    if (IsKeyPressed(KEY_B)) snooze_pop.at = (Vector2){jd_screen_w() / 2 - 140, jd_screen_h() / 3};
    (void)app;
}

void act_snooze(jd_app *app)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    snooze_open(app, ids, n);
}

void message_snooze(jd_app *app, int64_t id) { snooze_open(app, one_id(id), 1); }

static void snooze_until(jd_app *app, int64_t at)
{
    if (!app->sync || !nsnooze) return;
    jd_sync_snooze(app->sync, snooze_ids, nsnooze, at);
    char when[48];
    time_t t = (time_t)at;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(when, sizeof when, "%a %d %b, %H:%M", &tm);
    notify_user(app, "Snoozed until %s.", when);
    free(snooze_ids);
    snooze_ids = NULL, nsnooze = 0;
}

void snooze_typed(jd_app *app, const char *text)
{
    struct tm tm = {.tm_isdst = -1};
    if (sscanf(text, "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) {
        notify_user(app, "Not a time: \"%s\"; write it as 2026-11-20 08:00.", text);
        return;
    }
    tm.tm_year -= 1900, tm.tm_mon -= 1;
    int64_t at = (int64_t)mktime(&tm);
    if (at <= jd_now()) {
        notify_user(app, "Pick a time still to come.");
        return;
    }
    snooze_until(app, at);
}

void snooze_draw(jd_app *app)
{
    if (!snooze_pop.open) return;
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    struct tm evening = today, tomorrow = today, monday = today;
    evening.tm_hour = 18, evening.tm_min = evening.tm_sec = 0, evening.tm_isdst = -1;
    tomorrow.tm_mday += 1, tomorrow.tm_hour = 8, tomorrow.tm_min = tomorrow.tm_sec = 0, tomorrow.tm_isdst = -1;
    int to_monday = (8 - today.tm_wday) % 7;
    monday.tm_mday += to_monday ? to_monday : 7, monday.tm_hour = 8, monday.tm_min = monday.tm_sec = 0;
    monday.tm_isdst = -1;
    /* later today: this evening, or three hours on once the evening is near */
    int64_t later = mktime(&evening);
    if (later - now < 2 * 3600) later = (now + 3 * 3600) / 3600 * 3600;
    int64_t at[3] = {later, mktime(&tomorrow), mktime(&monday)};
    static const char *names[] = {"Later today", "Tomorrow morning", "Monday morning"};
    char labels[3][80];
    for (int i = 0; i < 3; i++) {
        char when[40];
        time_t t = (time_t)at[i];
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, i == 0 ? "%H:%M" : "%a %H:%M", &tm);
        snprintf(labels[i], sizeof labels[i], "%s (%s)", names[i], when);
    }
    jd_popup_item items[] = {{labels[0], "bell", NULL, 0}, {labels[1], "sun", NULL, 0}, {labels[2], "calendar", NULL, 0},
                             {"Pick a time...", "pencil", NULL, 0}};
    int i = jd_popup_draw(&app->ui, &snooze_pop, items, 4);
    if (i >= 0 && i < 3) snooze_until(app, at[i]);
    else if (i == 3) {
        char typed[32];
        strftime(typed, sizeof typed, "%Y-%m-%d %H:%M", &tomorrow);
        dialog_open(app, DLG_PROMPT, ACT_SNOOZE_AT, "Snooze until (YYYY-MM-DD HH:MM)", typed);
    }
}

int snooze_open_now(void)
{
    return snooze_pop.open;
}

void message_delete(jd_app *app, int64_t id, int64_t folder) { delete_ids(app, one_id(id), 1, folder, 1); }

/* the folder picker, the move or copy then for this one message (dialog.ids) */
void message_move_ask(jd_app *app, int64_t id, int copy)
{
    dialog_open(app, DLG_PICK_FOLDER, copy ? ACT_COPY : ACT_MOVE, copy ? "Copy to" : "Move to", NULL);
    app->dialog.ids = one_id(id);
    app->dialog.nids = 1;
}

static int selection_any(jd_app *app, int unread)
{
    for (int i = 0; i < app->nrows; i++) {
        if (app->npicked ? !is_picked(app, row_key(app, i)) : i != app->row) continue;
        if (unread ? app->rows[i].unread > 0 : app->rows[i].flagged > 0) return 1;
    }
    return 0;
}

void act_toggle_read(jd_app *app)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    if (app->sync && n) jd_sync_set_flag(app->sync, ids, n, JD_SEEN, selection_any(app, 1));
    free(ids);
}

void act_toggle_flag(jd_app *app)
{
    int n;
    int64_t *ids = selection_ids(app, &n);
    if (app->sync && n) jd_sync_set_flag(app->sync, ids, n, JD_FLAGGED, !selection_any(app, 0));
    free(ids);
}

static void set_query(jd_app *app);

static void run_search(jd_app *app)
{
    set_query(app);
    free(app->server_hits);
    app->server_hits = NULL;
    app->nserver_hits = 0;
    app->server_searching = 0;
    app->search_serial++;
    app->row = 0;
    app->list_scroll = 0;
    clear_picks(app);
    reader_close(app);
    list_reload(app);
}

static void server_search(jd_app *app)
{
    if (!app->query || !app->sync) return;
    jd_search parsed;
    jd_search_parse(app->query, &parsed);
    if (parsed.q.text) {
        app->server_searching = 1;
        jd_sync_search(app->sync, parsed.q.text, parsed.folder_name, app->search_serial);
    } else {
        free(app->error);
        app->error = jd_strdup("The server search needs words to look for, not only filters.");
    }
    jd_search_free(&parsed);
}

void mail_server_results(jd_app *app, int64_t serial, const int64_t *ids, int n)
{
    if (serial != app->search_serial) return;
    app->server_searching = 0;
    free(app->server_hits);
    app->server_hits = jd_alloc(sizeof *ids * (size_t)(n + 1));
    memcpy(app->server_hits, ids, sizeof *ids * (size_t)n);
    app->nserver_hits = n;
    list_reload(app);
}

static void toggle_filter(jd_app *app, int *filter)
{
    *filter = !*filter;
    list_reload(app);
}

static void filter_by_sender(jd_app *app)
{
    if (app->filter_from) {
        free(app->filter_from);
        app->filter_from = NULL;
    } else if (app->nrows && app->rows[app->row].sender_addr) {
        app->filter_from = jd_strdup(app->rows[app->row].sender_addr);
    }
    list_reload(app);
}

static void clear_filters(jd_app *app)
{
    app->filter_unread = app->filter_flagged = app->filter_attachment = 0;
    free(app->filter_from);
    app->filter_from = NULL;
    list_reload(app);
}

/* the newest message of what is open in the preview, else of the selected row */
static int64_t reply_source(jd_app *app)
{
    jd_conv *p = preview(app);
    if (app->pane == PANE_READER && p->n) return p->reading[p->n - 1].row.id;
    return app->nrows ? app->rows[app->row].latest : 0;
}

static int in_drafts(jd_app *app)
{
    return !app->query && app->nfolders && jd_streq(app->folders[app->folder].special_use, "Drafts");
}

/* a draft reopens for editing; anything else shows in the preview, or in its own tab */
static void open_row(jd_app *app, int in_tab)
{
    if (!app->nrows) return;
    if (in_drafts(app)) compose_open_draft(app, app->rows[app->row].latest);
    else reader_open(app, in_tab || app->prefs.reader_pos == 2);
}

static void move_row(jd_app *app, int to, int extend)
{
    if (!app->nrows) return;
    int from = app->row;
    app->row = to < 0 ? 0 : to >= app->nrows ? app->nrows - 1 : to;
    if (extend) {
        if (!app->npicked) app->anchor = from;
        clear_picks(app);
        pick_range(app, app->anchor, app->row);
    }
}

/* a new search starts in the folder on show (the scope pill); removing that pill widens it to every folder */
static void focus_search(jd_app *app)
{
    if (!app->searching && !app->query) app->search_here = 1;
    app->searching = 1;
    app->pane = PANE_LIST;
}

/* Pills: the operators of the search, each removable. The scope and the quick filters are shown as pills but
   kept in search_here and filter_*, which list_reload reads; the other operators are app->pills, part of the query. */
enum { P_SCOPE, P_UNREAD, P_FLAGGED, P_FILES, P_FROM, P_USER };

typedef struct {
    int kind, index;
    char text[300];
} pill;

static const char *folder_name_now(jd_app *app)
{
    return app->nfolders ? folder_label(&app->folders[app->folder]) : "this folder";
}

static int pills_shown(jd_app *app, pill *out, int max)
{
    int n = 0;
#define ADD(k, i, ...)                                                                                                 \
    do {                                                                                                               \
        if (n < max) {                                                                                                 \
            out[n] = (pill){.kind = k, .index = i};                                                                    \
            snprintf(out[n].text, sizeof out[n].text, __VA_ARGS__);                                                   \
            n++;                                                                                                       \
        }                                                                                                              \
    } while (0)
    if ((app->searching || app->query) && app->search_here) {
        const char *f = folder_name_now(app);
        ADD(P_SCOPE, 0, strchr(f, ' ') ? "in:\"%s\"" : "in:%s", f);
    }
    if (app->filter_unread) ADD(P_UNREAD, 0, "is:unread");
    if (app->filter_flagged) ADD(P_FLAGGED, 0, "is:flagged");
    if (app->filter_attachment) ADD(P_FILES, 0, "has:attachment");
    if (app->filter_from) ADD(P_FROM, 0, "from:%s", app->filter_from);
    for (int i = 0; i < app->npills; i++) ADD(P_USER, i, "%s", app->pills[i]);
#undef ADD
    return n;
}

/* the query list_reload parses: the typed pills, then the typed words */
static void set_query(jd_app *app)
{
    jd_buf b = {0};
    for (int i = 0; i < app->npills; i++) jd_buf_addf(&b, "%s%s", b.len ? " " : "", app->pills[i]);
    if (app->search.len) jd_buf_addf(&b, "%s%s", b.len ? " " : "", jd_field_text(&app->search));
    /* without the scope pill even quick filters alone search every folder */
    if (!b.s && !app->search_here) b.s = jd_strdup("");
    free(app->query);
    app->query = b.s;
}

char *search_full_query(jd_app *app, int with_scope)
{
    pill p[32];
    int n = pills_shown(app, p, 32);
    jd_buf b = {0};
    for (int i = 0; i < n; i++)
        if (p[i].kind == P_SCOPE && !with_scope) continue;
        else if (p[i].kind != P_SCOPE || !app->nfolders) jd_buf_addf(&b, "%s%s", b.len ? " " : "", p[i].text);
        else {
            const char *name = app->folders[app->folder].name;
            jd_buf_addf(&b, strchr(name, ' ') ? "%sin:\"%s\"" : "%sin:%s", b.len ? " " : "", name);
        }
    if (app->search.len) jd_buf_addf(&b, "%s%s", b.len ? " " : "", jd_field_text(&app->search));
    return b.s;
}

static void pill_push(jd_app *app, const char *text)
{
    app->pills = jd_realloc(app->pills, sizeof *app->pills * (size_t)(app->npills + 1));
    app->pills[app->npills++] = jd_strdup(text);
}

static void pill_drop(jd_app *app, int i)
{
    free(app->pills[i]);
    memmove(app->pills + i, app->pills + i + 1, sizeof *app->pills * (size_t)(app->npills - i - 1));
    app->npills--;
}

static void pills_clear(jd_app *app)
{
    for (int i = 0; i < app->npills; i++) free(app->pills[i]);
    free(app->pills);
    app->pills = NULL;
    app->npills = 0;
}

/* a typed "key:value" made into a pill; 0 when it is not an operator, and stays a word */
static int pill_from_token(jd_app *app, const char *tok)
{
    static const char *keys[] = {"from", "to", "subject", "in", "is", "has", "after", "before"};
    const char *colon = strchr(tok, ':');
    if (!colon || colon == tok) return 0;
    int known = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys && !known; i++)
        known = strlen(keys[i]) == (size_t)(colon - tok) && !strncasecmp(tok, keys[i], (size_t)(colon - tok));
    if (!known) return 0;
    char value[300];
    snprintf(value, sizeof value, "%s", colon + 1);
    size_t vl = strlen(value);
    if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') {
        vl -= 2;
        memmove(value, value + 1, vl);
        value[vl] = 0;
    }
    if (!vl || strchr(value, '"')) return 0;
    if (!strncasecmp(tok, "is:", 3) && !strcasecmp(value, "unread")) app->filter_unread = 1;
    else if (!strncasecmp(tok, "is:", 3) && !strcasecmp(value, "flagged")) app->filter_flagged = 1;
    else if (!strncasecmp(tok, "is:", 3) && strcasecmp(value, "read")) return 0;
    else if (!strncasecmp(tok, "has:", 4) && !strncasecmp(value, "attachment", 10)) app->filter_attachment = 1;
    else if (!strncasecmp(tok, "has:", 4)) return 0;
    else if (!strncasecmp(tok, "in:", 3) && app->nfolders &&
             (!strcasecmp(value, folder_name_now(app)) || !strcasecmp(value, app->folders[app->folder].name)))
        app->search_here = 1;
    else {
        if (!strncasecmp(tok, "in:", 3)) app->search_here = 0;
        pill_push(app, tok);
    }
    return 1;
}

/* Operators typed into the field become pills once complete: followed by a space, or every one when all is set
   (Enter). The words around them stay in the field. */
static int absorb(jd_app *app, int all)
{
    const char *t = jd_field_text(&app->search);
    jd_buf keep = {0};
    int took = 0, i = 0, len = app->search.len;
    while (i < len) {
        while (i < len && t[i] == ' ') i++;
        int start = i, quoted = 0;
        while (i < len && (quoted || t[i] != ' ')) {
            if (t[i] == '"') quoted = !quoted;
            i++;
        }
        if (i == start) break;
        int complete = (i < len && !quoted) || all;
        char tok[300];
        snprintf(tok, sizeof tok, "%.*s", i - start, t + start);
        if (complete && pill_from_token(app, tok)) {
            took = 1;
            continue;
        }
        jd_buf_addf(&keep, "%s%s", keep.len ? " " : "", tok);
        if (i == len && !complete) break;
    }
    if (took) {
        if (!all && len && t[len - 1] == ' ' && keep.len) jd_buf_adds(&keep, " ");
        jd_field_set(&app->search, keep.s);
    }
    jd_buf_free(&keep);
    return took;
}

static void pill_remove(jd_app *app, const pill *p, int edit)
{
    switch (p->kind) {
    case P_SCOPE: app->search_here = 0; break;
    case P_UNREAD: app->filter_unread = 0; break;
    case P_FLAGGED: app->filter_flagged = 0; break;
    case P_FILES: app->filter_attachment = 0; break;
    case P_FROM:
        free(app->filter_from);
        app->filter_from = NULL;
        break;
    case P_USER: {
        /* Backspace on a pill puts it back into the field, to change */
        if (edit) {
            char *back = jd_printf("%s%s%s", app->pills[p->index], app->search.len ? " " : "", jd_field_text(&app->search));
            jd_field_set(&app->search, back);
            free(back);
            app->search.cursor = (int)strlen(p->text);
        }
        pill_drop(app, p->index);
        break;
    }
    }
    if (app->query || p->kind == P_USER) {
        set_query(app);
        app->row = 0;
    }
    list_reload(app);
}

static void search_clear(jd_app *app)
{
    jd_field_set(&app->search, NULL);
    pills_clear(app);
    app->filter_unread = app->filter_flagged = app->filter_attachment = 0;
    free(app->filter_from);
    app->filter_from = NULL;
    app->searching = 0;
    app->search_here = 1;
    run_search(app);
}

static int hint_keys(jd_app *app);

static int search_keys(jd_app *app)
{
    if (hint_keys(app)) return 1;
    if (IsKeyPressed(KEY_ESCAPE)) {
        app->searching = 0;
        if (!app->query) {
            jd_field_set(&app->search, NULL);
            app->search_here = 1;
        }
        return 1;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        absorb(app, 1);
        app->searching = 0;
        run_search(app);
        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) server_search(app);
        return 1;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        app->searching = 0;
        return 1;
    }
    if (IsKeyPressed(KEY_BACKSPACE) && app->search.cursor == 0 && !app->search.selecting) {
        pill p[32];
        int n = pills_shown(app, p, 32);
        if (n) pill_remove(app, &p[n - 1], 1);
        return 1;
    }
    int before = app->search.len;
    jd_field_input(&app->search);
    if (app->search.len != before && app->search.cursor == app->search.len && absorb(app, 0)) {
        if (app->query) set_query(app);
        list_reload(app);
    }
    return 1;
}

/* g then g: the first row; g then a letter: a folder by its role. Ahead of every pane's own keys, which use
   the same letters (a, j, d in the folders, j in the reader). 1 when the key was used. */
int go_keys(jd_app *app)
{
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (app->pending_g && jd_key_went_down()) {
        app->pending_g = 0;
        if (IsKeyPressed(KEY_G) && !shift) {
            if (app->nrows) move_row(app, 0, 0);
            return 1;
        }
        if (IsKeyPressed(KEY_C)) {
            calendar_open(app);
            return 1;
        }
        static const struct {
            int key;
            const char *use;
        } go[] = {{KEY_I, "INBOX"}, {KEY_S, "Sent"}, {KEY_D, "Drafts"}, {KEY_A, "Archive"}, {KEY_T, "Trash"}, {KEY_J, "Junk"}};
        for (size_t k = 0; k < sizeof go / sizeof *go; k++)
            if (IsKeyPressed(go[k].key)) {
                if (!folder_go(app, go[k].use)) notify_user(app, "No %s folder on this account.", go[k].use);
                return 1;
            }
    }
    if (IsKeyPressed(KEY_G) && !shift && !ctrl) {
        app->pending_g = 1;
        return 1;
    }
    return 0;
}

int list_keys(jd_app *app)
{
    if (app->searching) return search_keys(app);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (app->pending_f && jd_key_went_down()) {
        app->pending_f = 0;
        if (IsKeyPressed(KEY_U)) toggle_filter(app, &app->filter_unread);
        else if (IsKeyPressed(KEY_F)) toggle_filter(app, &app->filter_flagged);
        else if (IsKeyPressed(KEY_A)) toggle_filter(app, &app->filter_attachment);
        else if (IsKeyPressed(KEY_S)) filter_by_sender(app);
        else if (IsKeyPressed(KEY_C)) clear_filters(app);
        return 1;
    }
    if ((IsKeyPressed(KEY_SLASH) && !shift) || (ctrl && IsKeyPressed(KEY_K))) {
        focus_search(app);
        GetCharPressed();
        return 1;
    }
    if (IsKeyPressed(KEY_Z) && !shift && app->pending.ids && app->pending.due >= 0) {
        pending_undo(app);
        return 1;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        app->visual = 0;
        if (app->npicked) clear_picks(app);
        else if (app->query) {
            jd_field_set(&app->search, NULL);
            run_search(app);
        }
        return 1;
    }
    if (IsKeyPressed(KEY_N) && ctrl) {
        compose_open(app, COMPOSE_NEW, 0);
        return 1;
    }
    if (IsKeyPressed(KEY_F) && shift && ctrl) {
        filters_open(app);
        return 1;
    }
    if (IsKeyPressed(KEY_L) && shift && ctrl) {
        dialog_open(app, DLG_RULES, ACT_NONE, "Rules", NULL);
        return 1;
    }
    if (IsKeyPressed(KEY_F) && !shift && !ctrl) {
        app->pending_f = 1;
        return 1;
    }
    if (IsKeyPressed(KEY_T) && !shift && !ctrl) {
        app->grouped = !app->grouped;
        clear_picks(app);
        save_prefs(app);
        reader_close(app);
        list_reload(app);
        return 1;
    }
    if (IsKeyPressed(KEY_S) && shift && !ctrl) {
        server_search(app);
        return 1;
    }
    if (IsKeyPressed(KEY_M) && shift) {
        char *match = search_full_query(app, 0);
        if (match && match[0]) {
            char *title = jd_printf("Rule for new mail matching \"%s\": read, flag, delete, move:<folder> or copy:<folder>",
                                    match);
            dialog_open(app, DLG_PROMPT, ACT_ADD_RULE, title, "move:");
            free(title);
        }
        free(match);
        return 1;
    }
    if (!app->nrows) return 0;
    /* delete is dd, as in vim, so one stray d does nothing; in visual mode one d deletes the selection */
    if (app->pending_d && jd_key_went_down()) {
        app->pending_d = 0;
        if (IsKeyPressed(KEY_D) && !shift) {
            act_delete(app);
            return 1;
        }
    } else if (IsKeyPressed(KEY_D) && !shift && !ctrl && !app->visual) {
        app->pending_d = 1;
        return 1;
    }
    int down = IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_J) || IsKeyPressedRepeat(KEY_DOWN);
    int up = IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_K) || IsKeyPressedRepeat(KEY_UP);
    /* visual mode: v starts at the current row, moving extends from there, an action or Esc ends it */
    if (IsKeyPressed(KEY_V) && !ctrl && app->pane != PANE_READER) {
        app->visual = !app->visual;
        clear_picks(app);
        if (app->visual) {
            app->anchor = app->row;
            toggle_pick(app, app->row);
        }
        return 1;
    }
    int extend = shift || app->visual;
    int moved = 1;
    if (IsKeyPressed(KEY_G) && shift) move_row(app, app->nrows - 1, app->visual);
    else if (down) move_row(app, app->row + 1, extend);
    else if (up) move_row(app, app->row - 1, extend);
    else moved = 0;
    if (moved) return 1;
    int acting = IsKeyPressed(KEY_E) || IsKeyPressed(KEY_D) || IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_M) ||
                 IsKeyPressed(KEY_C) || IsKeyPressed(KEY_U) || IsKeyPressed(KEY_S);
    if (acting) app->visual = 0;
    if (IsKeyPressed(KEY_A) && ctrl) pick_range(app, 0, app->nrows - 1);
    else if (IsKeyPressed(KEY_X) && !ctrl) toggle_pick(app, app->row);
    else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_L) || IsKeyPressed(KEY_RIGHT)) open_row(app, shift);
    else if (IsKeyPressed(KEY_R) && !ctrl) compose_open(app, shift ? COMPOSE_REPLY_ALL : COMPOSE_REPLY, reply_source(app));
    else if (IsKeyPressed(KEY_F) && shift) compose_open(app, COMPOSE_FORWARD, reply_source(app));
    else if (IsKeyPressed(KEY_U) && !shift) act_toggle_read(app);
    else if (IsKeyPressed(KEY_S) && !shift) act_toggle_flag(app);
    else if (IsKeyPressed(KEY_E) && !shift) act_archive(app);
    else if (IsKeyPressed(KEY_B) && !shift && !ctrl) act_snooze(app);
    else if ((IsKeyPressed(KEY_D) && !shift) || IsKeyPressed(KEY_DELETE)) act_delete(app);
    else if (IsKeyPressed(KEY_M) && !shift) dialog_open(app, DLG_PICK_FOLDER, ACT_MOVE, "Move to", NULL);
    else if (IsKeyPressed(KEY_C) && !shift && !ctrl) dialog_open(app, DLG_PICK_FOLDER, ACT_COPY, "Copy to", NULL);
    else return 0;
    return 1;
}

/* a filter chip that toggles on click; returns 1 when clicked */
static int chip(jd_app *app, const char *label, const char *icon, float *x, float y, int on)
{
    jd_ui *ui = &app->ui;
    float w = jd_text_width(ui, label, 13, 1) + (icon ? 40 : 22);
    Rectangle r = {*x, y, w, 26};
    int hover = CheckCollisionPointRec(GetMousePosition(), r) && !app->dialog.kind && !app->menu.open && !under_hints;
    DrawRectangleRounded(r, 0.5f, 8, on ? JD_NORD10 : hover ? JD_NORD2 : JD_NORD1);
    if (!on) DrawRectangleRoundedLinesEx(r, 0.5f, 8, 1, JD_NORD2);
    float tx = *x + 11;
    if (icon) {
        jd_icon(icon, tx, y + 6, 14, on ? JD_ON_ACCENT : JD_NORD4);
        tx += 20;
    }
    jd_text(ui, label, tx, y + 6, 13, on ? JD_ON_ACCENT : JD_NORD4, 1);
    *x += w + 6;
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static Color avatar_color(const char *key)
{
    static const unsigned hues[] = {0x5e81ac, 0x81a1c1, 0x8fbcbb, 0xa3be8c, 0xb48ead, 0xd08770, 0xbf616a, 0xebcb8b};
    unsigned h = 2166136261u;
    for (const char *c = key ? key : ""; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    unsigned v = hues[h % 8];
    return (Color){v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff, 0xff};
}

/* the first letter of the first and the last word, as UTF-8 */
static void initials(const char *name, char *out)
{
    out[0] = 0;
    const char *first = name, *last = NULL;
    while (*first == ' ' || *first == '"') first++;
    for (const char *p = first; *p && *p != ',' && *p != '<'; p++)
        if (*p == ' ' && p[1] && p[1] != ' ' && p[1] != '<') last = p + 1;
    int n = 0;
    const char *picks[2] = {first, last};
    for (int k = 0; k < 2; k++) {
        const char *s = picks[k];
        if (!s || !*s) continue;
        int len = 1;
        while (s[len] && ((unsigned char)s[len] & 0xc0) == 0x80) len++;
        memcpy(out + n, s, (size_t)len);
        n += len;
    }
    out[n] = 0;
    if (n && (unsigned char)out[0] < 0x80) out[0] = (char)toupper((unsigned char)out[0]);
}

void sender_avatar(jd_app *app, const char *name, const char *addr, float cx, float cy, float r)
{
    DrawCircleV((Vector2){cx, cy}, r, avatar_color(addr));
    char in[12];
    initials(name, in);
    float w = jd_text_width(&app->ui, in, r * 0.8f, 1);
    jd_text(&app->ui, in, cx - w / 2, cy - r * 0.42f, r * 0.8f, (Color){0xff, 0xff, 0xff, 0xff}, 1);
}

static void avatar(jd_app *app, const jd_thread_row *t, float cx, float cy, float r, int picked, int hover)
{
    if (picked || hover) {
        jd_icon(picked ? "square-check" : "square", cx - 10, cy - 10, 20, picked ? JD_NORD8 : JD_NORD4);
        return;
    }
    sender_avatar(app, t->senders, t->sender_addr, cx, cy, r);
    /* unread: a badge on the avatar's shoulder, ringed in the background so it reads on any avatar colour */
    if (t->unread) {
        Vector2 at = {cx + r * 0.72f, cy - r * 0.72f};
        DrawCircleV(at, r * 0.36f + 2, JD_NORD0);
        DrawCircleV(at, r * 0.36f, JD_NORD8);
    }
}

/* the actions shown on a hovered row; returns 1 when one was clicked */
static int hover_actions(jd_app *app, int i, float right, float y)
{
    const jd_thread_row *t = &app->rows[i];
    struct {
        const char *icon;
        int what;
    } acts[] = {{"archive", 0}, {"trash-2", 1}, {t->unread ? "mail-open" : "mail", 2}, {"star", 3}};
    float x = right - 4 * 30;
    DrawRectangleRounded((Rectangle){x - 6, y - 4, 4 * 30 + 8, 30}, 0.4f, 6, JD_NORD3);
    for (int k = 0; k < 4; k++, x += 30) {
        Rectangle b = {x, y - 2, 26, 26};
        int hover = CheckCollisionPointRec(GetMousePosition(), b);
        jd_icon(acts[k].icon, x + 5, y + 3, 16, hover ? JD_NORD8 : (k == 3 && t->flagged ? JD_NORD13 : JD_NORD6));
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int keep = app->row;
            app->row = i;
            clear_picks(app);
            if (acts[k].what == 0) act_archive(app);
            else if (acts[k].what == 1) act_delete(app);
            else if (acts[k].what == 2) act_toggle_read(app);
            else act_toggle_flag(app);
            if (acts[k].what >= 2) app->row = keep;
            return 1;
        }
    }
    return 0;
}

/* each kind of filter has its icon and colour, in the pills and the hints alike: where (folder), who (people),
   what (subject), status, files, when (dates) */
void filter_kind(const char *text, const char **icon, Color *tint)
{
    /* not static: the colours follow the theme */
    const struct {
        const char *key, *icon;
        Color colour;
    } kinds[] = {{"in:", "folder", JD_NORD8},        {"from:", "user", JD_GREEN},      {"to:", "users", JD_GREEN},
                 {"subject:", "type", JD_PURPLE},    {"is:", "mail-open", JD_NORD13},  {"has:", "paperclip", JD_NORD9},
                 {"after:", "calendar", JD_ORANGE},  {"before:", "calendar", JD_ORANGE}};
    *icon = "search";
    *tint = JD_NORD4;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++)
        if (!strncasecmp(text, kinds[k].key, strlen(kinds[k].key))) *icon = kinds[k].icon, *tint = kinds[k].colour;
    if (!strncasecmp(text, "is:flagged", 10)) *icon = "star";
}

/* The search box: its pills (each with an x), then the field for words and operators being typed. A complete
   operator becomes a pill; Backspace at the start of the field takes the last pill back. */
static void draw_search(jd_app *app, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    Rectangle box = {x, y, w, 34};
    Vector2 mouse = GetMousePosition();
    int free_mouse = !app->dialog.kind && !app->menu.open;
    /* a press anywhere else leaves the field, as Esc does (the hints over the list are part of it) */
    if (app->searching && free_mouse && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, box) &&
        !CheckCollisionPointRec(mouse, hints_box)) {
        app->searching = 0;
        if (!app->query) {
            jd_field_set(&app->search, NULL);
            app->search_here = 1;
        }
    }
    DrawRectangleRounded(box, 0.2f, 6, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.2f, 6, 1.5f, app->searching ? JD_NORD8 : JD_NORD2);
    pill p[32];
    int n = pills_shown(app, p, 32);
    int any = n || app->search.len || app->query;
    float end = box.x + box.width - 32, px = box.x + 6;
    int removed = -1, on_pill = 0;
    jd_scissor(box.x, box.y, box.width - 32, box.height);
    for (int i = 0; i < n; i++) {
        const char *colon = strchr(p[i].text, ':');
        int klen = colon ? (int)(colon - p[i].text) + 1 : 0;
        char key[32];
        snprintf(key, sizeof key, "%.*s", klen < 31 ? klen : 31, p[i].text);
        float kw = jd_text_width(ui, key, 13, 0), vw = jd_text_width(ui, p[i].text + klen, 13, 1);
        if (vw > 220) vw = 220;
        const char *icon;
        Color tint;
        filter_kind(p[i].text, &icon, &tint);
        Rectangle chip = {px, y + 5, kw + vw + 54, 24};
        if (chip.x + chip.width > end - 60) {
            char more[16];
            snprintf(more, sizeof more, "+%d", n - i);
            jd_text(ui, more, px + 2, y + 10, 13, JD_MUTED, 1);
            px += jd_text_width(ui, more, 13, 1) + 8;
            break;
        }
        int hover = free_mouse && CheckCollisionPointRec(mouse, chip);
        on_pill |= hover;
        DrawRectangleRounded(chip, 0.45f, 6, ColorAlpha(tint, hover ? 0.38f : 0.24f));
        jd_icon(icon, chip.x + 8, y + 10, 14, tint);
        jd_text(ui, key, chip.x + 28, y + 10, 13, JD_NORD4, 0);
        jd_text_fit(ui, p[i].text + klen, chip.x + 28 + kw, y + 10, vw, 13, JD_NORD6, 1);
        Rectangle xb = {chip.x + chip.width - 22, y + 9, 16, 16};
        int on_x = free_mouse && CheckCollisionPointRec(mouse, xb);
        jd_icon("x", xb.x + 2, xb.y + 2, 12, on_x ? JD_NORD11 : hover ? JD_NORD4 : JD_MUTED);
        if (on_x && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) removed = i;
        px += chip.width + 6;
    }
    jd_scissor_end();

    static char placeholder[160];
    snprintf(placeholder, sizeof placeholder, "Search %s   (/)", folder_name_now(app));
    app->search.placeholder = n ? (app->searching ? "Add words or from: to: subject: after: …" : NULL)
                                : app->searching ? "Search  (from: to: subject: is: has: after:DD-MM-YYYY)"
                                                 : placeholder;
    app->search.bare = 1;
    int clicked = jd_field_draw(ui, &app->search, (Rectangle){px, y, end - px, 34}, app->searching);
    clicked |= free_mouse && !on_pill && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, box);

    Rectangle clear = {box.x + box.width - 28, y + 6, 22, 22};
    int on_clear = free_mouse && CheckCollisionPointRec(mouse, clear);
    if (any) jd_icon("x", clear.x + 3, clear.y + 3, 16, on_clear ? JD_NORD11 : JD_MUTED);
    else jd_icon("search", clear.x + 2, clear.y + 3, 16, JD_MUTED);
    if (any && on_clear && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        search_clear(app);
        return;
    }
    if (removed >= 0) {
        pill_remove(app, &p[removed], 0);
        return;
    }
    if (clicked && !on_clear) focus_search(app);
}

/* Hints under the search field while typing: the operators that fit the word being typed, then values for the
   operator (senders and recipients from the contacts, folders, flags, dates). */
typedef jd_hint hint;

static hint hints[10];
static int nhints, hint_sel = -1;
static char hints_for[300] = "\x01";

/* the word being typed: from the last space outside quotes to the end, NULL unless the cursor is at the end */
static const char *typed_word(jd_app *app)
{
    if (app->search.cursor != app->search.len) return NULL;
    const char *t = jd_field_text(&app->search), *word = t;
    int quoted = 0;
    for (const char *p = t; *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == ' ' && !quoted) word = p + 1;
    }
    return word;
}

static void hint_add(const char *insert, const char *label, const char *note)
{
    if (nhints >= (int)(sizeof hints / sizeof *hints)) return;
    hint *h = &hints[nhints++];
    snprintf(h->insert, sizeof h->insert, "%s", insert);
    snprintf(h->label, sizeof h->label, "%s", label);
    snprintf(h->note, sizeof h->note, "%s", note ? note : "");
}

static void date_hint(const char *key, int days, const char *note)
{
    time_t t = time(NULL) - (time_t)days * 86400;
    struct tm tm;
    localtime_r(&t, &tm);
    char d[16], ins[48];
    strftime(d, sizeof d, "%d-%m-%Y", &tm);
    snprintf(ins, sizeof ins, "%s:%s ", key, d);
    hint_add(ins, ins, note);
}

static void hints_build(jd_app *app, const char *word)
{
    nhints = 0;
    hint_sel = -1;
    const char *colon = strchr(word, ':');
    if (!colon) {
        static const struct {
            const char *insert, *note;
        } ops[] = {{"from:", "sender name or address"}, {"to:", "recipient"}, {"subject:", "words in the subject"},
                   {"in:", "a folder"}, {"is:unread ", "unread mail"}, {"is:flagged ", "flagged mail"},
                   {"has:attachment ", "mail with files"}, {"after:", "on or after DD-MM-YYYY"},
                   {"before:", "before DD-MM-YYYY"}, {"is:read ", "read mail"}};
        for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
            if (!strncasecmp(ops[i].insert, word, strlen(word))) hint_add(ops[i].insert, ops[i].insert, ops[i].note);
        return;
    }
    char key[16];
    snprintf(key, sizeof key, "%.*s", (int)(colon - word) < 15 ? (int)(colon - word) : 15, word);
    const char *v = colon + 1;
    if (*v == '"') v++;
    size_t vl = strlen(v);
    if (!strcasecmp(key, "from") || !strcasecmp(key, "to")) {
        if (!app->view) return;
        int n;
        jd_contact *c = jd_store_contacts(app->view, v, 6, &n);
        for (int i = 0; i < n; i++) {
            char ins[300];
            snprintf(ins, sizeof ins, "%s:%s ", key, c[i].addr);
            hint_add(ins, ins, c[i].name);
        }
        jd_contacts_free(c, n);
    } else if (!strcasecmp(key, "in")) {
        for (int i = 0; i < app->nfolders; i++) {
            const char *name = app->folders[i].name, *label = folder_label(&app->folders[i]);
            if (strncasecmp(label, v, vl) && strncasecmp(name, v, vl)) continue;
            char ins[300];
            snprintf(ins, sizeof ins, strchr(label, ' ') ? "in:\"%s\" " : "in:%s ", label);
            hint_add(ins, ins, NULL);
        }
    } else if (!strcasecmp(key, "is")) {
        static const char *flags[] = {"unread", "read", "flagged"};
        for (int i = 0; i < 3; i++)
            if (!strncasecmp(flags[i], v, vl)) {
                char ins[32];
                snprintf(ins, sizeof ins, "is:%s ", flags[i]);
                hint_add(ins, ins, NULL);
            }
    } else if (!strcasecmp(key, "has")) {
        if (!strncasecmp("attachment", v, vl)) hint_add("has:attachment ", "has:attachment ", NULL);
    } else if ((!strcasecmp(key, "after") || !strcasecmp(key, "before")) && !vl) {
        date_hint(key, 0, "today");
        date_hint(key, 7, "a week ago");
        date_hint(key, 30, "30 days ago");
        date_hint(key, 365, "a year ago");
    }
}

int query_hints(jd_app *app, const char *word, jd_hint *out, int max)
{
    hints_build(app, word);
    int n = nhints < max ? nhints : max;
    memcpy(out, hints, sizeof *out * (size_t)n);
    /* the search field's own hints are built again when it next shows them */
    nhints = 0;
    snprintf(hints_for, sizeof hints_for, "\x01");
    return n;
}

static void hints_update(jd_app *app)
{
    const char *word = app->searching ? typed_word(app) : NULL;
    char now[300];
    snprintf(now, sizeof now, "%s", word ? word : "\x01");
    if (!strcmp(now, hints_for)) return;
    snprintf(hints_for, sizeof hints_for, "%s", now);
    nhints = 0;
    hint_sel = -1;
    if (word) hints_build(app, word);
}

/* the chosen hint in place of the word being typed; a complete operator becomes a pill */
static void hint_accept(jd_app *app, int i)
{
    const char *word = typed_word(app);
    if (!word || i < 0 || i >= nhints) return;
    char *text = jd_printf("%.*s%s", (int)(word - jd_field_text(&app->search)), jd_field_text(&app->search), hints[i].insert);
    jd_field_set(&app->search, text);
    free(text);
    if (absorb(app, 0)) {
        if (app->query) set_query(app);
        list_reload(app);
    }
    snprintf(hints_for, sizeof hints_for, "\x01");
}

/* Up/Down pick a hint, Tab or Enter (on a picked one) takes it; 1 when the key was used */
static int hint_keys(jd_app *app)
{
    hints_update(app);
    if (!nhints) return 0;
    if (IsKeyPressed(KEY_DOWN)) hint_sel = (hint_sel + 1) % nhints;
    else if (IsKeyPressed(KEY_UP)) hint_sel = hint_sel <= 0 ? nhints - 1 : hint_sel - 1;
    else if (IsKeyPressed(KEY_TAB)) hint_accept(app, hint_sel < 0 ? 0 : hint_sel);
    else if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && hint_sel >= 0) hint_accept(app, hint_sel);
    else return 0;
    return 1;
}

static void hints_draw(jd_app *app, float x, float y, float w)
{
    hints_update(app);
    hints_box = (Rectangle){0};
    if (!nhints) return;
    jd_ui *ui = &app->ui;
    float bw = w < 460 ? w : 460;
    /* with a keyboard, a last line inside the box says how to take a hint */
    hints_box = (Rectangle){x, y, bw, (float)nhints * 30 + 8 + (plat_touch() ? 0 : 22)};
    DrawRectangleRounded((Rectangle){x + 2, y + 3, bw, hints_box.height}, 0.06f, 6, ColorAlpha(BLACK, 0.25f));
    DrawRectangleRounded(hints_box, 0.06f, 6, JD_NORD2);
    /* the notes start in one column, after the widest label */
    float lw = 0;
    for (int i = 0; i < nhints; i++) {
        float t = jd_text_width(ui, hints[i].label, 14, 1);
        if (t > lw) lw = t;
    }
    if (lw > bw * 0.6f - 40) lw = bw * 0.6f - 40;
    for (int i = 0; i < nhints; i++) {
        Rectangle row = {x + 4, y + 4 + (float)i * 30, bw - 8, 28};
        int hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (i == hint_sel || hover) DrawRectangleRounded(row, 0.25f, 6, JD_NORD3);
        const char *icon;
        Color tint;
        filter_kind(hints[i].label, &icon, &tint);
        jd_icon(icon, row.x + 10, row.y + 7, 14, tint);
        jd_text_fit(ui, hints[i].label, row.x + 32, row.y + 7, lw, 14, JD_NORD6, 1);
        if (hints[i].note[0]) jd_text_fit(ui, hints[i].note, row.x + lw + 48, row.y + 7, row.width - lw - 58, 13, JD_MUTED, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            hint_accept(app, i);
            break;
        }
    }
    if (!plat_touch()) {
        const char *keys = "Tab takes the first   ↑ ↓ pick";
        jd_text(ui, keys, x + bw - jd_text_width(ui, keys, 11, 0) - 10, y + hints_box.height - 19, 11, JD_MUTED, 0);
    }
}

void list_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    float x = r.x + 16, y = r.y + 14, w = r.width - 32;
    /* the hints drawn last frame lie over the list: the list leaves the mouse to them */
    under_hints = CheckCollisionPointRec(GetMousePosition(), hints_box);
    draw_search(app, x, y, w);
    float hints_y = y + 38;
    y += 44;

    const char *title = app->query ? "Search results" : app->nfolders ? folder_label(&app->folders[app->folder]) : "";
    jd_text_fit(ui, title, x, y, w - 120, 20, JD_NORD6, 1);
    /* view toggles: conversations or messages, and where the reading pane goes */
    static const char *pane_icons[] = {"panel-right", "panel-bottom", "rows-2"};
    static const char *pane_tips[] = {"Reading pane: right (p)", "Reading pane: bottom (p)", "Reading pane: off (p)"};
    struct {
        const char *icon, *tip;
    } toggles[] = {{app->grouped ? "layout-grid" : "list", app->grouped ? "Showing conversations (t)" : "Showing messages (t)"},
                   {pane_icons[app->prefs.reader_pos % 3], pane_tips[app->prefs.reader_pos % 3]}};
    float tx2 = x + w;
    /* a phone has no room for a reading pane: no toggle for it */
    for (int k = plat_touch() ? 0 : 1; k >= 0; k--) {
        float bw = 30;
        tx2 -= bw;
        Rectangle b = {tx2, y - 2, bw, 26};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open && !under_hints;
        if (hover) DrawRectangleRounded(b, 0.4f, 6, JD_NORD2);
        jd_icon(toggles[k].icon, b.x + 8, y + 3, 14, hover ? JD_NORD8 : JD_NORD4);
        if (hover && !app->ui.touch) {
            const char *tip = toggles[k].tip;
            jd_tooltip(tip, b.x + b.width - (jd_text_width(ui, tip, 12, 0) + 16), y + 26);
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (k == 1) cycle_reader(app);
            else {
                app->grouped = !app->grouped;
                clear_picks(app);
                save_prefs(app);
                reader_close(app);
                list_reload(app);
            }
        }
        tx2 -= 6;
    }
    {
        Rectangle b = {tx2 - 30, y - 2, 30, 26};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open && !under_hints;
        if (hover) DrawRectangleRounded(b, 0.4f, 6, JD_NORD2);
        jd_icon("refresh-cw", b.x + 8, y + 3, 14, hover ? JD_NORD8 : JD_NORD4);
        if (hover && !app->ui.touch) jd_tooltip("Check for new mail (Ctrl+R)", b.x - 120, y + 26);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) list_refresh(app);
        tx2 -= 36;
    }
    y += 32;

    float cx = x;
    if (chip(app, "Unread", NULL, &cx, y, app->filter_unread)) toggle_filter(app, &app->filter_unread);
    if (chip(app, "Flagged", "star", &cx, y, app->filter_flagged)) toggle_filter(app, &app->filter_flagged);
    if (chip(app, "Files", "paperclip", &cx, y, app->filter_attachment)) toggle_filter(app, &app->filter_attachment);
    static int to_top;
    if (app->list_scroll > 1 || to_top) {
        Rectangle b = {x + w - 34, y, 34, 26};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open && !under_hints;
        DrawRectangleRounded(b, 0.5f, 8, hover ? JD_NORD2 : JD_NORD1);
        DrawRectangleRoundedLinesEx(b, 0.5f, 8, 1, JD_NORD2);
        jd_icon("arrow-up-to-line", b.x + 10, y + 6, 14, hover ? JD_NORD8 : JD_NORD4);
        if (hover && !app->ui.touch) jd_tooltip("Back to the top", b.x - 70, y + 30);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) to_top = 1;
    }
    y += 34;

    y += update_bar(app, x, y, w);
    if (app->npicked && !plat_touch()) {
        char line[80];
        snprintf(line, sizeof line, "%d selected", app->npicked);
        DrawRectangleRounded((Rectangle){x, y, w, 30}, 0.3f, 6, JD_NORD2);
        jd_text(ui, line, x + 10, y + 8, 13, JD_NORD6, 1);
        float bx = x + w - 6;
        struct {
            const char *icon;
            void (*fn)(jd_app *);
        } bar[] = {{"star", act_toggle_flag}, {"mail-open", act_toggle_read}, {"trash-2", act_delete}, {"archive", act_archive}};
        for (int k = 0; k < 4; k++) {
            bx -= 30;
            Rectangle b = {bx, y + 2, 26, 26};
            int hover = CheckCollisionPointRec(GetMousePosition(), b) && !under_hints;
            jd_icon(bar[k].icon, bx + 5, y + 7, 16, hover ? JD_NORD8 : JD_NORD6);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !app->dialog.kind) bar[k].fn(app);
        }
        y += 36;
    } else if (app->query) {
        char line[160];
        if (app->server_searching) snprintf(line, sizeof line, "%d found here. Searching the server...", app->nrows);
        else if (app->nserver_hits) snprintf(line, sizeof line, "%d found, the server's matches included.", app->nrows);
        else snprintf(line, sizeof line, "%d found here.  Shift+S searches the server too.", app->nrows);
        jd_text(ui, line, x, y + 2, 13, JD_MUTED, 0);
        y += 24;
    }

    const float pick_bar_h = 50; /* the rail's settings strip (sidebar.c), so their top lines meet */
    int pick_bar = app->npicked && plat_touch();
    Rectangle area = {r.x, y, r.width, r.y + r.height - y - (pick_bar ? pick_bar_h : 0)};
    DrawRectangle((int)r.x, (int)y - 1, (int)r.width, 1, JD_NORD1);
    if (!app->nrows) {
        jd_text(ui, app->query ? "Nothing matches." : "No messages here.", x, y + 16, 15, JD_MUTED, 0);
        return;
    }
    float rh = row_h(app);
    /* the list's text size, relative to the 15 px it was laid out for */
    float k = (float)(app->prefs.list_text ? app->prefs.list_text : 15) / 15.0f;
    int show_folders = app->query && !app->search_here;
    /* cached: converting ~1000 dates to local time each frame halved a phone's scrolling frame rate */
    static struct {
        const jd_thread_row *rows;
        int n;
        float rh;
        time_t day; /* local midnight */
        float *top;
        char (*heading)[24]; /* empty when row i continues the group above */
        char (*date)[24];
        float total;
        uint64_t sig; /* the rows' ids and dates: a reload may hand back the same block with other rows */
        int64_t anchor;   /* first row on screen last frame (thread or message id) */
        float anchor_off;
    } lay;
    uint64_t sig = 0;
    for (int i = 0; i < app->nrows; i++)
        sig = sig * 1099511628211u + (uint64_t)app->rows[i].latest * 31u + (uint64_t)app->rows[i].date;
    time_t now_t = time(NULL);
    static time_t day_checked, midnight;
    if (now_t - day_checked > 30) {
        struct tm m;
        localtime_r(&now_t, &m);
        m.tm_hour = m.tm_min = m.tm_sec = 0;
        midnight = mktime(&m), day_checked = now_t;
    }
    if (lay.rows != app->rows || lay.n != app->nrows || lay.rh != rh || lay.day != midnight || lay.sig != sig) {
        lay.rows = app->rows, lay.n = app->nrows, lay.rh = rh, lay.day = midnight, lay.sig = sig;
        lay.top = jd_realloc(lay.top, sizeof *lay.top * (size_t)(app->nrows + 1));
        lay.heading = jd_realloc(lay.heading, sizeof *lay.heading * (size_t)(app->nrows + 1));
        lay.date = jd_realloc(lay.date, sizeof *lay.date * (size_t)(app->nrows + 1));
        char prev[24] = "", group[24];
        float yy = 0;
        for (int i = 0; i < app->nrows; i++) {
            date_group(app->rows[i].date, group, sizeof group);
            lay.heading[i][0] = 0;
            if (strcmp(group, prev) != 0) {
                yy += HEADER_H;
                strcpy(prev, group);
                strcpy(lay.heading[i], group);
            }
            lay.top[i] = yy;
            yy += rh;
            format_date(app->rows[i].send_at ? app->rows[i].send_at : app->rows[i].date, lay.date[i], sizeof lay.date[i]);
        }
        lay.total = yy + 12;
        /* rows came or went above the view: the first row shown keeps its place */
        if (lay.anchor && app->list_scroll > 0)
            for (int i = 0; i < app->nrows; i++)
                if ((app->grouped ? app->rows[i].thread : app->rows[i].latest) == lay.anchor) {
                    app->list_scroll = lay.top[i] - lay.anchor_off;
                    break;
                }
    }
    const float *top = lay.top;
    float total = lay.total;
    /* the list follows the selection only when it moves; otherwise the wheel and the follow fight */
    if (app->row != app->followed_row) {
        if (top[app->row] - HEADER_H < app->list_scroll) app->list_scroll = top[app->row] - HEADER_H;
        if (top[app->row] + rh > app->list_scroll + area.height) app->list_scroll = top[app->row] + rh - area.height;
        app->followed_row = app->row;
    }
    int over = CheckCollisionPointRec(GetMousePosition(), area) && !app->dialog.kind && !app->menu.open && !under_hints;
    float move = over ? jd_wheel_y(rh * 2) : 0;
    /* pull to refresh; a fling reaching the top pulls nothing */
    static float pull;              /* logical px */
    static double refreshing_since; /* > 0 while a pulled check runs */
    static int saw_busy;            /* the sync reported work since the pull (it may not have started yet) */
    const float PULL_AT = 72, PULL_MAX = 130, REFRESH_H = 56;
    int pulling = plat_finger_scrolling();
    if (pulling && (pull > 0 || (app->list_scroll <= 0 && move > 0))) {
        /* resistance grows as it comes down, as SwipeRefreshLayout's */
        pull = fmaxf(0, fminf(PULL_MAX, pull + move * (1 - pull / PULL_MAX) * 0.6f));
        move = 0;
    } else if (!pulling && pull > 0) {
        if (pull >= PULL_AT && refreshing_since <= 0) {
            list_refresh(app);
            refreshing_since = GetTime(), saw_busy = 0;
        }
        float rest = refreshing_since > 0 ? REFRESH_H : 0;
        pull = rest + (pull - rest) * expf(-GetFrameTime() * 14);
        if (fabsf(pull - rest) < 0.5f) pull = rest;
    }
    if (refreshing_since > 0) {
        if (app->status) saw_busy = 1;
        double held = GetTime() - refreshing_since;
        if ((held > 0.8 && !app->status && (saw_busy || held > 2)) || held > 20) refreshing_since = 0;
    }
    app->list_scroll -= move;
    if (to_top) {
        if (move != 0 || pulling) to_top = 0;
        else if ((app->list_scroll *= expf(-GetFrameTime() * 12)) < 0.5f) app->list_scroll = 0, to_top = 0;
    }
    if (app->list_scroll > total - area.height) app->list_scroll = total - area.height;
    if (app->list_scroll < 0) app->list_scroll = 0;
    /* kept fractional: rounding every frame dropped a slow finger's movement (jd_text snaps text to pixels) */

    jd_scissor(area.x, area.y, area.width, area.height);
    int first = 0;
    for (int lo = 0, hi = app->nrows - 1; lo <= hi;) {
        int mid = (lo + hi) / 2;
        if (area.y + top[mid] - app->list_scroll + pull + rh < area.y) lo = mid + 1, first = lo;
        else hi = mid - 1;
    }
    if (first < app->nrows) {
        lay.anchor = app->grouped ? app->rows[first].thread : app->rows[first].latest;
        lay.anchor_off = top[first] - app->list_scroll;
    }
    for (int i = first; i < app->nrows; i++) {
        float ry = area.y + top[i] - app->list_scroll + pull;
        if (ry > area.y + area.height) break;
        if (ry + rh < area.y) continue;
        if (lay.heading[i][0]) jd_text(ui, lay.heading[i], x, ry - HEADER_H + 9, 12, JD_MUTED, 1);
        const jd_thread_row *t = &app->rows[i];
        Rectangle box = {r.x + 8, ry + 1, r.width - 16, rh - 2};
        int open = app->grouped ? t->thread == preview(app)->thread : t->latest == preview(app)->message;
        int picked = is_picked(app, row_key(app, i));
        int hover = over && CheckCollisionPointRec(GetMousePosition(), box);
        if (picked) DrawRectangleRounded(box, 0.12f, 6, ColorAlpha(JD_NORD10, 0.35f));
        else if (i == app->row) DrawRectangleRounded(box, 0.12f, 6, app->pane == PANE_LIST ? JD_NORD2 : JD_NORD1);
        /* the keyboard's place: outlined only while the list has the keys */
        if (i == app->row && app->pane == PANE_LIST && app->tab == 0 && !plat_touch())
            DrawRectangleRoundedLinesEx(box, 0.12f, 6, 1.5f, JD_NORD8);
        /* no hover on a touch screen: its pointer is wherever the last finger was, and rows slide past it */
        else if (open || (hover && !plat_touch())) DrawRectangleRounded(box, 0.12f, 6, JD_NORD1);

        float ax = box.x + (app->prefs.compact ? 20 : 26), ay = ry + (app->prefs.compact ? rh / 2 : 28);
        float ar = app->prefs.compact ? 11 : 17;
        Rectangle avatar_box = {ax - ar - 2, ay - ar - 2, 2 * ar + 4, 2 * ar + 4};
        int on_avatar = over && CheckCollisionPointRec(GetMousePosition(), avatar_box);
        avatar(app, t, ax, ay, ar, picked, on_avatar);
        float tx = ax + ar + 12, right = box.x + box.width - 10;

        int handled = 0;
        if (hover && !app->dialog.kind) {
            int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            /* a touch screen has no hover: its long press opens the row's menu instead */
            if (!app->prefs.compact && !plat_touch() && hover_actions(app, i, right, ry + 8)) handled = 1;
            else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && (on_avatar || ctrl)) {
                toggle_pick(app, i);
                app->anchor = i;
                handled = 1;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && shift) {
                pick_range(app, app->anchor, i);
                handled = 1;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                app->pane = PANE_LIST;
                app->row = app->anchor = i;
                clear_picks(app);
                open_row(app, 0);
                handled = 1;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
                app->row = i;
                open_row(app, 1);
                handled = 1;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                app->row = i;
                if (!is_picked(app, row_key(app, i))) clear_picks(app);
                app->menu = (jd_menu){.open = 1, .at = GetMousePosition(), .on_folder = 0, .index = i};
                handled = 1;
            }
        }
        if (handled && i >= app->nrows) break;

        char count[16] = "";
        const char *date = lay.date[i];
        jd_ui_use(ui, &ui->list);
        /* a scheduled draft shows when it goes, after a calendar */
        float iw = t->send_at ? 18 : 0;
        float dw = jd_text_width(ui, date, 12 * k, 0) + iw;
        if (t->count > 1) snprintf(count, sizeof count, "%d", t->count);
        float cw = count[0] ? jd_text_width(ui, count, 12 * k, 1) + 12 : 0;
        Color strong = t->unread ? JD_NORD6 : JD_NORD4;
        /* the hover actions take the date's place; a touch screen has no hover */
        int show_meta = !(hover && !app->prefs.compact && !plat_touch());
        /* outside the folder being shown, rows say where they are */
        float chips_w = show_folders ? folder_chips(app, t->folders, right, ry + (app->prefs.compact ? rh / 2 - 10 : 52 * k), 1, k) : 0;
        if (app->prefs.compact) {
            float sw = (right - tx) * 0.3f;
            jd_text_fit(ui, t->senders, tx, ry + rh / 2 - 8 * k, sw - 10, 14 * k, strong, t->unread > 0);
            float mx = right - dw - chips_w;
            if (iw) jd_icon("calendar", mx, ry + rh / 2 - 7, 14, JD_NORD8);
            jd_text(ui, date, mx + iw, ry + rh / 2 - 7 * k, 12 * k, t->unread ? JD_NORD8 : JD_NORD4, t->unread > 0);
            if (chips_w) folder_chips(app, t->folders, right, ry + rh / 2 - 10, 0, k);
            if (t->flagged) jd_icon("star", mx -= 20, ry + rh / 2 - 7, 14, JD_NORD13);
            if (t->has_attachment) jd_icon("paperclip", mx -= 20, ry + rh / 2 - 7, 14, JD_NORD4);
            if (count[0]) jd_text(ui, count, mx -= cw, ry + rh / 2 - 7 * k, 12 * k, JD_NORD4, 1);
            jd_text_fit(ui, t->subject, tx + sw, ry + rh / 2 - 8 * k, mx - tx - sw - 10, 14 * k, strong, t->unread > 0);
        } else {
            if (show_meta) {
                if (iw) jd_icon("calendar", right - dw, ry + 12 * k, 14, JD_NORD8);
                jd_text(ui, date, right - dw + iw, ry + 12 * k, 12 * k, t->unread ? JD_NORD8 : JD_NORD4, t->unread > 0);
                if (count[0]) {
                    Rectangle pill = {right - dw - cw - 8, ry + 10 * k, cw, 18 * k};
                    DrawRectangleRounded(pill, 0.5f, 6, JD_NORD2);
                    jd_text(ui, count, pill.x + 6, ry + 12 * k, 12 * k, JD_NORD4, 1);
                }
            }
            jd_text_fit(ui, t->senders, tx, ry + 10 * k, right - tx - dw - cw - 24, 15 * k, strong, t->unread > 0);
            float mx = right;
            if (t->flagged) jd_icon("star", mx -= 18, ry + 34 * k, 14, JD_NORD13);
            if (t->has_attachment) jd_icon("paperclip", mx -= 18, ry + 34 * k, 14, JD_NORD4);
            jd_text_fit(ui, t->subject, tx, ry + 33 * k, mx - tx - 8, 14 * k, strong, t->unread > 0);
            if (app->prefs.snippets && t->snippet && t->snippet[0])
                jd_text_fit(ui, t->snippet, tx, ry + 54 * k, right - tx - chips_w - (chips_w ? 8 : 0), 13 * k, JD_MUTED, 0);
            if (chips_w) folder_chips(app, t->folders, right, ry + 52 * k, 0, k);
        }
        jd_ui_use(ui, NULL);
    }
    jd_scissor_end();
    if (!app->dialog.kind && !app->menu.open) jd_scrollbar(area, total, &app->list_scroll);
    if (pull > 1) {
        float cx = area.x + area.width / 2, cy = area.y + pull / 2, rad = 11;
        float a = (float)GetTime() * 360;
        if (refreshing_since > 0) {
            DrawRing((Vector2){cx, cy}, rad - 2.5f, rad, a, a + 270, 32, JD_NORD8);
        } else {
            float k = fminf(1, pull / PULL_AT);
            DrawRing((Vector2){cx, cy}, rad - 2.5f, rad, -90 + k * 120, -90 + k * 120 + k * 300, 32,
                     k >= 1 ? JD_NORD8 : JD_NORD4);
        }
    }
    if (pick_bar) {
        Rectangle bar = {r.x, area.y + area.height, r.width, pick_bar_h};
        DrawRectangleRec(bar, JD_NORD1);
        DrawRectangle((int)bar.x, (int)bar.y, (int)bar.width, 1, JD_NORD2);
        struct {
            const char *icon;
            void (*fn)(jd_app *);
        } acts[] = {{"archive", act_archive}, {"trash-2", act_delete}, {"mail-open", act_toggle_read}, {"star", act_toggle_flag}};
        int free_tap = !app->dialog.kind && !app->menu.open;
        float step = fminf(64, (bar.width - 110) / 4), bx = bar.x + bar.width - 4 * step - 4;
        /* slot 0 clears the selection; 1..4 are the actions */
        for (int k = 0; k < 5; k++) {
            Rectangle slot = k ? (Rectangle){bx + step * (float)(k - 1), bar.y + 1, step, 48}
                               : (Rectangle){bar.x + 8, bar.y + 1, 48, 48};
            Rectangle face = {slot.x + (slot.width - 40) / 2, bar.y + 7, 40, 36};
            int hover = free_tap && CheckCollisionPointRec(GetMousePosition(), slot);
            if (hover) DrawRectangleRounded(face, 0.3f, 6, JD_NORD2);
            jd_icon(k ? acts[k - 1].icon : "x", face.x + 11, face.y + 9, 18, hover ? JD_NORD8 : JD_NORD4);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (k) acts[k - 1].fn(app);
                else clear_picks(app);
            }
        }
        char line[32];
        snprintf(line, sizeof line, "%d", app->npicked);
        jd_text(ui, line, bar.x + 58, bar.y + 14, 18, JD_NORD4, 1);
    }
    hints_draw(app, x, hints_y, w);
}
