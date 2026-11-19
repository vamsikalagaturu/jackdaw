#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app.h"
#include "cal/calsync.h"
#include "cal/ical.h"
#include "cal/oauth.h"
#include "core/util.h"
#include "mail/send.h"
#include "platform.h"
#include "ui/icons.h"

enum { V_AGENDA, V_WEEK, V_MONTH };
enum { AGENDA_DAYS = 30, HOUR_H = 48, MAX_REMINDERS = 32 };
/* Settings' focus numbers for the calendar fields, past settings.c's own */
enum { CF_URL = 200, CF_USER, CF_PASSWORD, CF_COUNT };
/* the editor's fields, in Tab order */
enum { EF_TITLE, EF_DATE0, EF_TIME0, EF_DATE1, EF_TIME1, EF_LOCATION, EF_NOTES, EF_COUNT };

/* an event's occurrence in the shown range */
typedef struct {
    int64_t event, start, end;
    int all_day, readonly, alarm, recurring;
    char *summary, *location;
    Color color;
} occ;

static struct {
    int view, opened;
    int64_t anchor; /* local midnight of the day the view is about */
    occ *occs;
    int nocc, sel;
    int64_t from, to;
    int stale;
    float scroll;
    int scrolled_for; /* the view the scroll was set for */
    int detail;       /* the occurrence shown in the details box, -1 none */
    char status[200], error[400];
    int64_t delete_event;
    int64_t serials, delete_serial, respond_serial, reply_serial;
} cv = {.stale = 1, .detail = -1, .sel = -1, .serials = 1LL << 40};

static struct {
    int open, focus, saving;
    int64_t event, calendar, serial;
    jd_field f[EF_COUNT - 1];
    jd_editor notes;
    int all_day, repeat, alarm;
    char *rrule_kept; /* a rule the choices do not cover, kept as it was */
    char *old_ics;
    char error[300];
} ed;

static struct {
    int64_t at;
    char *title, *text;
} rem[MAX_REMINDERS * 4];
static int nrem, rem_stale = 1, rem_by_platform;
static int64_t rem_built, rem_fired_until;

/* Settings' fields and the Google sign-in under way */
static jd_field cf[3];
static int cf_filled, cf_provider = -1;
static jd_oauth *signin;
static char settings_note[600];

/* ---- time ---- */

static int64_t mk_local(int y, int m, int d, int hh, int mm)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = hh, .tm_min = mm, .tm_isdst = -1};
    return (int64_t)mktime(&tm);
}

static struct tm local_tm(int64_t t)
{
    struct tm tm;
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    return tm;
}

static int64_t local_midnight(int64_t t)
{
    struct tm tm = local_tm(t);
    return mk_local(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, 0, 0);
}

static int64_t add_days(int64_t day, int n)
{
    struct tm tm = local_tm(day);
    return mk_local(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday + n, 0, 0);
}

/* the local midnight of an all-day value's date (a UTC midnight, cal/ical.h) */
static int64_t date_day(int64_t utc_midnight)
{
    int y, m, d;
    jd_civil_from_days(utc_midnight / 86400, &y, &m, &d);
    return mk_local(y, m, d, 0, 0);
}

static int64_t first_day(const occ *o)
{
    return o->all_day ? date_day(o->start) : local_midnight(o->start);
}

/* the day after the last one it touches */
static int64_t end_day(const occ *o)
{
    if (o->all_day) return date_day(o->end > o->start ? o->end : o->start + 86400);
    return add_days(local_midnight(o->end > o->start ? o->end - 1 : o->start), 1);
}

static int on_day(const occ *o, int64_t day)
{
    return first_day(o) <= day && day < end_day(o);
}

static int64_t week_start(int64_t day)
{
    struct tm tm = local_tm(day);
    return add_days(day, -((tm.tm_wday + 6) % 7));
}

static void fmt(int64_t t, const char *f, char *out, size_t n)
{
    struct tm tm = local_tm(t);
    strftime(out, n, f, &tm);
}

static void time_range(const occ *o, char *out, size_t n)
{
    if (o->all_day) {
        snprintf(out, n, "All day");
        return;
    }
    char a[16], b[16];
    fmt(o->start, "%H:%M", a, sizeof a);
    fmt(o->end, "%H:%M", b, sizeof b);
    if (o->end > o->start) snprintf(out, n, "%s – %s", a, b);
    else snprintf(out, n, "%s", a);
}

static Color hex_color(const char *s)
{
    unsigned r, g, b;
    if (s && s[0] == '#' && strlen(s) >= 7 && sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3)
        return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    return JD_NORD8;
}

/* ---- occurrences ---- */

static void occs_free(occ *o, int n)
{
    for (int i = 0; i < n; i++) free(o[i].summary), free(o[i].location);
    free(o);
}

static int by_start(const void *a, const void *b)
{
    const occ *x = a, *y = b;
    int64_t dx = first_day(x), dy = first_day(y);
    if (dx != dy) return dx < dy ? -1 : 1;
    if (x->all_day != y->all_day) return x->all_day ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return 0;
}

/* the occurrences of the shown calendars' events between local days from and to */
static occ *gather(jd_app *app, int64_t from, int64_t to, int *n)
{
    *n = 0;
    if (!app->view) return NULL;
    int nrows;
    /* an all-day event's dates are UTC midnights: a day either side covers every zone */
    jd_event_row *rows = jd_store_events(app->view, from - 86400, to + 86400, &nrows);
    occ *out = NULL;
    for (int i = 0; i < nrows; i++) {
        jd_occurrence *o = NULL;
        int k = 0;
        jd_ical_expand(rows[i].ics, from - 86400, to + 86400, &o, &k);
        for (int j = 0; j < k; j++) {
            occ x = {rows[i].id, o[j].start, o[j].end, o[j].all_day, rows[i].readonly, o[j].alarm, rows[i].recurring,
                     o[j].summary, o[j].location, hex_color(rows[i].color)};
            o[j].summary = o[j].location = NULL;
            if (end_day(&x) <= from || first_day(&x) >= to) {
                free(x.summary), free(x.location);
                continue;
            }
            out = jd_realloc(out, sizeof *out * (size_t)(*n + 1));
            out[(*n)++] = x;
        }
        jd_occurrences_free(o, k);
    }
    jd_event_rows_free(rows, nrows);
    if (*n) qsort(out, (size_t)*n, sizeof *out, by_start);
    return out;
}

static void range(int64_t *from, int64_t *to)
{
    if (cv.view == V_AGENDA) *from = cv.anchor, *to = add_days(cv.anchor, AGENDA_DAYS);
    else if (cv.view == V_WEEK) *from = week_start(cv.anchor), *to = add_days(*from, 7);
    else {
        struct tm tm = local_tm(cv.anchor);
        *from = week_start(mk_local(tm.tm_year + 1900, tm.tm_mon + 1, 1, 0, 0));
        *to = add_days(*from, 42);
    }
}

static void reload(jd_app *app)
{
    int64_t from, to;
    range(&from, &to);
    if (!cv.stale && from == cv.from && to == cv.to) return;
    occs_free(cv.occs, cv.nocc);
    cv.occs = gather(app, from, to, &cv.nocc);
    cv.from = from, cv.to = to, cv.stale = 0;
    if (cv.detail >= cv.nocc) cv.detail = -1;
    if (cv.sel >= cv.nocc) cv.sel = cv.nocc - 1;
}

/* ---- the worker ---- */

char *calendar_secret_key(jd_app *app)
{
    return jd_printf("%s calendar", app->account.address ? app->account.address : "");
}

void calendar_stop(jd_app *app)
{
    jd_calsync_stop(app->calsync);
    app->calsync = NULL;
    cv.status[0] = 0;
}

void calendar_start(jd_app *app)
{
    calendar_stop(app);
    cv.error[0] = 0;
    jd_prefs *p = &app->prefs;
    if (!p->cal_provider || !app->dir) return;
    char *key = calendar_secret_key(app);
    char *secret = plat_secret_load(key);
    free(key);
    if (!secret) {
        snprintf(cv.error, sizeof cv.error, p->cal_provider == 2 ? "Sign in with Google in Settings, Calendar."
                                                                 : "The calendar password is missing: enter it in Settings, Calendar.");
        return;
    }
    char *db = jd_printf("%s/mail.db", app->dir);
    app->calsync = jd_calsync_start(p->cal_provider == 2 ? JD_CAL_GOOGLE : JD_CAL_CALDAV, p->cal_url, p->cal_username,
                                    secret, p->cal_check_minutes, db);
    jd_wipe(secret, strlen(secret));
    free(secret), free(db);
}

const char *calendar_status(void)
{
    return cv.status[0] ? cv.status : NULL;
}

const char *calendar_error(void)
{
    return cv.error[0] ? cv.error : NULL;
}

const char *calendar_title(void)
{
    return "Calendar";
}

/* an iTIP answer by mail to the organizer (RFC 6047 2.4: text/calendar with its method) */
static void send_reply(jd_app *app, const char *reply_ics, const char *organizer)
{
    if (!app->sync) return;
    jd_invite inv;
    const char *summary = "", *verb = "Answered";
    if (jd_invite_read(reply_ics, app->account.address, &inv) == 0) {
        summary = inv.event.summary ? inv.event.summary : "";
        const char *ps = inv.reply_partstat ? inv.reply_partstat : "";
        verb = strcasecmp(ps, "ACCEPTED") == 0 ? "Accepted" : strcasecmp(ps, "TENTATIVE") == 0 ? "Tentative"
               : strcasecmp(ps, "DECLINED") == 0 ? "Declined" : "Answered";
    }
    char *subject = jd_printf("%s: %s", verb, summary);
    char *body = jd_printf("%s has %s the invitation to %s.\n", app->account.name ? app->account.name : app->account.address,
                           strcmp(verb, "Accepted") == 0 ? "accepted" : strcmp(verb, "Declined") == 0 ? "declined"
                           : strcmp(verb, "Tentative") == 0 ? "tentatively accepted" : "answered",
                           summary[0] ? summary : "the event");
    jd_part part = {.name = "reply.ics", .type = "text/calendar; method=REPLY; charset=UTF-8", .data = (char *)reply_ics,
                    .len = strlen(reply_ics)};
    jd_outgoing o = {.to = organizer, .subject = subject, .body = body, .parts = &part, .nparts = 1};
    size_t len;
    char **rcpts;
    int nrcpts;
    char err[300];
    char *msg = jd_message_build(&app->account, &o, &len, &rcpts, &nrcpts, err, sizeof err);
    if (msg) {
        cv.reply_serial = ++cv.serials;
        jd_sync_send(app->sync, msg, len, rcpts, nrcpts, NULL, 0, cv.reply_serial);
        jd_rcpts_free(rcpts, nrcpts);
        free(msg);
    } else {
        notify_user(app, "The answer to the organizer was not sent: %s", err);
    }
    jd_invite_free(&inv);
    free(subject), free(body);
}

int calendar_sent(jd_app *app, int64_t serial, const char *error)
{
    if (!serial || serial != cv.reply_serial) return 0;
    if (error) notify_user(app, "The answer to the organizer was not sent: %s", error);
    else notify_user(app, "Answer sent to the organizer.");
    return 1;
}

static void editor_close(void);

static void handle(jd_app *app, jd_cal_event *ev)
{
    switch (ev->kind) {
    case JD_CAL_EV_STATUS:
        snprintf(cv.status, sizeof cv.status, "%s", ev->text ? ev->text : "");
        if (ev->text) cv.error[0] = 0;
        break;
    case JD_CAL_EV_ERROR:
        snprintf(cv.error, sizeof cv.error, "Calendar: %s", ev->text ? ev->text : "failed");
        break;
    case JD_CAL_EV_AUTH:
        snprintf(cv.error, sizeof cv.error, "The calendar server refused the sign-in%s%s. Settings, Calendar signs in again.",
                 ev->text ? ": " : "", ev->text ? ev->text : "");
        break;
    case JD_CAL_EV_CHANGED:
        cv.stale = 1, rem_stale = 1;
        break;
    case JD_CAL_EV_SAVED:
        if (ed.open && ev->id == ed.serial) {
            ed.saving = 0;
            if (ev->text) snprintf(ed.error, sizeof ed.error, "%s", ev->text);
            else {
                editor_close();
                notify_user(app, "Event saved.");
            }
        } else if (ev->id == cv.delete_serial) {
            notify_user(app, "%s", ev->text ? ev->text : "Event deleted.");
        } else if (ev->id == cv.respond_serial) {
            if (ev->text) notify_user(app, "Not answered: %s", ev->text);
            else notify_user(app, "Answered; the calendar is updated.");
        } else if (ev->text) {
            notify_user(app, "%s", ev->text);
        }
        break;
    case JD_CAL_EV_REPLY:
        send_reply(app, ev->text, ev->text2);
        break;
    }
}

/* ---- reminders ---- */

static void reminders_free(void)
{
    for (int i = 0; i < nrem; i++) free(rem[i].title), free(rem[i].text);
    nrem = 0;
}

static int by_at(const void *a, const void *b)
{
    const typeof(rem[0]) *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

static void reminders_build(jd_app *app)
{
    reminders_free();
    int64_t now = jd_now();
    int n = 0;
    occ *o = gather(app, local_midnight(now), add_days(local_midnight(now), 8), &n);
    for (int i = 0; i < n && nrem < (int)(sizeof rem / sizeof *rem); i++) {
        if (o[i].alarm < 0) continue;
        int64_t starts = o[i].all_day ? first_day(&o[i]) : o[i].start;
        int64_t at = starts - (int64_t)o[i].alarm * 60;
        if (at <= now - 60) continue;
        char when[64], day[32];
        time_range(&o[i], when, sizeof when);
        fmt(starts, "%a %d %b", day, sizeof day);
        rem[nrem].at = at;
        rem[nrem].title = jd_strdup(o[i].summary && o[i].summary[0] ? o[i].summary : "(no title)");
        rem[nrem].text = o[i].location && o[i].location[0] ? jd_printf("%s, %s · %s", day, when, o[i].location)
                                                            : jd_printf("%s, %s", day, when);
        nrem++;
    }
    occs_free(o, n);
    if (nrem) qsort(rem, (size_t)nrem, sizeof *rem, by_at);
    int64_t at[MAX_REMINDERS];
    const char *title[MAX_REMINDERS], *text[MAX_REMINDERS];
    int k = 0;
    for (; k < nrem && k < MAX_REMINDERS; k++) at[k] = rem[k].at, title[k] = rem[k].title, text[k] = rem[k].text;
    rem_by_platform = plat_reminders(k, at, title, text);
    rem_built = now;
    rem_stale = 0;
}

static void reminders_tick(jd_app *app)
{
    int64_t now = jd_now();
    if (!rem_fired_until) rem_fired_until = now;
    if (!app->prefs.cal_reminders || !app->prefs.cal_provider) {
        if (nrem || rem_by_platform) {
            reminders_free();
            plat_reminders(0, NULL, NULL, NULL);
            rem_by_platform = 0;
        }
        rem_fired_until = now;
        return;
    }
    /* a fresh look every half hour: "tomorrow" moves, and a week ahead grows */
    if (rem_stale || now - rem_built > 1800) reminders_build(app);
    if (rem_by_platform) {
        rem_fired_until = now;
        return;
    }
    for (int i = 0; i < nrem; i++)
        if (rem[i].at > rem_fired_until && rem[i].at <= now) plat_notify(rem[i].title, rem[i].text, 0);
    rem_fired_until = now;
}

static void signin_tick(jd_app *app);

void calendar_tick(jd_app *app)
{
    jd_cal_event ev;
    while (app->calsync && jd_calsync_poll(app->calsync, &ev)) {
        handle(app, &ev);
        jd_cal_event_free(&ev);
    }
    signin_tick(app);
    if (app->view) reminders_tick(app);
}

/* ---- the editor ---- */

static void editor_close(void)
{
    ed.open = 0;
    free(ed.rrule_kept), free(ed.old_ics);
    ed.rrule_kept = ed.old_ics = NULL;
    ed.error[0] = 0;
}

static const char *const repeats[] = {"Never", "Daily", "Weekly", "Monthly", "Yearly"};
static const char *const rules[] = {NULL, "FREQ=DAILY", "FREQ=WEEKLY", "FREQ=MONTHLY", "FREQ=YEARLY"};
static const int alarms[] = {-1, 0, 10, 30, 60, 1440};
static const char *const alarm_names[] = {"None", "At start", "10 min", "30 min", "1 hour", "1 day"};

static int64_t first_writable(jd_app *app, int64_t prefer)
{
    int n;
    jd_calendar_row *c = app->view ? jd_store_calendars(app->view, &n) : NULL;
    int64_t id = 0;
    for (int i = 0; c && i < n && !id; i++)
        if (c[i].id == prefer && !c[i].readonly) id = c[i].id;
    for (int i = 0; c && i < n && !id; i++)
        if (!c[i].readonly && c[i].enabled) id = c[i].id;
    for (int i = 0; c && i < n && !id; i++)
        if (!c[i].readonly) id = c[i].id;
    if (c) jd_calendar_rows_free(c, n);
    return id;
}

static void set_times(int64_t start, int64_t end, int all_day)
{
    char s[32];
    if (all_day) {
        int y, m, d;
        jd_civil_from_days(start / 86400, &y, &m, &d);
        snprintf(s, sizeof s, "%04d-%02d-%02d", y, m, d);
        jd_field_set(&ed.f[EF_DATE0], s);
        jd_civil_from_days((end > start ? end - 86400 : start) / 86400, &y, &m, &d);
        snprintf(s, sizeof s, "%04d-%02d-%02d", y, m, d);
        jd_field_set(&ed.f[EF_DATE1], s);
        jd_field_set(&ed.f[EF_TIME0], "09:00");
        jd_field_set(&ed.f[EF_TIME1], "10:00");
        return;
    }
    fmt(start, "%Y-%m-%d", s, sizeof s);
    jd_field_set(&ed.f[EF_DATE0], s);
    fmt(start, "%H:%M", s, sizeof s);
    jd_field_set(&ed.f[EF_TIME0], s);
    fmt(end, "%Y-%m-%d", s, sizeof s);
    jd_field_set(&ed.f[EF_DATE1], s);
    fmt(end, "%H:%M", s, sizeof s);
    jd_field_set(&ed.f[EF_TIME1], s);
}

static void editor_new(jd_app *app, int64_t start, int all_day)
{
    editor_close();
    if (!app->calsync) {
        notify_user(app, "No calendar is connected: set one up in Settings, Calendar.");
        return;
    }
    ed.calendar = first_writable(app, 0);
    if (!ed.calendar) {
        notify_user(app, "Every calendar of this account is read-only.");
        return;
    }
    ed.open = 1, ed.event = 0, ed.focus = EF_TITLE, ed.saving = 0;
    for (int i = 0; i < EF_COUNT - 1; i++) jd_field_set(&ed.f[i], NULL);
    jd_editor_set(&ed.notes, "");
    ed.all_day = all_day, ed.repeat = 0, ed.alarm = 0;
    set_times(all_day ? jd_days_from_civil(local_tm(start).tm_year + 1900, local_tm(start).tm_mon + 1, local_tm(start).tm_mday) * 86400 : start,
              all_day ? 0 : start + 3600, all_day);
}

static void editor_edit(jd_app *app, int64_t event)
{
    jd_event_row *row = app->view ? jd_store_event(app->view, event) : NULL;
    if (!row) return;
    if (row->readonly) {
        notify_user(app, "That calendar is read-only.");
        jd_event_rows_free(row, 1);
        return;
    }
    if (!app->calsync) {
        notify_user(app, "No calendar is connected: set one up in Settings, Calendar.");
        jd_event_rows_free(row, 1);
        return;
    }
    jd_ical_event e;
    if (jd_ical_read(row->ics, &e) != 0) {
        jd_event_rows_free(row, 1);
        return;
    }
    editor_close();
    ed.open = 1, ed.event = event, ed.calendar = row->calendar, ed.focus = EF_TITLE, ed.saving = 0;
    ed.old_ics = jd_strdup(row->ics);
    jd_field_set(&ed.f[EF_TITLE], e.summary);
    jd_field_set(&ed.f[EF_LOCATION], e.location);
    jd_editor_set(&ed.notes, e.description ? e.description : "");
    ed.all_day = e.all_day;
    set_times(e.start, e.end, e.all_day);
    ed.repeat = 0;
    if (e.rrule) {
        ed.repeat = -1;
        for (int i = 1; i < 5; i++)
            if (strcmp(e.rrule, rules[i]) == 0) ed.repeat = i;
        if (ed.repeat < 0) ed.rrule_kept = jd_strdup(e.rrule);
    }
    ed.alarm = -1;
    for (int i = 0; i < 6; i++)
        if (alarms[i] == e.alarm) ed.alarm = i;
    if (ed.alarm < 0) ed.alarm = 100 + e.alarm;
    jd_ical_event_free(&e);
    jd_event_rows_free(row, 1);
}

static int parse_date(const char *s, int *y, int *m, int *d)
{
    return sscanf(s, "%d-%d-%d", y, m, d) == 3 && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31 && *y > 1900;
}

static int parse_time(const char *s, int *h, int *mi)
{
    *mi = 0;
    if (sscanf(s, "%d:%d", h, mi) < 1) return 0;
    return *h >= 0 && *h <= 23 && *mi >= 0 && *mi <= 59;
}

static void editor_save(jd_app *app)
{
    if (ed.saving) return;
    int y0, m0, d0, y1, m1, d1, h0 = 0, i0 = 0, h1 = 0, i1 = 0;
    if (!parse_date(ed.f[EF_DATE0].text, &y0, &m0, &d0) || !parse_date(ed.f[EF_DATE1].text, &y1, &m1, &d1)) {
        snprintf(ed.error, sizeof ed.error, "Dates are written 2026-11-19.");
        return;
    }
    if (!ed.all_day && (!parse_time(ed.f[EF_TIME0].text, &h0, &i0) || !parse_time(ed.f[EF_TIME1].text, &h1, &i1))) {
        snprintf(ed.error, sizeof ed.error, "Times are written 09:30.");
        return;
    }
    jd_ical_event e = {.summary = ed.f[EF_TITLE].text, .location = ed.f[EF_LOCATION].text,
                       .description = ed.notes.text, .all_day = ed.all_day};
    if (ed.all_day) {
        e.start = jd_days_from_civil(y0, m0, d0) * 86400;
        e.end = (jd_days_from_civil(y1, m1, d1) + 1) * 86400;
    } else {
        e.start = mk_local(y0, m0, d0, h0, i0);
        e.end = mk_local(y1, m1, d1, h1, i1);
    }
    if (e.end < e.start || (ed.all_day && e.end <= e.start)) {
        snprintf(ed.error, sizeof ed.error, "The event ends before it starts.");
        return;
    }
    e.rrule = ed.repeat < 0 ? ed.rrule_kept : (char *)rules[ed.repeat];
    e.alarm = ed.alarm >= 100 ? ed.alarm - 100 : alarms[ed.alarm];
    if (!e.summary[0]) e.summary = "(no title)";
    char *ics = jd_ical_write(ed.old_ics, &e);
    if (!ics || !app->calsync) {
        snprintf(ed.error, sizeof ed.error, app->calsync ? "The event could not be written." : "No calendar is connected.");
        free(ics);
        return;
    }
    ed.serial = ++cv.serials;
    ed.saving = 1;
    ed.error[0] = 0;
    jd_calsync_put(app->calsync, ed.calendar, ed.event, ics, ed.serial);
    free(ics);
}

static void ask_delete(jd_app *app, int64_t event)
{
    jd_event_row *row = app->view ? jd_store_event(app->view, event) : NULL;
    if (!row) return;
    if (row->readonly) notify_user(app, "That calendar is read-only.");
    else {
        cv.delete_event = event;
        char *title = jd_printf("Delete %s\"%s\"?", row->recurring ? "every occurrence of " : "",
                                row->summary && row->summary[0] ? row->summary : "(no title)");
        dialog_open(app, DLG_CONFIRM, ACT_DELETE_EVENT, title, NULL);
        free(title);
    }
    jd_event_rows_free(row, 1);
}

void calendar_delete_confirmed(jd_app *app)
{
    if (!cv.delete_event) return;
    if (!app->calsync) {
        notify_user(app, "No calendar is connected.");
        return;
    }
    cv.delete_serial = ++cv.serials;
    jd_calsync_delete(app->calsync, cv.delete_event, cv.delete_serial);
    if (ed.open && ed.event == cv.delete_event) editor_close();
    cv.detail = -1;
    cv.delete_event = 0;
}

int calendar_typing(void)
{
    return ed.open;
}

static void editor_keys(jd_app *app)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_ESCAPE)) {
        editor_close();
        return;
    }
    if (ctrl && (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_ENTER))) {
        editor_save(app);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        do ed.focus = (ed.focus + (shift ? EF_COUNT - 1 : 1)) % EF_COUNT;
        while (ed.all_day && (ed.focus == EF_TIME0 || ed.focus == EF_TIME1));
        return;
    }
    if (ed.focus == EF_NOTES) jd_editor_input(&app->ui, &ed.notes);
    else if (IsKeyPressed(KEY_ENTER)) editor_save(app);
    else jd_field_input(&ed.f[ed.focus]);
}

/* the editor on a phone's width: each label above its control, not in a column beside it */
static int stacked;

/* a label, and where its control goes: beside it, or (stacked) under it, moving *y */
static float row_label(jd_app *app, const char *label, float x, float *y)
{
    if (stacked) {
        jd_text(&app->ui, label, x, *y, 13, JD_NORD4, 1);
        *y += 20;
        return x;
    }
    jd_text(&app->ui, label, x, *y + 9, 13, JD_NORD4, 1);
    return x + 96;
}

/* a row of choices that wraps when the box is narrow; returns the y below it */
static float choices(jd_app *app, const char *label, float x, float y, float w, const char *const *names, int n, int *sel)
{
    jd_ui *ui = &app->ui;
    float left = row_label(app, label, x, &y);
    float cx = left, cy = y;
    for (int i = 0; i < n; i++) {
        float bw = jd_text_width(ui, names[i], 13, 0) + 22;
        if (cx + bw > x + w) cx = left, cy += 34;
        Rectangle b = {cx, cy, bw, 30};
        int on = *sel == i;
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind;
        DrawRectangleRounded(b, 0.3f, 6, on ? JD_NORD10 : hover ? JD_NORD2 : JD_NORD0);
        jd_text(ui, names[i], b.x + 11, b.y + 8, 13, on ? JD_ON_ACCENT : JD_NORD6, on);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) *sel = i;
        cx += bw + 6;
    }
    return cy + 40;
}

static void editor_draw(jd_app *app, Rectangle page)
{
    jd_ui *ui = &app->ui;
    DrawRectangleRec(page, ColorAlpha(BLACK, 0.35f));
    float w = page.width - 32 < 620 ? page.width - 32 : 620;
    float x = page.x + (page.width - w) / 2, top = page.y + 16;
    float h = page.height - 32;
    DrawRectangleRounded((Rectangle){x, top, w, h}, 0.02f, 8, JD_NORD1);
    float ix = x + 20, iw = w - 40, y = top + 16;
    jd_text(ui, ed.event ? "Edit event" : "New event", ix, y, 18, JD_NORD6, 1);
    y += 36;
    static const char *const placeholders[] = {"Title", "2026-11-19", "09:00", "2026-11-19", "10:00", "Location"};
    for (int i = 0; i < EF_COUNT - 1; i++) ed.f[i].placeholder = placeholders[i];
    stacked = iw < 420;
    if (jd_field_draw(ui, &ed.f[EF_TITLE], (Rectangle){ix, y, iw, 34}, ed.focus == EF_TITLE)) ed.focus = EF_TITLE;
    y += 44;
    jd_text(ui, "All day", ix, y + 6, 13, JD_NORD4, 1);
    if (jd_switch(ui, ix + 96, y + 4, ed.all_day)) ed.all_day = !ed.all_day;
    y += 36;
    float dw = stacked ? (iw - 8) * 0.6f : 130, tw = stacked ? iw - 8 - dw : 80;
    float cx = row_label(app, "Starts", ix, &y);
    if (jd_field_draw(ui, &ed.f[EF_DATE0], (Rectangle){cx, y, dw, 34}, ed.focus == EF_DATE0)) ed.focus = EF_DATE0;
    if (!ed.all_day && jd_field_draw(ui, &ed.f[EF_TIME0], (Rectangle){cx + dw + 8, y, tw, 34}, ed.focus == EF_TIME0))
        ed.focus = EF_TIME0;
    y += 42;
    cx = row_label(app, "Ends", ix, &y);
    if (jd_field_draw(ui, &ed.f[EF_DATE1], (Rectangle){cx, y, dw, 34}, ed.focus == EF_DATE1)) ed.focus = EF_DATE1;
    if (!ed.all_day && jd_field_draw(ui, &ed.f[EF_TIME1], (Rectangle){cx + dw + 8, y, tw, 34}, ed.focus == EF_TIME1))
        ed.focus = EF_TIME1;
    y += 46;
    const char *rep[6];
    int nrep = 5;
    for (int i = 0; i < 5; i++) rep[i] = repeats[i];
    if (ed.rrule_kept) rep[nrep++] = "As it was";
    int rsel = ed.repeat < 0 ? 5 : ed.repeat;
    y = choices(app, "Repeats", ix, y, iw, rep, nrep, &rsel);
    ed.repeat = rsel == 5 ? -1 : rsel;
    const char *al[7];
    int nal = 6;
    for (int i = 0; i < 6; i++) al[i] = alarm_names[i];
    char custom[32];
    if (ed.alarm >= 100) {
        snprintf(custom, sizeof custom, "%d min", ed.alarm - 100);
        al[nal++] = custom;
    }
    int asel = ed.alarm >= 100 ? 6 : ed.alarm;
    y = choices(app, "Reminder", ix, y, iw, al, nal, &asel);
    if (asel < 6) ed.alarm = asel;
    /* the calendar: a click moves to the next writable one */
    int nc;
    jd_calendar_row *cals = app->view ? jd_store_calendars(app->view, &nc) : NULL;
    const char *cname = "?";
    Color ccol = JD_NORD8;
    int at = -1;
    for (int i = 0; cals && i < nc; i++)
        if (cals[i].id == ed.calendar) at = i, cname = cals[i].name ? cals[i].name : "Calendar", ccol = hex_color(cals[i].color);
    float cbx = row_label(app, "Calendar", ix, &y);
    Rectangle cb = {cbx, y, ix + iw - cbx < 320 ? ix + iw - cbx : 320, 30};
    int chover = CheckCollisionPointRec(GetMousePosition(), cb) && !app->dialog.kind;
    DrawRectangleRounded(cb, 0.3f, 6, chover ? JD_NORD2 : JD_NORD0);
    DrawCircle((int)(cb.x + 14), (int)(cb.y + 15), 5, ccol);
    jd_text_fit(ui, cname, cb.x + 26, cb.y + 8, cb.width - 50, 13, JD_NORD6, 0);
    jd_icon("chevron-right", cb.x + cb.width - 22, cb.y + 7, 16, JD_NORD4);
    if (chover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && cals)
        for (int k = 1; k <= nc; k++) {
            int i = (at + k + nc) % nc;
            if (!cals[i].readonly) {
                ed.calendar = cals[i].id;
                break;
            }
        }
    if (cals) jd_calendar_rows_free(cals, nc);
    y += 40;
    if (jd_field_draw(ui, &ed.f[EF_LOCATION], (Rectangle){ix, y, iw, 34}, ed.focus == EF_LOCATION)) ed.focus = EF_LOCATION;
    y += 44;
    float bottom = top + h - 64;
    if (bottom - y > 40) {
        if (jd_editor_draw(ui, &ed.notes, (Rectangle){ix, y, iw, bottom - y}, ed.focus == EF_NOTES)) ed.focus = EF_NOTES;
        if (!ed.notes.len && ed.focus != EF_NOTES) jd_text(ui, "Notes", ix + 10, y + 9, 14, JD_MUTED, 0);
    }
    y = bottom + 14;
    if (jd_button(ui, ed.saving ? "Saving..." : "Save", (Rectangle){ix, y, 100, 36}, 1)) editor_save(app);
    if (jd_button(ui, "Cancel", (Rectangle){ix + 110, y, 100, 36}, 0)) {
        editor_close();
        return;
    }
    float ex = ix + 220;
    if (ed.event && jd_button(ui, "Delete", (Rectangle){ex, y, 100, 36}, 0)) ask_delete(app, ed.event);
    if (ed.event) ex += 110;
    const char *note = ed.error[0] ? ed.error : app->ui.touch ? "" : "Tab moves between fields; Ctrl+S saves, Esc cancels.";
    /* a phone's width has no room beside the buttons: an error goes above them */
    if (ix + iw - ex > 160) jd_text_fit(ui, note, ex + 4, y + 11, ix + iw - ex - 4, 13, ed.error[0] ? JD_NORD11 : JD_MUTED, 0);
    else if (ed.error[0]) jd_text_fit(ui, ed.error, ix, y - 22, iw, 13, JD_NORD11, 0);
}

/* ---- the views ---- */

void calendar_open(jd_app *app)
{
    for (int i = 0; i < app->ntabs; i++)
        if (app->tabs[i].kind == TAB_CALENDAR) {
            app->tab = i;
            return;
        }
    tab_add(app, TAB_CALENDAR);
    if (!cv.opened) {
        cv.view = app->ui.touch ? V_AGENDA : V_WEEK;
        cv.opened = 1;
    }
    cv.anchor = local_midnight(jd_now());
    cv.stale = 1, cv.detail = -1, cv.sel = -1, cv.scrolled_for = -1;
}

static void step(int dir)
{
    if (cv.view == V_AGENDA) cv.anchor = add_days(cv.anchor, dir * 7);
    else if (cv.view == V_WEEK) cv.anchor = add_days(cv.anchor, dir * 7);
    else {
        struct tm tm = local_tm(cv.anchor);
        cv.anchor = mk_local(tm.tm_year + 1900, tm.tm_mon + 1 + dir, 1, 0, 0);
    }
    cv.detail = -1, cv.sel = -1;
}

static void set_view(int v)
{
    if (cv.view == v) return;
    cv.view = v;
    cv.detail = -1, cv.sel = -1, cv.scrolled_for = -1, cv.scroll = 0;
}

void calendar_keys(jd_app *app)
{
    if (ed.open) {
        editor_keys(app);
        return;
    }
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (cv.detail >= 0) cv.detail = -1;
        else tab_close(app, app->tab);
        return;
    }
    int sel = cv.detail >= 0 ? cv.detail : cv.sel;
    if (IsKeyPressed(KEY_A)) set_view(V_AGENDA);
    else if (IsKeyPressed(KEY_W)) set_view(V_WEEK);
    else if (IsKeyPressed(KEY_M)) set_view(V_MONTH);
    else if (IsKeyPressed(KEY_T)) cv.anchor = local_midnight(jd_now()), cv.detail = -1, cv.sel = -1;
    else if (IsKeyPressed(KEY_L) || IsKeyPressed(KEY_RIGHT)) {
        if (cv.view == V_AGENDA || shift) step(1);
        else cv.anchor = add_days(cv.anchor, 1), cv.sel = -1;
    } else if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_LEFT)) {
        if (cv.view == V_AGENDA || shift) step(-1);
        else cv.anchor = add_days(cv.anchor, -1), cv.sel = -1;
    } else if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) {
        if (cv.view == V_MONTH) cv.anchor = add_days(cv.anchor, 7), cv.sel = -1;
        else if (cv.nocc) {
            /* the next occurrence on or after the selected day */
            int i = cv.sel + 1;
            if (cv.view == V_WEEK && cv.sel < 0)
                while (i < cv.nocc && end_day(&cv.occs[i]) <= cv.anchor) i++;
            if (cv.view == V_WEEK) while (i < cv.nocc && !on_day(&cv.occs[i], cv.anchor)) i++;
            if (i < cv.nocc) cv.sel = i;
        }
    } else if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) {
        if (cv.view == V_MONTH) cv.anchor = add_days(cv.anchor, -7), cv.sel = -1;
        else if (cv.sel > 0) {
            int i = cv.sel - 1;
            if (cv.view == V_WEEK) while (i >= 0 && !on_day(&cv.occs[i], cv.anchor)) i--;
            if (i >= 0) cv.sel = i;
        }
    } else if (IsKeyPressed(KEY_ENTER) && sel >= 0) {
        if (cv.detail >= 0) editor_edit(app, cv.occs[cv.detail].event);
        else cv.detail = cv.sel;
    } else if (IsKeyPressed(KEY_E) && sel >= 0) editor_edit(app, cv.occs[sel].event);
    else if ((IsKeyPressed(KEY_D) || IsKeyPressed(KEY_DELETE)) && sel >= 0) ask_delete(app, cv.occs[sel].event);
    else if (IsKeyPressed(KEY_N)) {
        GetCharPressed();
        editor_new(app, cv.anchor + 9 * 3600, 0);
    } else if (IsKeyPressed(KEY_R)) {
        if (app->calsync) jd_calsync_now(app->calsync);
        else calendar_start(app);
    }
}

/* where the details box was drawn last frame: the views under it take no clicks */
static Rectangle detail_box;
/* the y below the header: the details box goes under it */
static float header_bottom;

static int clicked(Rectangle b, Rectangle clip, jd_app *app)
{
    Vector2 m = GetMousePosition();
    return !app->dialog.kind && !ed.open && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, b) &&
           CheckCollisionPointRec(m, clip) && !(cv.detail >= 0 && CheckCollisionPointRec(m, detail_box));
}

static void event_chip(jd_app *app, int i, Rectangle b, Rectangle clip, int timed_block)
{
    jd_ui *ui = &app->ui;
    occ *o = &cv.occs[i];
    int chosen = i == cv.sel || i == cv.detail;
    int hover = CheckCollisionPointRec(GetMousePosition(), b) && CheckCollisionPointRec(GetMousePosition(), clip) &&
                !app->dialog.kind && !ed.open;
    Color fill = ColorAlpha(o->color, chosen ? 0.55f : hover ? 0.4f : 0.28f);
    DrawRectangleRounded(b, timed_block ? 0.12f : 0.3f, 6, fill);
    char when[48] = "";
    if (!o->all_day) fmt(o->start, "%H:%M", when, sizeof when);
    const char *title = o->summary && o->summary[0] ? o->summary : "(no title)";
    if (timed_block && b.height > 34) {
        jd_text_fit(ui, title, b.x + 6, b.y + 4, b.width - 10, 13, JD_NORD6, 1);
        char range_text[64];
        time_range(o, range_text, sizeof range_text);
        jd_text_fit(ui, range_text, b.x + 6, b.y + 20, b.width - 10, 12, JD_NORD4, 0);
        if (o->location && b.height > 52) jd_text_fit(ui, o->location, b.x + 6, b.y + 35, b.width - 10, 12, JD_NORD4, 0);
    } else {
        char line[300];
        snprintf(line, sizeof line, "%s%s%s", when, when[0] ? " " : "", title);
        jd_text_fit(ui, line, b.x + 6, b.y + (b.height - 13) / 2, b.width - 10, 12, JD_NORD6, 0);
    }
    if (clicked(b, clip, app)) {
        if (jd_click_count() >= 2) editor_edit(app, o->event);
        else cv.detail = i, cv.sel = i;
    }
}

static float header(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    float x = r.x + 16, y = r.y + 12;
    char title[96];
    if (cv.view == V_MONTH) fmt(cv.anchor, "%B %Y", title, sizeof title);
    else if (cv.view == V_WEEK) {
        int64_t ws = week_start(cv.anchor), we = add_days(ws, 6);
        char a[32], b[32];
        fmt(ws, "%d %b", a, sizeof a);
        fmt(we, "%d %b %Y", b, sizeof b);
        snprintf(title, sizeof title, "%s – %s", a, b);
    } else fmt(cv.anchor, "From %a %d %b %Y", title, sizeof title);
    if (jd_icon_button(ui, "chevron-left", NULL, (Rectangle){x, y, 34, 32}, 0)) step(-1);
    if (jd_icon_button(ui, "chevron-right", NULL, (Rectangle){x + 38, y, 34, 32}, 0)) step(1);
    if (jd_button(ui, "Today", (Rectangle){x + 78, y, 70, 32}, 0)) cv.anchor = local_midnight(jd_now()), cv.detail = -1;
    static const char *const views[] = {"Agenda", "Week", "Month"};
    float segw = 4;
    for (int i = 0; i < 3; i++) segw += jd_text_width(ui, views[i], 13, 1) + 28;
    /* a phone's width: the buttons, then the views, then the title, each on a line of its own */
    int narrow = r.width < 160 + 220 + segw + 112 + 40;
    float nw = narrow ? 40 : 112, nx = r.x + r.width - 16 - nw;
    if (jd_icon_button(ui, "pencil", narrow ? NULL : "New", (Rectangle){nx, y, nw, 32}, 1)) editor_new(app, cv.anchor + 9 * 3600, 0);
    float sx = narrow ? x : nx - segw - 12, sy = narrow ? y + 40 : y;
    int v = jd_segmented(ui, sx, sy, views, 3, cv.view);
    if (v != cv.view) set_view(v);
    if (!narrow) {
        jd_text_fit(ui, title, x + 160, y + 7, sx - x - 170, 17, JD_NORD6, 1);
        return y + 44;
    }
    jd_text_fit(ui, title, x, y + 82, r.width - 32, 16, JD_NORD6, 1);
    return y + 108;
}

static void empty_note(jd_app *app, Rectangle r, float y)
{
    const char *why = app->prefs.cal_provider ? (calendar_error() ? calendar_error() : "No events here.")
                                              : "No calendar set up yet: Settings, Calendar connects a CalDAV server or Google.";
    jd_text_fit(&app->ui, why, r.x + 16, y, r.width - 32, 14, calendar_error() ? JD_NORD13 : JD_MUTED, 0);
    if (!app->prefs.cal_provider && jd_button(&app->ui, "Open Settings", (Rectangle){r.x + 16, y + 30, 140, 34}, 1))
        settings_show_calendar(app);
}

static void agenda(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    float content = 0, sel_y = -1;
    int64_t day = -1;
    for (int i = 0; i < cv.nocc; i++) {
        int64_t d = first_day(&cv.occs[i]) < cv.anchor ? cv.anchor : first_day(&cv.occs[i]);
        if (d != day) content += 40, day = d;
        if (i == cv.sel) sel_y = content;
        content += 46;
    }
    if (CheckCollisionPointRec(GetMousePosition(), r) && !ed.open) cv.scroll -= jd_wheel_y(46);
    /* j and k keep the chosen event in sight */
    static int followed = -1;
    if (cv.sel != followed && sel_y >= 0) {
        if (sel_y < cv.scroll + 40) cv.scroll = sel_y - 40;
        if (sel_y + 50 > cv.scroll + r.height) cv.scroll = sel_y + 50 - r.height;
    }
    followed = cv.sel;
    if (cv.scroll > content - r.height + 20) cv.scroll = content - r.height + 20;
    if (cv.scroll < 0) cv.scroll = 0;
    if (!cv.nocc) {
        empty_note(app, r, r.y + 12);
        return;
    }
    jd_scissor(r.x, r.y, r.width, r.height);
    float y = r.y + 4 - cv.scroll;
    int64_t today = local_midnight(jd_now());
    day = -1;
    for (int i = 0; i < cv.nocc; i++) {
        occ *o = &cv.occs[i];
        int64_t d = first_day(o) < cv.anchor ? cv.anchor : first_day(o);
        if (d != day) {
            day = d;
            char label[64];
            fmt(d, "%A %d %B", label, sizeof label);
            jd_text(ui, label, r.x + 16, y + 14, 14, d == today ? JD_NORD8 : JD_NORD4, 1);
            y += 40;
        }
        Rectangle row = {r.x + 8, y, r.width - 24, 42};
        int chosen = i == cv.sel;
        int hover = CheckCollisionPointRec(GetMousePosition(), row) && CheckCollisionPointRec(GetMousePosition(), r) && !ed.open;
        if (chosen || hover) DrawRectangleRounded(row, 0.2f, 6, chosen ? JD_NORD2 : JD_NORD1);
        char when[48];
        time_range(o, when, sizeof when);
        jd_text_fit(ui, when, row.x + 10, row.y + 13, 110, 13, JD_NORD4, 0);
        DrawCircle((int)(row.x + 132), (int)(row.y + 21), 5, o->color);
        const char *title = o->summary && o->summary[0] ? o->summary : "(no title)";
        float tw = row.width - 150;
        if (o->location && o->location[0]) {
            jd_text_fit(ui, title, row.x + 146, row.y + 5, tw, 14, JD_NORD6, 1);
            jd_text_fit(ui, o->location, row.x + 146, row.y + 23, tw, 12, JD_MUTED, 0);
        } else {
            jd_text_fit(ui, title, row.x + 146, row.y + 13, tw, 14, JD_NORD6, 1);
        }
        if (clicked(row, r, app)) {
            if (jd_click_count() >= 2) editor_edit(app, o->event);
            else cv.sel = i, cv.detail = i;
        }
        y += 46;
    }
    jd_scissor_end();
    jd_scrollbar(r, content + 20, &cv.scroll);
}

static void week(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    int64_t ws = week_start(cv.anchor), today = local_midnight(jd_now());
    float gutter = 52, colw = (r.width - gutter - 12) / 7;
    /* day names, the chosen day underlined */
    for (int d = 0; d < 7; d++) {
        int64_t day = add_days(ws, d);
        char label[32];
        fmt(day, colw > 90 ? "%a %d" : "%d", label, sizeof label);
        float cx = r.x + gutter + colw * (float)d;
        Rectangle head = {cx, r.y, colw, 28};
        if (day == today) DrawRectangleRounded((Rectangle){cx + 2, r.y + 2, colw - 4, 24}, 0.3f, 6, ColorAlpha(JD_NORD10, 0.35f));
        jd_text_fit(ui, label, cx + 8, r.y + 7, colw - 12, 13, day == today ? JD_NORD6 : JD_NORD4, day == cv.anchor);
        if (day == cv.anchor) DrawRectangle((int)cx + 4, (int)r.y + 27, (int)colw - 8, 2, JD_NORD8);
        if (clicked(head, r, app)) cv.anchor = day, cv.sel = -1;
    }
    float y = r.y + 32;
    /* all-day events, up to three lines */
    int lanes = 0;
    int lane_of[512];
    for (int i = 0; i < cv.nocc && i < 512; i++) lane_of[i] = -1;
    for (int i = 0; i < cv.nocc && i < 512; i++) {
        if (!cv.occs[i].all_day && end_day(&cv.occs[i]) - first_day(&cv.occs[i]) <= 86400 + 3600) continue;
        int l = 0;
        for (;; l++) {
            int clash = 0;
            for (int k = 0; k < i && !clash; k++)
                clash = lane_of[k] == l && first_day(&cv.occs[k]) < end_day(&cv.occs[i]) && first_day(&cv.occs[i]) < end_day(&cv.occs[k]);
            if (!clash) break;
        }
        lane_of[i] = l;
        if (l + 1 > lanes) lanes = l + 1;
    }
    int shown = lanes > 3 ? 3 : lanes;
    for (int i = 0; i < cv.nocc && i < 512; i++) {
        if (lane_of[i] < 0 || lane_of[i] >= 3) continue;
        int64_t a = first_day(&cv.occs[i]), b = end_day(&cv.occs[i]);
        int d0 = 0, d1 = 7;
        while (d0 < 7 && add_days(ws, d0 + 1) <= a) d0++;
        while (d1 > 0 && add_days(ws, d1 - 1) >= b) d1--;
        if (d1 <= d0) continue;
        Rectangle chip = {r.x + gutter + colw * (float)d0 + 2, y + 22 * (float)lane_of[i], colw * (float)(d1 - d0) - 4, 20};
        event_chip(app, i, chip, r, 0);
    }
    if (lanes > 3) jd_text(ui, "more", r.x + 8, y + 48, 11, JD_MUTED, 0);
    y += 22 * (float)shown + (shown ? 6 : 0);
    DrawRectangle((int)r.x, (int)y, (int)r.width, 1, JD_NORD1);
    Rectangle grid = {r.x, y + 1, r.width, r.y + r.height - y - 1};
    float content = 24 * HOUR_H + 8;
    if (cv.scrolled_for != V_WEEK) {
        cv.scroll = 8 * HOUR_H - 8;
        cv.scrolled_for = V_WEEK;
    }
    if (CheckCollisionPointRec(GetMousePosition(), grid) && !ed.open) cv.scroll -= jd_wheel_y(HOUR_H);
    if (cv.scroll > content - grid.height) cv.scroll = content - grid.height;
    if (cv.scroll < 0) cv.scroll = 0;
    jd_scissor(grid.x, grid.y, grid.width, grid.height);
    float gy = grid.y + 6 - cv.scroll;
    for (int h = 0; h < 24; h++) {
        float ly = gy + (float)h * HOUR_H;
        DrawRectangle((int)(r.x + gutter), (int)ly, (int)(r.width - gutter - 12), 1, JD_NORD1);
        char hh[8];
        snprintf(hh, sizeof hh, "%02d:00", h);
        if (h) jd_text(ui, hh, r.x + 8, ly - 7, 11, JD_MUTED, 0);
    }
    for (int d = 0; d < 7; d++) {
        int64_t day = add_days(ws, d), next = add_days(day, 1);
        float cx = r.x + gutter + colw * (float)d;
        DrawRectangle((int)cx, (int)grid.y, 1, (int)grid.height + 0, JD_NORD1);
        /* a double click on the empty column starts an event at that half hour */
        Rectangle col = {cx, gy, colw, 24 * HOUR_H};
        if (clicked(col, grid, app) && jd_click_count() >= 2) {
            float rel = (GetMousePosition().y - gy) / HOUR_H;
            int half = (int)(rel * 2);
            editor_new(app, day + half * 1800, 0);
        }
        /* overlapping events share the column side by side */
        int idx[128], n = 0;
        for (int i = 0; i < cv.nocc && n < 128; i++) {
            occ *o = &cv.occs[i];
            if (o->all_day || (i < 512 && lane_of[i] >= 0) || !(o->start < next && (o->end > day || (o->end == o->start && o->start >= day))))
                continue;
            idx[n++] = i;
        }
        int colof[128], ncols[128];
        for (int a = 0; a < n; a++) {
            int c = 0;
            for (;; c++) {
                int clash = 0;
                for (int b = 0; b < a && !clash; b++) {
                    occ *x = &cv.occs[idx[a]], *z = &cv.occs[idx[b]];
                    int64_t xe = x->end > x->start ? x->end : x->start + 1800, ze = z->end > z->start ? z->end : z->start + 1800;
                    clash = colof[b] == c && x->start < ze && z->start < xe;
                }
                if (!clash) break;
            }
            colof[a] = c;
        }
        for (int a = 0; a < n; a++) {
            ncols[a] = colof[a] + 1;
            occ *x = &cv.occs[idx[a]];
            int64_t xe = x->end > x->start ? x->end : x->start + 1800;
            for (int b = 0; b < n; b++) {
                occ *z = &cv.occs[idx[b]];
                int64_t ze = z->end > z->start ? z->end : z->start + 1800;
                if (x->start < ze && z->start < xe && colof[b] + 1 > ncols[a]) ncols[a] = colof[b] + 1;
            }
        }
        for (int a = 0; a < n; a++) {
            occ *o = &cv.occs[idx[a]];
            int64_t s = o->start < day ? day : o->start;
            int64_t e = o->end > next ? next : o->end > o->start ? o->end : o->start + 1800;
            float top = gy + (float)(s - day) / 3600.0f * HOUR_H, bot = gy + (float)(e - day) / 3600.0f * HOUR_H;
            if (bot - top < 18) bot = top + 18;
            float w = (colw - 6) / (float)ncols[a];
            Rectangle b = {cx + 3 + w * (float)colof[a], top + 1, w - 2, bot - top - 2};
            event_chip(app, idx[a], b, grid, 1);
        }
        if (day == today) {
            float ny = gy + (float)(jd_now() - day) / 3600.0f * HOUR_H;
            DrawRectangle((int)cx, (int)ny, (int)colw, 2, JD_NORD11);
            DrawCircle((int)cx, (int)ny + 1, 4, JD_NORD11);
        }
    }
    jd_scissor_end();
    jd_scrollbar(grid, content, &cv.scroll);
}

static void month(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    int64_t first = cv.from, today = local_midnight(jd_now());
    int cur_month = local_tm(cv.anchor).tm_mon;
    float cw = r.width / 7, top = r.y + 24, ch = (r.height - 24) / 6;
    for (int d = 0; d < 7; d++) {
        char name[16];
        fmt(add_days(first, d), "%a", name, sizeof name);
        jd_text_fit(ui, name, r.x + cw * (float)d + 8, r.y + 4, cw - 10, 12, JD_MUTED, 1);
    }
    int lines = (int)((ch - 26) / 17);
    if (lines < 0) lines = 0;
    for (int cell = 0; cell < 42; cell++) {
        int64_t day = add_days(first, cell);
        float x = r.x + cw * (float)(cell % 7), y = top + ch * (float)(cell / 7);
        Rectangle box = {x, y, cw, ch};
        DrawRectangleLinesEx(box, 0.5f, JD_NORD1);
        int inside = local_tm(day).tm_mon == cur_month;
        if (day == cv.anchor) DrawRectangleRec((Rectangle){x + 1, y + 1, cw - 2, ch - 2}, ColorAlpha(JD_NORD2, 0.5f));
        char num[8];
        fmt(day, "%e", num, sizeof num);
        if (day == today) DrawCircle((int)(x + 16), (int)(y + 13), 11, JD_NORD10);
        jd_text(ui, num[0] == ' ' ? num + 1 : num, x + (num[0] == ' ' ? 12 : 8), y + 6, 12,
                day == today ? JD_ON_ACCENT : inside ? JD_NORD6 : JD_MUTED, day == today);
        int count = 0;
        for (int i = 0; i < cv.nocc; i++) count += on_day(&cv.occs[i], day);
        /* the last line says how many more there are when not all fit */
        int room = count > lines ? lines - 1 : lines, shown = 0;
        for (int i = 0; i < cv.nocc && shown < room; i++) {
            if (!on_day(&cv.occs[i], day)) continue;
            event_chip(app, i, (Rectangle){x + 3, y + 24 + 17 * (float)shown, cw - 6, 16}, r, 0);
            shown++;
        }
        int more = count - shown;
        if (more) {
            char m[24];
            snprintf(m, sizeof m, "+%d more", more);
            jd_text(ui, m, x + 6, y + ch - 15, 11, JD_NORD4, 0);
        }
        Rectangle hit = {x, y, cw, 24};
        if (clicked(hit, r, app) || (clicked(box, r, app) && cv.detail < 0)) {
            if (jd_click_count() >= 2) editor_new(app, day + 9 * 3600, 0);
            else if (day != cv.anchor) cv.anchor = day, cv.sel = -1;
        }
    }
}

/* the chosen occurrence: when, where, notes, and what can be done */
static void details(jd_app *app, Rectangle page)
{
    if (cv.detail < 0 || cv.detail >= cv.nocc) return;
    jd_ui *ui = &app->ui;
    occ *o = &cv.occs[cv.detail];
    jd_event_row *row = app->view ? jd_store_event(app->view, o->event) : NULL;
    if (!row) {
        cv.detail = -1;
        return;
    }
    jd_ical_event e = {.alarm = -1};
    jd_ical_read(row->ics, &e);
    float w = page.width - 32 < 420 ? page.width - 32 : 420;
    float x = page.x + page.width - w - 16, y = header_bottom + 6;
    float h = 170 + (e.description ? 60 : 0) + (o->location ? 22 : 0);
    Rectangle box = {x, y, w, h};
    detail_box = box;
    DrawRectangleRounded((Rectangle){x + 2, y + 3, w, h}, 0.04f, 6, ColorAlpha(BLACK, 0.3f));
    DrawRectangleRounded(box, 0.04f, 6, JD_NORD1);
    DrawCircle((int)(x + 22), (int)(y + 26), 6, o->color);
    jd_text_fit(ui, o->summary && o->summary[0] ? o->summary : "(no title)", x + 36, y + 17, w - 80, 16, JD_NORD6, 1);
    if (jd_icon_button(ui, "x", NULL, (Rectangle){x + w - 40, y + 10, 30, 30}, 0)) cv.detail = -1;
    float ty = y + 50;
    char day[64], when[64], line[160];
    fmt(o->all_day ? first_day(o) : o->start, "%A %d %B %Y", day, sizeof day);
    time_range(o, when, sizeof when);
    snprintf(line, sizeof line, "%s, %s%s", day, when, o->recurring ? " · repeats" : "");
    jd_text_fit(ui, line, x + 20, ty, w - 40, 13, JD_NORD4, 0);
    ty += 22;
    if (o->location) {
        jd_text_fit(ui, o->location, x + 20, ty, w - 40, 13, JD_NORD4, 0);
        ty += 22;
    }
    jd_text_fit(ui, row->calendar_name ? row->calendar_name : "", x + 20, ty, w - 40, 12, JD_MUTED, 0);
    ty += 24;
    if (e.description) {
        int n;
        int *starts = jd_wrap(ui, e.description, w - 40, 13, &n);
        for (int i = 0; i < n && i < 3; i++)
            jd_text_range(ui, e.description, starts[i], i + 1 < n ? starts[i + 1] : (int)strlen(e.description), x + 20,
                          ty + 18 * (float)i, 13, JD_NORD6, 0);
        free(starts);
        ty += 60;
    }
    if (!row->readonly) {
        if (jd_icon_button(ui, "pencil", "Edit", (Rectangle){x + 20, y + h - 52, 100, 34}, 1)) editor_edit(app, o->event);
        if (jd_icon_button(ui, "trash-2", "Delete", (Rectangle){x + 130, y + h - 52, 110, 34}, 0)) ask_delete(app, o->event);
    } else {
        jd_text(ui, "Read-only calendar", x + 20, y + h - 42, 13, JD_MUTED, 0);
    }
    jd_ical_event_free(&e);
    jd_event_rows_free(row, 1);
}

void calendar_draw(jd_app *app, Rectangle r)
{
    /* the editor takes the page: nothing under it may take a click */
    if (ed.open) {
        editor_draw(app, r);
        return;
    }
    reload(app);
    float y = header_bottom = header(app, r);
    Rectangle body = {r.x + 8, y, r.width - 16, r.y + r.height - y};
    if (cv.view == V_AGENDA) agenda(app, body);
    else if (cv.view == V_WEEK) week(app, body);
    else month(app, body);
    if (!cv.nocc && cv.view != V_AGENDA && !app->prefs.cal_provider) empty_note(app, body, body.y + 40);
    details(app, r);
}

/* ---- invitations in mail ---- */

static const char *partstat_label(const char *p)
{
    if (!p) return NULL;
    if (strcasecmp(p, "ACCEPTED") == 0) return "You accepted.";
    if (strcasecmp(p, "TENTATIVE") == 0) return "You answered maybe.";
    if (strcasecmp(p, "DECLINED") == 0) return "You declined.";
    return NULL;
}

float invite_card(jd_app *app, const char *ics, float x, float y, float w, Rectangle clip)
{
    jd_invite inv;
    if (jd_invite_read(ics, app->account.address, &inv) != 0) return y;
    jd_ui *ui = &app->ui;
    float h = inv.method == JD_ITIP_REQUEST && inv.partstat ? 118 : 86;
    Rectangle box = {x, y, w, h};
    DrawRectangleRounded(box, 0.06f, 6, JD_NORD1);
    jd_icon("calendar", x + 14, y + 14, 18, JD_NORD8);
    const char *kind = inv.method == JD_ITIP_REQUEST ? "Invitation" : inv.method == JD_ITIP_CANCEL ? "Cancelled"
                       : inv.method == JD_ITIP_REPLY ? "Answer" : "Event";
    char head[300];
    snprintf(head, sizeof head, "%s: %s", kind, inv.event.summary ? inv.event.summary : "(no title)");
    jd_text_fit(ui, head, x + 42, y + 14, w - 56, 15, JD_NORD6, 1);
    occ o = {.start = inv.event.start, .end = inv.event.end, .all_day = inv.event.all_day};
    char day[64], when[64], line[400];
    fmt(o.all_day ? first_day(&o) : o.start, "%a %d %b %Y", day, sizeof day);
    time_range(&o, when, sizeof when);
    snprintf(line, sizeof line, "%s, %s%s%s%s", day, when, inv.event.rrule ? " · repeats" : "",
             inv.event.location ? " · " : "", inv.event.location ? inv.event.location : "");
    jd_text_fit(ui, line, x + 42, y + 36, w - 56, 13, JD_NORD4, 0);
    char who[300] = "";
    if (inv.method == JD_ITIP_REPLY && inv.replier)
        snprintf(who, sizeof who, "%s %s.", inv.replier,
                 strcasecmp(inv.reply_partstat ? inv.reply_partstat : "", "ACCEPTED") == 0 ? "accepted"
                 : strcasecmp(inv.reply_partstat ? inv.reply_partstat : "", "DECLINED") == 0 ? "declined"
                 : strcasecmp(inv.reply_partstat ? inv.reply_partstat : "", "TENTATIVE") == 0 ? "answered maybe" : "answered");
    else if (inv.organizer)
        snprintf(who, sizeof who, "From %s", inv.organizer_name ? inv.organizer_name : inv.organizer);
    jd_text_fit(ui, who, x + 42, y + 56, w - 56, 12, JD_MUTED, 0);
    if (inv.method == JD_ITIP_REQUEST && inv.partstat) {
        const char *said = partstat_label(inv.partstat);
        jd_event_row *have = app->view && inv.event.uid ? jd_store_event_by_uid(app->view, inv.event.uid) : NULL;
        char now_said[64] = "";
        if (have) {
            jd_invite mine;
            if (jd_invite_read(have->ics, app->account.address, &mine) == 0 && partstat_label(mine.partstat))
                snprintf(now_said, sizeof now_said, "%s", partstat_label(mine.partstat));
            jd_invite_free(&mine);
            jd_event_rows_free(have, 1);
        }
        static const char *const labels[] = {"Accept", "Maybe", "Decline"};
        static const char *const answers[] = {"ACCEPTED", "TENTATIVE", "DECLINED"};
        float bx = x + 42;
        for (int i = 0; i < 3; i++) {
            Rectangle b = {bx, y + 78, i == 0 ? 90 : 84, 30};
            int over = CheckCollisionPointRec(GetMousePosition(), clip);
            if (jd_button(ui, labels[i], b, i == 0) && over && !app->dialog.kind) {
                if (!app->calsync) notify_user(app, "No calendar is connected: set one up in Settings, Calendar.");
                else {
                    cv.respond_serial = ++cv.serials;
                    jd_calsync_respond(app->calsync, ics, app->account.address, answers[i], cv.respond_serial);
                    notify_user(app, "Answering...");
                }
            }
            bx += b.width + 8;
        }
        const char *state = now_said[0] ? now_said : said;
        if (state) jd_text_fit(ui, state, bx + 6, y + 86, x + w - bx - 16, 13, JD_NORD4, 0);
    } else if (inv.method == JD_ITIP_CANCEL && inv.event.uid && app->view) {
        jd_event_row *have = jd_store_event_by_uid(app->view, inv.event.uid);
        if (have) {
            int64_t id = have->id;
            jd_event_rows_free(have, 1);
            if (jd_button(ui, "Remove from calendar", (Rectangle){x + w - 196, y + 46, 180, 30}, 0) &&
                CheckCollisionPointRec(GetMousePosition(), clip))
                ask_delete(app, id);
        }
    }
    jd_invite_free(&inv);
    return y + h + 12;
}

/* ---- Settings, Calendar ---- */

static void fill_settings(jd_app *app)
{
    jd_field_set(&cf[0], app->prefs.cal_url);
    jd_field_set(&cf[1], app->prefs.cal_username);
    jd_field_set(&cf[2], NULL);
    cf[2].secret = 1;
    cf[0].placeholder = "https://cloud.example.org/remote.php/dav";
    cf[1].placeholder = "Username";
    cf[2].placeholder = "Password (kept in the keystore)";
    cf_provider = app->prefs.cal_provider;
    cf_filled = 1;
}

static void forget_calendars(jd_app *app)
{
    if (app->view) jd_store_calendars_keep(app->view, NULL, 0);
    cv.stale = 1, rem_stale = 1;
}

static void save_caldav(jd_app *app)
{
    if (!cf[0].len || !cf[1].len) {
        snprintf(settings_note, sizeof settings_note, "The server address and the username are needed.");
        return;
    }
    char *key = calendar_secret_key(app);
    if (cf[2].len && plat_secret_save(key, cf[2].text) != 0) {
        snprintf(settings_note, sizeof settings_note, "The keystore did not take the password.");
        free(key);
        return;
    }
    char *had = plat_secret_load(key);
    free(key);
    if (!had) {
        snprintf(settings_note, sizeof settings_note, "Enter the password too.");
        return;
    }
    jd_wipe(had, strlen(had));
    free(had);
    jd_field_set(&cf[2], NULL);
    if (app->prefs.cal_provider != 1 || !jd_streq(app->prefs.cal_url, cf[0].text) ||
        !jd_streq(app->prefs.cal_username, cf[1].text))
        forget_calendars(app);
    app->prefs.cal_provider = 1;
    free(app->prefs.cal_url), free(app->prefs.cal_username);
    app->prefs.cal_url = jd_strdup(cf[0].text);
    app->prefs.cal_username = jd_strdup(cf[1].text);
    save_prefs(app);
    calendar_start(app);
    snprintf(settings_note, sizeof settings_note, "Saved; connecting to the calendars.");
}

static void google_sign_in(jd_app *app)
{
    if (!cf[1].len || !strchr(cf[1].text, '@')) {
        snprintf(settings_note, sizeof settings_note, "Enter the Google address first.");
        return;
    }
    jd_oauth_stop(signin);
    char *url = NULL, err[300];
    signin = jd_oauth_start(cf[1].text, &url, err, sizeof err);
    if (!signin) {
        snprintf(settings_note, sizeof settings_note, "%s", err);
        return;
    }
    if (plat_open_uri(url) != 0) {
        SetClipboardText(url);
        snprintf(settings_note, sizeof settings_note, "No browser opened: the sign-in address is on the clipboard.");
    } else {
        snprintf(settings_note, sizeof settings_note, "Finish signing in in the browser...");
    }
    free(url);
}

static void signin_tick(jd_app *app)
{
    if (!signin) return;
    char *refresh = NULL, err[512];
    int r = jd_oauth_poll(signin, &refresh, err, sizeof err);
    if (r == JD_OAUTH_WAITING) return;
    jd_oauth_stop(signin);
    signin = NULL;
    if (r != JD_OAUTH_DONE) {
        snprintf(settings_note, sizeof settings_note, "%s", err);
        return;
    }
    char *key = calendar_secret_key(app);
    int saved = plat_secret_save(key, refresh) == 0;
    free(key);
    jd_wipe(refresh, strlen(refresh));
    free(refresh);
    if (!saved) {
        snprintf(settings_note, sizeof settings_note, "Signed in, but the keystore did not take the sign-in.");
        return;
    }
    if (app->prefs.cal_provider != 2 || !jd_streq(app->prefs.cal_username, cf[1].text)) forget_calendars(app);
    app->prefs.cal_provider = 2;
    free(app->prefs.cal_username);
    app->prefs.cal_username = jd_strdup(cf[1].text);
    save_prefs(app);
    calendar_start(app);
    snprintf(settings_note, sizeof settings_note, "Signed in with Google; reading the calendars.");
    SetWindowFocused();
}

int calendar_settings_keys(jd_app *app)
{
    int f = app->settings_focus;
    if (f < CF_URL || f >= CF_COUNT) return 0;
    if (IsKeyPressed(KEY_TAB)) {
        int first = cf_provider == 2 ? CF_USER : CF_URL, last = cf_provider == 2 ? CF_USER : CF_PASSWORD;
        app->settings_focus = f >= last ? first : f + 1;
    } else if (IsKeyPressed(KEY_ENTER)) {
        if (cf_provider == 2) google_sign_in(app);
        else save_caldav(app);
    } else {
        jd_field_input(&cf[f - CF_URL]);
    }
    return 1;
}

static float calendar_list(jd_app *app, float x, float y, float w)
{
    int n;
    jd_calendar_row *c = app->view ? jd_store_calendars(app->view, &n) : NULL;
    if (!c || !n) {
        if (c) jd_calendar_rows_free(c, n);
        return y;
    }
    jd_text(&app->ui, "Shown calendars", x, y, 15, JD_NORD6, 1);
    y += 28;
    for (int i = 0; i < n; i++) {
        Rectangle row = {x, y, w > 480 ? 480 : w, 30};
        int hover = CheckCollisionPointRec(GetMousePosition(), row) && !app->dialog.kind;
        if (hover) DrawRectangleRounded(row, 0.3f, 6, JD_NORD1);
        jd_icon(c[i].enabled ? "square-check" : "square", x + 6, y + 7, 16, c[i].enabled ? JD_NORD8 : JD_NORD4);
        DrawCircle((int)(x + 36), (int)(y + 15), 5, hex_color(c[i].color));
        char label[300];
        snprintf(label, sizeof label, "%s%s", c[i].name ? c[i].name : c[i].href, c[i].readonly ? "  (read-only)" : "");
        jd_text_fit(&app->ui, label, x + 48, y + 8, row.width - 56, 13, JD_NORD6, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            jd_store_calendar_enable(app->view, c[i].id, !c[i].enabled);
            cv.stale = 1, rem_stale = 1;
        }
        y += 32;
    }
    jd_calendar_rows_free(c, n);
    return y + 10;
}

float calendar_settings(jd_app *app, float x, float y, float w, int *focus)
{
    jd_ui *ui = &app->ui;
    if (!cf_filled) fill_settings(app);
    static const char *const providers[] = {"None", "CalDAV", "Google"};
    jd_text(ui, "Server", x, y + 8, 14, JD_NORD6, 1);
    int p = jd_segmented(ui, x + 110, y, providers, 3, cf_provider);
    if (p != cf_provider) {
        cf_provider = p;
        settings_note[0] = 0;
        if (p == 0 && app->prefs.cal_provider) {
            app->prefs.cal_provider = 0;
            save_prefs(app);
            calendar_stop(app);
            forget_calendars(app);
            char *key = calendar_secret_key(app);
            plat_secret_delete(key);
            free(key);
            snprintf(settings_note, sizeof settings_note, "The calendar is disconnected.");
        }
    }
    y += 48;
    float fw = w > 520 ? 520 : w;
    if (cf_provider == 1) {
        static const char *const labels[] = {"Address", "Username", "Password"};
        for (int i = 0; i < 3; i++) {
            jd_text(ui, labels[i], x, y + 10, 13, JD_NORD4, 1);
            if (jd_field_draw(ui, &cf[i], (Rectangle){x + 110, y, fw - 110, 34}, *focus == CF_URL + i)) *focus = CF_URL + i;
            y += 42;
        }
        if (jd_button(ui, "Save and connect", (Rectangle){x + 110, y + 4, 170, 36}, 1)) save_caldav(app);
        y += 50;
    } else if (cf_provider == 2) {
        jd_text(ui, "Google address", x, y + 10, 13, JD_NORD4, 1);
        cf[1].placeholder = "you@gmail.com";
        if (jd_field_draw(ui, &cf[1], (Rectangle){x + 110, y, fw - 110, 34}, *focus == CF_USER)) *focus = CF_USER;
        y += 42;
        if (jd_icon_button(ui, "external-link", signin ? "Waiting for the browser..." : "Sign in with Google",
                           (Rectangle){x + 110, y + 4, 240, 36}, 1))
            google_sign_in(app);
        y += 50;
    }
    if (settings_note[0]) {
        jd_text_fit(ui, settings_note, x, y, fw, 13, JD_NORD8, 0);
        y += 26;
    }
    if (app->prefs.cal_provider && calendar_error()) {
        int n;
        int *starts = jd_wrap(ui, calendar_error(), fw, 13, &n);
        for (int i = 0; i < n; i++)
            jd_text_range(ui, calendar_error(), starts[i], i + 1 < n ? starts[i + 1] : (int)strlen(calendar_error()), x,
                          y + 18 * (float)i, 13, JD_NORD13, 0);
        free(starts);
        y += 18 * (float)n + 10;
    }
    if (app->prefs.cal_provider) {
        y = calendar_list(app, x, y + 6, w);
        if (jd_icon_button(ui, "refresh-cw", "Sync now", (Rectangle){x, y, 130, 34}, 0)) {
            if (app->calsync) jd_calsync_now(app->calsync);
            else calendar_start(app);
        }
        y += 48;
    }
    return y;
}
