#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mail/account.h"
#include "store/store.h"

typedef struct jd_imap jd_imap;

enum { JD_IMAP_OK = 0, JD_IMAP_ERROR = -1, JD_IMAP_AUTH = -2 };

/* err gets the server's or the library's reason on failure */
jd_imap *jd_imap_connect(const jd_account *a, const char *password, int *status, char *err, int errlen);
void jd_imap_close(jd_imap *m);
int jd_imap_condstore(jd_imap *m);
int jd_imap_can_idle(jd_imap *m);

typedef struct {
    char *name, *special_use;
    char delim;
    int noselect;
} jd_mailbox;

int jd_imap_list(jd_imap *m, jd_mailbox **out, int *n);
void jd_mailboxes_free(jd_mailbox *b, int n);

typedef struct {
    uint32_t uidvalidity, uidnext, exists;
    uint64_t highestmodseq;
} jd_selected;

int jd_imap_select(jd_imap *m, const char *name, jd_selected *sel);
/* UID SEARCH ALL, or SINCE the day of since when since > 0; ascending */
int jd_imap_search(jd_imap *m, int64_t since, uint32_t **uids, int *n);
typedef void jd_header_cb(void *ctx, jd_header *h);
/* ENVELOPE, FLAGS, INTERNALDATE, RFC822.SIZE, BODYSTRUCTURE and the References field of the given uids */
int jd_imap_fetch_headers(jd_imap *m, const uint32_t *uids, int n, jd_header_cb *cb, void *ctx);
typedef void jd_flags_cb(void *ctx, uint32_t uid, int flags, const char *keywords, uint64_t modseq);
/* every message's flags, or only those changed since modseq (CONDSTORE, RFC 7162) when modseq > 0 */
int jd_imap_fetch_flags(jd_imap *m, uint64_t changedsince, jd_flags_cb *cb, void *ctx);
typedef void jd_body_cb(void *ctx, uint32_t uid, const char *data, size_t len);
/* BODY.PEEK[]: whole messages, without setting \Seen */
int jd_imap_fetch_bodies(jd_imap *m, const uint32_t *uids, int n, jd_body_cb *cb, void *ctx);

/* UID STORE +FLAGS.SILENT or -FLAGS.SILENT of one JD_ flag */
int jd_imap_set_flag(jd_imap *m, const uint32_t *uids, int n, int flag, int on);
/* UID SEARCH CHARSET UTF-8 TEXT w1 TEXT w2 ... in the selected folder */
int jd_imap_search_text(jd_imap *m, const char *words, uint32_t **uids, int *n);

int jd_imap_create(jd_imap *m, const char *name);
int jd_imap_rename(jd_imap *m, const char *from, const char *to);
int jd_imap_delete(jd_imap *m, const char *name);
/* From the selected folder. Move uses MOVE (RFC 6851), else COPY + \Deleted + UID EXPUNGE (RFC 4315); without
   either extension it refuses rather than EXPUNGE other messages marked \Deleted. */
int jd_imap_copy(jd_imap *m, const uint32_t *uids, int n, const char *to);
int jd_imap_move(jd_imap *m, const uint32_t *uids, int n, const char *to);
int jd_imap_expunge(jd_imap *m, const uint32_t *uids, int n);
/* APPEND with the given JD_ flags */
int jd_imap_append(jd_imap *m, const char *folder, const char *data, size_t len, int flags);

/* IDLE (RFC 2177): start, wait on the fd, then done */
int jd_imap_idle_start(jd_imap *m);
int jd_imap_idle_fd(jd_imap *m);
int jd_imap_idle_done(jd_imap *m);
