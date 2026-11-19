/* src/cal/ical.c against calendar data written as servers write it: zones, repeats, cancelled and moved
   occurrences, all-day events, invitations and the answers to them */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal/ical.h"

static int failed;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
            failed = 1;                                                                                                \
        }                                                                                                              \
    } while (0)

static const char *weekly =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n"
    "BEGIN:VTIMEZONE\r\nTZID:Europe/Berlin\r\n"
    "BEGIN:DAYLIGHT\r\nTZOFFSETFROM:+0100\r\nTZOFFSETTO:+0200\r\nTZNAME:CEST\r\nDTSTART:19700329T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\nEND:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nTZNAME:CET\r\nDTSTART:19701025T030000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
    "BEGIN:VEVENT\r\nUID:weekly-1\r\nSUMMARY:Standup\r\nLOCATION:Room 4\r\n"
    "DTSTART;TZID=Europe/Berlin:20261020T090000\r\nDTEND;TZID=Europe/Berlin:20261020T093000\r\n"
    "RRULE:FREQ=WEEKLY;COUNT=6\r\nEXDATE;TZID=Europe/Berlin:20261103T090000\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Standup\r\nTRIGGER:-PT10M\r\nEND:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\nUID:weekly-1\r\nRECURRENCE-ID;TZID=Europe/Berlin:20261110T090000\r\nSUMMARY:Standup (moved)\r\n"
    "DTSTART;TZID=Europe/Berlin:20261110T110000\r\nDTEND;TZID=Europe/Berlin:20261110T113000\r\nEND:VEVENT\r\n"
    "END:VCALENDAR\r\n";

static const char *allday = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:day-1\r\nSUMMARY:Holiday\r\n"
                            "DTSTART;VALUE=DATE:20261224\r\nDTEND;VALUE=DATE:20261227\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

static const char *invite =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nMETHOD:REQUEST\r\nBEGIN:VEVENT\r\nUID:inv-1\r\nSEQUENCE:2\r\n"
    "SUMMARY:Review\r\nDTSTART:20261201T140000Z\r\nDTEND:20261201T150000Z\r\n"
    "ORGANIZER;CN=Ada Lovelace:mailto:ada@example.org\r\n"
    "ATTENDEE;CN=Ada Lovelace;PARTSTAT=ACCEPTED:mailto:ada@example.org\r\n"
    "ATTENDEE;RSVP=TRUE;PARTSTAT=NEEDS-ACTION:mailto:Me@Example.com\r\n"
    "END:VEVENT\r\nEND:VCALENDAR\r\n";

/* 2026-10-20 09:00 CEST */
#define T0 1792479600LL

int main(void)
{
    jd_ical_event e;
    CHECK(jd_ical_read(weekly, &e) == 0);
    CHECK(e.uid && strcmp(e.uid, "weekly-1") == 0);
    CHECK(e.start == T0 && e.end == T0 + 1800 && !e.all_day);
    CHECK(e.rrule && strstr(e.rrule, "FREQ=WEEKLY"));
    CHECK(e.alarm == 10);
    jd_ical_event_free(&e);

    jd_occurrence *o = NULL;
    int n = 0;
    jd_ical_expand(weekly, T0 - 86400, T0 + 60 * 86400, &o, &n);
    /* six weeks: one cancelled (Nov 3), one moved (Nov 10, 11:00 CET); Oct 27 on is CET, an hour later in UTC */
    CHECK(n == 5);
    int moved = 0, dst = 0;
    for (int i = 0; i < n; i++) {
        if (o[i].summary && strcmp(o[i].summary, "Standup (moved)") == 0) moved = o[i].start == T0 + 21 * 86400 + 3600 + 7200;
        if (o[i].start == T0 + 7 * 86400 + 3600) dst = 1;
        CHECK(o[i].start != T0 + 14 * 86400 + 3600);
    }
    CHECK(moved && dst);
    jd_occurrences_free(o, n);

    o = NULL, n = 0;
    jd_ical_expand(allday, jd_days_from_civil(2026, 12, 25) * 86400, jd_days_from_civil(2026, 12, 26) * 86400, &o, &n);
    CHECK(n == 1 && o[0].all_day && o[0].start == jd_days_from_civil(2026, 12, 24) * 86400 &&
          o[0].end == jd_days_from_civil(2026, 12, 27) * 86400);
    jd_occurrences_free(o, n);

    jd_ical_event w = {.summary = "Dentist", .location = "Main St", .start = T0, .end = T0 + 3600, .alarm = 30,
                       .rrule = "FREQ=MONTHLY"};
    char *made = jd_ical_write(NULL, &w);
    CHECK(made && strstr(made, "SUMMARY:Dentist") && strstr(made, "RRULE:FREQ=MONTHLY") && strstr(made, "TRIGGER:-PT30M"));
    CHECK(jd_ical_read(made, &e) == 0 && e.start == T0 && e.end == T0 + 3600 && e.alarm == 30 && e.uid);
    char *uid = e.uid ? strdup(e.uid) : NULL;
    jd_ical_event_free(&e);
    w.rrule = NULL, w.summary = "Dentist (moved)", w.start += 3600, w.end += 3600;
    char *again = jd_ical_write(made, &w);
    CHECK(jd_ical_read(again, &e) == 0 && !e.rrule && e.uid && uid && strcmp(e.uid, uid) == 0 && e.start == T0 + 3600);
    CHECK(strstr(again, "SEQUENCE:1"));
    jd_ical_event_free(&e);
    free(made), free(again), free(uid);

    jd_ical_event d = {.summary = "Trip", .all_day = 1, .start = jd_days_from_civil(2027, 1, 4) * 86400,
                       .end = jd_days_from_civil(2027, 1, 6) * 86400, .alarm = -1};
    char *day = jd_ical_write(NULL, &d);
    CHECK(day && strstr(day, "DTSTART;VALUE=DATE:20270104") && strstr(day, "DTEND;VALUE=DATE:20270106"));
    CHECK(day && !strstr(day, "VALARM"));
    free(day);

    jd_invite inv;
    CHECK(jd_invite_read(invite, "me@example.com", &inv) == 0);
    CHECK(inv.method == JD_ITIP_REQUEST && inv.organizer && strcmp(inv.organizer, "ada@example.org") == 0);
    CHECK(inv.organizer_name && strcmp(inv.organizer_name, "Ada Lovelace") == 0);
    CHECK(inv.partstat && strcmp(inv.partstat, "NEEDS-ACTION") == 0);
    jd_invite_free(&inv);
    char *reply = jd_itip_reply(invite, "me@example.com", "ACCEPTED");
    CHECK(reply && strstr(reply, "METHOD:REPLY") && strstr(reply, "PARTSTAT=ACCEPTED") && strstr(reply, "UID:inv-1"));
    CHECK(reply && !strstr(reply, "ada@example.org;") && !strstr(reply, "RSVP"));
    CHECK(reply && strstr(reply, "SEQUENCE:2"));
    CHECK(jd_invite_read(reply, "ada@example.org", &inv) == 0 && inv.method == JD_ITIP_REPLY && inv.replier &&
          strcmp(inv.replier, "Me@Example.com") == 0 && strcmp(inv.reply_partstat, "ACCEPTED") == 0);
    jd_invite_free(&inv);
    free(reply);
    char *kept = jd_itip_keep(invite, "me@example.com", "TENTATIVE", 1);
    CHECK(kept && !strstr(kept, "METHOD") && strstr(kept, "PARTSTAT=TENTATIVE") && strstr(kept, "SCHEDULE-AGENT=CLIENT"));
    free(kept);
    CHECK(jd_itip_reply(invite, "nobody@example.com", "ACCEPTED") == NULL);

    int y, m, dd;
    jd_civil_from_days(jd_days_from_civil(2024, 2, 29), &y, &m, &dd);
    CHECK(y == 2024 && m == 2 && dd == 29);
    if (!failed) printf("ical: all passed\n");
    return failed;
}
