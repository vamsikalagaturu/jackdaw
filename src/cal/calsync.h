#pragma once
#include <stdint.h>

/* The calendar worker: a thread of its own beside the mail one, with its own store connection. It finds the
   account's calendars, keeps their events in the store (collection sync where the server has it, else ETags),
   and writes changes back with If-Match / If-None-Match, so nothing another client changed meanwhile is overwritten.
   The UI reads the store and this worker's events once per frame. */
typedef struct jd_calsync jd_calsync;

typedef enum { JD_CAL_NONE, JD_CAL_CALDAV, JD_CAL_GOOGLE } jd_cal_provider;

typedef enum {
    JD_CAL_EV_STATUS,  /* text: what the worker does, NULL when idle */
    JD_CAL_EV_ERROR,   /* text: why the last attempt failed */
    JD_CAL_EV_AUTH,    /* text: the server refused the sign-in; the worker stops */
    JD_CAL_EV_CHANGED, /* the store's calendars or events changed */
    JD_CAL_EV_SAVED,   /* id: a put, delete or answer's serial; text: why it failed, NULL when done */
    JD_CAL_EV_REPLY,   /* an invitation answered on a server that does not send answers itself: text the iTIP REPLY
                          to mail to text2, the organizer (RFC 6047) */
} jd_cal_event_kind;

typedef struct {
    jd_cal_event_kind kind;
    char *text, *text2;
    int64_t id;
} jd_cal_event;

/* One worker per calendar account. account: what its calendars are filed under in the store (jd_cal_account_id).
   secret: the CalDAV password, or Google's refresh token. url: where discovery starts (Google's is fixed). */
jd_calsync *jd_calsync_start(const char *account, jd_cal_provider provider, const char *url, const char *username,
                             const char *secret, int check_minutes, const char *db_path);
void jd_calsync_stop(jd_calsync *s);
void jd_calsync_now(jd_calsync *s);
int jd_calsync_poll(jd_calsync *s, jd_cal_event *ev);
void jd_cal_event_free(jd_cal_event *ev);

/* ics into calendar: over event (its id in the store), or new when event is 0; an event moved to another calendar is
   written there and removed from its old one. Answered with JD_CAL_EV_SAVED. */
void jd_calsync_put(jd_calsync *s, int64_t calendar, int64_t event, const char *ics, int64_t serial);
void jd_calsync_delete(jd_calsync *s, int64_t event, int64_t serial);
/* an invitation (the iTIP REQUEST's ics) answered as me with partstat: kept in the calendar that holds its UID, else
   in calendar (a decline removes it); JD_CAL_EV_REPLY when the server leaves the answer to jackdaw */
void jd_calsync_respond(jd_calsync *s, int64_t calendar, const char *ics, const char *me, const char *partstat,
                        int64_t serial);
