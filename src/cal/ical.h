#pragma once
#include <stdint.h>

/* iCalendar objects (RFC 5545) as CalDAV stores them: one VCALENDAR per resource, holding one event (a VEVENT
   without RECURRENCE-ID) and its changed occurrences (VEVENTs with it), and the VTIMEZONEs they name.

   Times are Unix seconds. An all-day event's are the UTC midnights of its dates, end exclusive: a one-day event on
   2026-11-19 runs from that day's UTC midnight to the next. Floating times (no zone) are read as local time. */

typedef struct {
    char *uid, *summary, *location, *description;
    int64_t start, end;
    int all_day;
    char *rrule;  /* the RRULE value ("FREQ=WEEKLY;BYDAY=TU"), NULL when it does not repeat */
    int alarm;    /* minutes before the start of its first reminder (VALARM with a relative TRIGGER), -1 none */
} jd_ical_event;

/* the event of an object; -1 when it holds none */
int jd_ical_read(const char *ics, jd_ical_event *out);
void jd_ical_event_free(jd_ical_event *e);

typedef struct {
    int64_t start, end;
    int all_day;
    char *summary, *location;
    int alarm; /* as in jd_ical_event */
} jd_occurrence;

/* the occurrences of an object's event that overlap [from, to), repeats and changed occurrences included, those
   cancelled (EXDATE) left out; appended to *out, which holds *n */
void jd_ical_expand(const char *ics, int64_t from, int64_t to, jd_occurrence **out, int *n);
void jd_occurrences_free(jd_occurrence *o, int n);

/* old (NULL for a new object) with e's fields: UID kept (a new one made when e->uid is NULL), SEQUENCE raised,
   times in the local zone (with its VTIMEZONE) when libical knows it, else UTC. A repeating event whose start
   moves loses its changed and cancelled occurrences, which no longer line up. malloc'd. */
char *jd_ical_write(const char *old, const jd_ical_event *e);

/* iTIP (RFC 5546) as iMIP (RFC 6047) carries it in mail */
typedef enum { JD_ITIP_OTHER, JD_ITIP_REQUEST, JD_ITIP_CANCEL, JD_ITIP_REPLY } jd_itip_method;

typedef struct {
    jd_itip_method method;
    char *organizer, *organizer_name; /* the address without mailto: */
    char *partstat;                   /* me's PARTSTAT ("NEEDS-ACTION", "ACCEPTED", ...), NULL when not invited */
    char *replier, *reply_partstat;   /* a REPLY: who answered, and how */
    jd_ical_event event;
} jd_invite;

/* an invitation, update, cancellation or answer, as me (an address) sees it; -1 when ics holds no event */
int jd_invite_read(const char *ics, const char *me, jd_invite *out);
void jd_invite_free(jd_invite *i);
/* METHOD:REPLY to the organizer: the event's identity and me's ATTENDEE alone, with partstat ("ACCEPTED",
   "TENTATIVE", "DECLINED"); NULL when me is not an attendee */
char *jd_itip_reply(const char *ics, const char *me, const char *partstat);
/* the copy for me's calendar: no METHOD, me's PARTSTAT set. client_replies marks the ORGANIZER
   SCHEDULE-AGENT=CLIENT (RFC 6638 7.1), so a server that schedules does not send its own answer as well. */
char *jd_itip_keep(const char *ics, const char *me, const char *partstat, int client_replies);

/* the local time zone's IANA name ("Europe/Berlin"), "UTC" when unknown */
const char *jd_local_tzid(void);
/* days since 1970-01-01 of a civil date, and back (H. Hinnant's algorithms) */
int64_t jd_days_from_civil(int y, int m, int d);
void jd_civil_from_days(int64_t z, int *y, int *m, int *d);
