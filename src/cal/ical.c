#define _GNU_SOURCE
#include "ical.h"

#include <libical/ical.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "core/util.h"

enum { MAX_EVENTS = 256, MAX_STEPS = 20000 };

int64_t jd_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void jd_civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)((int64_t)yoe + era * 400 + (*m <= 2));
}

const char *jd_local_tzid(void)
{
    static char name[128];
    if (name[0]) return name;
    const char *tz = getenv("TZ");
    if (tz && *tz == ':') tz++;
    if (tz && *tz && *tz != '/' && strchr(tz, '/')) snprintf(name, sizeof name, "%s", tz);
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (!name[0] && __system_property_get("persist.sys.timezone", v) > 0) snprintf(name, sizeof name, "%s", v);
#else
    char link[256];
    ssize_t n = name[0] ? -1 : readlink("/etc/localtime", link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        const char *z = strstr(link, "zoneinfo/");
        if (z) snprintf(name, sizeof name, "%s", z + 9);
    }
    if (!name[0]) {
        char *f = jd_read_file("/etc/timezone", NULL);
        if (f) {
            f[strcspn(f, " \t\r\n")] = 0;
            snprintf(name, sizeof name, "%s", f);
            free(f);
        }
    }
#endif
    if (!name[0]) snprintf(name, sizeof name, "UTC");
    return name;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;

/* zones from the system's zoneinfo keep their IANA names as TZID, which servers and other clients know */
static void init_once(void)
{
    icaltimezone_set_tzid_prefix("");
}

static icalcomponent *parse(const char *ics)
{
    pthread_once(&once, init_once);
    if (!ics || !ics[0]) return NULL;
    icalcomponent *c = icalparser_parse_string(ics);
    if (!c) return NULL;
    if (icalcomponent_isa(c) == ICAL_VCALENDAR_COMPONENT) return c;
    if (icalcomponent_isa(c) == ICAL_VEVENT_COMPONENT) {
        icalcomponent *cal = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
        icalcomponent_add_component(cal, c);
        return cal;
    }
    icalcomponent_free(c);
    return NULL;
}

static char *dup_text(const char *s)
{
    return s && s[0] ? jd_strdup(s) : NULL;
}

static char *take(char *libical_buffer)
{
    char *s = jd_strdup(libical_buffer);
    icalmemory_free_buffer(libical_buffer);
    return s;
}

static int64_t to_unix(struct icaltimetype t)
{
    if (icaltime_is_null_time(t)) return 0;
    if (t.is_date) return jd_days_from_civil(t.year, t.month, t.day) * 86400;
    if (t.zone) return (int64_t)icaltime_as_timet_with_zone(t, t.zone);
    struct tm tm = {.tm_year = t.year - 1900, .tm_mon = t.month - 1, .tm_mday = t.day, .tm_hour = t.hour,
                    .tm_min = t.minute, .tm_sec = t.second, .tm_isdst = -1};
    return (int64_t)mktime(&tm);
}

static int is_override(icalcomponent *ev)
{
    return icalcomponent_get_first_property(ev, ICAL_RECURRENCEID_PROPERTY) != NULL;
}

static int collect(icalcomponent *cal, icalcomponent **out)
{
    int n = 0;
    for (icalcomponent *e = icalcomponent_get_first_component(cal, ICAL_VEVENT_COMPONENT); e && n < MAX_EVENTS;
         e = icalcomponent_get_next_component(cal, ICAL_VEVENT_COMPONENT))
        out[n++] = e;
    return n;
}

static icalcomponent *master_of(icalcomponent *cal)
{
    icalcomponent *ev[MAX_EVENTS];
    int n = collect(cal, ev);
    for (int i = 0; i < n; i++)
        if (!is_override(ev[i])) return ev[i];
    return n ? ev[0] : NULL;
}

static int alarm_of(icalcomponent *ev)
{
    for (icalcomponent *a = icalcomponent_get_first_component(ev, ICAL_VALARM_COMPONENT); a;
         a = icalcomponent_get_next_component(ev, ICAL_VALARM_COMPONENT)) {
        icalproperty *tp = icalcomponent_get_first_property(a, ICAL_TRIGGER_PROPERTY);
        if (!tp) continue;
        struct icaltriggertype tr = icalproperty_get_trigger(tp);
        if (!icaltime_is_null_time(tr.time)) continue;
        icalparameter *rel = icalproperty_get_first_parameter(tp, ICAL_RELATED_PARAMETER);
        if (rel && icalparameter_get_related(rel) == ICAL_RELATED_END) continue;
        int s = icaldurationtype_as_seconds(tr.duration);
        return s < 0 ? -s / 60 : 0;
    }
    return -1;
}

static void read_event(icalcomponent *ev, jd_ical_event *e)
{
    *e = (jd_ical_event){.alarm = -1};
    e->uid = dup_text(icalcomponent_get_uid(ev));
    e->summary = dup_text(icalcomponent_get_summary(ev));
    e->location = dup_text(icalcomponent_get_location(ev));
    e->description = dup_text(icalcomponent_get_description(ev));
    struct icaltimetype s = icalcomponent_get_dtstart(ev), en = icalcomponent_get_dtend(ev);
    e->all_day = s.is_date;
    e->start = to_unix(s);
    e->end = icaltime_is_null_time(en) ? e->start : to_unix(en);
    if (e->end < e->start) e->end = e->start;
    if (e->all_day && e->end == e->start) e->end += 86400;
    icalproperty *rr = icalcomponent_get_first_property(ev, ICAL_RRULE_PROPERTY);
    if (rr && icalproperty_get_rrule(rr)) e->rrule = take(icalrecurrencetype_as_string_r(icalproperty_get_rrule(rr)));
    e->alarm = alarm_of(ev);
}

int jd_ical_read(const char *ics, jd_ical_event *out)
{
    *out = (jd_ical_event){.alarm = -1};
    icalcomponent *cal = parse(ics);
    icalcomponent *ev = cal ? master_of(cal) : NULL;
    if (ev) read_event(ev, out);
    if (cal) icalcomponent_free(cal);
    return ev ? 0 : -1;
}

void jd_ical_event_free(jd_ical_event *e)
{
    free(e->uid), free(e->summary), free(e->location), free(e->description), free(e->rrule);
    *e = (jd_ical_event){.alarm = -1};
}

typedef struct {
    jd_occurrence **out;
    int *n;
    int64_t from, to;
} span;

static void add(span *sp, int64_t start, const jd_ical_event *e)
{
    int64_t end = start + (e->end - e->start);
    int inside = end > start ? end > sp->from && start < sp->to : start >= sp->from && start < sp->to;
    if (!inside) return;
    *sp->out = jd_realloc(*sp->out, sizeof **sp->out * (size_t)(*sp->n + 1));
    (*sp->out)[(*sp->n)++] = (jd_occurrence){start, end, e->all_day, dup_text(e->summary), dup_text(e->location), e->alarm};
}

/* a component's properties of a kind, gathered first: anything that reads the component (a getter) moves the one
   property iterator libical keeps per component */
static int props(icalcomponent *c, icalproperty_kind kind, icalproperty **out)
{
    int n = 0;
    for (icalproperty *p = icalcomponent_get_first_property(c, kind); p && n < MAX_EVENTS;
         p = icalcomponent_get_next_property(c, kind))
        out[n++] = p;
    return n;
}

static int listed(const int64_t *v, int n, int64_t x)
{
    for (int i = 0; i < n; i++)
        if (v[i] == x) return 1;
    return 0;
}

void jd_ical_expand(const char *ics, int64_t from, int64_t to, jd_occurrence **out, int *n)
{
    icalcomponent *cal = parse(ics);
    if (!cal) return;
    span sp = {out, n, from, to};
    icalcomponent *ev[MAX_EVENTS];
    int nev = collect(cal, ev);
    icalcomponent *master = NULL;
    int64_t *skip = jd_alloc(sizeof *skip * (size_t)(nev + 1));
    int nskip = 0;
    for (int i = 0; i < nev; i++) {
        if (!is_override(ev[i])) {
            if (!master) master = ev[i];
            continue;
        }
        icalproperty *rid = icalcomponent_get_first_property(ev[i], ICAL_RECURRENCEID_PROPERTY);
        skip[nskip++] = to_unix(icalproperty_get_datetime_with_component(rid, ev[i]));
        if (icalcomponent_get_status(ev[i]) == ICAL_STATUS_CANCELLED) continue;
        jd_ical_event e;
        read_event(ev[i], &e);
        add(&sp, e.start, &e);
        jd_ical_event_free(&e);
    }
    if (master && icalcomponent_get_status(master) != ICAL_STATUS_CANCELLED) {
        jd_ical_event base;
        read_event(master, &base);
        icalproperty *p[MAX_EVENTS];
        int np = props(master, ICAL_EXDATE_PROPERTY, p);
        skip = jd_realloc(skip, sizeof *skip * (size_t)(nskip + np + 1));
        for (int i = 0; i < np; i++) skip[nskip++] = to_unix(icalproperty_get_datetime_with_component(p[i], master));
        struct icaltimetype dts = icalcomponent_get_dtstart(master);
        np = props(master, ICAL_RRULE_PROPERTY, p);
        int rules = 0;
        for (int i = 0; i < np; i++) {
            struct icalrecurrencetype *rule = icalproperty_get_rrule(p[i]);
            icalrecur_iterator *it = rule ? icalrecur_iterator_new(rule, dts) : NULL;
            if (!it) continue;
            rules++;
            int steps = 0;
            for (struct icaltimetype t = icalrecur_iterator_next(it); !icaltime_is_null_time(t) && steps++ < MAX_STEPS;
                 t = icalrecur_iterator_next(it)) {
                if (!t.zone && !t.is_date) t.zone = dts.zone;
                int64_t s = to_unix(t);
                if (s >= to) break;
                if (!listed(skip, nskip, s)) add(&sp, s, &base);
            }
            icalrecur_iterator_free(it);
        }
        if (!rules && !listed(skip, nskip, base.start)) add(&sp, base.start, &base);
        np = props(master, ICAL_RDATE_PROPERTY, p);
        for (int i = 0; i < np; i++) {
            struct icaldatetimeperiodtype r = icalproperty_get_rdate(p[i]);
            struct icaltimetype t = icaltime_is_null_time(r.time) ? r.period.start : r.time;
            if (icaltime_is_null_time(t)) continue;
            if (!t.zone && !t.is_date) {
                icalparameter *tzid = icalproperty_get_first_parameter(p[i], ICAL_TZID_PARAMETER);
                t.zone = tzid ? icalcomponent_get_timezone(cal, icalparameter_get_tzid(tzid)) : dts.zone;
            }
            int64_t s = to_unix(t);
            if (!listed(skip, nskip, s)) add(&sp, s, &base);
        }
        jd_ical_event_free(&base);
    }
    free(skip);
    icalcomponent_free(cal);
}

void jd_occurrences_free(jd_occurrence *o, int n)
{
    for (int i = 0; i < n; i++) free(o[i].summary), free(o[i].location);
    free(o);
}

static char *new_uid(void)
{
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1)
        for (size_t i = 0; i < sizeof b; i++) b[i] = (unsigned char)rand();
    b[6] = (b[6] & 0x0f) | 0x40, b[8] = (b[8] & 0x3f) | 0x80;
    return jd_printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                     b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void set_text(icalcomponent *ev, icalproperty_kind kind, const char *v)
{
    icalcomponent_remove_property_by_kind(ev, kind);
    if (!v || !v[0]) return;
    icalproperty *p = icalproperty_new(kind);
    icalproperty_set_value(p, icalvalue_new_text(v));
    icalcomponent_add_property(ev, p);
}

static struct icaltimetype date_of(int64_t t)
{
    struct icaltimetype d = icaltime_null_date();
    jd_civil_from_days(t / 86400, &d.year, &d.month, &d.day);
    return d;
}

static void drop_kind(icalcomponent *c, icalcomponent_kind kind, int overrides_only)
{
    icalcomponent *gone[MAX_EVENTS];
    int n = 0;
    for (icalcomponent *e = icalcomponent_get_first_component(c, kind); e && n < MAX_EVENTS;
         e = icalcomponent_get_next_component(c, kind))
        if (!overrides_only || is_override(e)) gone[n++] = e;
    for (int i = 0; i < n; i++) {
        icalcomponent_remove_component(c, gone[i]);
        icalcomponent_free(gone[i]);
    }
}

static icaltimezone *local_zone(void)
{
    const char *name = jd_local_tzid();
    return strcmp(name, "UTC") == 0 ? NULL : icaltimezone_get_builtin_timezone(name);
}

/* the zone's rules from a year before from on: the system's zoneinfo goes back to 1893, kilobytes for every event */
static void need_zone(icalcomponent *cal, icaltimezone *z, int64_t from)
{
    const char *tzid = icaltimezone_get_tzid(z);
    if (!tzid || icalcomponent_get_timezone(cal, tzid)) return;
    icalcomponent *vtz = icaltimezone_get_component(z);
    if (!vtz) return;
    vtz = icalcomponent_clone(vtz);
    struct icaltimetype since = icaltime_from_timet_with_zone((icaltime_t)(from - 366 * 86400), 0,
                                                              icaltimezone_get_utc_timezone());
    icaltimezone_truncate_vtimezone(vtz, since, icaltime_null_time(), true);
    icalcomponent_add_component(cal, vtz);
}

char *jd_ical_write(const char *old, const jd_ical_event *e)
{
    icalcomponent *cal = parse(old);
    icalcomponent *ev = cal ? master_of(cal) : NULL;
    jd_ical_event was = {.alarm = -1};
    if (ev) {
        read_event(ev, &was);
    } else {
        if (cal) icalcomponent_free(cal);
        cal = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
        icalcomponent_add_property(cal, icalproperty_new_version("2.0"));
        icalcomponent_add_property(cal, icalproperty_new_prodid("-//jackdaw//jackdaw//EN"));
        ev = icalcomponent_new(ICAL_VEVENT_COMPONENT);
        icalcomponent_add_component(cal, ev);
    }
    if (!icalcomponent_get_uid(ev)) {
        char *uid = e->uid ? jd_strdup(e->uid) : new_uid();
        icalcomponent_set_uid(ev, uid);
        free(uid);
    }
    set_text(ev, ICAL_SUMMARY_PROPERTY, e->summary);
    set_text(ev, ICAL_LOCATION_PROPERTY, e->location);
    set_text(ev, ICAL_DESCRIPTION_PROPERTY, e->description);

    icalcomponent_remove_property_by_kind(ev, ICAL_DURATION_PROPERTY);
    icalcomponent_remove_property_by_kind(ev, ICAL_DTEND_PROPERTY);
    struct icaltimetype ts, te;
    if (e->all_day) {
        ts = date_of(e->start);
        te = date_of(e->end > e->start ? e->end : e->start + 86400);
    } else {
        icaltimezone *z = local_zone();
        icaltimezone *zone = z ? z : icaltimezone_get_utc_timezone();
        ts = icaltime_from_timet_with_zone((icaltime_t)e->start, 0, zone);
        te = icaltime_from_timet_with_zone((icaltime_t)(e->end > e->start ? e->end : e->start), 0, zone);
        if (z) need_zone(cal, z, e->start);
    }
    icalcomponent_set_dtstart(ev, ts);
    icalcomponent_set_dtend(ev, te);

    int same_rule = (!was.rrule && !e->rrule) || (was.rrule && e->rrule && strcmp(was.rrule, e->rrule) == 0);
    if (!same_rule) {
        icalcomponent_remove_property_by_kind(ev, ICAL_RRULE_PROPERTY);
        struct icalrecurrencetype *r = e->rrule ? icalrecurrencetype_new_from_string(e->rrule) : NULL;
        if (r) {
            icalcomponent_add_property(ev, icalproperty_new_rrule(r));
            icalrecurrencetype_unref(r);
        }
    }
    if (old && was.rrule && (!same_rule || was.start != e->start || was.all_day != e->all_day)) {
        icalcomponent_remove_property_by_kind(ev, ICAL_EXDATE_PROPERTY);
        icalcomponent_remove_property_by_kind(ev, ICAL_RDATE_PROPERTY);
        drop_kind(cal, ICAL_VEVENT_COMPONENT, 1);
    }

    if (!old || e->alarm != was.alarm) {
        drop_kind(ev, ICAL_VALARM_COMPONENT, 0);
        if (e->alarm >= 0) {
            icalcomponent *a = icalcomponent_new(ICAL_VALARM_COMPONENT);
            icalcomponent_add_property(a, icalproperty_new_action(ICAL_ACTION_DISPLAY));
            icalcomponent_add_property(a, icalproperty_new_description(e->summary && e->summary[0] ? e->summary : "Reminder"));
            struct icaltriggertype tr = {.time = icaltime_null_time()};
            tr.duration.is_neg = e->alarm > 0;
            tr.duration.minutes = (unsigned)e->alarm;
            icalcomponent_add_property(a, icalproperty_new_trigger(tr));
            icalcomponent_add_component(ev, a);
        }
    }
    if (old) icalcomponent_set_sequence(ev, icalcomponent_get_sequence(ev) + 1);
    icaltimezone *utc = icaltimezone_get_utc_timezone();
    struct icaltimetype now = icaltime_from_timet_with_zone((icaltime_t)time(NULL), 0, utc);
    icalcomponent_set_dtstamp(ev, now);
    icalcomponent_remove_property_by_kind(ev, ICAL_LASTMODIFIED_PROPERTY);
    icalcomponent_add_property(ev, icalproperty_new_lastmodified(now));
    char *out = take(icalcomponent_as_ical_string_r(cal));
    jd_ical_event_free(&was);
    icalcomponent_free(cal);
    return out;
}

/* ---- iTIP ---- */

static const char *address(const char *cal_address)
{
    if (!cal_address) return "";
    return strncasecmp(cal_address, "mailto:", 7) == 0 ? cal_address + 7 : cal_address;
}

static icalproperty *attendee(icalcomponent *ev, const char *me)
{
    for (icalproperty *p = icalcomponent_get_first_property(ev, ICAL_ATTENDEE_PROPERTY); p;
         p = icalcomponent_get_next_property(ev, ICAL_ATTENDEE_PROPERTY))
        if (me && strcasecmp(address(icalproperty_get_attendee(p)), me) == 0) return p;
    return NULL;
}

static char *partstat_of(icalproperty *p)
{
    char *v = icalproperty_get_parameter_as_string_r(p, "PARTSTAT");
    return v ? take(v) : jd_strdup("NEEDS-ACTION");
}

int jd_invite_read(const char *ics, const char *me, jd_invite *out)
{
    *out = (jd_invite){.event.alarm = -1};
    icalcomponent *cal = parse(ics);
    icalcomponent *ev = cal ? master_of(cal) : NULL;
    if (!ev) {
        if (cal) icalcomponent_free(cal);
        return -1;
    }
    icalproperty_method m = icalcomponent_get_method(cal);
    out->method = m == ICAL_METHOD_REQUEST ? JD_ITIP_REQUEST : m == ICAL_METHOD_CANCEL ? JD_ITIP_CANCEL
                  : m == ICAL_METHOD_REPLY ? JD_ITIP_REPLY : JD_ITIP_OTHER;
    read_event(ev, &out->event);
    icalproperty *org = icalcomponent_get_first_property(ev, ICAL_ORGANIZER_PROPERTY);
    if (org) {
        out->organizer = dup_text(address(icalproperty_get_organizer(org)));
        icalparameter *cn = icalproperty_get_first_parameter(org, ICAL_CN_PARAMETER);
        if (cn) out->organizer_name = dup_text(icalparameter_get_cn(cn));
    }
    icalproperty *mine = attendee(ev, me);
    if (mine) out->partstat = partstat_of(mine);
    icalproperty *first = icalcomponent_get_first_property(ev, ICAL_ATTENDEE_PROPERTY);
    if (out->method == JD_ITIP_REPLY && first) {
        out->replier = dup_text(address(icalproperty_get_attendee(first)));
        out->reply_partstat = partstat_of(first);
    }
    icalcomponent_free(cal);
    return 0;
}

void jd_invite_free(jd_invite *i)
{
    free(i->organizer), free(i->organizer_name), free(i->partstat), free(i->replier), free(i->reply_partstat);
    jd_ical_event_free(&i->event);
    *i = (jd_invite){.event.alarm = -1};
}

static void answer(icalproperty *p, const char *partstat)
{
    icalproperty_remove_parameter_by_kind(p, ICAL_RSVP_PARAMETER);
    icalproperty_set_parameter_from_string(p, "PARTSTAT", partstat);
}

char *jd_itip_reply(const char *ics, const char *me, const char *partstat)
{
    icalcomponent *cal = parse(ics);
    icalcomponent *ev = cal ? master_of(cal) : NULL;
    icalproperty *mine = ev ? attendee(ev, me) : NULL;
    if (!mine) {
        if (cal) icalcomponent_free(cal);
        return NULL;
    }
    icalcomponent *rcal = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
    icalcomponent_add_property(rcal, icalproperty_new_version("2.0"));
    icalcomponent_add_property(rcal, icalproperty_new_prodid("-//jackdaw//jackdaw//EN"));
    icalcomponent_add_property(rcal, icalproperty_new_method(ICAL_METHOD_REPLY));
    for (icalcomponent *tz = icalcomponent_get_first_component(cal, ICAL_VTIMEZONE_COMPONENT); tz;
         tz = icalcomponent_get_next_component(cal, ICAL_VTIMEZONE_COMPONENT))
        icalcomponent_add_component(rcal, icalcomponent_clone(tz));
    icalcomponent *rev = icalcomponent_new(ICAL_VEVENT_COMPONENT);
    static const icalproperty_kind keep[] = {ICAL_UID_PROPERTY,     ICAL_SEQUENCE_PROPERTY, ICAL_ORGANIZER_PROPERTY,
                                             ICAL_DTSTART_PROPERTY, ICAL_DTEND_PROPERTY,    ICAL_DURATION_PROPERTY,
                                             ICAL_SUMMARY_PROPERTY, ICAL_RECURRENCEID_PROPERTY};
    for (size_t k = 0; k < sizeof keep / sizeof *keep; k++) {
        icalproperty *p = icalcomponent_get_first_property(ev, keep[k]);
        if (p) icalcomponent_add_property(rev, icalproperty_clone(p));
    }
    icalproperty *a = icalproperty_clone(mine);
    answer(a, partstat);
    icalcomponent_add_property(rev, a);
    icalcomponent_set_dtstamp(rev, icaltime_from_timet_with_zone((icaltime_t)time(NULL), 0, icaltimezone_get_utc_timezone()));
    icalcomponent_add_component(rcal, rev);
    char *out = take(icalcomponent_as_ical_string_r(rcal));
    icalcomponent_free(rcal);
    icalcomponent_free(cal);
    return out;
}

char *jd_itip_keep(const char *ics, const char *me, const char *partstat, int client_replies)
{
    icalcomponent *cal = parse(ics);
    if (!cal) return NULL;
    icalcomponent_remove_property_by_kind(cal, ICAL_METHOD_PROPERTY);
    icalcomponent *ev[MAX_EVENTS];
    int n = collect(cal, ev);
    for (int i = 0; i < n; i++) {
        icalproperty *mine = attendee(ev[i], me);
        if (mine) answer(mine, partstat);
        icalproperty *org = icalcomponent_get_first_property(ev[i], ICAL_ORGANIZER_PROPERTY);
        if (org && client_replies) {
            icalproperty_remove_parameter_by_kind(org, ICAL_SCHEDULEAGENT_PARAMETER);
            icalproperty_add_parameter(org, icalparameter_new_scheduleagent(ICAL_SCHEDULEAGENT_CLIENT));
        }
    }
    char *out = take(icalcomponent_as_ical_string_r(cal));
    icalcomponent_free(cal);
    return out;
}
