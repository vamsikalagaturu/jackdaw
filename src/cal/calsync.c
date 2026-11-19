#define _GNU_SOURCE
#include "calsync.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cal/caldav.h"
#include "cal/ical.h"
#include "cal/oauth.h"
#include "core/util.h"
#include "store/store.h"

enum { BACKOFF_MIN = 30, BACKOFF_MAX = 30 * 60 };

typedef enum { JOB_PUT, JOB_DELETE, JOB_RESPOND } job_kind;

typedef struct {
    job_kind kind;
    int64_t calendar, event, serial;
    char *ics, *me, *partstat;
} job;

struct jd_calsync {
    jd_cal_provider provider;
    char *account, *url, *username, *secret, *db_path; /* account: jd_cal_account_id, what its calendars are filed under */
    int check_minutes;
    pthread_t thread;
    int wake[2];

    pthread_mutex_t lock;
    int quit, sync_now;
    job *jobs;
    int njobs;
    jd_cal_event *events;
    int nevents;

    jd_store *store;
    jd_dav *dav;
    int64_t access_until; /* Google: when the access token runs out */
    int stopped;          /* the sign-in was refused: no more requests until restarted */
};

static void emit(jd_calsync *s, jd_cal_event_kind kind, const char *text, const char *text2, int64_t id)
{
    pthread_mutex_lock(&s->lock);
    s->events = jd_realloc(s->events, sizeof *s->events * (size_t)(s->nevents + 1));
    s->events[s->nevents++] = (jd_cal_event){kind, jd_strdup(text), jd_strdup(text2), id};
    pthread_mutex_unlock(&s->lock);
}

static void poke(jd_calsync *s)
{
    char c = 1;
    if (write(s->wake[1], &c, 1) < 0 && errno != EAGAIN) perror("jackdaw: calendar wake");
}

static int quitting(jd_calsync *s)
{
    pthread_mutex_lock(&s->lock);
    int q = s->quit;
    pthread_mutex_unlock(&s->lock);
    return q;
}

static void job_free(job *j)
{
    free(j->ics), free(j->me), free(j->partstat);
}

/* a session that may make requests: Google's access token renewed a minute before it runs out */
static int session(jd_calsync *s)
{
    if (s->stopped) return JD_DAV_AUTH;
    if (!s->dav) s->dav = jd_dav_new(s->provider == JD_CAL_GOOGLE ? NULL : s->username, s->provider == JD_CAL_GOOGLE ? NULL : s->secret);
    if (s->provider != JD_CAL_GOOGLE || jd_now() < s->access_until - 60) return JD_DAV_OK;
    char *access = NULL, err[512];
    int64_t until = 0;
    int r = jd_oauth_refresh(s->secret, &access, &until, err, sizeof err);
    if (r == JD_OAUTH_REVOKED) {
        s->stopped = 1;
        emit(s, JD_CAL_EV_AUTH, err, NULL, 0);
        return JD_DAV_AUTH;
    }
    if (r != JD_OAUTH_DONE) {
        emit(s, JD_CAL_EV_ERROR, err, NULL, 0);
        return JD_DAV_ERROR;
    }
    jd_dav_set_bearer(s->dav, access);
    jd_wipe(access, strlen(access));
    free(access);
    s->access_until = until;
    return JD_DAV_OK;
}

/* a refused request: the sign-in is wrong (CalDAV), or Google's token was revoked since it was renewed */
static void refused(jd_calsync *s)
{
    if (s->provider == JD_CAL_GOOGLE && s->access_until) {
        /* one more try with a fresh token before giving up */
        s->access_until = 0;
        if (session(s) == JD_DAV_OK) return;
    }
    s->stopped = 1;
    emit(s, JD_CAL_EV_AUTH, jd_dav_error(s->dav), NULL, 0);
}

static void store_item(jd_calsync *s, int64_t calendar, const jd_dav_item *it)
{
    jd_ical_event e;
    if (!it->data || jd_ical_read(it->data, &e) != 0) return;
    jd_store_event_put(s->store, calendar, it->href, it->etag, it->data, e.uid, e.summary, e.location, e.start, e.end,
                       e.all_day, e.rrule != NULL);
    jd_ical_event_free(&e);
}

static int fetch_changed(jd_calsync *s, const char *href, int64_t id, char **hrefs, int n, int *changed)
{
    if (!n) return JD_DAV_OK;
    jd_dav_item *got = NULL;
    int ngot = 0;
    int r = jd_dav_multiget(s->dav, href, hrefs, n, &got, &ngot);
    if (r != JD_DAV_OK) return r;
    jd_store_begin(s->store);
    for (int i = 0; i < ngot; i++) {
        if (got[i].gone) jd_store_event_delete(s->store, id, got[i].href);
        else store_item(s, id, &got[i]);
    }
    jd_store_commit(s->store);
    jd_dav_items_free(got, ngot);
    *changed = 1;
    return JD_DAV_OK;
}

/* the server's members (href, etag) against the stored events: what is new or changed is fetched; with complete,
   what the server lacks goes */
static int reconcile(jd_calsync *s, const char *href, int64_t id, jd_dav_item *items, int n, int complete, int *changed)
{
    int nstored;
    jd_event_row *stored = jd_store_calendar_events(s->store, id, &nstored);
    char **want = NULL;
    int nwant = 0;
    jd_store_begin(s->store);
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < nstored && strcmp(stored[k].href, items[i].href) != 0) k++;
        if (items[i].gone) {
            if (k < nstored) jd_store_event_delete(s->store, id, items[i].href), *changed = 1;
            continue;
        }
        if (k < nstored && items[i].etag && jd_streq(stored[k].etag, items[i].etag)) continue;
        want = jd_realloc(want, sizeof *want * (size_t)(nwant + 1));
        want[nwant++] = items[i].href;
    }
    if (complete)
        for (int k = 0; k < nstored; k++) {
            int seen = 0;
            for (int i = 0; i < n && !seen; i++) seen = !items[i].gone && strcmp(stored[k].href, items[i].href) == 0;
            if (!seen) jd_store_event_delete(s->store, id, stored[k].href), *changed = 1;
        }
    jd_store_commit(s->store);
    int r = fetch_changed(s, href, id, want, nwant, changed);
    free(want);
    jd_event_rows_free(stored, nstored);
    return r;
}

static int sync_calendar(jd_calsync *s, const jd_dav_calendar *c, int64_t id, int *changed)
{
    int n;
    jd_calendar_row *rows = jd_store_calendars(s->store, &n);
    char *token = NULL, *ctag = NULL;
    for (int i = 0; i < n; i++)
        if (rows[i].id == id) token = jd_strdup(rows[i].sync_token), ctag = jd_strdup(rows[i].ctag);
    jd_calendar_rows_free(rows, n);
    int r = JD_DAV_OK;
    /* the getctag shortcut only where there is no collection sync, which asks for nothing more when nothing changed */
    int same = c->sync_token ? jd_streq(c->sync_token, token) : c->ctag && ctag && strcmp(c->ctag, ctag) == 0;
    if (same) goto done;
    jd_dav_item *items = NULL;
    int nitems = 0;
    char *next = NULL;
    r = c->sync_token ? jd_dav_sync(s->dav, c->href, token, &items, &nitems, &next) : JD_DAV_RESYNC;
    if (r == JD_DAV_RESYNC && token) {
        /* the token expired on the server (RFC 6578 3.2): everything again */
        free(token);
        token = NULL;
        r = c->sync_token ? jd_dav_sync(s->dav, c->href, NULL, &items, &nitems, &next) : JD_DAV_RESYNC;
    }
    if (r == JD_DAV_OK) {
        r = reconcile(s, c->href, id, items, nitems, token == NULL, changed);
    } else if (r == JD_DAV_RESYNC) {
        free(next);
        next = NULL;
        r = jd_dav_etags(s->dav, c->href, &items, &nitems);
        if (r == JD_DAV_OK) r = reconcile(s, c->href, id, items, nitems, 1, changed);
    }
    jd_dav_items_free(items, nitems);
    if (r == JD_DAV_OK) jd_store_calendar_state(s->store, id, next ? next : c->sync_token, c->ctag);
    free(next);
done:
    free(token), free(ctag);
    return r;
}

static int sync_all(jd_calsync *s)
{
    int r = session(s);
    if (r != JD_DAV_OK) return r;
    emit(s, JD_CAL_EV_STATUS, "Looking at the calendars", NULL, 0);
    const char *start = s->url;
    char *google = NULL;
    if (s->provider == JD_CAL_GOOGLE) start = google = jd_printf("%s%s/user", JD_GOOGLE_CALDAV, s->username);
    jd_dav_calendar *cals = NULL;
    int n = 0;
    r = jd_dav_discover(s->dav, start, &cals, &n);
    free(google);
    /* Google's principal (developers.google.com/workspace/calendar/caldav/v2/guide) lists the calendars; failing
       that, the primary one alone, whose ID is the address */
    if (s->provider == JD_CAL_GOOGLE && r != JD_DAV_AUTH && (r != JD_DAV_OK || !n)) {
        jd_dav_calendars_free(cals, n);
        cals = NULL, n = 0;
        char *primary = jd_printf("%s%s/events/", JD_GOOGLE_CALDAV, s->username);
        r = jd_dav_discover(s->dav, primary, &cals, &n);
        free(primary);
    }
    if (r != JD_DAV_OK) {
        jd_dav_calendars_free(cals, n);
        return r;
    }
    char **hrefs = jd_alloc(sizeof *hrefs * (size_t)(n + 1));
    int64_t *ids = jd_alloc(sizeof *ids * (size_t)(n + 1));
    jd_store_begin(s->store);
    for (int i = 0; i < n; i++) {
        hrefs[i] = cals[i].href;
        ids[i] = jd_store_calendar_put(s->store, s->account, cals[i].href, cals[i].name, cals[i].color, cals[i].readonly);
    }
    jd_store_calendars_keep(s->store, s->account, hrefs, n);
    jd_store_commit(s->store);
    int nrows;
    jd_calendar_row *rows = jd_store_calendars(s->store, &nrows);
    /* names and colours may have changed even when no event did */
    int changed = 1;
    for (int i = 0; i < n && r == JD_DAV_OK && !quitting(s); i++) {
        int chosen = 0;
        for (int k = 0; k < nrows; k++)
            if (rows[k].id == ids[i]) chosen = rows[k].enabled;
        if (!chosen) continue;
        char *what = jd_printf("Syncing the calendar %s", cals[i].name ? cals[i].name : "");
        emit(s, JD_CAL_EV_STATUS, what, NULL, 0);
        free(what);
        r = sync_calendar(s, &cals[i], ids[i], &changed);
    }
    jd_calendar_rows_free(rows, nrows);
    free(hrefs), free(ids);
    jd_dav_calendars_free(cals, n);
    if (changed) emit(s, JD_CAL_EV_CHANGED, NULL, NULL, 0);
    return r;
}

/* ---- writing ---- */

static char *calendar_href(jd_calsync *s, int64_t id, int *readonly)
{
    int n;
    jd_calendar_row *rows = jd_store_calendars(s->store, &n);
    char *href = NULL;
    for (int i = 0; i < n; i++)
        if (rows[i].id == id) href = jd_strdup(rows[i].href), *readonly = rows[i].readonly;
    jd_calendar_rows_free(rows, n);
    return href;
}

/* what the server holds at href now, into the store; its etag when it is there */
static char *refresh_one(jd_calsync *s, const char *cal, int64_t calendar, const char *href)
{
    char *list[] = {(char *)href};
    jd_dav_item *got = NULL;
    int n = 0;
    char *etag = NULL;
    if (jd_dav_multiget(s->dav, cal, list, 1, &got, &n) == JD_DAV_OK)
        for (int i = 0; i < n; i++) {
            if (got[i].gone) jd_store_event_delete(s->store, calendar, got[i].href);
            else store_item(s, calendar, &got[i]), etag = jd_strdup(got[i].etag);
        }
    jd_dav_items_free(got, n);
    return etag;
}

/* ics at href in calendar (over etag, or new); the stored copy follows */
static int write_event(jd_calsync *s, int64_t calendar, const char *href, const char *etag, const char *ics, char *err,
                       int errlen)
{
    int readonly = 0;
    char *cal = calendar_href(s, calendar, &readonly);
    if (!cal || readonly) {
        snprintf(err, (size_t)errlen, cal ? "That calendar is read-only." : "That calendar is gone.");
        free(cal);
        return JD_DAV_ERROR;
    }
    char *new_etag = NULL;
    int r = jd_dav_put(s->dav, cal, href, ics, etag, &new_etag);
    if (r == JD_DAV_OK) {
        jd_ical_event e;
        if (new_etag && jd_ical_read(ics, &e) == 0) {
            /* RFC 4791 5.3.4: a strong ETag means the server kept the data as sent */
            jd_store_event_put(s->store, calendar, href, new_etag, ics, e.uid, e.summary, e.location, e.start, e.end,
                               e.all_day, e.rrule != NULL);
            jd_ical_event_free(&e);
        } else {
            free(refresh_one(s, cal, calendar, href));
        }
    } else if (r == JD_DAV_CONFLICT) {
        free(refresh_one(s, cal, calendar, href));
        snprintf(err, (size_t)errlen, "The event was changed elsewhere meanwhile; this is its newer version. Edit again to change it.");
    } else {
        snprintf(err, (size_t)errlen, "%s", jd_dav_error(s->dav));
    }
    free(new_etag), free(cal);
    return r;
}

static char *new_href(const char *ics)
{
    jd_ical_event e;
    char *href = NULL;
    if (jd_ical_read(ics, &e) == 0 && e.uid) {
        jd_buf b = {0};
        for (const char *c = e.uid; *c; c++)
            if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.@", *c)) jd_buf_add(&b, c, 1);
            else jd_buf_addf(&b, "%%%02X", (unsigned char)*c);
        jd_buf_adds(&b, ".ics");
        href = b.s;
    }
    jd_ical_event_free(&e);
    return href;
}

/* an event's href for calendar: the collection's path and the name */
static char *member_href(jd_calsync *s, int64_t calendar, const char *name)
{
    int readonly;
    char *cal = calendar_href(s, calendar, &readonly);
    if (!cal) return NULL;
    char *url = jd_dav_url(cal, name);
    char *href = jd_strdup(jd_dav_path(url));
    free(url), free(cal);
    return href;
}

static int run_put(jd_calsync *s, job *j, char *err, int errlen)
{
    jd_event_row *old = j->event ? jd_store_event(s->store, j->event) : NULL;
    if (j->event && !old) {
        snprintf(err, (size_t)errlen, "The event is gone.");
        return JD_DAV_ERROR;
    }
    int r;
    if (old && old->calendar == j->calendar) {
        r = write_event(s, j->calendar, old->href, old->etag, j->ics, err, errlen);
    } else {
        char *name = new_href(j->ics);
        char *href = name ? member_href(s, j->calendar, name) : NULL;
        if (!href) {
            snprintf(err, (size_t)errlen, "The event has no UID.");
            r = JD_DAV_ERROR;
        } else {
            r = write_event(s, j->calendar, href, NULL, j->ics, err, errlen);
        }
        /* moved: gone from the calendar it was in */
        if (r == JD_DAV_OK && old) {
            int ro;
            char *cal = calendar_href(s, old->calendar, &ro);
            if (cal && jd_dav_delete(s->dav, cal, old->href, old->etag) == JD_DAV_OK)
                jd_store_event_delete(s->store, old->calendar, old->href);
            free(cal);
        }
        free(name), free(href);
    }
    jd_event_rows_free(old, old ? 1 : 0);
    return r;
}

static int run_delete(jd_calsync *s, job *j, char *err, int errlen)
{
    jd_event_row *old = jd_store_event(s->store, j->event);
    if (!old) return JD_DAV_OK;
    int ro = 0;
    char *cal = calendar_href(s, old->calendar, &ro);
    int r = cal ? jd_dav_delete(s->dav, cal, old->href, old->etag) : JD_DAV_OK;
    if (r == JD_DAV_OK) jd_store_event_delete(s->store, old->calendar, old->href);
    else if (r == JD_DAV_CONFLICT) {
        free(refresh_one(s, cal, old->calendar, old->href));
        snprintf(err, (size_t)errlen, "The event was changed elsewhere meanwhile, so it was kept. Delete it again to remove it.");
    } else snprintf(err, (size_t)errlen, "%s", jd_dav_error(s->dav));
    free(cal);
    jd_event_rows_free(old, 1);
    return r;
}

static int run_respond(jd_calsync *s, job *j, char *err, int errlen)
{
    jd_invite inv;
    if (jd_invite_read(j->ics, j->me, &inv) != 0 || !inv.event.uid) {
        snprintf(err, (size_t)errlen, "The invitation holds no event.");
        return JD_DAV_ERROR;
    }
    jd_event_row *have = jd_store_event_by_uid(s->store, inv.event.uid);
    int ro = 0;
    char *target = calendar_href(s, have ? have->calendar : j->calendar, &ro);
    if (target) jd_dav_options(s->dav, target);
    free(target);
    int auto_schedule = jd_dav_auto_schedule(s->dav);
    int declined = strcasecmp(j->partstat, "DECLINED") == 0;
    int r = JD_DAV_OK;
    if (declined) {
        if (have) {
            job del = {.event = have->id};
            r = run_delete(s, &del, err, errlen);
        }
    } else {
        char *kept = jd_itip_keep(j->ics, j->me, j->partstat, !auto_schedule);
        int64_t calendar = have ? have->calendar : j->calendar;
        if (!calendar) {
            snprintf(err, (size_t)errlen, "No calendar is synced to keep the event in: choose one in Settings, Calendar.");
            r = JD_DAV_ERROR;
        } else if (have) {
            r = write_event(s, calendar, have->href, have->etag, kept, err, errlen);
        } else {
            job put = {.calendar = calendar, .ics = kept};
            r = run_put(s, &put, err, errlen);
        }
        free(kept);
    }
    if (r == JD_DAV_OK && !auto_schedule && inv.organizer) {
        char *reply = jd_itip_reply(j->ics, j->me, j->partstat);
        if (reply) emit(s, JD_CAL_EV_REPLY, reply, inv.organizer, j->serial);
        free(reply);
    }
    jd_event_rows_free(have, have ? 1 : 0);
    jd_invite_free(&inv);
    return r;
}

static void run_jobs(jd_calsync *s)
{
    for (;;) {
        pthread_mutex_lock(&s->lock);
        if (!s->njobs) {
            pthread_mutex_unlock(&s->lock);
            return;
        }
        job j = s->jobs[0];
        memmove(s->jobs, s->jobs + 1, sizeof *s->jobs * (size_t)--s->njobs);
        pthread_mutex_unlock(&s->lock);
        char err[600] = "";
        int r = session(s);
        if (r == JD_DAV_OK) {
            emit(s, JD_CAL_EV_STATUS, "Saving to the calendar", NULL, 0);
            r = j.kind == JOB_PUT ? run_put(s, &j, err, sizeof err)
                : j.kind == JOB_DELETE ? run_delete(s, &j, err, sizeof err)
                                       : run_respond(s, &j, err, sizeof err);
            if (r == JD_DAV_AUTH) refused(s);
        } else {
            snprintf(err, sizeof err, "%s", s->stopped ? "The calendar server refused the sign-in." : "No connection to the calendar server.");
        }
        emit(s, JD_CAL_EV_SAVED, r == JD_DAV_OK ? NULL : err[0] ? err : "Not saved.", NULL, j.serial);
        emit(s, JD_CAL_EV_CHANGED, NULL, NULL, 0);
        /* idle again: the status a save set must not outlive it */
        emit(s, JD_CAL_EV_STATUS, NULL, NULL, 0);
        job_free(&j);
    }
}

static void wait_for(jd_calsync *s, int seconds)
{
    struct pollfd p = {s->wake[0], POLLIN, 0};
    int r;
    do r = poll(&p, 1, seconds * 1000);
    while (r < 0 && errno == EINTR);
    char buf[64];
    while (read(s->wake[0], buf, sizeof buf) > 0) {}
}

static void *worker(void *arg)
{
    jd_calsync *s = arg;
    char err[512];
    s->store = jd_store_open(s->db_path, err, sizeof err);
    if (!s->store) {
        emit(s, JD_CAL_EV_ERROR, err, NULL, 0);
        return NULL;
    }
    int64_t next = 0;
    int backoff = 0;
    while (!quitting(s)) {
        run_jobs(s);
        pthread_mutex_lock(&s->lock);
        if (s->sync_now) next = 0, s->sync_now = 0;
        pthread_mutex_unlock(&s->lock);
        if (!s->stopped && jd_now() >= next) {
            int r = sync_all(s);
            emit(s, JD_CAL_EV_STATUS, NULL, NULL, 0);
            if (r == JD_DAV_OK) {
                backoff = 0;
                next = jd_now() + (int64_t)s->check_minutes * 60;
            } else if (r == JD_DAV_AUTH) {
                if (!s->stopped) refused(s);
                next = jd_now() + BACKOFF_MIN;
            } else {
                emit(s, JD_CAL_EV_ERROR, jd_dav_error(s->dav), NULL, 0);
                backoff = backoff ? (backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : backoff * 2) : BACKOFF_MIN;
                next = jd_now() + backoff;
            }
        }
        pthread_mutex_lock(&s->lock);
        int pending = s->njobs > 0 || s->sync_now;
        pthread_mutex_unlock(&s->lock);
        if (pending) continue;
        int64_t until = next - jd_now();
        wait_for(s, s->stopped ? 3600 : until < 1 ? 1 : until > 3600 ? 3600 : (int)until);
    }
    jd_dav_free(s->dav);
    s->dav = NULL;
    jd_store_close(s->store);
    s->store = NULL;
    return NULL;
}

jd_calsync *jd_calsync_start(const char *account, jd_cal_provider provider, const char *url, const char *username,
                             const char *secret, int check_minutes, const char *db_path)
{
    jd_calsync *s = jd_alloc(sizeof *s);
    s->account = jd_strdup(account);
    s->provider = provider;
    s->url = jd_strdup(url);
    s->username = jd_strdup(username);
    s->secret = jd_strdup(secret);
    s->db_path = jd_strdup(db_path);
    s->check_minutes = check_minutes > 0 ? check_minutes : 15;
    pthread_mutex_init(&s->lock, NULL);
    if (pipe2(s->wake, O_NONBLOCK | O_CLOEXEC) != 0 || pthread_create(&s->thread, NULL, worker, s) != 0) abort();
    return s;
}

void jd_calsync_stop(jd_calsync *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->quit = 1;
    pthread_mutex_unlock(&s->lock);
    poke(s);
    pthread_join(s->thread, NULL);
    close(s->wake[0]), close(s->wake[1]);
    for (int i = 0; i < s->nevents; i++) jd_cal_event_free(&s->events[i]);
    for (int i = 0; i < s->njobs; i++) job_free(&s->jobs[i]);
    free(s->events), free(s->jobs);
    if (s->secret) jd_wipe(s->secret, strlen(s->secret));
    free(s->account), free(s->url), free(s->username), free(s->secret), free(s->db_path);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void jd_calsync_now(jd_calsync *s)
{
    pthread_mutex_lock(&s->lock);
    s->sync_now = 1;
    pthread_mutex_unlock(&s->lock);
    poke(s);
}

int jd_calsync_poll(jd_calsync *s, jd_cal_event *ev)
{
    pthread_mutex_lock(&s->lock);
    int have = s->nevents > 0;
    if (have) {
        *ev = s->events[0];
        memmove(s->events, s->events + 1, sizeof *s->events * (size_t)--s->nevents);
    }
    pthread_mutex_unlock(&s->lock);
    return have;
}

void jd_cal_event_free(jd_cal_event *ev)
{
    free(ev->text), free(ev->text2);
    *ev = (jd_cal_event){0};
}

static void add_job(jd_calsync *s, job j)
{
    pthread_mutex_lock(&s->lock);
    s->jobs = jd_realloc(s->jobs, sizeof *s->jobs * (size_t)(s->njobs + 1));
    s->jobs[s->njobs++] = j;
    pthread_mutex_unlock(&s->lock);
    poke(s);
}

void jd_calsync_put(jd_calsync *s, int64_t calendar, int64_t event, const char *ics, int64_t serial)
{
    add_job(s, (job){.kind = JOB_PUT, .calendar = calendar, .event = event, .ics = jd_strdup(ics), .serial = serial});
}

void jd_calsync_delete(jd_calsync *s, int64_t event, int64_t serial)
{
    add_job(s, (job){.kind = JOB_DELETE, .event = event, .serial = serial});
}

void jd_calsync_respond(jd_calsync *s, int64_t calendar, const char *ics, const char *me, const char *partstat,
                        int64_t serial)
{
    add_job(s, (job){.kind = JOB_RESPOND, .calendar = calendar, .ics = jd_strdup(ics), .me = jd_strdup(me),
                     .partstat = jd_strdup(partstat), .serial = serial});
}
