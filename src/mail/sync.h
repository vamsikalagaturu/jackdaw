#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mail/account.h"

/* One worker thread per account. It owns the IMAP connection and the writing store connection; the UI sends
   it jobs and reads its events once per frame. Changes to messages land in the store at once, so the UI sees
   them while offline too, and reach the server, in order, when connected. */
typedef struct jd_sync jd_sync;

typedef enum {
    JD_EV_STATUS,   /* text: what the worker is doing, NULL when idle */
    JD_EV_ERROR,    /* text: why the last attempt failed */
    JD_EV_AUTH,     /* text: the server refused the password; the worker stops */
    JD_EV_CHANGED,  /* the store changed: re-read what is shown */
    JD_EV_NEW_MAIL, /* count new unread Inbox messages, id the newest, text its sender, text2 its subject, text3 the
                       opening of its body (the list's snippet; NULL while the body is not downloaded) */
    JD_EV_BODY,     /* id: that message's body is cached */
    JD_EV_SEARCH,   /* id: the search's serial; ids: messages the server found */
    JD_EV_SENT,     /* id: the send's serial; text: why it failed, NULL when sent */
    JD_EV_SAVED,    /* id: the draft save's serial; text: why it failed, NULL when saved */
    JD_EV_SIEVE,    /* the server's script: text the script, text2 its name; count -1 and text the reason on failure */
    JD_EV_SIEVE_SAVED, /* text: why the server refused the script, NULL when stored and active */
    JD_EV_SCHEDULED,   /* a scheduled draft's time came: text2 its subject; text why it was not sent (it is back in
                          Drafts, unscheduled), NULL when sent */
} jd_event_kind;

typedef struct {
    jd_event_kind kind;
    char *text, *text2, *text3;
    int64_t id;
    int count;
    int64_t *ids;
    int nids;
} jd_event;

jd_sync *jd_sync_start(const jd_account *a, const char *password, const char *account_dir, const char *db_path);
void jd_sync_stop(jd_sync *s);
void jd_sync_now(jd_sync *s);
int jd_sync_poll(jd_sync *s, jd_event *ev);
/* called on the worker's thread, besides JD_EV_NEW_MAIL, for a platform whose event loop stops in the
   background (account: the worker's address); NULL by default */
extern void (*jd_sync_new_mail_hook)(const char *account, const char *from, const char *subject, const char *preview,
                                     int64_t id, int count);
/* the same for JD_EV_SCHEDULED: err NULL when sent */
extern void (*jd_sync_scheduled_hook)(const char *subject, const char *err);
void jd_event_free(jd_event *ev);

void jd_sync_fetch_body(jd_sync *s, int64_t message_id);
/* flag: JD_SEEN or JD_FLAGGED */
void jd_sync_set_flag(jd_sync *s, const int64_t *ids, int n, int flag, int on);
/* to the folder named to; copy keeps the originals */
void jd_sync_move(jd_sync *s, const int64_t *ids, int n, const char *to, int copy);
/* out of sight until then (Unix time): into the Snoozed folder (jd_store_snoozed_folder, made when missing) with the
   time as a keyword; whichever client of the account runs then moves them back to the Inbox, unread */
void jd_sync_snooze(jd_sync *s, const int64_t *ids, int n, int64_t until);
/* removes the messages for good (\Deleted and UID EXPUNGE) */
void jd_sync_destroy(jd_sync *s, const int64_t *ids, int n);
/* server-side full-text search of every folder, or of folder_name; answered with JD_EV_SEARCH */
void jd_sync_search(jd_sync *s, const char *words, const char *folder_name, int64_t serial);
void jd_sync_create_folder(jd_sync *s, const char *name);
/* stores msg as the shared settings (jd_store_settings_folder, made when missing), replacing the copies there, when
   the newest there is still merged_with (its UID, 0 for none); else another jackdaw was first and only
   JD_EV_CHANGED comes, for the UI to merge again */
void jd_sync_put_settings(jd_sync *s, const char *msg, size_t len, uint32_t merged_with);
void jd_sync_rename_folder(jd_sync *s, const char *from, const char *to);
void jd_sync_delete_folder(jd_sync *s, const char *name);
/* Sends msg over SMTP, then stores a copy in Sent, marks replied_to \Answered (when not 0) and removes drafts
   with msgid. Unlike the other jobs it is not retried: the answer is JD_EV_SENT. */
void jd_sync_send(jd_sync *s, const char *msg, size_t len, char **rcpts, int nrcpts, const char *msgid,
                  int64_t replied_to, int64_t serial);
void jd_sync_sieve_get(jd_sync *s);
void jd_sync_sieve_put(jd_sync *s, const char *name, const char *script);
/* stores msg in Drafts and removes the older drafts with msgid; answered with JD_EV_SAVED. A draft with
   X-Send-Later-* fields (jd_outgoing.send_at) is sent at its time by the worker that finds it due first. */
void jd_sync_save_draft(jd_sync *s, const char *msg, size_t len, const char *msgid, int64_t serial);
/* while held, a scheduled draft with msgid (open in compose) is not sent, though due; once per hold */
void jd_sync_hold(jd_sync *s, const char *msgid, int on);
