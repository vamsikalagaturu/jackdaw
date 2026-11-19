#pragma once
#include <stdint.h>

/* mail.db: the sync worker writes through one connection, the UI reads through its own (WAL allows both). */
typedef struct jd_store jd_store;

enum { JD_SEEN = 1, JD_ANSWERED = 2, JD_FLAGGED = 4, JD_DELETED = 8, JD_DRAFT = 16 };
enum { JD_ROLE_FROM, JD_ROLE_TO, JD_ROLE_CC, JD_ROLE_REPLY_TO, JD_ROLE_COUNT };

typedef struct {
    char *name, *addr;
} jd_addr;

typedef struct {
    uint32_t uid;
    uint64_t modseq;
    int flags;
    char *keywords;
    int64_t internaldate, date, size;
    char *msgid, *in_reply_to, *subject;
    jd_addr *addrs[JD_ROLE_COUNT];
    int naddrs[JD_ROLE_COUNT];
    char **refs;
    int nrefs;
    int has_attachment;
    int64_t send_at; /* X-Send-Later-At, for a draft scheduled by this account's clients; else 0 */
    char *send_uuid; /* X-Send-Later-Uuid */
} jd_header;

void jd_header_free(jd_header *h);

jd_store *jd_store_open(const char *path, char *err, int errlen);
void jd_store_close(jd_store *s);
int jd_store_begin(jd_store *s);
int jd_store_commit(jd_store *s);

typedef struct {
    int64_t id;
    uint32_t uidvalidity, uidnext;
    uint64_t highestmodseq;
    int count;
} jd_folder_state;

/* Folders not upserted between mark_absent and sweep_absent are gone from the server and dropped. */
void jd_folders_mark_absent(jd_store *s);
int64_t jd_folder_upsert(jd_store *s, const char *name, char delim, const char *special_use);
void jd_folders_sweep_absent(jd_store *s);
int jd_folder_state_get(jd_store *s, int64_t folder, jd_folder_state *st);
void jd_folder_state_set(jd_store *s, int64_t folder, uint32_t uidvalidity, uint32_t uidnext, uint64_t modseq);
/* every message of the folder, for a changed UIDVALIDITY */
void jd_folder_clear(jd_store *s, int64_t folder);

uint32_t *jd_folder_uids(jd_store *s, int64_t folder, int *n);
/* Inserts the message and threads it, or updates its flags when (folder, uid) is known; returns its id. */
int64_t jd_message_put(jd_store *s, int64_t folder, const jd_header *h, int *is_new);
void jd_message_set_flags(jd_store *s, int64_t folder, uint32_t uid, int flags, const char *keywords, uint64_t modseq);
void jd_message_delete(jd_store *s, int64_t folder, uint32_t uid);
void jd_message_set_body(jd_store *s, int64_t id, const char *text);
/* the stored message's subject and addresses replaced by h's; its id, 0 when it is not stored */
int64_t jd_message_refresh(jd_store *s, int64_t folder, const jd_header *h);
/* headers stored before every charset could be decoded (Android before schema 4): sync fetches them again, once */
int jd_folder_headers_stale(jd_store *s, int64_t folder);
void jd_folder_headers_done(jd_store *s, int64_t folder);
/* snippets and search text made before HTML read as text without its chrome (schema 5): sync makes them again
   from the bodies on disk, once */
int jd_folder_texts_stale(jd_store *s, int64_t folder);
void jd_folder_texts_done(jd_store *s, int64_t folder);
/* uids in folder with a cached body */
uint32_t *jd_folder_cached(jd_store *s, int64_t folder, int *n);
int jd_message_locate(jd_store *s, int64_t id, int64_t *folder, char **folder_name, uint32_t *uid, int64_t *size);
/* uids in folder without a cached body, of messages at or after since and at most max_size bytes */
uint32_t *jd_folder_uncached(jd_store *s, int64_t folder, int64_t since, int64_t max_size, int *n);
int64_t jd_message_id(jd_store *s, int64_t folder, uint32_t uid);
int64_t jd_message_thread(jd_store *s, int64_t id);
/* refreshes threads.last_date and threads.unread for the folder's threads and drops empty threads */
void jd_threads_refresh(jd_store *s, int64_t folder);

/* $dir/bodies/<folder>/<uidvalidity>-<uid>.eml */
char *jd_body_path(const char *account_dir, int64_t folder, uint32_t uidvalidity, uint32_t uid);

typedef struct {
    int64_t id;
    char *name, *special_use;
    char *path; /* name without the personal namespace's prefix ("INBOX.Orders" is "Orders"); else name */
    char delim;
    int collapsed;
    int total, unread;
} jd_folder_row;

/* the personal namespace's prefix (jd_imap_namespace), as last heard from the server; "" when none */
void jd_store_set_namespace(jd_store *s, const char *prefix, char delim);
char *jd_store_namespace(jd_store *s);

void jd_folder_set_collapsed(jd_store *s, int64_t folder, int collapsed);

/* The settings shared between the account's jackdaws are one message in this folder under the personal namespace,
   kept out of the folder list and of searches */
#define JD_SETTINGS_FOLDER "jackdaw"
/* its id, 0 when the server has none yet; name (malloc'd) is its full name either way */
int64_t jd_store_settings_folder(jd_store *s, char **name);

/* Inbox first, then the special-use folders (the settings folder left out), then the rest by name */
jd_folder_row *jd_store_folders(jd_store *s, int *n);
void jd_folder_rows_free(jd_folder_row *rows, int n);

/* What the message list shows: a folder (or every folder), narrowed by filters and search terms. */
typedef struct jd_list_query jd_list_query;
/* terms of which at least one must match, each a query of its own (search's `a OR b`) */
typedef struct {
    jd_list_query *alt;
    int n;
} jd_query_any;
struct jd_list_query {
    int64_t folder; /* 0: every folder */
    int grouped;    /* one row per conversation, else one per message */
    int unread, read, flagged, attachment;
    char *from, *to, *subject; /* substrings of names/addresses, of the subject */
    char *text;                /* words for the full-text index, all must match */
    int64_t after, before;     /* message date range, 0 = open */
    const int64_t *extra;      /* messages the server found for text, shown as matches too */
    int nextra;
    const int64_t *only;       /* when set, nothing outside these messages */
    int nonly;
    jd_query_any *any;         /* every group must match too */
    int nany;
};

/* A row of the list: a conversation (count >= 1) or a single message, counts over the matching messages */
typedef struct {
    int64_t thread, latest;
    char *subject, *senders;
    char *snippet, *sender_addr; /* of the newest message */
    char *folders;               /* the folders the matching messages are in, comma-separated */
    int64_t date;
    int64_t send_at; /* of the newest message: a scheduled draft's time, else 0 */
    int count, unread, flagged, has_attachment;
} jd_thread_row;

jd_thread_row *jd_store_list(jd_store *s, const jd_list_query *q, int *n);
void jd_thread_rows_free(jd_thread_row *rows, int n);
int64_t jd_store_folder_by_name(jd_store *s, const char *name);
/* the folder with this RFC 6154 special use, 0 when none */
int64_t jd_store_special(jd_store *s, const char *use, char **name);
/* messages in folder with this Message-ID */
int64_t *jd_store_by_msgid(jd_store *s, int64_t folder, const char *msgid, int *n);
/* the scheduled drafts in folder not yet claimed (\Deleted), soonest first */
typedef struct {
    int64_t id, send_at;
    uint32_t uid;
    uint64_t modseq;
} jd_scheduled;
jd_scheduled *jd_store_scheduled(jd_store *s, int64_t folder, int *n);
/* a message's keywords, space-separated ("" for none); NULL when the folder has no message uid */
char *jd_store_keywords(jd_store *s, int64_t folder, uint32_t uid);
/* Message-ID and References of a message, for replying; refs space-separated */
int jd_store_reply_ids(jd_store *s, int64_t id, char **msgid, char **refs);

/* addresses seen in mail whose name or address starts with prefix (or a word of the name does), the ones
   written to most first */
typedef struct {
    char *name, *addr;
} jd_contact;
jd_contact *jd_store_contacts(jd_store *s, const char *prefix, int limit, int *n);
void jd_contacts_free(jd_contact *c, int n);

/* Every message of a conversation in any folder, oldest first */
typedef struct {
    int64_t id, folder, date;
    uint32_t uid, uidvalidity;
    int flags, body_cached, has_attachment;
    char *subject, *from, *to, *cc, *reply_to, *folder_name;
    char *special_use; /* of its folder */
} jd_message_row;

void jd_message_row_clear(jd_message_row *r);

jd_message_row *jd_store_conversation(jd_store *s, int64_t thread, int *n);
/* the conversation's messages in folder (every folder when 0) */
int64_t *jd_store_thread_ids(jd_store *s, int64_t thread, int64_t folder, int *n);
void jd_message_rows_free(jd_message_row *rows, int n);

void jd_store_set_flag(jd_store *s, int64_t id, int flag, int on);
void jd_message_delete_id(jd_store *s, int64_t id);

/* Calendars of the account's [calendar] server (schema 8), and their events as the server holds them: one iCalendar
   object per resource (href), with the fields the views and searches need taken out of it (cal/ical.h). */
typedef struct {
    int64_t id;
    char *href, *name, *color; /* href: the collection's absolute URL; color "#rrggbb", NULL when unset */
    char *sync_token, *ctag;   /* RFC 6578 and getctag: where the last look left off */
    int readonly, enabled;     /* enabled: chosen to be synced (and shown) */
    char *account;             /* jd_cal_account_id of its account; NULL for one kept before schema 9 */
    int shown;                 /* a synced calendar can be hidden from the views, its events kept */
    /* color above is the one in use: user_color, chosen in jackdaw (NULL: none), else server_color */
    char *user_color, *server_color;
} jd_calendar_row;
jd_calendar_row *jd_store_calendars(jd_store *s, int *n);
void jd_calendar_rows_free(jd_calendar_row *r, int n);
/* added (not enabled), or renamed and recoloured (its events and enabled kept), filed under account; returns its id */
int64_t jd_store_calendar_put(jd_store *s, const char *account, const char *href, const char *name, const char *color,
                              int readonly);
void jd_store_calendar_state(jd_store *s, int64_t id, const char *sync_token, const char *ctag);
/* chosen to be synced, or not: then its events and sync state go */
void jd_store_calendar_enable(jd_store *s, int64_t id, int on);
void jd_store_calendar_show(jd_store *s, int64_t id, int on);
/* a colour of the user's ("#rrggbb") over the server's; NULL goes back to the server's */
void jd_store_calendar_recolor(jd_store *s, int64_t id, const char *color);
/* account's calendars (and those filed under no account) whose href is not among hrefs go, with their events;
   account NULL: every calendar's */
void jd_store_calendars_keep(jd_store *s, const char *account, char **hrefs, int n);

/* dtstart, dtend: Unix seconds (cal/ical.h); a recurring event's dtend is its first occurrence's */
typedef struct {
    int64_t id, calendar;
    char *href, *etag, *uid, *ics, *summary, *location;
    int64_t dtstart, dtend;
    int all_day, recurring;
    char *color, *calendar_name; /* of its calendar */
    int readonly;
} jd_event_row;
void jd_store_event_put(jd_store *s, int64_t calendar, const char *href, const char *etag, const char *ics,
                        const char *uid, const char *summary, const char *location, int64_t dtstart, int64_t dtend,
                        int all_day, int recurring);
void jd_store_event_delete(jd_store *s, int64_t calendar, const char *href);
/* the events of synced and shown calendars that may have an occurrence in [from, to): every recurring one starting
   before to */
jd_event_row *jd_store_events(jd_store *s, int64_t from, int64_t to, int *n);
jd_event_row *jd_store_calendar_events(jd_store *s, int64_t calendar, int *n);
/* one event, NULL when gone */
jd_event_row *jd_store_event(jd_store *s, int64_t id);
jd_event_row *jd_store_event_by_uid(jd_store *s, const char *uid);
void jd_event_rows_free(jd_event_row *r, int n);
