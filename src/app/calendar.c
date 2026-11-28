#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "cal/calsync.h"
#include "cal/ical.h"
#include "cal/oauth.h"
#include "core/toml.h"
#include "core/util.h"
#include "mail/send.h"
#include "platform.h"
#include "ui/icons.h"

/* as [view] calendar numbers them (jd_prefs.cal_view) */
enum { V_AGENDA, V_DAY, V_WEEK, V_MONTH };
enum { AGENDA_DAYS = 30, HOUR_H = 56, MAX_REMINDERS = 32 };
/* Settings' focus numbers for the calendar fields, past settings.c's own */
enum { CF_URL = 200, CF_USER, CF_PASSWORD, CF_GOOGLE, CF_COUNT };
/* the editor's fields, in Tab order */
enum { EF_TITLE, EF_DATE0, EF_TIME0, EF_DATE1, EF_TIME1, EF_TZ, EF_LOCATION, EF_NOTES, EF_COUNT };

/* an event's occurrence in the shown range */
typedef struct {
    int64_t event, start, end;
    int all_day, readonly, alarm, recurring;
    char *summary, *location;
    Color color;
    /* its first local day and the day after its last, worked out once when gathered (0: not yet): every frame asks
       for them many times, and each local-time conversion is costly */
    int64_t day0, day1;
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
    int64_t delete_event;
    int64_t serials, delete_serial, respond_serial, reply_serial;
} cv = {.stale = 1, .detail = -1, .sel = -1, .serials = 1LL << 40};
/* The calendar's own store, calendar.db in jackdaw's data folder: calendars are no mail account's (the UI's
   connection; each account's worker opens its own) */
static jd_store *cal_store;
/* the list of calendars to show or hide is open (the header's Calendars button) */
static int shown_open;
/* searching the events (/): the field in the header, and the text the results in cv.occs were found for */
static struct {
    int open, focus;
    jd_field q;
    char *found_for;
} cs;
/* the editor's calendar dropdown: open, and where its list was drawn last frame */
static int cal_dd_open;
static Rectangle cal_dd_box;

/* the views' text sizes: events, block titles, the smaller lines (times, places, hours) */
#define FS_EVENT 14.0f
#define FS_TITLE 15.0f
#define FS_SMALL 13.0f

/* the y for text of size s to sit centred in b: its visual middle is about 0.45 of the size below the box's top */
static float mid_y(Rectangle b, float s)
{
    return b.y + b.height / 2 - 0.45f * s;
}

static int of_account(jd_app *app, const jd_calendar_row *c, int i);

/* ---- a calendar's colour: presets, a saturation/brightness square with a hue bar, and the hex ---- */

/* Settings' focus number for the hex field (beyond the calendar form's) */
enum { CF_HEX = 300 };

static struct {
    int64_t cal; /* the calendar whose colour is being chosen, 0 none */
    float h, s, v;
    int drag; /* 1 the square, 2 the hue bar: the colour is written when the mouse comes up */
    jd_field hex;
    int hex_focus;
} cp;

static const char *const swatches[] = {"#bf616a", "#d08770", "#ebcb8b", "#a3be8c", "#8fbcbb", "#88c0d0", "#81a1c1",
                                       "#5e81ac", "#b48ead", "#d50000", "#e67c73", "#f4511e", "#f6bf26", "#33b679",
                                       "#0b8043", "#039be5", "#3f51b5", "#7986cb", "#8e24aa", "#616161"};
enum { PICKER_H = 262 };

static Color hex_color(const char *s);

static void picker_set(Color c)
{
    Vector3 hsv = ColorToHSV(c);
    cp.h = hsv.x, cp.s = hsv.y, cp.v = hsv.z;
    char hex[8];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", c.r, c.g, c.b);
    jd_field_set(&cp.hex, hex);
}

static void picker_toggle(const jd_calendar_row *c)
{
    if (cp.cal == c->id) {
        cp.cal = 0, cp.hex_focus = 0;
        return;
    }
    cp.cal = c->id, cp.drag = 0, cp.hex_focus = 0;
    picker_set(hex_color(c->color));
}

static void picker_apply(jd_app *app, Color c)
{
    picker_set(c);
    if (cal_store) jd_store_calendar_recolor(cal_store, cp.cal, jd_field_text(&cp.hex));
    cv.stale = 1;
    share_changed(app);
}

/* the hex field's text as a colour, when it is one */
static int hex_valid(const char *s, Color *out)
{
    unsigned r, g, b;
    char tail;
    const char *p = s[0] == '#' ? s + 1 : s;
    if (strlen(p) != 6 || sscanf(p, "%2x%2x%2x%c", &r, &g, &b, &tail) != 3) return 0;
    *out = (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    return 1;
}

/* typing in the hex field: Enter takes the colour, Esc leaves it; 1 when it had the keys */
static int picker_keys(jd_app *app)
{
    if (!cp.cal || !cp.hex_focus) return 0;
    if (IsKeyPressed(KEY_ESCAPE)) cp.hex_focus = 0;
    else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        Color c;
        if (hex_valid(jd_field_text(&cp.hex), &c)) picker_apply(app, c);
        cp.hex_focus = 0;
    } else jd_field_input(&cp.hex);
    return 1;
}

/* the picker for calendar c, PICKER_H tall from y; returns the y below it */
static float picker_draw(jd_app *app, const jd_calendar_row *c, float x, float y, float w, int *focus)
{
    jd_ui *ui = &app->ui;
    Vector2 m = GetMousePosition();
    int press = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !app->dialog.kind;
    DrawRectangleRounded((Rectangle){x, y, w, PICKER_H - 8}, 0.04f, 6, JD_NORD0);
    float px = x + 12, pw = w - 24, py = y + 12;
    /* presets */
    int per = (int)(pw / 26);
    if (per < 1) per = 1;
    int nsw = (int)(sizeof swatches / sizeof *swatches);
    for (int i = 0; i < nsw; i++) {
        Color sc = hex_color(swatches[i]);
        Vector2 at = {px + 11 + 26 * (float)(i % per), py + 11 + 26 * (float)(i / per)};
        int on = CheckCollisionPointCircle(m, at, 11);
        DrawCircleV(at, on ? 11 : 10, sc);
        if (strcasecmp(jd_field_text(&cp.hex), swatches[i]) == 0) DrawCircleLinesV(at, 13, JD_NORD6);
        if (on && press) picker_apply(app, sc);
    }
    int rows = (nsw + per - 1) / per;
    py += 26 * (float)rows + 8;
    /* saturation (across) and brightness (down) of the hue */
    Rectangle sq = {px, py, pw, PICKER_H - 8 - 12 - 26 * (float)rows - 8 - 8 - 14 - 12 - 32 - 12};
    DrawRectangleGradientH((int)sq.x, (int)sq.y, (int)sq.width, (int)sq.height, WHITE, ColorFromHSV(cp.h, 1, 1));
    DrawRectangleGradientV((int)sq.x, (int)sq.y, (int)sq.width, (int)sq.height, BLANK, BLACK);
    Rectangle hue = {px, sq.y + sq.height + 8, pw, 14};
    for (int k = 0; k < 6; k++)
        DrawRectangleGradientH((int)(hue.x + hue.width * (float)k / 6), (int)hue.y, (int)(hue.width / 6) + 1, (int)hue.height,
                               ColorFromHSV(60.0f * (float)k, 1, 1), ColorFromHSV(60.0f * (float)(k + 1), 1, 1));
    if (press && CheckCollisionPointRec(m, sq)) cp.drag = 1;
    if (press && CheckCollisionPointRec(m, hue)) cp.drag = 2;
    if (cp.drag) {
        if (cp.drag == 1) {
            cp.s = fminf(1, fmaxf(0, (m.x - sq.x) / sq.width));
            cp.v = fminf(1, fmaxf(0, 1 - (m.y - sq.y) / sq.height));
        } else {
            cp.h = fminf(359.9f, fmaxf(0, (m.x - hue.x) / hue.width * 360));
        }
        Color now = ColorFromHSV(cp.h, cp.s, cp.v);
        char hex[8];
        snprintf(hex, sizeof hex, "#%02x%02x%02x", now.r, now.g, now.b);
        jd_field_set(&cp.hex, hex);
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            cp.drag = 0;
            picker_apply(app, now);
        }
    }
    DrawCircleLinesV((Vector2){sq.x + cp.s * sq.width, sq.y + (1 - cp.v) * sq.height}, 6, WHITE);
    DrawRectangleRec((Rectangle){hue.x + cp.h / 360 * hue.width - 2, hue.y - 2, 4, hue.height + 4}, WHITE);
    py = hue.y + hue.height + 12;
    /* the colour, its hex to type, and the server's back */
    DrawRectangleRounded((Rectangle){px, py, 34, 32}, 0.3f, 6, ColorFromHSV(cp.h, cp.s, cp.v));
    cp.hex.placeholder = "#rrggbb";
    if (jd_field_draw(ui, &cp.hex, (Rectangle){px + 42, py, 110, 32}, cp.hex_focus)) {
        cp.hex_focus = 1;
        if (focus) *focus = CF_HEX;
    }
    Rectangle reset = {px + 160, py, 72, 32};
    if (c->user_color && CheckCollisionPointRec(m, reset) && !plat_touch()) jd_tooltip("Back to the server's colour", reset.x - 40, reset.y + 38);
    if (c->user_color && jd_button(ui, "Reset", reset, 0)) {
        if (cal_store) jd_store_calendar_recolor(cal_store, cp.cal, NULL);
        picker_set(hex_color(c->server_color));
        cv.stale = 1;
        share_changed(app);
    }
    if (jd_button(ui, "Done", (Rectangle){x + w - 12 - 70, py, 70, 32}, 1)) {
        Color typed;
        if (cp.hex_focus && hex_valid(jd_field_text(&cp.hex), &typed)) picker_apply(app, typed);
        cp.cal = 0, cp.hex_focus = 0;
        if (focus && *focus == CF_HEX) *focus = -1;
    }
    return y + PICKER_H;
}

static struct {
    int open, focus, saving;
    int64_t event, calendar, serial;
    jd_field f[EF_COUNT - 1];
    jd_editor notes;
    int all_day, repeat, alarm;
    char *rrule_kept; /* a rule the choices do not cover, kept as it was */
    char *old_ics;
    char error[300];
    char tz[96]; /* the zone its times are typed in (IANA name); the field f[EF_TZ] searches the zones */
} ed;

static struct {
    int64_t at;
    char *title, *text;
} rem[MAX_REMINDERS * 4];
static int nrem, rem_stale = 1, rem_by_platform;
static int64_t rem_built, rem_fired_until;

/* Settings' fields and the Google sign-in under way */
static jd_field cf[CF_COUNT - CF_URL];
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
    if (o->day1) return o->day0;
    return o->all_day ? date_day(o->start) : local_midnight(o->start);
}

/* the day after the last one it touches */
static int64_t end_day(const occ *o)
{
    if (o->day1) return o->day1;
    if (o->all_day) return date_day(o->end > o->start ? o->end : o->start + 86400);
    return add_days(local_midnight(o->end > o->start ? o->end - 1 : o->start), 1);
}

/* local days between two local midnights: whole days, as a day of 23 or 25 hours (summer time) rounds */
static int days_between(int64_t from, int64_t to)
{
    return (int)((to - from + (to >= from ? 43200 : -43200)) / 86400);
}

/* the time and today's midnight, read once per frame (calendar_draw) for the many past/present checks */
static int64_t frame_now, frame_today;

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
    if (!cal_store) return NULL;
    int nrows;
    /* an all-day event's dates are UTC midnights: a day either side covers every zone */
    jd_event_row *rows = jd_store_events(cal_store, from - 86400, to + 86400, &nrows);
    occ *out = NULL;
    for (int i = 0; i < nrows; i++) {
        jd_occurrence *o = NULL;
        int k = 0;
        jd_ical_expand(rows[i].ics, from - 86400, to + 86400, &o, &k);
        for (int j = 0; j < k; j++) {
            occ x = {rows[i].id, o[j].start, o[j].end, o[j].all_day, rows[i].readonly, o[j].alarm, rows[i].recurring,
                     o[j].summary, o[j].location, hex_color(rows[i].color), 0, 0};
            o[j].summary = o[j].location = NULL;
            x.day0 = first_day(&x);
            x.day1 = end_day(&x);
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
    else if (cv.view == V_DAY) *from = cv.anchor, *to = add_days(cv.anchor, 1);
    else if (cv.view == V_WEEK) *from = week_start(cv.anchor), *to = add_days(*from, 7);
    else {
        struct tm tm = local_tm(cv.anchor);
        *from = week_start(mk_local(tm.tm_year + 1900, tm.tm_mon + 1, 1, 0, 0));
        *to = add_days(*from, 42);
    }
}

static void reload(jd_app *app)
{
    /* the search's results stand in for the view's events (search_run) */
    if (cs.open) return;
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

/* one worker per [[calendar]] account, in the order of app->prefs.cals, and what it last said */
enum { MAX_ACCOUNTS = 16 };
static struct {
    jd_calsync *worker;
    char status[200], error[400];
    int64_t synced_at; /* when it last went idle without a failure, 0 before */
    int asked;         /* a sync asked for by hand is under way: its end is told */
    int refused;       /* the server refused the sign-in: the worker stopped, a sync starts it again */
    int unsigned_;     /* no password or sign-in on this device (an account another jackdaw shared): Sign in */
} acc[MAX_ACCOUNTS];
static int nacc;

/* the keystore entry of an account's password or Google sign-in */
static char *secret_key(const jd_cal_account *a)
{
    char *id = jd_cal_account_id(a);
    char *key = jd_printf("calendar %s", id);
    free(id);
    return key;
}

/* Where an earlier jackdaw kept it, under a mail account's address: "<address> calendar <id>", or for the only
   account there was then "<address> calendar". Moved to secret_key's entry when found; NULL when none. */
static char *old_secret(jd_app *app, const jd_cal_account *a, int first)
{
    char *id = jd_cal_account_id(a);
    char *secret = NULL;
    for (int k = 0; k < accounts_count(app) && !secret; k++) {
        const char *address = account_at(app, k)->address;
        if (!address) continue;
        for (int legacy = 0; legacy <= first && !secret; legacy++) {
            char *old = legacy ? jd_printf("%s calendar", address) : jd_printf("%s calendar %s", address, id);
            secret = plat_secret_load(old);
            char *key = secret ? secret_key(a) : NULL;
            if (secret && plat_secret_save(key, secret) == 0) plat_secret_delete(old);
            free(key), free(old);
        }
    }
    free(id);
    return secret;
}

/* account a's sign-in, calendars and events, gone from here (it stays in prefs) */
static void forget_account(jd_app *app, int a)
{
    const jd_cal_account *ac = &app->prefs.cals[a];
    char *key = secret_key(ac);
    plat_secret_delete(key);
    free(key);
    char *id = jd_cal_account_id(ac);
    if (cal_store) jd_store_calendars_keep(cal_store, id, NULL, 0);
    free(id);
}

int calendar_connected(void)
{
    for (int i = 0; i < nacc; i++)
        if (acc[i].worker && !acc[i].refused) return 1;
    return 0;
}

void calendar_stop(jd_app *app)
{
    for (int i = 0; i < nacc; i++) {
        jd_calsync_stop(acc[i].worker);
        acc[i] = (typeof(acc[0])){0};
    }
    nacc = 0;
}

static void start_one(jd_app *app, int i, const char *db)
{
    jd_cal_account *a = &app->prefs.cals[i];
    acc[i].refused = 0;
    acc[i].error[0] = 0;
    char *key = secret_key(a);
    char *secret = plat_secret_load(key);
    if (!secret) secret = old_secret(app, a, i == 0);
    free(key);
    acc[i].unsigned_ = !secret;
    if (!secret) {
        /* also an account another jackdaw shared: its card's Sign in signs this device in */
        snprintf(acc[i].error, sizeof acc[i].error, "%s", a->provider == 2 ? "Not signed in on this device: Sign in from Settings > Calendar."
                                                                            : "No password on this device: Sign in from Settings > Calendar.");
        return;
    }
    char *id = jd_cal_account_id(a);
    acc[i].worker = jd_calsync_start(id, a->provider == 2 ? JD_CAL_GOOGLE : JD_CAL_CALDAV, a->url, a->username, secret,
                                     a->check_minutes, db);
    snprintf(acc[i].status, sizeof acc[i].status, "Connecting");
    free(id);
    jd_wipe(secret, strlen(secret));
    free(secret);
}

static char *cal_db(void)
{
    jd_mkdirs(jd_data_dir());
    return jd_printf("%s/calendar.db", jd_data_dir());
}

void calendar_start(jd_app *app)
{
    calendar_stop(app);
    char *db = cal_db();
    if (!cal_store) {
        char err[300];
        cal_store = jd_store_open(db, err, sizeof err);
        if (!cal_store) fprintf(stderr, "jackdaw: %s: %s\n", db, err);
        /* calendars an earlier jackdaw kept in a mail account's store come over, once */
        int n = 0;
        jd_calendar_row *have = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
        if (have) jd_calendar_rows_free(have, n);
        for (int k = 0; cal_store && !n && k < accounts_count(app); k++) {
            char *dir = jd_account_dir(account_at(app, k));
            char *old = jd_printf("%s/mail.db", dir);
            if (access(old, F_OK) == 0) n = jd_store_calendars_import(cal_store, old);
            free(old), free(dir);
        }
    }
    nacc = app->prefs.ncals < MAX_ACCOUNTS ? app->prefs.ncals : MAX_ACCOUNTS;
    for (int i = 0; i < nacc; i++) start_one(app, i, db);
    free(db);
    cv.stale = 1, rem_stale = 1;
}

void calendar_quit(jd_app *app)
{
    calendar_stop(app);
    jd_store_close(cal_store);
    cal_store = NULL;
}

/* Sync now: every running worker looks again; a stopped one (refused, or never started) starts afresh */
static void sync_now(jd_app *app)
{
    if (nacc != app->prefs.ncals) {
        calendar_start(app);
        return;
    }
    char *db = cal_db();
    for (int i = 0; i < nacc; i++) {
        if (acc[i].worker && !acc[i].refused) {
            jd_calsync_now(acc[i].worker);
            snprintf(acc[i].status, sizeof acc[i].status, "Looking at the calendars");
            acc[i].asked = 1;
            acc[i].error[0] = 0;
        } else if (db) {
            jd_calsync_stop(acc[i].worker);
            acc[i].worker = NULL;
            start_one(app, i, db);
        }
    }
    free(db);
}

/* ---- shared with the account's other jackdaws (settings.c) ---- */

static int by_account_href(const void *a, const void *b)
{
    const jd_calendar_row *x = a, *y = b;
    int c = strcmp(x->account ? x->account : "", y->account ? y->account : "");
    return c ? c : strcmp(x->href, y->href);
}

static void choice_text(jd_buf *b, const char *account, const char *href, int sync, int shown, const char *color)
{
    jd_buf_adds(b, "\n[[calendar_choice]]\naccount = ");
    jd_toml_quote(b, account ? account : "");
    jd_buf_adds(b, "\nhref = ");
    jd_toml_quote(b, href);
    jd_buf_addf(b, "\nsync = %s\nshown = %s\ncolor = ", sync ? "true" : "false", shown ? "true" : "false");
    jd_toml_quote(b, color ? color : "");
    jd_buf_adds(b, "\n");
}

/* entry i of a shared copy's [[calendar_choice]]; href NULL when it has none */
static jd_calendar_row choice_of(const jd_toml *t, int i)
{
    char table[48];
    snprintf(table, sizeof table, "calendar_choice.%d", i);
    const char *color = jd_toml_str(t, table, "color");
    return (jd_calendar_row){.account = jd_strdup(jd_toml_str(t, table, "account")),
                             .href = jd_strdup(jd_toml_str(t, table, "href")),
                             .enabled = jd_toml_bool(t, table, "sync", 0),
                             .shown = jd_toml_bool(t, table, "shown", 1),
                             .user_color = color && color[0] ? jd_strdup(color) : NULL};
}

/* which calendars sync, show and in what colour: a shared copy's (t), else this store's with keep's for the
   calendars not found here yet */
char *calendar_choices_text(jd_app *app, const jd_toml *t, const jd_toml *keep)
{
    int nkeep = t ? jd_toml_count(t, "calendar_choice") : keep ? jd_toml_count(keep, "calendar_choice") : 0;
    jd_calendar_row *from = t ? NULL : keep ? jd_alloc(sizeof *from * (size_t)(nkeep + 1)) : NULL;
    for (int i = 0; from && i < nkeep; i++) from[i] = choice_of(keep, i);
    int n = 0, nstore = 0;
    jd_calendar_row *store = !t && cal_store ? jd_store_calendars(cal_store, &nstore) : NULL;
    jd_calendar_row *out = jd_alloc(sizeof *out * (size_t)(nkeep + nstore + 1));
    for (int i = 0; t && i < nkeep; i++) {
        out[n] = choice_of(t, i);
        if (out[n].href) n++;
        else free(out[n].account), free(out[n].user_color);
    }
    for (int i = 0; i < nstore; i++) {
        if (!store[i].account) continue;
        /* one found here and left as found (not synced, shown, the server's colour) is no choice, unless the
           shared copy has one for it */
        int chosen = store[i].enabled || !store[i].shown || store[i].user_color;
        for (int k = 0; k < nkeep && !chosen && from; k++)
            chosen = jd_streq(from[k].account, store[i].account) && jd_streq(from[k].href, store[i].href);
        if (chosen)
            out[n++] = (jd_calendar_row){.account = jd_strdup(store[i].account), .href = jd_strdup(store[i].href),
                                         .enabled = store[i].enabled, .shown = store[i].shown,
                                         .user_color = jd_strdup(store[i].user_color)};
    }
    for (int k = 0; from && k < nkeep; k++) {
        int here = !from[k].href;
        for (int i = 0; i < nstore && !here; i++)
            here = jd_streq(store[i].account, from[k].account) && jd_streq(store[i].href, from[k].href);
        if (!here) out[n++] = from[k], from[k] = (jd_calendar_row){0};
    }
    if (n) qsort(out, (size_t)n, sizeof *out, by_account_href);
    jd_buf b = {0};
    for (int i = 0; i < n; i++) choice_text(&b, out[i].account, out[i].href, out[i].enabled, out[i].shown, out[i].user_color);
    jd_calendar_rows_free(out, n);
    if (from) jd_calendar_rows_free(from, nkeep);
    if (store) jd_calendar_rows_free(store, nstore);
    return b.s ? b.s : jd_strdup("");
}

/* a shared copy's choices onto the calendars found here; the others wait for their account's next look */
void calendar_choices_apply(jd_app *app, const jd_toml *t)
{
    int n, synced = 0;
    jd_calendar_row *c = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
    for (int i = 0; c && i < jd_toml_count(t, "calendar_choice"); i++) {
        char table[48];
        snprintf(table, sizeof table, "calendar_choice.%d", i);
        const char *account = jd_toml_str(t, table, "account"), *href = jd_toml_str(t, table, "href");
        const char *color = jd_toml_str(t, table, "color");
        if (color && !color[0]) color = NULL;
        for (int j = 0; j < n; j++) {
            if (!jd_streq(c[j].account, account) || !jd_streq(c[j].href, href)) continue;
            int sync = jd_toml_bool(t, table, "sync", 0), shown = jd_toml_bool(t, table, "shown", 1);
            if (sync != c[j].enabled) jd_store_calendar_enable(cal_store, c[j].id, sync), synced |= sync;
            if (shown != c[j].shown) jd_store_calendar_show(cal_store, c[j].id, shown);
            if (!jd_streq(color, c[j].user_color)) jd_store_calendar_recolor(cal_store, c[j].id, color);
        }
    }
    if (c) jd_calendar_rows_free(c, n);
    cv.stale = 1, rem_stale = 1;
    if (synced) sync_now(app);
}

/* a shared copy's calendar accounts in place of these: the ones it no longer has are forgotten here */
void calendar_accounts_apply(jd_app *app, const jd_toml *t)
{
    jd_prefs next = {0};
    jd_share_apply(&next, JD_SHARE_CALENDARS, t);
    for (int a = 0; a < app->prefs.ncals; a++) {
        char *id = jd_cal_account_id(&app->prefs.cals[a]);
        int kept = 0;
        for (int b = 0; b < next.ncals && !kept; b++) {
            char *other = jd_cal_account_id(&next.cals[b]);
            kept = strcmp(id, other) == 0;
            free(other);
        }
        free(id);
        if (!kept) forget_account(app, a);
    }
    jd_share_apply(&app->prefs, JD_SHARE_CALENDARS, t);
    jd_prefs_free(&next);
    calendar_start(app);
    cv.stale = 1, rem_stale = 1;
}

/* the account a calendar is filed under: its worker, NULL when that account does not run */
static jd_calsync *worker_of(jd_app *app, int64_t calendar)
{
    int n;
    jd_calendar_row *rows = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
    const char *account = NULL;
    int found = 0;
    for (int i = 0; rows && i < n; i++)
        if (rows[i].id == calendar) account = rows[i].account, found = 1;
    jd_calsync *w = NULL;
    for (int i = 0; found && i < nacc && !w; i++) {
        char *id = jd_cal_account_id(&app->prefs.cals[i]);
        /* one kept before accounts were told apart is the first account's */
        if (account ? strcmp(account, id) == 0 : i == 0) w = acc[i].refused ? NULL : acc[i].worker;
        free(id);
    }
    if (rows) jd_calendar_rows_free(rows, n);
    return w;
}

const char *calendar_status(void)
{
    for (int i = 0; i < nacc; i++)
        if (acc[i].status[0]) return acc[i].status;
    return NULL;
}

const char *calendar_error(void)
{
    for (int i = 0; i < nacc; i++)
        if (acc[i].error[0]) return acc[i].error;
    return NULL;
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

/* account i's worker said ev */
static void handle(jd_app *app, int i, jd_cal_event *ev)
{
    switch (ev->kind) {
    case JD_CAL_EV_STATUS:
        if (!ev->text && acc[i].status[0] && !acc[i].error[0]) acc[i].synced_at = jd_now();
        /* the end of a sync asked for by hand, not of a save that ran before it */
        if (!ev->text && acc[i].asked && strcmp(acc[i].status, "Saving to the calendar") != 0) {
            acc[i].asked = 0;
            if (acc[i].error[0]) notify_user(app, "Not synced: %s", acc[i].error);
            else notify_user(app, "Calendars are up to date.");
        }
        snprintf(acc[i].status, sizeof acc[i].status, "%s", ev->text ? ev->text : "");
        if (ev->text) acc[i].error[0] = 0;
        break;
    case JD_CAL_EV_ERROR:
        snprintf(acc[i].error, sizeof acc[i].error, "%s", ev->text ? ev->text : "failed");
        break;
    case JD_CAL_EV_AUTH:
        acc[i].refused = 1;
        snprintf(acc[i].error, sizeof acc[i].error, "The server refused the sign-in%s%s. Remove the account and add it again.",
                 ev->text ? ": " : "", ev->text ? ev->text : "");
        break;
    case JD_CAL_EV_CHANGED:
        cv.stale = 1, rem_stale = 1;
        free(cs.found_for);
        cs.found_for = NULL;
        /* calendars just found here take the choices made for them on another device */
        share_calendars_found(app);
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
    if (!app->prefs.cal_reminders || !app->prefs.ncals) {
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
    for (int i = 0; i < nacc; i++)
        while (acc[i].worker && jd_calsync_poll(acc[i].worker, &ev)) {
            handle(app, i, &ev);
            jd_cal_event_free(&ev);
        }
    signin_tick(app);
    if (cal_store) reminders_tick(app);
}

/* ---- the editor ---- */

static void editor_close(void)
{
    ed.open = 0;
    cal_dd_open = 0;
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
    jd_calendar_row *c = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
    int64_t id = 0;
    for (int i = 0; c && i < n && !id; i++)
        if (c[i].id == prefer && !c[i].readonly && c[i].enabled) id = c[i].id;
    for (int i = 0; c && i < n && !id; i++)
        if (!c[i].readonly && c[i].enabled) id = c[i].id;
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
    /* as the clock in the event's zone reads them */
    int y, m, d, hh, mm;
    jd_tz_wall(ed.tz, start, &y, &m, &d, &hh, &mm);
    snprintf(s, sizeof s, "%04d-%02d-%02d", y, m, d);
    jd_field_set(&ed.f[EF_DATE0], s);
    snprintf(s, sizeof s, "%02d:%02d", hh, mm);
    jd_field_set(&ed.f[EF_TIME0], s);
    jd_tz_wall(ed.tz, end, &y, &m, &d, &hh, &mm);
    snprintf(s, sizeof s, "%04d-%02d-%02d", y, m, d);
    jd_field_set(&ed.f[EF_DATE1], s);
    snprintf(s, sizeof s, "%02d:%02d", hh, mm);
    jd_field_set(&ed.f[EF_TIME1], s);
}

/* up to max zones whose name holds text (any case; a space matches '_'), the ones it starts a part of first */
static int tz_matches(const char *text, const char **out, int max)
{
    int n, k = 0;
    const char *const *names = jd_tz_names(&n);
    char want[96];
    snprintf(want, sizeof want, "%s", text);
    for (char *c = want; *c; c++)
        if (*c == ' ') *c = '_';
    for (int pass = 0; pass < 2 && k < max; pass++)
        for (int i = 0; i < n && k < max; i++) {
            const char *at = strcasestr(names[i], want);
            if (!at) continue;
            int starts = at == names[i] || at[-1] == '/' || at[-1] == '_';
            if (starts != !pass) continue;
            out[k++] = names[i];
        }
    return k;
}

/* "America/New_York" as "New York" */
static void zone_city(const char *tzid, char *out, size_t n)
{
    const char *slash = strrchr(tzid, '/');
    snprintf(out, n, "%s", slash ? slash + 1 : tzid);
    for (char *c = out; *c; c++)
        if (*c == '_') *c = ' ';
}

/* the zone its times are typed in: the event's own when this system knows it, else the local one */
static void set_zone(const char *tzid)
{
    snprintf(ed.tz, sizeof ed.tz, "%s", tzid && jd_tz_known(tzid) ? tzid : jd_local_tzid());
    jd_field_set(&ed.f[EF_TZ], ed.tz);
}

static void editor_new(jd_app *app, int64_t start, int all_day)
{
    editor_close();
    if (!calendar_connected()) {
        notify_user(app, "No calendar is connected: set one up in Settings, Calendar.");
        return;
    }
    ed.calendar = first_writable(app, 0);
    if (!ed.calendar) {
        notify_user(app, "No writable calendar is synced: choose one in Settings, Calendar.");
        return;
    }
    ed.open = 1, ed.event = 0, ed.focus = EF_TITLE, ed.saving = 0;
    for (int i = 0; i < EF_COUNT - 1; i++) jd_field_set(&ed.f[i], NULL);
    jd_editor_set(&ed.notes, "");
    ed.all_day = all_day, ed.repeat = 0, ed.alarm = 0;
    set_zone(NULL);
    set_times(all_day ? jd_days_from_civil(local_tm(start).tm_year + 1900, local_tm(start).tm_mon + 1, local_tm(start).tm_mday) * 86400 : start,
              all_day ? 0 : start + 3600, all_day);
}

static void editor_edit(jd_app *app, int64_t event)
{
    jd_event_row *row = cal_store ? jd_store_event(cal_store, event) : NULL;
    if (!row) return;
    if (row->readonly) {
        notify_user(app, "That calendar is read-only.");
        jd_event_rows_free(row, 1);
        return;
    }
    if (!worker_of(app, row->calendar)) {
        notify_user(app, "That calendar's account is not connected: see Settings, Calendar.");
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
    set_zone(e.tzid);
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
    if (!parse_date(jd_field_text(&ed.f[EF_DATE0]), &y0, &m0, &d0) || !parse_date(jd_field_text(&ed.f[EF_DATE1]), &y1, &m1, &d1)) {
        snprintf(ed.error, sizeof ed.error, "Dates are written 2026-11-19.");
        return;
    }
    if (!ed.all_day && (!parse_time(jd_field_text(&ed.f[EF_TIME0]), &h0, &i0) || !parse_time(jd_field_text(&ed.f[EF_TIME1]), &h1, &i1))) {
        snprintf(ed.error, sizeof ed.error, "Times are written 09:30.");
        return;
    }
    jd_ical_event e = {.summary = (char *)jd_field_text(&ed.f[EF_TITLE]), .location = (char *)jd_field_text(&ed.f[EF_LOCATION]),
                       .description = ed.notes.text, .all_day = ed.all_day};
    if (ed.all_day) {
        e.start = jd_days_from_civil(y0, m0, d0) * 86400;
        e.end = (jd_days_from_civil(y1, m1, d1) + 1) * 86400;
    } else {
        e.start = jd_tz_time(ed.tz, y0, m0, d0, h0, i0);
        e.end = jd_tz_time(ed.tz, y1, m1, d1, h1, i1);
        if (strcmp(ed.tz, jd_local_tzid()) != 0) e.tzid = ed.tz;
    }
    if (e.end < e.start || (ed.all_day && e.end <= e.start)) {
        snprintf(ed.error, sizeof ed.error, "The event ends before it starts.");
        return;
    }
    e.rrule = ed.repeat < 0 ? ed.rrule_kept : (char *)rules[ed.repeat];
    e.alarm = ed.alarm >= 100 ? ed.alarm - 100 : alarms[ed.alarm];
    if (!e.summary[0]) e.summary = "(no title)";
    char *ics = jd_ical_write(ed.old_ics, &e);
    jd_calsync *to = worker_of(app, ed.calendar);
    if (!ics || !to) {
        snprintf(ed.error, sizeof ed.error, to ? "The event could not be written." : "That calendar's account is not connected.");
        free(ics);
        return;
    }
    ed.serial = ++cv.serials;
    ed.saving = 1;
    ed.error[0] = 0;
    /* moved to a calendar of another account: written there anew, then removed by the account it was in */
    jd_event_row *was = ed.event && cal_store ? jd_store_event(cal_store, ed.event) : NULL;
    jd_calsync *from = was ? worker_of(app, was->calendar) : NULL;
    if (was && from && from != to) {
        jd_calsync_put(to, ed.calendar, 0, ics, ed.serial);
        jd_calsync_delete(from, ed.event, ++cv.serials);
    } else {
        jd_calsync_put(to, ed.calendar, ed.event, ics, ed.serial);
    }
    if (was) jd_event_rows_free(was, 1);
    free(ics);
}

static void ask_delete(jd_app *app, int64_t event)
{
    jd_event_row *row = cal_store ? jd_store_event(cal_store, event) : NULL;
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
    jd_event_row *row = cal_store ? jd_store_event(cal_store, cv.delete_event) : NULL;
    jd_calsync *w = row ? worker_of(app, row->calendar) : NULL;
    if (row) jd_event_rows_free(row, 1);
    if (!w) {
        notify_user(app, "That calendar's account is not connected.");
        return;
    }
    cv.delete_serial = ++cv.serials;
    jd_calsync_delete(w, cv.delete_event, cv.delete_serial);
    if (ed.open && ed.event == cv.delete_event) editor_close();
    cv.detail = -1;
    cv.delete_event = 0;
}

int calendar_typing(void)
{
    return ed.open || (cp.cal && cp.hex_focus) || (cs.open && cs.focus);
}

static void editor_keys(jd_app *app)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (cal_dd_open) cal_dd_open = 0;
        else editor_close();
        return;
    }
    if (ctrl && (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_ENTER))) {
        editor_save(app);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        if (ed.focus == EF_TZ) jd_field_set(&ed.f[EF_TZ], ed.tz);
        int ntz;
        jd_tz_names(&ntz);
        do ed.focus = (ed.focus + (shift ? EF_COUNT - 1 : 1)) % EF_COUNT;
        while ((ed.all_day && (ed.focus == EF_TIME0 || ed.focus == EF_TIME1)) || (ed.focus == EF_TZ && (ed.all_day || !ntz)));
        /* arriving there starts a search */
        if (ed.focus == EF_TZ) jd_field_set(&ed.f[EF_TZ], NULL);
        return;
    }
    if (ed.focus == EF_TZ && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        /* the first zone the typed text finds */
        const char *found[1];
        if (tz_matches(jd_field_text(&ed.f[EF_TZ]), found, 1)) snprintf(ed.tz, sizeof ed.tz, "%s", found[0]);
        jd_field_set(&ed.f[EF_TZ], ed.tz);
        ed.focus = EF_LOCATION;
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
    static const char *const placeholders[] = {"Title", "2026-11-19", "09:00", "2026-11-19", "10:00", "Type a city or zone",
                                               "Location"};
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
    y += 42;
    /* the zone the times above are in; typing searches the system's zones, listed under the field */
    Rectangle tzf = {0};
    int ntz = 0;
    if (!ed.all_day && jd_tz_names(&ntz) && ntz) {
        cx = row_label(app, "Time zone", ix, &y);
        tzf = (Rectangle){cx, y, ix + iw - cx < 360 ? ix + iw - cx : 360, 34};
        ed.f[EF_TZ].inert = cal_dd_open && CheckCollisionPointRec(GetMousePosition(), cal_dd_box);
        if (jd_field_draw(ui, &ed.f[EF_TZ], tzf, ed.focus == EF_TZ)) {
            if (ed.focus != EF_TZ) jd_field_set(&ed.f[EF_TZ], NULL);
            ed.focus = EF_TZ;
        }
        if (ed.focus != EF_TZ && strcmp(jd_field_text(&ed.f[EF_TZ]), ed.tz) != 0) jd_field_set(&ed.f[EF_TZ], ed.tz);
        y += 42;
        /* while typing: the zones found, in line (Enter takes the first) */
        const char *found[6];
        int nf = ed.focus == EF_TZ && ed.f[EF_TZ].len && strcmp(jd_field_text(&ed.f[EF_TZ]), ed.tz) != 0
                     ? tz_matches(jd_field_text(&ed.f[EF_TZ]), found, 6) : 0;
        if (ed.focus == EF_TZ && ed.f[EF_TZ].len && !nf) {
            jd_text(ui, "No zone has that name.", tzf.x + 4, y, FS_SMALL, JD_MUTED, 0);
            y += 24;
        }
        for (int i = 0; i < nf; i++) {
            Rectangle row = {tzf.x, y, tzf.width, 28};
            int hover = CheckCollisionPointRec(GetMousePosition(), row) && !app->dialog.kind;
            if (hover || i == 0) DrawRectangleRounded(row, 0.3f, 6, hover ? JD_NORD3 : JD_NORD2);
            char city[64], line[160];
            zone_city(found[i], city, sizeof city);
            snprintf(line, sizeof line, "%s  ·  %s", city, found[i]);
            jd_text_fit(ui, line, row.x + 10, mid_y(row, FS_EVENT), row.width - 20, FS_EVENT, JD_NORD6, i == 0);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                snprintf(ed.tz, sizeof ed.tz, "%s", found[i]);
                jd_field_set(&ed.f[EF_TZ], ed.tz);
                ed.focus = EF_LOCATION;
            }
            y += 30;
        }
        if (nf) y += 4;
    }
    y += 4;
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
    /* the calendar: a dropdown of the synced ones that can be written, under their accounts */
    int nc;
    jd_calendar_row *cals = cal_store ? jd_store_calendars(cal_store, &nc) : NULL;
    const char *cname = "?";
    Color ccol = JD_NORD8;
    for (int i = 0; cals && i < nc; i++)
        if (cals[i].id == ed.calendar) cname = cals[i].name ? cals[i].name : "Calendar", ccol = hex_color(cals[i].color);
    float cbx = row_label(app, "Calendar", ix, &y);
    Rectangle cb = {cbx, y, ix + iw - cbx < 360 ? ix + iw - cbx : 360, 32};
    int over_list = cal_dd_open && CheckCollisionPointRec(GetMousePosition(), cal_dd_box);
    int chover = CheckCollisionPointRec(GetMousePosition(), cb) && !app->dialog.kind && !over_list;
    DrawRectangleRounded(cb, 0.3f, 6, chover || cal_dd_open ? JD_NORD2 : JD_NORD0);
    DrawCircle((int)(cb.x + 14), (int)(cb.y + 16), 5, ccol);
    jd_text_fit(ui, cname, cb.x + 28, mid_y(cb, FS_EVENT), cb.width - 56, FS_EVENT, JD_NORD6, 0);
    jd_icon(cal_dd_open ? "chevron-down" : "chevron-right", cb.x + cb.width - 24, cb.y + 8, 16, JD_NORD4);
    if (chover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) cal_dd_open = !cal_dd_open;
    Rectangle dd_at = cb;
    y += 42;
    /* cals stays for the list, drawn last */
    /* what lies under an open list takes no clicks */
    ed.f[EF_LOCATION].inert = over_list;
    ed.notes.inert = over_list;
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
        if (cals) jd_calendar_rows_free(cals, nc);
        return;
    }
    float ex = ix + 220;
    if (ed.event && jd_button(ui, "Delete", (Rectangle){ex, y, 100, 36}, 0)) ask_delete(app, ed.event);
    if (ed.event) ex += 110;
    const char *note = ed.error[0] ? ed.error : app->ui.touch ? "" : "Tab moves between fields; Ctrl+S saves, Esc cancels.";
    /* a phone's width has no room beside the buttons: an error goes above them */
    if (ix + iw - ex > 160) jd_text_fit(ui, note, ex + 4, y + 11, ix + iw - ex - 4, 13, ed.error[0] ? JD_NORD11 : JD_MUTED, 0);
    else if (ed.error[0]) jd_text_fit(ui, ed.error, ix, y - 22, iw, 13, JD_NORD11, 0);

    /* the open calendar list, over the fields under it, above the buttons */
    cal_dd_box = (Rectangle){0};
    if (!cal_dd_open || !cals) {
        if (cals) jd_calendar_rows_free(cals, nc);
        return;
    }
    int rows = 0, groups = 0;
    for (int a = 0; a < app->prefs.ncals; a++) {
        int k = 0;
        for (int i = 0; i < nc; i++) k += cals[i].enabled && !cals[i].readonly && of_account(app, &cals[i], a);
        rows += k, groups += k > 0;
    }
    Rectangle box = {dd_at.x, dd_at.y + dd_at.height + 4, dd_at.width, 12 + 32 * (float)rows + 28 * (float)groups};
    if (box.y + box.height > bottom) box.height = bottom - box.y;
    cal_dd_box = box;
    DrawRectangleRounded((Rectangle){box.x + 2, box.y + 3, box.width, box.height}, 0.05f, 6, ColorAlpha(BLACK, 0.3f));
    DrawRectangleRounded(box, 0.05f, 6, JD_NORD2);
    jd_scissor(box.x, box.y, box.width, box.height);
    float ly = box.y + 6;
    for (int a = 0; a < app->prefs.ncals; a++) {
        int k = 0;
        for (int i = 0; i < nc; i++) k += cals[i].enabled && !cals[i].readonly && of_account(app, &cals[i], a);
        if (!k) continue;
        jd_text_fit(ui, app->prefs.cals[a].username ? app->prefs.cals[a].username : "", box.x + 12, ly + 7, box.width - 24,
                    FS_SMALL, JD_MUTED, 1);
        ly += 28;
        for (int i = 0; i < nc; i++) {
            if (!cals[i].enabled || cals[i].readonly || !of_account(app, &cals[i], a)) continue;
            Rectangle row = {box.x + 4, ly, box.width - 8, 30};
            int hover = CheckCollisionPointRec(GetMousePosition(), row) && CheckCollisionPointRec(GetMousePosition(), box);
            if (hover || cals[i].id == ed.calendar) DrawRectangleRounded(row, 0.3f, 6, hover ? JD_NORD3 : JD_NORD1);
            DrawCircle((int)(row.x + 16), (int)(row.y + 15), 5, hex_color(cals[i].color));
            jd_text_fit(ui, cals[i].name ? cals[i].name : cals[i].href, row.x + 30, mid_y(row, FS_EVENT), row.width - 40,
                        FS_EVENT, JD_NORD6, cals[i].id == ed.calendar);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ed.calendar = cals[i].id, cal_dd_open = 0;
            ly += 32;
        }
    }
    jd_scissor_end();
    jd_calendar_rows_free(cals, nc);
    /* a press anywhere else closes it */
    Vector2 m = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(m, box) && !CheckCollisionPointRec(m, dd_at))
        cal_dd_open = 0;
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
        int v = app->prefs.cal_view;
        cv.view = v >= V_AGENDA && v <= V_MONTH ? v : app->ui.touch ? V_AGENDA : V_WEEK;
        cv.opened = 1;
    }
    cv.anchor = local_midnight(jd_now());
    cv.stale = 1, cv.detail = -1, cv.sel = -1, cv.scrolled_for = -1;
}

static void step(int dir)
{
    if (cv.view == V_DAY) cv.anchor = add_days(cv.anchor, dir);
    else if (cv.view == V_AGENDA || cv.view == V_WEEK) cv.anchor = add_days(cv.anchor, dir * 7);
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

static void search_open(void)
{
    cs.open = cs.focus = 1;
    cv.detail = -1, cv.sel = -1, cv.scroll = 0;
    shown_open = 0;
}

static void search_close(void)
{
    cs.open = cs.focus = 0;
    jd_field_set(&cs.q, NULL);
    free(cs.found_for);
    cs.found_for = NULL;
    cv.stale = 1, cv.detail = -1, cv.sel = -1, cv.scroll = 0;
}

/* the search's keys while it is open: 1 when it took them */
static int search_keys(jd_app *app)
{
    if (!cs.open) return 0;
    if (cs.focus) {
        if (IsKeyPressed(KEY_ESCAPE)) search_close();
        else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_DOWN))
            cs.focus = 0, cv.sel = cv.nocc ? 0 : -1;
        else jd_field_input(&cs.q);
        return 1;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (cv.detail >= 0) cv.detail = -1;
        else search_close();
    } else if (IsKeyPressed(KEY_SLASH)) {
        cs.focus = 1;
        while (GetCharPressed()) {}
    } else if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) {
        if (cv.sel + 1 < cv.nocc) cv.sel++;
    } else if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) {
        if (cv.sel > 0) cv.sel--;
        else cs.focus = 1;
    } else if (IsKeyPressed(KEY_ENTER) && cv.sel >= 0) {
        if (cv.detail >= 0) editor_edit(app, cv.occs[cv.detail].event);
        else cv.detail = cv.sel;
    } else if (IsKeyPressed(KEY_E) && cv.sel >= 0) editor_edit(app, cv.occs[cv.sel].event);
    else {
        /* a view, a day or today: back from the results to the calendar */
        static const int leave[] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_A, KEY_W, KEY_M, KEY_T, KEY_H, KEY_L};
        for (int i = 0; i < (int)(sizeof leave / sizeof *leave); i++)
            if (IsKeyPressed(leave[i])) search_close();
        return 0;
    }
    return 1;
}

void calendar_keys(jd_app *app)
{
    if (picker_keys(app)) return;
    if (ed.open) {
        editor_keys(app);
        return;
    }
    /* the search field takes every key while it has the focus, Ctrl+V and the like too */
    if (cs.open && cs.focus) {
        search_keys(app);
        return;
    }
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl) return;
    if (search_keys(app)) return;
    if (IsKeyPressed(KEY_SLASH)) {
        search_open();
        while (GetCharPressed()) {}
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (shown_open) shown_open = 0;
        else if (cv.detail >= 0) cv.detail = -1;
        else tab_close(app, app->tab);
        return;
    }
    int sel = cv.detail >= 0 ? cv.detail : cv.sel;
    if (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_FOUR)) set_view(V_AGENDA);
    else if (IsKeyPressed(KEY_ONE)) set_view(V_DAY);
    else if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_TWO)) set_view(V_WEEK);
    else if (IsKeyPressed(KEY_M) || IsKeyPressed(KEY_THREE)) set_view(V_MONTH);
    else if (IsKeyPressed(KEY_T)) cv.anchor = local_midnight(jd_now()), cv.detail = -1, cv.sel = -1;
    else if (IsKeyPressed(KEY_L) || IsKeyPressed(KEY_RIGHT)) {
        if (cv.view == V_AGENDA || cv.view == V_DAY || shift) step(1);
        else cv.anchor = add_days(cv.anchor, 1), cv.sel = -1;
    } else if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_LEFT)) {
        if (cv.view == V_AGENDA || cv.view == V_DAY || shift) step(-1);
        else cv.anchor = add_days(cv.anchor, -1), cv.sel = -1;
    } else if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) {
        if (cv.view == V_MONTH) cv.anchor = add_days(cv.anchor, 7), cv.sel = -1;
        else if (cv.nocc) {
            /* the next occurrence on or after the selected day */
            int i = cv.sel + 1;
            if ((cv.view == V_WEEK || cv.view == V_DAY) && cv.sel < 0)
                while (i < cv.nocc && end_day(&cv.occs[i]) <= cv.anchor) i++;
            if (cv.view == V_WEEK || cv.view == V_DAY) while (i < cv.nocc && !on_day(&cv.occs[i], cv.anchor)) i++;
            if (i < cv.nocc) cv.sel = i;
        }
    } else if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) {
        if (cv.view == V_MONTH) cv.anchor = add_days(cv.anchor, -7), cv.sel = -1;
        else if (cv.sel > 0) {
            int i = cv.sel - 1;
            if (cv.view == V_WEEK || cv.view == V_DAY) while (i >= 0 && !on_day(&cv.occs[i], cv.anchor)) i--;
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
        sync_now(app);
    }
}

/* where the details box and the calendars list were drawn last frame: the views under them take no clicks */
static Rectangle detail_box, shown_box;
/* the y below the header: the details box goes under it */
static float header_bottom;

static int clicked(Rectangle b, Rectangle clip, jd_app *app)
{
    Vector2 m = GetMousePosition();
    return !app->dialog.kind && !ed.open && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, b) &&
           CheckCollisionPointRec(m, clip) && !(cv.detail >= 0 && CheckCollisionPointRec(m, detail_box)) &&
           !(shown_open && CheckCollisionPointRec(m, shown_box));
}

/* over before now: drawn faded, so what is still to come stands out */
static int is_past(const occ *o)
{
    int64_t now = frame_now ? frame_now : jd_now();
    if (o->all_day) return end_day(o) <= (frame_today ? frame_today : local_midnight(now));
    return (o->end > o->start ? o->end : o->start + 1) <= now;
}

/* all day, or running over midnight into another day: drawn as a bar across its days */
static int spans(const occ *o)
{
    return o->all_day || end_day(o) - first_day(o) > 86400 + 3600;
}

/* the text on a filled bar: dark on the light colours calendars use, light on dark ones */
static Color on_color(Color c)
{
    float l = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return l > 140 ? (Color){0x2e, 0x34, 0x40, 255} : (Color){0xec, 0xef, 0xf4, 255};
}

enum { CHIP_BAR, CHIP_LINE, CHIP_BLOCK };
/* clipped at a side: the event goes on beyond the shown days there */
enum { GOES_ON_LEFT = 1, GOES_ON_RIGHT = 2 };

/* An occurrence as the views draw it: a filled bar for an all-day or multi-day one (CHIP_BAR), a dot with its
   time for a timed one in a list of a day (CHIP_LINE), a tinted block in the hours grid (CHIP_BLOCK). Past ones
   fade; the chosen one is outlined. */
static void event_chip(jd_app *app, int i, Rectangle b, Rectangle clip, int style, int goes_on)
{
    jd_ui *ui = &app->ui;
    occ *o = &cv.occs[i];
    int chosen = i == cv.sel || i == cv.detail;
    int hover = CheckCollisionPointRec(GetMousePosition(), b) && CheckCollisionPointRec(GetMousePosition(), clip) &&
                !app->dialog.kind && !ed.open;
    float fade = is_past(o) ? 0.45f : 1.0f;
    const char *title = o->summary && o->summary[0] ? o->summary : "(no title)";
    if (style == CHIP_BAR) {
        /* a past one: a light tint of its colour with the page's text, still readable */
        Color fill = fade < 1 ? ColorAlpha(o->color, hover ? 0.4f : 0.28f) : ColorAlpha(o->color, hover ? 1.0f : 0.88f);
        DrawRectangleRounded(b, 0.35f, 6, fill);
        /* square where it goes on past the shown days */
        if (goes_on & GOES_ON_LEFT) DrawRectangleRec((Rectangle){b.x, b.y, 8, b.height}, fill);
        if (goes_on & GOES_ON_RIGHT) DrawRectangleRec((Rectangle){b.x + b.width - 8, b.y, 8, b.height}, fill);
        Color tc = fade < 1 ? JD_NORD4 : on_color(o->color);
        char line[320];
        if (o->all_day) snprintf(line, sizeof line, "%s", title);
        else {
            char t[16];
            fmt(o->start, "%H:%M", t, sizeof t);
            snprintf(line, sizeof line, "%s %s", t, title);
        }
        jd_text_fit(ui, line, b.x + 8, mid_y(b, FS_EVENT), b.width - 14, FS_EVENT, tc, 1);
    } else if (style == CHIP_LINE) {
        if (hover || chosen) DrawRectangleRounded(b, 0.35f, 6, chosen ? JD_NORD2 : JD_NORD1);
        DrawCircle((int)(b.x + 8), (int)(b.y + b.height / 2), 4, ColorAlpha(o->color, fade));
        char t[16];
        fmt(o->start, "%H:%M", t, sizeof t);
        float tw = jd_text_width(ui, t, FS_EVENT, 0);
        jd_text(ui, t, b.x + 18, mid_y(b, FS_EVENT), FS_EVENT, ColorAlpha(JD_NORD4, fade), 0);
        jd_text_fit(ui, title, b.x + 24 + tw, mid_y(b, FS_EVENT), b.width - 28 - tw, FS_EVENT, ColorAlpha(JD_NORD6, fade),
                    !is_past(o));
    } else {
        DrawRectangleRounded(b, 0.12f, 6, ColorAlpha(o->color, (chosen ? 0.6f : hover ? 0.48f : 0.36f) * fade));
        Color tc = ColorAlpha(JD_NORD6, fade < 1 ? 0.6f : 1.0f);
        if (b.height > 40) {
            jd_text_fit(ui, title, b.x + 7, b.y + 5, b.width - 12, FS_TITLE, tc, 1);
            char range_text[64];
            time_range(o, range_text, sizeof range_text);
            jd_text_fit(ui, range_text, b.x + 7, b.y + 25, b.width - 12, FS_SMALL, ColorAlpha(JD_NORD4, fade), 0);
            if (o->location && b.height > 62)
                jd_text_fit(ui, o->location, b.x + 7, b.y + 43, b.width - 12, FS_SMALL, ColorAlpha(JD_NORD4, fade), 0);
        } else {
            char t[16], line[320];
            fmt(o->start, "%H:%M", t, sizeof t);
            snprintf(line, sizeof line, "%s %s", t, title);
            jd_text_fit(ui, line, b.x + 7, mid_y(b, FS_EVENT), b.width - 12, FS_EVENT, tc, 0);
        }
    }
    if (chosen && style != CHIP_LINE) DrawRectangleRoundedLinesEx(b, style == CHIP_BAR ? 0.35f : 0.12f, 6, 1.5f, JD_NORD6);
    if (clicked(b, clip, app)) {
        if (jd_click_count() >= 2) editor_edit(app, o->event);
        else cv.detail = i, cv.sel = i;
    }
}

static float header(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    float x = r.x + 16, y = r.y + 12;
    if (jd_icon_button(ui, "chevron-left", NULL, (Rectangle){x, y, 34, 32}, 0)) step(-1);
    if (jd_icon_button(ui, "chevron-right", NULL, (Rectangle){x + 38, y, 34, 32}, 0)) step(1);
    if (jd_button(ui, "Today", (Rectangle){x + 78, y, 70, 32}, 0)) cv.anchor = local_midnight(jd_now()), cv.detail = -1;
    /* the segments in the order shown, and the views they are */
    static const char *const views[] = {"Day", "Week", "Month", "Agenda"};
    static const int view_of[] = {V_DAY, V_WEEK, V_MONTH, V_AGENDA};
    float segw = 4;
    for (int i = 0; i < 4; i++) segw += jd_text_width(ui, views[i], 13, 1) + 28;
    /* labels on Calendars and New where there is room; icons alone on a narrower page; on a phone's width the
       views and the title go on lines of their own */
    float title_w = 230, icons_row = 44 + 44 + 44 + 46 + 46;
    int labelled = r.width >= 160 + title_w + segw + 126 + 112 + icons_row - 84 + 40;
    int narrow = r.width < 160 + title_w + segw + icons_row + 40;
    float nw = labelled ? 112 : 40, nx = r.x + r.width - 16 - nw;
    if (jd_icon_button(ui, "pencil", labelled ? "New" : NULL, (Rectangle){nx, y, nw, 32}, 1)) editor_new(app, cv.anchor + 9 * 3600, 0);
    /* the footer says what the sync does; the icon turns accent while it runs */
    Rectangle sync = {nx - 44, y, 38, 32};
    if (jd_icon_button(ui, NULL, NULL, sync, 0) && !app->dialog.kind) sync_now(app);
    jd_icon("refresh-cw", sync.x + 11, sync.y + 8, 16, calendar_status() ? JD_NORD8 : JD_NORD6);
    if (CheckCollisionPointRec(GetMousePosition(), sync) && !plat_touch()) jd_tooltip("Sync now (r)", sync.x - 40, sync.y + 38);
    /* the calendar's settings: accounts, which calendars sync, the view it opens in */
    Rectangle gear = {sync.x - 44, y, 38, 32};
    if (jd_icon_button(ui, "settings", NULL, gear, 0) && !app->dialog.kind) settings_show_calendar(app);
    if (CheckCollisionPointRec(GetMousePosition(), gear) && !plat_touch()) jd_tooltip("Calendar settings", gear.x - 60, gear.y + 38);
    /* which synced calendars show: a list under this button */
    float cw = labelled ? 126 : 38;
    Rectangle cals = {gear.x - cw - 6, y, cw, 32};
    if (jd_icon_button(ui, "eye", labelled ? "Calendars" : NULL, cals, 0) && !app->dialog.kind) shown_open = !shown_open;
    if (!labelled && CheckCollisionPointRec(GetMousePosition(), cals) && !plat_touch()) jd_tooltip("Show calendars", cals.x - 40, cals.y + 38);
    shown_box.x = cals.x + cals.width - 300 < r.x + 8 ? r.x + 8 : cals.x + cals.width - 300;
    shown_box.y = y + 38;
    Rectangle find = {cals.x - 44, y, 38, 32};
    if (jd_icon_button(ui, "search", NULL, find, cs.open) && !app->dialog.kind) {
        if (cs.open) search_close();
        else search_open();
    }
    if (CheckCollisionPointRec(GetMousePosition(), find) && !plat_touch()) jd_tooltip("Search events (/)", find.x - 50, find.y + 38);
    float sx = narrow ? x : find.x - segw - 12, sy = narrow ? y + 40 : y;
    int cur = 0;
    for (int i = 0; i < 4; i++)
        if (view_of[i] == cv.view) cur = i;
    int v = jd_segmented(ui, sx, sy, views, 4, cur);
    if (v != cur) {
        set_view(view_of[v]);
        /* drawn again over itself, so the frame of the click already shows the view chosen */
        jd_segmented(ui, sx, sy, views, 4, v);
    }
    /* the title last: the arrows, Today and the views above may have just changed what it names */
    char title[96];
    if (cv.view == V_MONTH) fmt(cv.anchor, "%B %Y", title, sizeof title);
    else if (cv.view == V_WEEK) {
        int64_t ws = week_start(cv.anchor), we = add_days(ws, 6);
        char a[32], b[32];
        fmt(ws, "%d %b", a, sizeof a);
        fmt(we, "%d %b %Y", b, sizeof b);
        snprintf(title, sizeof title, "%s – %s", a, b);
    } else if (cv.view == V_DAY) fmt(cv.anchor, "%A %d %B %Y", title, sizeof title);
    else fmt(cv.anchor, "From %a %d %b %Y", title, sizeof title);
    /* searching: the field in the title's place */
    if (cs.open) {
        cs.q.placeholder = "Search events: title, place, notes";
        Rectangle f = narrow ? (Rectangle){x, y + 78, r.width - 32, 32} : (Rectangle){x + 160, y, sx - x - 172, 32};
        if (jd_field_draw(ui, &cs.q, f, cs.focus)) cs.focus = 1;
        return narrow ? y + 118 : y + 44;
    }
    if (!narrow) {
        jd_text_fit(ui, title, x + 160, y + 7, sx - x - 170, 17, JD_NORD6, 1);
        return y + 44;
    }
    jd_text_fit(ui, title, x, y + 82, r.width - 32, 16, JD_NORD6, 1);
    return y + 108;
}

/* the synced calendars, each to show or hide; drawn over the views */
static void shown_list(jd_app *app, Rectangle page)
{
    if (!shown_open) return;
    jd_ui *ui = &app->ui;
    int n;
    jd_calendar_row *c = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
    /* each account with synced calendars: its name, then those calendars */
    int synced = 0, groups = 0;
    for (int a = 0; a < app->prefs.ncals; a++) {
        int k = 0;
        for (int i = 0; c && i < n; i++) k += c[i].enabled && of_account(app, &c[i], a);
        synced += k, groups += k > 0;
    }
    /* the colour picker of one of them opens inside, under its row */
    int picking = 0;
    for (int i = 0; c && i < n; i++) picking |= c[i].enabled && c[i].id == cp.cal;
    float w = 340, h = synced ? 52 + 34 * (float)synced + 30 * (float)groups + (picking ? PICKER_H : 0) : 110;
    if (shown_box.x + w > page.x + page.width - 8) shown_box.x = page.x + page.width - 8 - w;
    shown_box.width = w, shown_box.height = h;
    Rectangle b = shown_box;
    DrawRectangleRounded((Rectangle){b.x + 2, b.y + 3, w, h}, 0.06f, 6, ColorAlpha(BLACK, 0.3f));
    DrawRectangleRounded(b, 0.06f, 6, JD_NORD1);
    jd_text(ui, "Show calendars", b.x + 16, b.y + 14, FS_TITLE, JD_NORD6, 1);
    float y = b.y + 44;
    for (int a = 0; a < app->prefs.ncals; a++) {
        int k = 0;
        for (int i = 0; c && i < n; i++) k += c[i].enabled && of_account(app, &c[i], a);
        if (!k) continue;
        const jd_cal_account *ac = &app->prefs.cals[a];
        jd_icon("calendar", b.x + 16, y + 6, 14, JD_MUTED);
        char who[300];
        snprintf(who, sizeof who, "%s%s", ac->username ? ac->username : "", ac->provider == 2 ? "  ·  Google" : "  ·  CalDAV");
        jd_text_fit(ui, who, b.x + 38, y + 6, w - 54, FS_SMALL, JD_MUTED, 1);
        y += 30;
        for (int i = 0; c && i < n; i++) {
            if (!c[i].enabled || !of_account(app, &c[i], a)) continue;
            Rectangle row = {b.x + 8, y, w - 16 - 40, 32};
            Rectangle paint = {b.x + w - 8 - 38, y, 38, 32};
            int hover = CheckCollisionPointRec(GetMousePosition(), row) && !app->dialog.kind && !cp.drag;
            if (hover) DrawRectangleRounded(row, 0.3f, 6, JD_NORD2);
            if (jd_icon_button(ui, "paintbrush", NULL, paint, cp.cal == c[i].id) && !app->dialog.kind) picker_toggle(&c[i]);
            Color col = hex_color(c[i].color);
            /* a box in the calendar's own colour, filled when shown */
            Rectangle box = {row.x + 14, y + 8, 16, 16};
            if (c[i].shown) DrawRectangleRounded(box, 0.3f, 6, col);
            else DrawRectangleRoundedLinesEx(box, 0.3f, 6, 1.5f, col);
            if (c[i].shown) jd_icon("check", box.x + 1, box.y + 1, 14, on_color(col));
            jd_text_fit(ui, c[i].name ? c[i].name : c[i].href, row.x + 40, mid_y(row, FS_EVENT), row.width - 48, FS_EVENT,
                        c[i].shown ? JD_NORD6 : JD_MUTED, 0);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                jd_store_calendar_show(cal_store, c[i].id, !c[i].shown);
                cv.stale = 1, rem_stale = 1;
                share_changed(app);
            }
            y += 34;
            if (cp.cal == c[i].id) y = picker_draw(app, &c[i], b.x + 8, y, w - 16, NULL);
        }
    }
    if (!synced) {
        jd_text_fit(ui, "No calendar is synced yet.", b.x + 16, b.y + 42, w - 32, 13, JD_MUTED, 0);
        if (jd_button(ui, "Choose in Settings", (Rectangle){b.x + 16, b.y + 64, 170, 32}, 1)) {
            shown_open = 0;
            settings_show_calendar(app);
        }
    }
    if (c) jd_calendar_rows_free(c, n);
    /* a press outside closes it (the button that opened it toggles it itself) */
    Vector2 m = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(m, b) && m.y > b.y - 6) shown_open = 0;
}

static void empty_note(jd_app *app, Rectangle r, float y)
{
    const char *why = app->prefs.ncals ? (calendar_error() ? calendar_error() : "No events here.")
                                              : "No calendar set up yet: Settings, Calendar connects a CalDAV server or Google.";
    jd_text_fit(&app->ui, why, r.x + 16, y, r.width - 32, 14, calendar_error() ? JD_NORD13 : JD_MUTED, 0);
    /* nothing set up, or an account still to sign in here: where to do it */
    int unsigned_ = 0;
    for (int i = 0; i < nacc; i++) unsigned_ |= acc[i].unsigned_;
    if ((!app->prefs.ncals || unsigned_) && jd_button(&app->ui, "Open Settings", (Rectangle){r.x + 16, y + 30, 140, 34}, 1))
        settings_show_calendar(app);
}

/* text centred on (cx, cy) by its drawn glyphs */
static void centred_number(jd_app *app, const char *n, float cx, float cy, float size, Color c, int bold)
{
    Rectangle ink = jd_text_ink(&app->ui, n, size, bold);
    jd_text(&app->ui, n, cx - ink.x - ink.width / 2, cy - ink.y - ink.height / 2, size, c, bold);
}

/* the date as the agenda and the day headers show it: the weekday small, the day's number large, today's in an
   accent circle with the number centred in it */
static void day_badge(jd_app *app, int64_t day, float cx, float y, int dim)
{
    jd_ui *ui = &app->ui;
    int64_t today = local_midnight(jd_now());
    char wd[16], num[8];
    fmt(day, "%a", wd, sizeof wd);
    fmt(day, "%e", num, sizeof num);
    const char *n = num[0] == ' ' ? num + 1 : num;
    float ww = jd_text_width(ui, wd, FS_SMALL, 1);
    jd_text(ui, wd, cx - ww / 2, y, FS_SMALL, day == today ? JD_NORD8 : dim ? JD_MUTED : JD_NORD4, 1);
    if (day == today) DrawCircleV((Vector2){cx, y + 33}, 18, JD_NORD10);
    centred_number(app, n, cx, y + 33, 22, day == today ? JD_ON_ACCENT : dim ? JD_MUTED : JD_NORD6, 1);
}

typedef struct {
    int64_t day;
    int i;
} agenda_row;

enum { AG_ROW = 40, AG_DAY_MIN = 66, AG_GAP = 14 };

/* Every day from the anchor on that has something, its date in a column on the left; all-day and multi-day events
   first, as bars, then the timed ones with their times. A multi-day event shows on each of its days. Past events
   are dimmed, and a line marks now among today's. */
static void agenda(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    int64_t today = local_midnight(jd_now()), now = jd_now();
    agenda_row *rows = NULL;
    int nrows = 0;
    for (int64_t day = cv.from; day < cv.to; day = add_days(day, 1))
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < cv.nocc; i++) {
                if (!on_day(&cv.occs[i], day) || spans(&cv.occs[i]) != !pass) continue;
                rows = jd_realloc(rows, sizeof *rows * (size_t)(nrows + 1));
                rows[nrows++] = (agenda_row){day, i};
            }
    if (!nrows) {
        free(rows);
        empty_note(app, r, r.y + 12);
        return;
    }
    /* heights first: the scroll range, and where the chosen event is */
    float content = 8, sel_y = -1;
    for (int k = 0; k < nrows;) {
        int k1 = k;
        while (k1 < nrows && rows[k1].day == rows[k].day) {
            if (rows[k1].i == cv.sel && sel_y < 0) sel_y = content + (float)(k1 - k) * AG_ROW;
            k1++;
        }
        float h = (float)(k1 - k) * AG_ROW;
        content += (h < AG_DAY_MIN ? AG_DAY_MIN : h) + AG_GAP;
        k = k1;
    }
    if (CheckCollisionPointRec(GetMousePosition(), r) && !ed.open) cv.scroll -= jd_wheel_y(AG_ROW);
    /* j and k keep the chosen event in sight */
    static int followed = -1;
    if (cv.sel != followed && sel_y >= 0) {
        if (sel_y < cv.scroll + 20) cv.scroll = sel_y - 20;
        if (sel_y + AG_ROW + 20 > cv.scroll + r.height) cv.scroll = sel_y + AG_ROW + 20 - r.height;
    }
    followed = cv.sel;
    if (cv.scroll > content - r.height + 20) cv.scroll = content - r.height + 20;
    if (cv.scroll < 0) cv.scroll = 0;

    jd_scissor(r.x, r.y, r.width, r.height);
    float y = r.y + 8 - cv.scroll, col = 76, x = r.x + col, w = r.width - col - 20;
    for (int k = 0; k < nrows;) {
        int64_t day = rows[k].day;
        int k1 = k;
        while (k1 < nrows && rows[k1].day == day) k1++;
        float h = (float)(k1 - k) * AG_ROW;
        if (h < AG_DAY_MIN) h = AG_DAY_MIN;
        day_badge(app, day, r.x + col / 2, y + 2, day < today);
        int now_drawn = day != today;
        float ry = y;
        for (int q = k; q < k1; q++) {
            occ *o = &cv.occs[rows[q].i];
            if (!now_drawn && !spans(o) && o->start >= now) {
                DrawRectangle((int)x, (int)ry - 1, (int)w, 2, JD_NORD11);
                DrawCircle((int)x, (int)ry, 4, JD_NORD11);
                now_drawn = 1;
            }
            Rectangle row = {x, ry + 2, w, AG_ROW - 4};
            if (spans(o)) {
                event_chip(app, rows[q].i, row, r, CHIP_BAR, 0);
                /* which day of several this is */
                int total = (int)((end_day(o) - first_day(o) + 43200) / 86400), nth = (int)((day - first_day(o) + 43200) / 86400) + 1;
                if (total > 1) {
                    char part[32];
                    snprintf(part, sizeof part, "day %d of %d", nth, total);
                    float pw = jd_text_width(ui, part, FS_SMALL, 0);
                    jd_text(ui, part, row.x + row.width - pw - 10, mid_y(row, FS_SMALL), FS_SMALL,
                            is_past(o) ? JD_MUTED : ColorAlpha(on_color(o->color), 0.8f), 0);
                }
            } else {
                float fade = is_past(o) ? 0.45f : 1.0f;
                int chosen = rows[q].i == cv.sel || rows[q].i == cv.detail;
                int hover = CheckCollisionPointRec(GetMousePosition(), row) && CheckCollisionPointRec(GetMousePosition(), r) &&
                            !ed.open && !app->dialog.kind;
                if (chosen || hover) DrawRectangleRounded(row, 0.25f, 6, chosen ? JD_NORD2 : JD_NORD1);
                char when[48];
                time_range(o, when, sizeof when);
                jd_text_fit(ui, when, row.x + 10, mid_y(row, FS_TITLE), 116, FS_TITLE, ColorAlpha(JD_NORD4, fade), 0);
                DrawCircle((int)(row.x + 138), (int)(row.y + row.height / 2), 5, ColorAlpha(o->color, fade));
                const char *title = o->summary && o->summary[0] ? o->summary : "(no title)";
                float tx = row.x + 154, tw = row.width - 164;
                float titw = jd_text_width(ui, title, 16, 1);
                jd_text_fit(ui, title, tx, mid_y(row, 16), tw, 16, ColorAlpha(JD_NORD6, fade), 1);
                if (o->location && o->location[0] && titw + 40 < tw) {
                    char where[300];
                    snprintf(where, sizeof where, "·  %s", o->location);
                    jd_text_fit(ui, where, tx + titw + 10, mid_y(row, FS_TITLE), tw - titw - 10, FS_TITLE, ColorAlpha(JD_MUTED, fade), 0);
                }
                if (clicked(row, r, app)) {
                    if (jd_click_count() >= 2) editor_edit(app, o->event);
                    else cv.sel = rows[q].i, cv.detail = rows[q].i;
                }
            }
            ry += AG_ROW;
        }
        if (!now_drawn) {
            DrawRectangle((int)x, (int)ry - 1, (int)w, 2, JD_NORD11);
            DrawCircle((int)x, (int)ry, 4, JD_NORD11);
        }
        y += h + AG_GAP;
        DrawRectangle((int)x, (int)(y - AG_GAP / 2), (int)w, 1, JD_NORD1);
        k = k1;
    }
    jd_scissor_end();
    jd_scrollbar(r, content + 20, &cv.scroll);
    free(rows);
}

/* the events holding the search's words (each, in any case) in their title, place or notes, into cv.occs; a
   repeating event once, at its first date */
static void search_run(jd_app *app)
{
    if (cs.found_for && strcmp(cs.found_for, jd_field_text(&cs.q)) == 0) return;
    free(cs.found_for);
    cs.found_for = jd_strdup(jd_field_text(&cs.q));
    occs_free(cv.occs, cv.nocc);
    cv.occs = NULL, cv.nocc = 0, cv.sel = -1, cv.detail = -1, cv.scroll = 0;
    cv.from = cv.to = 0;
    char *words = jd_strdup(jd_field_text(&cs.q)), *save = NULL;
    char *first = strtok_r(words, " \t", &save);
    if (!first || !cal_store) {
        free(words);
        return;
    }
    char *rest[16];
    int nrest = 0;
    for (char *t; nrest < 16 && (t = strtok_r(NULL, " \t", &save));) rest[nrest++] = t;
    int n;
    jd_event_row *rows = jd_store_events_search(cal_store, first, &n);
    for (int i = 0; i < n; i++) {
        jd_ical_event e = {.alarm = -1};
        jd_ical_read(rows[i].ics, &e);
        int ok = 1;
        for (int w = -1; w < nrest && ok; w++) {
            const char *word = w < 0 ? first : rest[w];
            ok = (rows[i].summary && strcasestr(rows[i].summary, word)) ||
                 (rows[i].location && strcasestr(rows[i].location, word)) ||
                 (e.description && strcasestr(e.description, word));
        }
        jd_ical_event_free(&e);
        if (!ok) continue;
        occ x = {rows[i].id, rows[i].dtstart, rows[i].dtend, rows[i].all_day, rows[i].readonly, -1, rows[i].recurring,
                 jd_strdup(rows[i].summary), jd_strdup(rows[i].location), hex_color(rows[i].color), 0, 0};
        if (rows[i].recurring) {
            /* a series still running shows at its next date */
            jd_occurrence *o = NULL;
            int k = 0;
            int64_t now = jd_now();
            jd_ical_expand(rows[i].ics, now, now + 400 * 86400LL, &o, &k);
            if (k) x.start = o[0].start, x.end = o[0].end;
            jd_occurrences_free(o, k);
        }
        x.day0 = first_day(&x);
        x.day1 = end_day(&x);
        cv.occs = jd_realloc(cv.occs, sizeof *cv.occs * (size_t)(cv.nocc + 1));
        cv.occs[cv.nocc++] = x;
    }
    jd_event_rows_free(rows, n);
    free(words);
    /* latest first */
    if (cv.nocc) qsort(cv.occs, (size_t)cv.nocc, sizeof *cv.occs, by_start);
    for (int i = 0, j = cv.nocc - 1; i < j; i++, j--) {
        occ t = cv.occs[i];
        cv.occs[i] = cv.occs[j], cv.occs[j] = t;
    }
}

int calendar_find(const char *text, cal_found *out, int max)
{
    if (!cal_store || !text[0]) return 0;
    int n, k = 0;
    jd_event_row *rows = jd_store_events_search(cal_store, text, &n);
    for (int i = 0; i < n && k < max; i++) {
        out[k].id = rows[i].id, out[k].start = rows[i].dtstart, out[k].all_day = rows[i].all_day;
        snprintf(out[k].title, sizeof out[k].title, "%s", rows[i].summary ? rows[i].summary : "(no title)");
        snprintf(out[k].place, sizeof out[k].place, "%s", rows[i].location ? rows[i].location : "");
        k++;
    }
    jd_event_rows_free(rows, n);
    return k;
}

int calendar_upcoming(jd_app *app, cal_found *out, int max, int *total)
{
    int64_t now = jd_now(), today = local_midnight(now);
    *total = 0;
    for (int d = 0; d < 7; d++) {
        int n, k = 0;
        occ *o = gather(app, add_days(today, d), add_days(today, d + 1), &n);
        for (int i = 0; i < n; i++) {
            if (o[i].end <= now) continue;
            if (k < max) {
                out[k] = (cal_found){.id = o[i].event, .start = o[i].start, .all_day = o[i].all_day};
                snprintf(out[k].title, sizeof out[k].title, "%s", o[i].summary ? o[i].summary : "(no title)");
                snprintf(out[k].place, sizeof out[k].place, "%s", o[i].location ? o[i].location : "");
            }
            k++;
        }
        occs_free(o, n);
        if (k) {
            *total = k;
            return k < max ? k : max;
        }
    }
    return 0;
}

void calendar_open_at(jd_app *app, int64_t t)
{
    calendar_open(app);
    cv.anchor = local_midnight(t);
    cv.stale = 1;
}

void calendar_show_event(jd_app *app, const char *text, int64_t id)
{
    calendar_open(app);
    search_open();
    cs.focus = 0;
    jd_field_set(&cs.q, text);
    search_run(app);
    for (int i = 0; i < cv.nocc; i++)
        if (cv.occs[i].event == id) cv.sel = cv.detail = i;
}

/* the search's results, latest first: date, time, title and place */
static void search_list(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    search_run(app);
    if (!cs.q.len) {
        jd_text(ui, "Type to search every event, past and future.", r.x + 24, r.y + 20, FS_TITLE, JD_MUTED, 0);
        return;
    }
    if (!cv.nocc) {
        jd_text(ui, "No events match.", r.x + 24, r.y + 20, FS_TITLE, JD_MUTED, 0);
        return;
    }
    float content = 8 + (float)cv.nocc * AG_ROW;
    if (CheckCollisionPointRec(GetMousePosition(), r) && !ed.open) cv.scroll -= jd_wheel_y(AG_ROW);
    static int followed = -1;
    if (cv.sel != followed && cv.sel >= 0) {
        float sy = 8 + (float)cv.sel * AG_ROW;
        if (sy < cv.scroll + 20) cv.scroll = sy - 20;
        if (sy + AG_ROW + 20 > cv.scroll + r.height) cv.scroll = sy + AG_ROW + 20 - r.height;
    }
    followed = cv.sel;
    if (cv.scroll > content - r.height + 20) cv.scroll = content - r.height + 20;
    if (cv.scroll < 0) cv.scroll = 0;
    jd_scissor(r.x, r.y, r.width, r.height);
    float x = r.x + 16, w = r.width - 36;
    for (int i = 0; i < cv.nocc; i++) {
        Rectangle row = {x, r.y + 8 - cv.scroll + (float)i * AG_ROW, w, AG_ROW - 4};
        if (row.y + row.height < r.y || row.y > r.y + r.height) continue;
        occ *o = &cv.occs[i];
        float fade = is_past(o) ? 0.45f : 1.0f;
        int chosen = i == cv.sel || i == cv.detail;
        int hover = CheckCollisionPointRec(GetMousePosition(), row) && CheckCollisionPointRec(GetMousePosition(), r) &&
                    !ed.open && !app->dialog.kind;
        if (chosen || hover) DrawRectangleRounded(row, 0.25f, 6, chosen ? JD_NORD2 : JD_NORD1);
        char day[48], when[48];
        fmt(o->all_day ? first_day(o) : o->start, "%a %d %b %Y", day, sizeof day);
        time_range(o, when, sizeof when);
        jd_text_fit(ui, day, row.x + 10, mid_y(row, FS_TITLE), 130, FS_TITLE, ColorAlpha(JD_NORD6, fade), 1);
        jd_text_fit(ui, when, row.x + 150, mid_y(row, FS_TITLE), 116, FS_TITLE, ColorAlpha(JD_NORD4, fade), 0);
        DrawCircle((int)(row.x + 280), (int)(row.y + row.height / 2), 5, ColorAlpha(o->color, fade));
        char line[400];
        snprintf(line, sizeof line, "%s%s%s%s", o->summary && o->summary[0] ? o->summary : "(no title)",
                 o->location && o->location[0] ? "  ·  " : "", o->location ? o->location : "", o->recurring ? "  ·  repeats" : "");
        jd_text_fit(ui, line, row.x + 296, mid_y(row, FS_TITLE), row.width - 306, FS_TITLE, ColorAlpha(JD_NORD6, fade), 0);
        if (clicked(row, r, app)) {
            if (jd_click_count() >= 2) editor_edit(app, o->event);
            else cv.sel = i, cv.detail = i, cs.focus = 0;
        }
    }
    jd_scissor_end();
    jd_scrollbar(r, content + 20, &cv.scroll);
}

/* the columns d0..d1 (exclusive) of the shown days an occurrence covers, from first; 0 when it covers none */
static int columns(const occ *o, int64_t first, int ndays, int *d0, int *d1, int *goes_on)
{
    int a = days_between(first, first_day(o)), b = days_between(first, end_day(o));
    *d0 = a < 0 ? 0 : a > ndays ? ndays : a;
    *d1 = b > ndays ? ndays : b < 0 ? 0 : b;
    *goes_on = (a < 0 ? GOES_ON_LEFT : 0) | (b > ndays ? GOES_ON_RIGHT : 0);
    return *d1 > *d0;
}

/* Lanes for the bars of all-day and multi-day occurrences among the days from first: lane_of[i] for each (-1 for
   the rest); returns how many lanes there are */
static int bar_lanes(int64_t first, int ndays, int *lane_of, int max)
{
    int lanes = 0;
    for (int i = 0; i < cv.nocc && i < max; i++) {
        lane_of[i] = -1;
        int d0, d1, g;
        if (!spans(&cv.occs[i]) || !columns(&cv.occs[i], first, ndays, &d0, &d1, &g)) continue;
        int l = 0;
        for (;; l++) {
            int clash = 0;
            for (int k = 0; k < i && !clash; k++)
                clash = lane_of[k] == l && first_day(&cv.occs[k]) < end_day(&cv.occs[i]) &&
                        first_day(&cv.occs[i]) < end_day(&cv.occs[k]);
            if (!clash) break;
        }
        lane_of[i] = l;
        if (l + 1 > lanes) lanes = l + 1;
    }
    return lanes;
}

enum { MAX_DRAWN = 2048 };

/* Day and week: ndays columns from first, all-day and multi-day events as bars across their days on top, the timed
   ones in the hours grid; what is over is shaded */
static void days_view(jd_app *app, Rectangle r, int64_t first, int ndays)
{
    jd_ui *ui = &app->ui;
    int64_t today = local_midnight(jd_now());
    float gutter = 52, colw = (r.width - gutter - 12) / (float)ndays;
    for (int d = 0; d < ndays; d++) {
        int64_t day = add_days(first, d);
        float cx = r.x + gutter + colw * (float)d;
        Rectangle head = {cx, r.y, colw, 58};
        day_badge(app, day, cx + colw / 2, r.y + 2, day < today);
        if (day == cv.anchor && ndays > 1) DrawRectangle((int)cx + 8, (int)r.y + 56, (int)colw - 16, 2, JD_NORD8);
        if (clicked(head, r, app)) cv.anchor = day, cv.sel = -1;
    }
    float y = r.y + 64;
    static int lane_of[MAX_DRAWN];
    int lanes = bar_lanes(first, ndays, lane_of, MAX_DRAWN), shown = lanes > 4 ? 4 : lanes;
    for (int i = 0; i < cv.nocc && i < MAX_DRAWN; i++) {
        if (lane_of[i] < 0 || lane_of[i] >= 4) continue;
        int d0, d1, goes_on;
        columns(&cv.occs[i], first, ndays, &d0, &d1, &goes_on);
        Rectangle bar = {r.x + gutter + colw * (float)d0 + 2, y + 28 * (float)lane_of[i], colw * (float)(d1 - d0) - 4, 25};
        event_chip(app, i, bar, r, CHIP_BAR, goes_on);
    }
    if (lanes > 4) {
        char more[24];
        snprintf(more, sizeof more, "+%d", lanes - 4);
        jd_text(ui, more, r.x + 8, y + 28 * 3 + 6, 12, JD_MUTED, 0);
    }
    if (shown) jd_text(ui, "all day", r.x + 6, y + 6, 12, JD_MUTED, 0);
    y += 28 * (float)shown + (shown ? 6 : 0);
    DrawRectangle((int)r.x, (int)y, (int)r.width, 1, JD_NORD1);
    Rectangle grid = {r.x, y + 1, r.width, r.y + r.height - y - 1};
    float content = 24 * HOUR_H + 8;
    if (cv.scrolled_for != cv.view) {
        cv.scroll = 8 * HOUR_H - 8;
        cv.scrolled_for = cv.view;
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
        if (h) jd_text(ui, hh, r.x + 6, ly - 8, FS_SMALL, JD_MUTED, 0);
    }
    for (int d = 0; d < ndays; d++) {
        int64_t day = add_days(first, d), next = add_days(day, 1);
        float cx = r.x + gutter + colw * (float)d;
        DrawRectangle((int)cx, (int)grid.y, 1, (int)grid.height + 0, JD_NORD1);
        /* the part of the day that is over */
        float over = day < today ? 24 * HOUR_H : day == today ? (float)(jd_now() - day) / 3600.0f * HOUR_H : 0;
        if (over > 0) DrawRectangleRec((Rectangle){cx + 1, gy, colw - 1, over}, ColorAlpha(BLACK, 0.12f));
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
            if (spans(o) || !(o->start < next && (o->end > day || (o->end == o->start && o->start >= day)))) continue;
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
            event_chip(app, idx[a], b, grid, CHIP_BLOCK, 0);
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
        jd_text_fit(ui, name, r.x + cw * (float)d + 8, r.y + 3, cw - 10, FS_SMALL, JD_MUTED, 1);
    }
    const float LINE = 22;
    int lines = (int)((ch - 32) / LINE);
    if (lines < 0) lines = 0;
    static int lane_of[MAX_DRAWN];
    for (int row = 0; row < 6; row++) {
        int64_t wk = add_days(first, row * 7);
        float y = top + ch * (float)row;
        /* the cells, the past ones shaded, the chosen one tinted */
        for (int d = 0; d < 7; d++) {
            int64_t day = add_days(wk, d);
            float x = r.x + cw * (float)d;
            Rectangle box = {x, y, cw, ch};
            if (day < today) DrawRectangleRec(box, ColorAlpha(BLACK, 0.12f));
            if (day == cv.anchor) DrawRectangleRec((Rectangle){x + 1, y + 1, cw - 2, ch - 2}, ColorAlpha(JD_NORD2, 0.5f));
            DrawRectangleLinesEx(box, 0.5f, JD_NORD1);
            char num[8];
            fmt(day, "%e", num, sizeof num);
            const char *n = num[0] == ' ' ? num + 1 : num;
            float ncx = x + 18, ncy = y + 15;
            if (day == today) DrawCircleV((Vector2){ncx, ncy}, 13, JD_NORD10);
            int inside = local_tm(day).tm_mon == cur_month;
            centred_number(app, n, ncx, ncy, 14, day == today ? JD_ON_ACCENT : inside && day >= today ? JD_NORD6 : JD_MUTED,
                           day == today);
            if (clicked(box, r, app) && cv.detail < 0) {
                if (jd_click_count() >= 2) editor_new(app, day + 9 * 3600, 0);
                else if (day != cv.anchor) cv.anchor = day, cv.sel = -1;
            }
        }
        /* all-day and multi-day events as bars across their days, in lanes under the dates */
        int lanes = bar_lanes(wk, 7, lane_of, MAX_DRAWN);
        int barcap = lanes < lines ? lanes : lines - 1; /* the last line stays free for "+n more" */
        int top_of[7] = {0}, hidden[7] = {0};
        for (int i = 0; i < cv.nocc && i < MAX_DRAWN; i++) {
            if (lane_of[i] < 0) continue;
            int d0, d1, goes_on;
            columns(&cv.occs[i], wk, 7, &d0, &d1, &goes_on);
            for (int d = d0; d < d1; d++) {
                if (lane_of[i] < barcap && lane_of[i] + 1 > top_of[d]) top_of[d] = lane_of[i] + 1;
                if (lane_of[i] >= barcap) hidden[d]++;
            }
            if (lane_of[i] >= barcap) continue;
            Rectangle bar = {r.x + cw * (float)d0 + 3, y + 32 + LINE * (float)lane_of[i], cw * (float)(d1 - d0) - 6, LINE - 2};
            event_chip(app, i, bar, r, CHIP_BAR, goes_on);
        }
        /* then each day's timed events; the last line says how many more there are when not all fit */
        for (int d = 0; d < 7; d++) {
            int64_t day = add_days(wk, d);
            float x = r.x + cw * (float)d;
            int count = 0;
            for (int i = 0; i < cv.nocc; i++) count += !spans(&cv.occs[i]) && on_day(&cv.occs[i], day);
            int room = lines - top_of[d];
            if (count + hidden[d] > room && room > 0) room--;
            int shown = 0;
            for (int i = 0; i < cv.nocc && shown < room; i++) {
                if (spans(&cv.occs[i]) || !on_day(&cv.occs[i], day)) continue;
                event_chip(app, i, (Rectangle){x + 3, y + 32 + LINE * (float)(top_of[d] + shown), cw - 6, LINE - 2}, r,
                           CHIP_LINE, 0);
                shown++;
            }
            int more = count - shown + hidden[d];
            if (more > 0) {
                char m[24];
                snprintf(m, sizeof m, "+%d more", more);
                jd_text(ui, m, x + 8, y + 32 + LINE * (float)(top_of[d] + shown) + 3, FS_SMALL, JD_NORD4, 1);
            }
        }
    }
}

/* the chosen occurrence: when, where, notes, and what can be done */
static void details(jd_app *app, Rectangle page)
{
    if (cv.detail < 0 || cv.detail >= cv.nocc) return;
    jd_ui *ui = &app->ui;
    occ *o = &cv.occs[cv.detail];
    jd_event_row *row = cal_store ? jd_store_event(cal_store, o->event) : NULL;
    if (!row) {
        cv.detail = -1;
        return;
    }
    jd_ical_event e = {.alarm = -1};
    jd_ical_read(row->ics, &e);
    float w = page.width - 32 < 420 ? page.width - 32 : 420;
    float x = page.x + page.width - w - 16, y = header_bottom + 6;
    /* an event in another zone: its times as the clock there reads them too */
    char there[160] = "";
    if (e.tzid && !o->all_day && jd_tz_known(e.tzid) && strcmp(e.tzid, jd_local_tzid()) != 0) {
        int y0, m0, d0, h0, i0, y1, m1, d1, h1, i1;
        jd_tz_wall(e.tzid, o->start, &y0, &m0, &d0, &h0, &i0);
        jd_tz_wall(e.tzid, o->end, &y1, &m1, &d1, &h1, &i1);
        char city[64];
        zone_city(e.tzid, city, sizeof city);
        snprintf(there, sizeof there, "%02d:%02d – %02d:%02d in %s", h0, i0, h1, i1, city);
    }
    float h = 170 + (e.description ? 60 : 0) + (o->location ? 22 : 0) + (there[0] ? 22 : 0);
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
    if (there[0]) {
        jd_text_fit(ui, there, x + 20, ty, w - 40, 13, JD_NORD8, 0);
        ty += 22;
    }
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
    frame_now = jd_now();
    frame_today = local_midnight(frame_now);
    float y = header_bottom = header(app, r);
    /* after the header: a view or day chosen there is drawn with its own events in this very frame */
    reload(app);
    Rectangle body = {r.x + 8, y, r.width - 16, r.y + r.height - y};
    if (cs.open) search_list(app, body);
    else if (cv.view == V_AGENDA) agenda(app, body);
    else if (cv.view == V_DAY) days_view(app, body, cv.anchor, 1);
    else if (cv.view == V_WEEK) days_view(app, body, week_start(cv.anchor), 7);
    else month(app, body);
    if (!cv.nocc && cv.view != V_AGENDA && !app->prefs.ncals) empty_note(app, body, body.y + 40);
    details(app, r);
    shown_list(app, r);
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
        jd_event_row *have = cal_store && inv.event.uid ? jd_store_event_by_uid(cal_store, inv.event.uid) : NULL;
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
                /* the calendar that has the event already, else the first synced one that can be written */
                jd_event_row *had = cal_store && inv.event.uid ? jd_store_event_by_uid(cal_store, inv.event.uid) : NULL;
                int64_t calendar = had ? had->calendar : first_writable(app, 0);
                if (had) jd_event_rows_free(had, 1);
                jd_calsync *w = calendar ? worker_of(app, calendar) : NULL;
                if (!w) notify_user(app, "No calendar is synced to keep the event in: see Settings, Calendar.");
                else {
                    cv.respond_serial = ++cv.serials;
                    jd_calsync_respond(w, calendar, ics, app->account.address, answers[i], cv.respond_serial);
                    notify_user(app, "Answering...");
                }
            }
            bx += b.width + 8;
        }
        const char *state = now_said[0] ? now_said : said;
        if (state) jd_text_fit(ui, state, bx + 6, y + 86, x + w - bx - 16, 13, JD_NORD4, 0);
    } else if (inv.method == JD_ITIP_CANCEL && inv.event.uid && cal_store) {
        jd_event_row *have = jd_store_event_by_uid(cal_store, inv.event.uid);
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

/* the add-account form, empty; shown when there is no account yet, or after Add account */
static int adding;

static void fill_settings(jd_app *app)
{
    for (int i = 0; i < CF_COUNT - CF_URL; i++) jd_field_set(&cf[i], NULL);
    cf[2].secret = 1;
    cf[0].placeholder = "https://cloud.example.org/remote.php/dav";
    cf[1].placeholder = "Username";
    cf[2].placeholder = "Password (kept in the keystore)";
    cf[3].placeholder = "you@gmail.com";
    if (cf_provider < 1) cf_provider = 1;
    cf_filled = 1;
}

/* a new account into the settings, its worker started with the others */
static void account_added(jd_app *app, int provider, const char *url, const char *username)
{
    jd_prefs_add_calendar(&app->prefs, provider, url, username, app->prefs.ncals ? app->prefs.cals[0].check_minutes : 15);
    save_prefs(app);
    calendar_start(app);
    adding = 0;
    fill_settings(app);
    settings_note[0] = 0;
    cv.stale = 1, rem_stale = 1;
}

static void save_caldav(jd_app *app)
{
    if (!cf[0].len || !cf[1].len || !cf[2].len) {
        snprintf(settings_note, sizeof settings_note, "The server address, the username and the password are needed.");
        return;
    }
    jd_cal_account a = {1, (char *)jd_field_text(&cf[0]), (char *)jd_field_text(&cf[1]), 15};
    char *key = secret_key(&a);
    int saved = plat_secret_save(key, jd_field_text(&cf[2])) == 0;
    free(key);
    if (!saved) {
        snprintf(settings_note, sizeof settings_note, "The keystore did not take the password.");
        return;
    }
    account_added(app, 1, jd_field_text(&cf[0]), jd_field_text(&cf[1]));
}

static void google_sign_in(jd_app *app)
{
    if (!cf[3].len || !strchr(jd_field_text(&cf[3]), '@')) {
        snprintf(settings_note, sizeof settings_note, "Enter the Google address first.");
        return;
    }
    jd_oauth_stop(signin);
    char *url = NULL, err[300];
    signin = jd_oauth_start(jd_field_text(&cf[3]), &url, err, sizeof err);
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
    jd_cal_account a = {2, NULL, (char *)jd_field_text(&cf[3]), 15};
    char *key = secret_key(&a);
    int saved = plat_secret_save(key, refresh) == 0;
    free(key);
    jd_wipe(refresh, strlen(refresh));
    free(refresh);
    if (!saved) {
        snprintf(settings_note, sizeof settings_note, "Signed in, but the keystore did not take the sign-in.");
        return;
    }
    account_added(app, 2, NULL, jd_field_text(&cf[3]));
    SetWindowFocused();
}

int calendar_settings_keys(jd_app *app)
{
    int f = app->settings_focus;
    if (f == CF_HEX) {
        picker_keys(app);
        if (!cp.hex_focus || !cp.cal) app->settings_focus = -1;
        return 1;
    }
    if (f < CF_URL || f >= CF_COUNT) return 0;
    if (IsKeyPressed(KEY_TAB)) {
        int first = cf_provider == 2 ? CF_GOOGLE : CF_URL, last = cf_provider == 2 ? CF_GOOGLE : CF_PASSWORD;
        app->settings_focus = f >= last ? first : f + 1;
    } else if (IsKeyPressed(KEY_ENTER)) {
        if (cf_provider == 2) google_sign_in(app);
        else save_caldav(app);
    } else {
        jd_field_input(&cf[f - CF_URL]);
    }
    return 1;
}

/* calendar row c belongs to account i (one kept before accounts were told apart is the first's) */
static int of_account(jd_app *app, const jd_calendar_row *c, int i)
{
    if (!c->account) return i == 0;
    char *id = jd_cal_account_id(&app->prefs.cals[i]);
    int mine = strcmp(c->account, id) == 0;
    free(id);
    return mine;
}

/* account i's calendars, each with a box: ticked ones are synced */
static float calendar_list(jd_app *app, int a, float x, float y, float w, int *focus)
{
    int n;
    jd_calendar_row *c = cal_store ? jd_store_calendars(cal_store, &n) : NULL;
    for (int i = 0; c && i < n; i++) {
        if (!of_account(app, &c[i], a)) continue;
        Rectangle row = {x, y, w - 44, 32};
        Rectangle paint = {x + w - 38, y, 38, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), row) && !app->dialog.kind && !cp.drag;
        if (hover) DrawRectangleRounded(row, 0.3f, 6, JD_NORD1);
        jd_icon(c[i].enabled ? "square-check" : "square", x + 6, y + 8, 16, c[i].enabled ? JD_NORD8 : JD_NORD4);
        DrawCircleV((Vector2){x + 36, y + 16}, 6, hex_color(c[i].color));
        char label[300];
        snprintf(label, sizeof label, "%s%s", c[i].name ? c[i].name : c[i].href, c[i].readonly ? "  (read-only)" : "");
        jd_text_fit(&app->ui, label, x + 50, mid_y(row, FS_EVENT), row.width - 58, FS_EVENT, JD_NORD6, 0);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            jd_store_calendar_enable(cal_store, c[i].id, !c[i].enabled);
            cv.stale = 1, rem_stale = 1;
            /* a calendar just chosen is downloaded now, not at the next check */
            if (!c[i].enabled) sync_now(app);
            share_changed(app);
        }
        /* its colour: the picker opens under it */
        if (jd_icon_button(&app->ui, "paintbrush", NULL, paint, cp.cal == c[i].id) && !app->dialog.kind) picker_toggle(&c[i]);
        if (CheckCollisionPointRec(GetMousePosition(), paint) && !plat_touch()) jd_tooltip("Colour", paint.x - 10, paint.y + 38);
        y += 34;
        if (cp.cal == c[i].id) y = picker_draw(app, &c[i], x, y, w, focus);
    }
    if (c) jd_calendar_rows_free(c, n);
    return y;
}

/* account a: who, where, how far its sync is, the one way to remove it, and its calendars to tick; the y below */
static float account_card(jd_app *app, int a, float x, float y, float w, int *focus)
{
    jd_ui *ui = &app->ui;
    jd_cal_account *ac = &app->prefs.cals[a];
    int n = 0, chosen = 0, all = 0;
    jd_calendar_row *rows = cal_store ? jd_store_calendars(cal_store, &all) : NULL;
    for (int i = 0; i < all; i++)
        if (of_account(app, &rows[i], a)) n++, chosen += rows[i].enabled;
    if (rows) jd_calendar_rows_free(rows, all);
    /* a phone's width puts the button under the text */
    int stack = w < 480;
    float h = stack ? 140 : 92;
    DrawRectangleRounded((Rectangle){x, y, w, h}, 0.1f, 8, JD_NORD1);
    float cy = stack ? y + 46 : y + h / 2;
    DrawCircle((int)(x + 34), (int)cy, 20, ColorAlpha(JD_NORD8, 0.18f));
    jd_icon("calendar", x + 24, cy - 10, 20, JD_NORD8);
    float tx = x + 68, tw = stack ? w - 84 : w - 68 - 190;
    char title[300];
    if (ac->provider == 2) snprintf(title, sizeof title, "Google Calendar");
    else {
        const char *u = ac->url ? ac->url : "";
        const char *host = strstr(u, "://") ? strstr(u, "://") + 3 : u;
        snprintf(title, sizeof title, "CalDAV · %.*s", (int)strcspn(host, "/"), host);
    }
    jd_text_fit(ui, title, tx, y + 16, tw, 15, JD_NORD6, 1);
    jd_text_fit(ui, ac->username ? ac->username : "", tx, y + 38, tw, 13, JD_NORD4, 0);
    char status[440];
    Color sc = JD_MUTED;
    int running = a < nacc && acc[a].worker && !acc[a].refused;
    if (a < nacc && acc[a].error[0]) snprintf(status, sizeof status, "%s", acc[a].error), sc = JD_NORD11;
    else if (a < nacc && acc[a].status[0]) snprintf(status, sizeof status, "%s...", acc[a].status), sc = JD_NORD8;
    else if (n && chosen) snprintf(status, sizeof status, "%d of %d calendars synced", chosen, n), sc = JD_GREEN;
    else if (n) snprintf(status, sizeof status, "Choose the calendars to sync below"), sc = JD_NORD13;
    else if (running) snprintf(status, sizeof status, "Looking for its calendars...");
    else snprintf(status, sizeof status, "Not connected");
    jd_text_fit(ui, status, tx, y + 60, tw, 13, sc, 0);
    int sign = a < nacc && acc[a].unsigned_;
    /* narrow: the buttons on a row of their own across the card, halves of it when there are two */
    float half = (w - 32 - 8) / 2;
    Rectangle rm = stack ? (sign ? (Rectangle){x + 16 + half + 8, y + 90, half, 34} : (Rectangle){tx, y + 90, 170, 34})
                 : (Rectangle){x + w - 174, sign ? y + h - 34 - 10 : y + (h - 34) / 2, 158, 34};
    /* signed in elsewhere only: Google's sign-in in the browser, or the form with the password still to type */
    Rectangle si = stack ? (Rectangle){x + 16, y + 90, half, 34} : (Rectangle){x + w - 174, y + 10, 158, 34};
    if (sign && jd_button(ui, "Sign in", si, 1) && !app->dialog.kind) {
        fill_settings(app);
        adding = 1;
        cf_provider = ac->provider;
        if (ac->provider == 2) {
            jd_field_set(&cf[3], ac->username);
            google_sign_in(app);
        } else {
            jd_field_set(&cf[0], ac->url);
            jd_field_set(&cf[1], ac->username);
            if (focus) *focus = CF_PASSWORD;
            snprintf(settings_note, sizeof settings_note, "Type the password of %s, then Save and connect.", ac->username ? ac->username : "it");
        }
    }
    /* the only way an account goes: its sign-in from the keystore, its calendars and events from here */
    if (jd_icon_button(ui, "trash-2", stack && sign ? "Remove" : "Remove account", rm, 0) && !app->dialog.kind) {
        forget_account(app, a);
        jd_prefs_remove_calendar(&app->prefs, a);
        save_prefs(app);
        calendar_start(app);
        cv.stale = 1, rem_stale = 1;
        snprintf(settings_note, sizeof settings_note, "The calendar account is removed.");
        return y;
    }
    y += h + 10;
    /* its calendars, under it */
    if (n) {
        static const char hint[] = "Tick the calendars to sync; unticking one removes its events from here.";
        int lines;
        int *starts = jd_wrap(ui, hint, w - 8, 13, &lines);
        for (int i = 0; i < lines; i++)
            jd_text_range(ui, hint, starts[i], i + 1 < lines ? starts[i + 1] : (int)strlen(hint), x + 4, y + 18 * (float)i,
                          13, JD_MUTED, 0);
        free(starts);
        y += 18 * (float)lines + 8;
        y = calendar_list(app, a, x, y, w, focus);
    }
    return y + 22;
}

float calendar_settings(jd_app *app, float x, float y, float w, int *focus)
{
    jd_ui *ui = &app->ui;
    if (!cf_filled) fill_settings(app);
    float fw = w > 560 ? 560 : w;
    for (int a = 0; a < app->prefs.ncals; a++) y = account_card(app, a, x, y, fw, focus);
    if (app->prefs.ncals && !adding) {
        if (jd_icon_button(ui, "calendar", "Add account", (Rectangle){x, y, 170, 36}, 0)) {
            adding = 1;
            fill_settings(app);
            settings_note[0] = 0;
        }
        y += 50;
    } else {
        /* the choice only switches the form: nothing is saved until its button says so */
        static const char *const providers[] = {"CalDAV", "Google"};
        jd_text(ui, app->prefs.ncals ? "Add" : "Connect", x, y + 8, 14, JD_NORD6, 1);
        int p = jd_segmented(ui, x + 110, y, providers, 2, cf_provider == 2) + 1;
        if (p != cf_provider) cf_provider = p, settings_note[0] = 0;
        y += 48;
        if (cf_provider == 1) {
            static const char *const labels[] = {"Address", "Username", "Password"};
            for (int i = 0; i < 3; i++) {
                jd_text(ui, labels[i], x, y + 10, 13, JD_NORD4, 1);
                if (jd_field_draw(ui, &cf[i], (Rectangle){x + 110, y, fw - 110, 34}, *focus == CF_URL + i)) *focus = CF_URL + i;
                y += 42;
            }
            if (jd_button(ui, "Save and connect", (Rectangle){x + 110, y + 4, 170, 36}, 1)) save_caldav(app);
            y += 50;
        } else {
            jd_text(ui, "Google address", x, y + 10, 13, JD_NORD4, 1);
            if (jd_field_draw(ui, &cf[3], (Rectangle){x + 110, y, fw - 110, 34}, *focus == CF_GOOGLE)) *focus = CF_GOOGLE;
            y += 42;
            if (jd_icon_button(ui, "external-link", signin ? "Waiting for the browser..." : "Sign in with Google",
                               (Rectangle){x + 110, y + 4, 240, 36}, 1))
                google_sign_in(app);
            y += 50;
        }
        if (app->prefs.ncals && jd_button(ui, "Cancel", (Rectangle){x + 110, y - 6, 110, 34}, 0)) {
            adding = 0;
            settings_note[0] = 0;
        }
        if (app->prefs.ncals) y += 44;
    }
    if (settings_note[0]) {
        jd_text_fit(ui, settings_note, x, y, fw, 13, JD_NORD8, 0);
        y += 26;
    }
    if (app->prefs.ncals) {
        const char *busy = calendar_status();
        if (jd_icon_button(ui, "refresh-cw", busy ? "Syncing..." : "Sync now", (Rectangle){x, y, 140, 34}, 0))
            sync_now(app);
        int64_t synced = 0;
        for (int i = 0; i < nacc; i++)
            if (acc[i].synced_at && (!synced || acc[i].synced_at < synced)) synced = acc[i].synced_at;
        char line[300];
        if (busy) snprintf(line, sizeof line, "%s...", busy);
        else if (synced) {
            char when[32];
            fmt(synced, local_midnight(synced) == local_midnight(jd_now()) ? "%H:%M" : "%d %b %H:%M", when, sizeof when);
            snprintf(line, sizeof line, "Up to date: last synced at %s.", when);
        } else snprintf(line, sizeof line, "%s", calendar_connected() ? "Not synced yet." : "Not connected.");
        jd_text_fit(ui, line, x + 156, y + 10, w - 160, 13, JD_NORD4, 0);
        y += 48;
    }
    return y;
}
