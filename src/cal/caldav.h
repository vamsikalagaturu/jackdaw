#pragma once

/* CalDAV (RFC 4791) over WebDAV (RFC 4918), with collection sync (RFC 6578) and the getctag of Apple's
   calendarserver where sync is missing. Blocking; one session per thread. Hrefs of events are kept as the server
   wrote them (a path, mostly) and resolved against the calendar's URL for requests. */
typedef struct jd_dav jd_dav;

enum { JD_DAV_OK = 0, JD_DAV_ERROR = -1, JD_DAV_AUTH = -2, JD_DAV_CONFLICT = -3, JD_DAV_RESYNC = -4 };

/* Basic or Digest with user and password, or an OAuth bearer token (RFC 6750) when bearer is set */
jd_dav *jd_dav_new(const char *user, const char *password);
void jd_dav_set_bearer(jd_dav *d, const char *token);
void jd_dav_free(jd_dav *d);
/* why the last request failed */
const char *jd_dav_error(jd_dav *d);
/* a DAV: response header named calendar-auto-schedule (RFC 6638 2): the server sends iTIP replies itself */
int jd_dav_auto_schedule(jd_dav *d);
/* OPTIONS on url: what the server does there, in its DAV header */
int jd_dav_options(jd_dav *d, const char *url);

typedef struct {
    char *href; /* absolute URL */
    char *name, *color, *ctag, *sync_token;
    int readonly;
} jd_dav_calendar;

/* The event calendars reachable from url: a server (its /.well-known/caldav, RFC 6764), a principal, a calendar
   home, or one calendar */
int jd_dav_discover(jd_dav *d, const char *url, jd_dav_calendar **out, int *n);
void jd_dav_calendars_free(jd_dav_calendar *c, int n);

typedef struct {
    char *href, *etag, *data;
    int gone;
} jd_dav_item;
void jd_dav_items_free(jd_dav_item *it, int n);

/* the collection's changes since token (NULL: all its members): hrefs with etags, and those gone; *next gets the new
   token. JD_DAV_RESYNC when the token is refused or the server has no sync-collection. */
int jd_dav_sync(jd_dav *d, const char *cal, const char *token, jd_dav_item **out, int *n, char **next);
/* every event of the collection, href and etag (calendar-query) */
int jd_dav_etags(jd_dav *d, const char *cal, jd_dav_item **out, int *n);
/* the iCalendar data of hrefs (calendar-multiget) */
int jd_dav_multiget(jd_dav *d, const char *cal, char **hrefs, int nhrefs, jd_dav_item **out, int *n);
/* getctag and sync-token of a collection, each NULL when the server has none */
int jd_dav_state(jd_dav *d, const char *cal, char **ctag, char **token);
/* PUT of an iCalendar object at href (relative to cal): over etag when given (If-Match), else only when nothing is
   there (If-None-Match: *). *new_etag gets the ETag answered, NULL when the server gave none. JD_DAV_CONFLICT when
   the resource changed (412). */
int jd_dav_put(jd_dav *d, const char *cal, const char *href, const char *ics, const char *etag, char **new_etag);
/* DELETE, with If-Match when etag is given; gone already counts as done */
int jd_dav_delete(jd_dav *d, const char *cal, const char *href, const char *etag);

/* href resolved against base (an absolute URL); malloc'd */
char *jd_dav_url(const char *base, const char *href);
/* the path of an absolute URL ("/" when it has none); a path comes back as it is */
const char *jd_dav_path(const char *url);
