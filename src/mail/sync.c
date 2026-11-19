#define _GNU_SOURCE
#include "sync.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "core/util.h"
#include "mail/imap.h"
#include "mail/mime.h"
#include "mail/query.h"
#include "mail/send.h"
#include "mail/sieve.h"
#include "store/store.h"

enum {
    INBOX_POLL_EVERY = 120, /* without IDLE */
    IDLE_RESTART = 25 * 60, /* RFC 2177: re-issue IDLE at least every 29 minutes */
    HEADER_BATCH = 200,
    BODY_BATCH = 10,
    PREFETCH_MAX_SIZE = 25 * 1024 * 1024,
    BACKOFF_MIN = 15,
    BACKOFF_MAX = 300,
    OP_ATTEMPTS = 3,
    OUTBOX_RETRY = 5 * 60,
};

typedef enum {
    OP_FLAG, OP_MOVE, OP_COPY, OP_DESTROY, OP_BODY, OP_SEARCH, OP_CREATE, OP_RENAME, OP_DELETE_FOLDER, OP_SEND, OP_DRAFT,
    OP_SIEVE_GET, OP_SIEVE_PUT
} op_kind;

/* A request from the UI, by message ids; the worker turns it into ops. */
typedef struct {
    op_kind kind;
    int64_t *ids;
    int n;
    int flag, on;
    char *a, *b; /* OP_SEND/OP_DRAFT: a the message, b its Message-ID */
    size_t len;
    char **rcpts;
    int nrcpts;
    int64_t serial, replied;
} request;

/* Work for the server, resolved to a folder and its UIDs, run in order once connected */
typedef struct {
    op_kind kind;
    int64_t folder;
    char *folder_name;
    uint32_t *uids;
    int n;
    int flag, on;
    char *a, *b;
    size_t len;
    char **rcpts;
    int nrcpts;
    int64_t serial, replied;
    int attempts;
} op;

struct jd_sync {
    jd_account account;
    char *password, *dir, *db_path;
    char *send_later_uuid;
    pthread_t thread;
    int wake[2];

    pthread_mutex_t lock;
    int quit, sync_now;
    request *requests;
    int nrequests;
    jd_event *events;
    int nevents;

    jd_store *store;
    jd_imap *imap;
    op *ops;
    int nops;
    int64_t inbox;
    int first_sync_done, want_full;
    int prefetch_days, check_minutes; /* from [sync], read each round */
    char **held; /* Message-IDs of drafts open in compose, not sent while open; under lock */
    int nheld;
    int64_t outbox_at; /* when to look at the outbox again, 0 when it is empty */
};

static void emit_ev(jd_sync *s, jd_event ev)
{
    pthread_mutex_lock(&s->lock);
    s->events = jd_realloc(s->events, sizeof *s->events * (size_t)(s->nevents + 1));
    s->events[s->nevents++] = ev;
    pthread_mutex_unlock(&s->lock);
}

static void emit(jd_sync *s, jd_event_kind kind, const char *text, const char *text2, int64_t id, int count)
{
    emit_ev(s, (jd_event){.kind = kind, .text = jd_strdup(text), .text2 = jd_strdup(text2), .id = id, .count = count});
}

static void status(jd_sync *s, const char *fmt, const char *arg)
{
    char *text = fmt ? jd_printf(fmt, arg) : NULL;
    emit(s, JD_EV_STATUS, text, NULL, 0, 0);
    free(text);
}

static void poke(jd_sync *s)
{
    char c = 1;
    if (write(s->wake[1], &c, 1) < 0 && errno != EAGAIN) perror("jackdaw: wake");
}

static void drain(jd_sync *s)
{
    char buf[64];
    while (read(s->wake[0], buf, sizeof buf) > 0) {}
}

static int quitting(jd_sync *s)
{
    pthread_mutex_lock(&s->lock);
    int q = s->quit;
    pthread_mutex_unlock(&s->lock);
    return q;
}

/* ---- folders ---- */

static void remove_bodies(jd_sync *s, int64_t folder)
{
    char *path = jd_printf("%s/bodies/%lld", s->dir, (long long)folder);
    DIR *d = opendir(path);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char *f = jd_printf("%s/%s", path, e->d_name);
        unlink(f);
        free(f);
    }
    if (d) closedir(d);
    rmdir(path);
    free(path);
}

typedef struct {
    jd_sync *s;
    int64_t folder;
    int is_inbox;
    int64_t *fresh; /* new Inbox messages */
    int nfresh;
} fetch_ctx;

static void on_header(void *data, jd_header *h)
{
    fetch_ctx *c = data;
    /* another program's schedule (Thunderbird's own uuid) is that program's to send */
    if (h->send_at && !(h->send_uuid && strcasecmp(h->send_uuid, c->s->send_later_uuid) == 0)) h->send_at = 0;
    int is_new;
    int64_t id = jd_message_put(c->s->store, c->folder, h, &is_new);
    if (!is_new || !c->is_inbox || !id) return;
    c->fresh = jd_realloc(c->fresh, sizeof *c->fresh * (size_t)(c->nfresh + 1));
    c->fresh[c->nfresh++] = id;
}

typedef struct {
    jd_sync *s;
    int64_t folder;
    uint32_t uidvalidity;
} refresh_ctx;

/* a stale message's headers again, and its snippet and search text from the body already on disk */
static void on_refresh(void *data, jd_header *h)
{
    refresh_ctx *c = data;
    int64_t id = jd_message_refresh(c->s->store, c->folder, h);
    if (!id) return;
    char *path = jd_body_path(c->s->dir, c->folder, c->uidvalidity, h->uid);
    size_t len;
    char *raw = jd_read_file(path, &len);
    if (raw) {
        char *text = jd_mime_body_text(raw, len);
        jd_message_set_body(c->s->store, id, text);
        free(text), free(raw);
    }
    free(path);
}

static void on_flags(void *data, uint32_t uid, int flags, const char *keywords, uint64_t modseq)
{
    fetch_ctx *c = data;
    jd_message_set_flags(c->s->store, c->folder, uid, flags, keywords, modseq);
}

typedef struct {
    jd_sync *s;
    int64_t folder;
    uint32_t uidvalidity;
} body_ctx;

static void on_body(void *data, uint32_t uid, const char *raw, size_t len)
{
    body_ctx *c = data;
    int64_t id = jd_message_id(c->s->store, c->folder, uid);
    if (!id) return;
    char *path = jd_body_path(c->s->dir, c->folder, c->uidvalidity, uid);
    char *dir = jd_strdup(path);
    *strrchr(dir, '/') = 0;
    if (jd_mkdirs(dir) == 0 && jd_write_file(path, raw, len, 0600) == 0) {
        char *text = jd_mime_body_text(raw, len);
        jd_message_set_body(c->s->store, id, text);
        free(text);
        emit(c->s, JD_EV_BODY, NULL, NULL, id, 0);
    }
    free(dir), free(path);
}

static int fetch_bodies(jd_sync *s, int64_t folder, uint32_t uidvalidity, const uint32_t *uids, int n)
{
    body_ctx c = {s, folder, uidvalidity};
    for (int i = 0; i < n && !quitting(s); i += BODY_BATCH) {
        int k = n - i < BODY_BATCH ? n - i : BODY_BATCH;
        jd_store_begin(s->store);
        int r = jd_imap_fetch_bodies(s->imap, uids + i, k, on_body, &c);
        jd_store_commit(s->store);
        if (r != JD_IMAP_OK) return r;
    }
    return JD_IMAP_OK;
}

static int contains(const uint32_t *sorted, int n, uint32_t v)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sorted[mid] == v) return 1;
        if (sorted[mid] < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static void push_per_folder(jd_sync *s, const request *rq, int forget);

/* new mail still in the Inbox and unread after the rules: one event, with the newest one's sender and subject */
static void notify_fresh(jd_sync *s, int64_t inbox, const int64_t *ids, int n)
{
    jd_list_query q = {.folder = inbox, .unread = 1, .only = ids, .nonly = n};
    int nrows;
    jd_thread_row *rows = jd_store_list(s->store, &q, &nrows);
    if (nrows) emit(s, JD_EV_NEW_MAIL, rows[0].senders, rows[0].subject, rows[0].latest, nrows);
    if (nrows && jd_sync_new_mail_hook) jd_sync_new_mail_hook(rows[0].senders, rows[0].subject, rows[0].latest, nrows);
    jd_thread_rows_free(rows, nrows);
}

void (*jd_sync_new_mail_hook)(const char *from, const char *subject, int64_t id, int count);
void (*jd_sync_scheduled_hook)(const char *subject, const char *err);

/* [[rule]]s in order; a message moved or deleted by one is out of reach of the next */
static void apply_rules(jd_sync *s, int64_t inbox, const int64_t *fresh, int nfresh)
{
    jd_prefs p;
    jd_prefs_load(&p);
    int64_t *left = jd_alloc(sizeof *left * (size_t)nfresh);
    memcpy(left, fresh, sizeof *left * (size_t)nfresh);
    int nleft = nfresh, applied = 0;
    jd_store_begin(s->store);
    for (int i = 0; i < p.nrules && nleft; i++) {
        jd_search search;
        jd_search_parse(p.rules[i].match, &search);
        search.q.folder = inbox;
        search.q.only = left;
        search.q.nonly = nleft;
        int n;
        jd_thread_row *rows = jd_store_list(s->store, &search.q, &n);
        jd_search_free(&search);
        if (!n) {
            jd_thread_rows_free(rows, n);
            continue;
        }
        int64_t *ids = jd_alloc(sizeof *ids * (size_t)n);
        for (int k = 0; k < n; k++) ids[k] = rows[k].latest;
        jd_thread_rows_free(rows, n);
        const char *action = p.rules[i].action;
        request rq = {.ids = ids, .n = n};
        int forget = 0;
        char *trash = NULL;
        if (jd_streq(action, "read")) rq.kind = OP_FLAG, rq.flag = JD_SEEN, rq.on = 1;
        else if (jd_streq(action, "flag")) rq.kind = OP_FLAG, rq.flag = JD_FLAGGED, rq.on = 1;
        else if (jd_streq(action, "delete") && jd_store_special(s->store, "Trash", &trash))
            rq.kind = OP_MOVE, rq.a = trash, forget = 1;
        else if (jd_streq(action, "delete")) rq.kind = OP_DESTROY, forget = 1;
        else if (!strncmp(action, "move:", 5) && action[5]) rq.kind = OP_MOVE, rq.a = jd_strdup(action + 5), forget = 1;
        else if (!strncmp(action, "copy:", 5) && action[5]) rq.kind = OP_COPY, rq.a = jd_strdup(action + 5);
        else {
            char *why = jd_printf("Rule %d: unknown action \"%s\".", i + 1, action);
            emit(s, JD_EV_ERROR, why, NULL, 0, 0);
            free(why), free(ids);
            continue;
        }
        push_per_folder(s, &rq, forget);
        applied = 1;
        if (forget) {
            int k = 0;
            for (int j = 0; j < nleft; j++) {
                int gone = 0;
                for (int m = 0; m < n; m++) gone |= left[j] == ids[m];
                if (!gone) left[k++] = left[j];
            }
            nleft = k;
        }
        free(ids), free(rq.a);
    }
    jd_store_commit(s->store);
    if (applied) emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);
    free(left);
    jd_prefs_free(&p);
}

static int sync_folder(jd_sync *s, int64_t folder, const char *name)
{
    jd_selected sel;
    jd_folder_state st;
    if (jd_imap_select(s->imap, name, &sel) != JD_IMAP_OK || jd_folder_state_get(s->store, folder, &st) != 0)
        return JD_IMAP_ERROR;
    fetch_ctx c = {.s = s, .folder = folder, .is_inbox = folder == s->inbox};

    /* RFC 9051 section 2.3.1.1: a new UIDVALIDITY means the old UIDs mean nothing */
    if (st.uidvalidity && st.uidvalidity != sel.uidvalidity) {
        jd_store_begin(s->store);
        jd_folder_clear(s->store, folder);
        jd_store_commit(s->store);
        remove_bodies(s, folder);
        st = (jd_folder_state){.id = folder};
    }
    int unchanged = st.uidvalidity == sel.uidvalidity && st.uidnext == sel.uidnext && (uint32_t)st.count == sel.exists;
    int r = JD_IMAP_OK;
    if (!unchanged) {
        status(s, "Syncing %s", name);
        uint32_t *server, *local;
        int ns, nl;
        if (jd_imap_search(s->imap, 0, &server, &ns) != JD_IMAP_OK) return JD_IMAP_ERROR;
        local = jd_folder_uids(s->store, folder, &nl);
        jd_store_begin(s->store);
        for (int i = 0; i < nl; i++) {
            if (contains(server, ns, local[i])) continue;
            jd_message_delete(s->store, folder, local[i]);
            char *path = jd_body_path(s->dir, folder, sel.uidvalidity, local[i]);
            unlink(path);
            free(path);
        }
        jd_threads_refresh(s->store, folder);
        jd_store_commit(s->store);
        int nnew = 0;
        uint32_t *fresh = jd_alloc(sizeof *fresh * (size_t)(ns + 1));
        for (int i = ns - 1; i >= 0; i--)
            if (!contains(local, nl, server[i])) fresh[nnew++] = server[i];
        for (int i = 0; i < nnew && r == JD_IMAP_OK && !quitting(s); i += HEADER_BATCH) {
            int k = nnew - i < HEADER_BATCH ? nnew - i : HEADER_BATCH;
            jd_store_begin(s->store);
            r = jd_imap_fetch_headers(s->imap, fresh + i, k, on_header, &c);
            jd_threads_refresh(s->store, folder);
            jd_store_commit(s->store);
            emit(s, JD_EV_CHANGED, NULL, NULL, folder, 0);
        }
        if (!nnew) emit(s, JD_EV_CHANGED, NULL, NULL, folder, 0);
        free(fresh), free(server), free(local);
    }
    if (r == JD_IMAP_OK && jd_folder_headers_stale(s->store, folder)) {
        status(s, "Refreshing %s", name);
        int nl;
        uint32_t *local = jd_folder_uids(s->store, folder, &nl);
        refresh_ctx rc = {s, folder, sel.uidvalidity};
        for (int i = 0; i < nl && r == JD_IMAP_OK && !quitting(s); i += HEADER_BATCH) {
            int k = nl - i < HEADER_BATCH ? nl - i : HEADER_BATCH;
            jd_store_begin(s->store);
            r = jd_imap_fetch_headers(s->imap, local + i, k, on_refresh, &rc);
            jd_store_commit(s->store);
            emit(s, JD_EV_CHANGED, NULL, NULL, folder, 0);
        }
        if (r == JD_IMAP_OK && !quitting(s)) {
            jd_store_begin(s->store);
            jd_folder_headers_done(s->store, folder);
            jd_store_commit(s->store);
        }
        free(local);
    }
    if (r == JD_IMAP_OK && !quitting(s) && jd_folder_texts_stale(s->store, folder)) {
        int nm;
        uint32_t *uids = jd_folder_cached(s->store, folder, &nm);
        jd_store_begin(s->store);
        for (int i = 0; i < nm; i++) {
            char *path = jd_body_path(s->dir, folder, sel.uidvalidity, uids[i]);
            size_t len;
            char *raw = jd_read_file(path, &len);
            int64_t id = raw ? jd_message_id(s->store, folder, uids[i]) : 0;
            if (id) {
                char *text = jd_mime_body_text(raw, len);
                jd_message_set_body(s->store, id, text);
                free(text);
            }
            free(raw), free(path);
        }
        jd_folder_texts_done(s->store, folder);
        jd_store_commit(s->store);
        if (nm) emit(s, JD_EV_CHANGED, NULL, NULL, folder, 0);
        free(uids);
    }
    int flags_unchanged = jd_imap_condstore(s->imap) && st.highestmodseq && sel.highestmodseq == st.highestmodseq;
    if (r == JD_IMAP_OK && !flags_unchanged && st.uidvalidity) {
        jd_store_begin(s->store);
        r = jd_imap_fetch_flags(s->imap, jd_imap_condstore(s->imap) ? st.highestmodseq : 0, on_flags, &c);
        jd_threads_refresh(s->store, folder);
        jd_store_commit(s->store);
        emit(s, JD_EV_CHANGED, NULL, NULL, folder, 0);
    }
    if (r == JD_IMAP_OK && !quitting(s)) {
        jd_store_begin(s->store);
        jd_folder_state_set(s->store, folder, sel.uidvalidity, sel.uidnext, sel.highestmodseq);
        jd_store_commit(s->store);
    }
    if (r == JD_IMAP_OK && c.is_inbox) {
        int n;
        uint32_t *want = s->prefetch_days
                             ? jd_folder_uncached(s->store, folder, jd_now() - (int64_t)s->prefetch_days * 86400,
                                                  PREFETCH_MAX_SIZE, &n)
                             : (n = 0, NULL);
        if (n) status(s, "Downloading recent mail in %s", name);
        r = fetch_bodies(s, folder, sel.uidvalidity, want, n);
        free(want);
    }
    /* rules run on mail that arrived since the folder was last synced, not on a new account's whole Inbox */
    if (c.nfresh && st.uidvalidity) apply_rules(s, folder, c.fresh, c.nfresh);
    if (c.nfresh && s->first_sync_done) notify_fresh(s, folder, c.fresh, c.nfresh);
    free(c.fresh);
    return r;
}

static int sync_all(jd_sync *s)
{
    status(s, "Listing folders", NULL);
    jd_mailbox *boxes;
    int n;
    if (jd_imap_list(s->imap, &boxes, &n) != JD_IMAP_OK) return JD_IMAP_ERROR;
    int64_t *ids = jd_alloc(sizeof *ids * (size_t)(n + 1));
    jd_store_begin(s->store);
    jd_folders_mark_absent(s->store);
    for (int i = 0; i < n; i++) {
        if (boxes[i].noselect) continue;
        ids[i] = jd_folder_upsert(s->store, boxes[i].name, boxes[i].delim, boxes[i].special_use);
        if (strcasecmp(boxes[i].name, "INBOX") == 0) s->inbox = ids[i];
    }
    jd_folders_sweep_absent(s->store);
    jd_store_commit(s->store);
    emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);

    int r = JD_IMAP_OK;
    for (int pass = 0; pass < 2 && r == JD_IMAP_OK; pass++)
        for (int i = 0; i < n && r == JD_IMAP_OK && !quitting(s); i++)
            if (ids[i] && (ids[i] == s->inbox) == (pass == 0)) r = sync_folder(s, ids[i], boxes[i].name);
    free(ids);
    jd_mailboxes_free(boxes, n);
    return r;
}

/* ---- requests: applied to the store at once, queued as ops for the server ---- */

static void op_free(op *o)
{
    free(o->folder_name), free(o->uids), free(o->a), free(o->b);
    for (int i = 0; i < o->nrcpts; i++) free(o->rcpts[i]);
    free(o->rcpts);
}

static void push_op(jd_sync *s, op o)
{
    s->ops = jd_realloc(s->ops, sizeof *s->ops * (size_t)(s->nops + 1));
    s->ops[s->nops++] = o;
}

/* one op per folder the messages are in; with forget, the messages leave the store (and their cached bodies) */
static void push_per_folder(jd_sync *s, const request *rq, int forget)
{
    int64_t *folders = jd_alloc(sizeof *folders * (size_t)rq->n);
    char **names = jd_alloc(sizeof *names * (size_t)rq->n);
    uint32_t *uids = jd_alloc(sizeof *uids * (size_t)rq->n);
    int64_t *ids = jd_alloc(sizeof *ids * (size_t)rq->n);
    int m = 0;
    for (int i = 0; i < rq->n; i++) {
        int64_t size;
        if (jd_message_locate(s->store, rq->ids[i], &folders[m], &names[m], &uids[m], &size) == 0) ids[m++] = rq->ids[i];
    }
    for (int i = 0; i < m; i++) {
        if (!names[i]) continue;
        op o = {.kind = rq->kind, .folder = folders[i], .folder_name = names[i], .flag = rq->flag, .on = rq->on,
                .a = jd_strdup(rq->a), .uids = jd_alloc(sizeof(uint32_t) * (size_t)m)};
        jd_folder_state st = {0};
        jd_folder_state_get(s->store, folders[i], &st);
        for (int j = i; j < m; j++) {
            if (!names[j] || folders[j] != folders[i]) continue;
            o.uids[o.n++] = uids[j];
            if (rq->kind == OP_FLAG) {
                jd_store_set_flag(s->store, ids[j], rq->flag, rq->on);
            } else if (forget) {
                jd_message_delete_id(s->store, ids[j]);
                char *path = jd_body_path(s->dir, folders[j], st.uidvalidity, uids[j]);
                unlink(path);
                free(path);
            }
            if (j != i) free(names[j]), names[j] = NULL;
        }
        jd_threads_refresh(s->store, folders[i]);
        names[i] = NULL;
        push_op(s, o);
    }
    free(folders), free(names), free(uids), free(ids);
}

static void apply_requests(jd_sync *s)
{
    pthread_mutex_lock(&s->lock);
    request *rqs = s->requests;
    int n = s->nrequests;
    s->requests = NULL;
    s->nrequests = 0;
    pthread_mutex_unlock(&s->lock);
    if (!n) return;
    jd_store_begin(s->store);
    for (int i = 0; i < n; i++) {
        request *rq = &rqs[i];
        switch (rq->kind) {
        case OP_FLAG:
        case OP_COPY:
        case OP_BODY: push_per_folder(s, rq, 0); break;
        case OP_MOVE:
        case OP_DESTROY: push_per_folder(s, rq, 1); break;
        case OP_SEARCH:
        case OP_CREATE:
        case OP_RENAME:
        case OP_DELETE_FOLDER:
        case OP_SEND:
        case OP_DRAFT:
        case OP_SIEVE_GET:
        case OP_SIEVE_PUT:
            push_op(s, (op){.kind = rq->kind, .a = rq->a, .b = rq->b, .serial = rq->serial, .len = rq->len,
                            .rcpts = rq->rcpts, .nrcpts = rq->nrcpts, .replied = rq->replied});
            rq->a = rq->b = NULL;
            rq->rcpts = NULL;
            break;
        }
        free(rq->ids), free(rq->a), free(rq->b);
    }
    jd_store_commit(s->store);
    free(rqs);
    emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);
}

static int search_folder(jd_sync *s, int64_t folder, const char *name, const char *words, jd_event *ev)
{
    jd_selected sel;
    uint32_t *uids;
    int n;
    if (jd_imap_select(s->imap, name, &sel) != JD_IMAP_OK || jd_imap_search_text(s->imap, words, &uids, &n) != JD_IMAP_OK)
        return JD_IMAP_ERROR;
    ev->ids = jd_realloc(ev->ids, sizeof *ev->ids * (size_t)(ev->nids + n + 1));
    for (int i = 0; i < n; i++) {
        int64_t id = jd_message_id(s->store, folder, uids[i]);
        if (id) ev->ids[ev->nids++] = id;
    }
    free(uids);
    return JD_IMAP_OK;
}

static int run_search(jd_sync *s, op *o)
{
    status(s, "Searching the server", NULL);
    jd_event ev = {.kind = JD_EV_SEARCH, .id = o->serial};
    int nf, r = JD_IMAP_OK;
    jd_folder_row *rows = jd_store_folders(s->store, &nf);
    for (int i = 0; i < nf && r == JD_IMAP_OK; i++)
        if (!o->b || jd_streq(rows[i].name, o->b) || jd_store_folder_by_name(s->store, o->b) == rows[i].id)
            r = search_folder(s, rows[i].id, rows[i].name, o->a, &ev);
    jd_folder_rows_free(rows, nf);
    if (r == JD_IMAP_OK) emit_ev(s, ev);
    else free(ev.ids);
    return r;
}

/* expunges the messages with this Message-ID from the Drafts folder */
static int drop_drafts(jd_sync *s, const char *msgid)
{
    char *name = NULL;
    int64_t drafts = jd_store_special(s->store, "Drafts", &name);
    if (!drafts || !msgid) {
        free(name);
        return JD_IMAP_OK;
    }
    int r = sync_folder(s, drafts, name), n;
    int64_t *ids = jd_store_by_msgid(s->store, drafts, msgid, &n);
    uint32_t *uids = jd_alloc(sizeof *uids * (size_t)(n + 1));
    int k = 0;
    for (int i = 0; i < n; i++) {
        int64_t folder, size;
        char *fname;
        if (jd_message_locate(s->store, ids[i], &folder, &fname, &uids[k], &size) == 0) k++, free(fname);
    }
    jd_selected sel;
    if (r == JD_IMAP_OK && k && jd_imap_select(s->imap, name, &sel) == JD_IMAP_OK) r = jd_imap_expunge(s->imap, uids, k);
    if (r == JD_IMAP_OK) r = sync_folder(s, drafts, name);
    free(ids), free(uids), free(name);
    return r;
}

/* the SMTP part fails on its own (reported, never retried: the server may have taken it); the IMAP
   bookkeeping after it is best effort */
static int run_send(jd_sync *s, op *o)
{
    status(s, "Sending", NULL);
    char err[512];
    if (jd_smtp_send(&s->account, s->password, o->rcpts, o->nrcpts, o->a, o->len, err, sizeof err) != 0) {
        emit(s, JD_EV_SENT, err, NULL, o->serial, 0);
        return JD_IMAP_OK;
    }
    emit(s, JD_EV_SENT, NULL, NULL, o->serial, 0);
    char *sent = NULL;
    int64_t sent_id = jd_store_special(s->store, "Sent", &sent);
    /* Gmail files submitted mail in Sent itself */
    int r = JD_IMAP_OK;
    if (sent_id && !strcasestr(s->account.smtp.host, "gmail.com"))
        r = jd_imap_append(s->imap, sent, o->a, o->len, JD_SEEN);
    if (r == JD_IMAP_OK && sent_id) r = sync_folder(s, sent_id, sent);
    free(sent);
    if (r == JD_IMAP_OK && o->replied) {
        int64_t folder, size;
        char *name;
        uint32_t uid;
        jd_selected sel;
        if (jd_message_locate(s->store, o->replied, &folder, &name, &uid, &size) == 0) {
            jd_store_begin(s->store);
            jd_store_set_flag(s->store, o->replied, JD_ANSWERED, 1);
            jd_store_commit(s->store);
            if (jd_imap_select(s->imap, name, &sel) == JD_IMAP_OK) jd_imap_set_flag(s->imap, &uid, 1, JD_ANSWERED, 1);
            free(name);
        }
    }
    if (r == JD_IMAP_OK) r = drop_drafts(s, o->b);
    emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);
    return JD_IMAP_OK;
}

static int run_draft(jd_sync *s, op *o)
{
    char *drafts = NULL;
    int64_t id = jd_store_special(s->store, "Drafts", &drafts);
    if (!id) {
        emit(s, JD_EV_SAVED, "The server has no Drafts folder.", NULL, o->serial, 0);
        return JD_IMAP_OK;
    }
    int r = drop_drafts(s, o->b);
    if (r == JD_IMAP_OK) r = jd_imap_append(s->imap, drafts, o->a, o->len, JD_SEEN | JD_DRAFT);
    if (r == JD_IMAP_OK) r = sync_folder(s, id, drafts);
    free(drafts);
    emit(s, JD_EV_SAVED, r == JD_IMAP_OK ? NULL : "The server did not take the draft.", NULL, o->serial, 0);
    emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);
    return r;
}

static void on_raw(void *data, uint32_t uid, const char *raw, size_t len)
{
    jd_buf_add(data, raw, len);
}

/* A claimed draft, from its copy in the outbox: path ends in .eml (claimed) or .sent (SMTP took it). SMTP
   refusing it puts it back in Drafts unscheduled, as no other client would do better; a failure that may pass
   leaves it for OUTBOX_RETRY. The copy goes once Drafts and Sent are done with, so a crash in between repeats
   that bookkeeping and never the sending. */
static int deliver(jd_sync *s, int64_t drafts, const char *drafts_name, uint32_t uidvalidity, uint32_t uid,
                   const char *path, const char *raw, size_t len)
{
    status(s, "Sending scheduled mail", NULL);
    jd_draft_headers h;
    jd_mime_draft_headers(raw, len, &h);
    char err[512] = "";
    size_t mlen = 0;
    char **rcpts = NULL;
    int nrcpts = 0;
    char *msg = jd_draft_release(raw, len, 1, &mlen, &rcpts, &nrcpts, err, sizeof err);
    size_t plen = strlen(path);
    int was_sent = plen > 5 && strcmp(path + plen - 5, ".sent") == 0;
    int rc = was_sent ? 0 : msg ? jd_smtp_send(&s->account, s->password, rcpts, nrcpts, msg, mlen, err, sizeof err) : -1;
    int r = JD_IMAP_OK;
    if (rc == -2) {
        char *why = jd_printf("Scheduled mail not sent yet (%s). Trying again in a few minutes.", err);
        emit(s, JD_EV_ERROR, why, NULL, 0, 0);
        free(why);
        s->outbox_at = jd_now() + OUTBOX_RETRY;
        goto done;
    }
    char *sent_path = jd_printf("%.*s.sent", (int)(plen - (was_sent ? 5 : 4)), path);
    if (rc == 0 && !was_sent) rename(path, sent_path);
    if (rc != 0) {
        size_t dlen;
        char *draft = jd_draft_release(raw, len, 0, &dlen, NULL, NULL, NULL, 0);
        r = jd_imap_append(s->imap, drafts_name, draft, dlen, JD_SEEN | JD_DRAFT);
        free(draft);
    }
    jd_selected sel;
    if (r == JD_IMAP_OK) r = jd_imap_select(s->imap, drafts_name, &sel);
    if (r == JD_IMAP_OK && sel.uidvalidity == uidvalidity) jd_imap_expunge(s->imap, &uid, 1);
    char *sent_name = NULL;
    int64_t sent_id = rc == 0 ? jd_store_special(s->store, "Sent", &sent_name) : 0;
    if (r == JD_IMAP_OK && sent_id && msg && !strcasestr(s->account.smtp.host, "gmail.com"))
        r = jd_imap_append(s->imap, sent_name, msg, mlen, JD_SEEN);
    if (r == JD_IMAP_OK) unlink(rc == 0 ? sent_path : path);
    if (r == JD_IMAP_OK) r = sync_folder(s, drafts, drafts_name);
    if (r == JD_IMAP_OK && sent_id) r = sync_folder(s, sent_id, sent_name);
    emit(s, JD_EV_SCHEDULED, rc == 0 ? NULL : err, h.subject, 0, 0);
    if (jd_sync_scheduled_hook) jd_sync_scheduled_hook(h.subject, rc == 0 ? NULL : err);
    emit(s, JD_EV_CHANGED, NULL, NULL, 0, 0);
    free(sent_path), free(sent_name);
    /* after the reconnect, not at the next retry */
    if (r != JD_IMAP_OK) s->outbox_at = jd_now();
done:
    jd_draft_headers_free(&h);
    jd_rcpts_free(rcpts, nrcpts);
    free(msg);
    return r;
}

/* Fetched, kept in the outbox, then claimed, so one client sends it and a crash at any point leaves a copy that
   says how far it got. The claim's keyword is what tells this client its own claim after a crash (resume_outbox). */
static int send_scheduled(jd_sync *s, int64_t drafts, const char *drafts_name, uint32_t uidvalidity,
                          const jd_scheduled *d)
{
    jd_buf raw = {0};
    int r = jd_imap_fetch_bodies(s->imap, &d->uid, 1, on_raw, &raw);
    if (r != JD_IMAP_OK || !raw.len) {
        jd_buf_free(&raw);
        return r;
    }
    unsigned char bytes[8];
    char token[17];
    RAND_bytes(bytes, sizeof bytes);
    for (int i = 0; i < 8; i++) snprintf(token + 2 * i, 3, "%02x", bytes[i]);
    char *dir = jd_printf("%s/outbox", s->dir);
    char *path = jd_printf("%s/%u-%u-%s.claim", dir, uidvalidity, d->uid, token);
    char *eml = jd_printf("%s/%u-%u-%s.eml", dir, uidvalidity, d->uid, token);
    char *keyword = jd_printf("$JdClaim%s", token);
    int claimed = 0;
    if (jd_mkdirs(dir) == 0 && jd_write_file(path, raw.s, raw.len, 0600) == 0)
        r = jd_imap_claim(s->imap, d->uid, jd_imap_condstore(s->imap) ? d->modseq : 0, keyword, &claimed);
    else emit(s, JD_EV_ERROR, "Scheduled mail: cannot write to the outbox.", NULL, 0, 0);
    if (r != JD_IMAP_OK) s->outbox_at = jd_now();
    if (r == JD_IMAP_OK && !claimed) unlink(path);
    if (r == JD_IMAP_OK && claimed && rename(path, eml) == 0)
        r = deliver(s, drafts, drafts_name, uidvalidity, d->uid, eml, raw.s, raw.len);
    free(dir), free(path), free(eml), free(keyword);
    jd_buf_free(&raw);
    return r;
}

/* Drafts selected. A .claim copy whose claim is not known to have held is this client's only when the draft
   carries its keyword; otherwise another client has it, or it is gone. */
static int resume_outbox(jd_sync *s, int64_t drafts, const char *drafts_name, uint32_t uidvalidity)
{
    char *dir = jd_printf("%s/outbox", s->dir);
    DIR *d = opendir(dir);
    char **names = NULL;
    int n = 0, r = JD_IMAP_OK;
    for (struct dirent *e; d && (e = readdir(d));) {
        names = jd_realloc(names, sizeof *names * (size_t)(n + 1));
        names[n++] = jd_strdup(e->d_name);
    }
    if (d) closedir(d);
    s->outbox_at = 0;
    for (int i = 0; i < n; i++) {
        unsigned uv, uid;
        char token[17], ext[8];
        if (r != JD_IMAP_OK || sscanf(names[i], "%u-%u-%16[0-9a-f].%7s", &uv, &uid, token, ext) != 4) continue;
        char *path = jd_printf("%s/%s", dir, names[i]);
        int ours = strcmp(ext, "eml") == 0 || strcmp(ext, "sent") == 0;
        if (strcmp(ext, "claim") == 0 && uv == uidvalidity) {
            char *kw = jd_store_keywords(s->store, drafts, uid), *mine = jd_printf("$JdClaim%s", token);
            ours = kw && strcasestr(kw, mine);
            free(kw), free(mine);
            char *eml = jd_printf("%s/%u-%u-%s.eml", dir, uv, uid, token);
            if (ours && rename(path, eml) == 0) free(path), path = eml;
            else free(eml);
        }
        size_t len;
        char *raw = ours ? jd_read_file(path, &len) : NULL;
        if (raw) r = deliver(s, drafts, drafts_name, uv, uid, path, raw, len);
        else if (strcmp(ext, "claim") == 0) unlink(path);
        free(raw), free(path);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names), free(dir);
    return r;
}

static int held(jd_sync *s, int64_t drafts, int64_t id)
{
    int found = 0;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->nheld && !found; i++) {
        int n;
        int64_t *ids = jd_store_by_msgid(s->store, drafts, s->held[i], &n);
        for (int k = 0; k < n; k++) found |= ids[k] == id;
        free(ids);
    }
    pthread_mutex_unlock(&s->lock);
    return found;
}

/* the soonest scheduled draft's time (not one open in compose) or outbox retry, 0 when none */
static int64_t next_scheduled(jd_sync *s)
{
    int64_t drafts = jd_store_special(s->store, "Drafts", NULL), at = s->outbox_at;
    int n = 0;
    jd_scheduled *due = drafts ? jd_store_scheduled(s->store, drafts, &n) : NULL;
    for (int i = 0; i < n; i++)
        if (!held(s, drafts, due[i].id)) {
            if (!at || due[i].send_at < at) at = due[i].send_at;
            break;
        }
    free(due);
    return at;
}

/* Drafts synced first: another client may have edited, unscheduled or sent one since */
static int send_due(jd_sync *s)
{
    char *name = NULL;
    int64_t drafts = jd_store_special(s->store, "Drafts", &name);
    jd_selected sel;
    int r = drafts ? sync_folder(s, drafts, name) : JD_IMAP_OK, n = 0;
    if (r == JD_IMAP_OK && drafts) r = jd_imap_select(s->imap, name, &sel);
    if (r == JD_IMAP_OK && drafts) r = resume_outbox(s, drafts, name, sel.uidvalidity);
    jd_scheduled *due = r == JD_IMAP_OK && drafts ? jd_store_scheduled(s->store, drafts, &n) : NULL;
    int64_t now = jd_now();
    for (int i = 0; i < n && r == JD_IMAP_OK && due[i].send_at <= now && !quitting(s); i++) {
        if (held(s, drafts, due[i].id)) continue;
        r = jd_imap_select(s->imap, name, &sel);
        if (r == JD_IMAP_OK) r = send_scheduled(s, drafts, name, sel.uidvalidity, &due[i]);
    }
    free(due), free(name);
    return r;
}

static int run_op(jd_sync *s, op *o)
{
    jd_selected sel;
    int needs_folder = o->kind == OP_FLAG || o->kind == OP_MOVE || o->kind == OP_COPY || o->kind == OP_DESTROY ||
                       o->kind == OP_BODY;
    if (needs_folder && jd_imap_select(s->imap, o->folder_name, &sel) != JD_IMAP_OK) return JD_IMAP_ERROR;
    switch (o->kind) {
    case OP_FLAG: return jd_imap_set_flag(s->imap, o->uids, o->n, o->flag, o->on);
    case OP_COPY:
    case OP_MOVE: {
        int r = o->kind == OP_MOVE ? jd_imap_move(s->imap, o->uids, o->n, o->a) : jd_imap_copy(s->imap, o->uids, o->n, o->a);
        int64_t to = jd_store_folder_by_name(s->store, o->a);
        if (r == JD_IMAP_OK && to) r = sync_folder(s, to, o->a);
        return r;
    }
    case OP_DESTROY: return jd_imap_expunge(s->imap, o->uids, o->n);
    case OP_BODY: return fetch_bodies(s, o->folder, sel.uidvalidity, o->uids, o->n);
    case OP_SEARCH: return run_search(s, o);
    case OP_CREATE: s->want_full = 1; return jd_imap_create(s->imap, o->a);
    case OP_RENAME: s->want_full = 1; return jd_imap_rename(s->imap, o->a, o->b);
    case OP_DELETE_FOLDER: s->want_full = 1; return jd_imap_delete(s->imap, o->a);
    case OP_SEND: return run_send(s, o);
    case OP_DRAFT: return run_draft(s, o);
    case OP_SIEVE_GET:
    case OP_SIEVE_PUT: {
        /* its own connection, answered either way: a missing ManageSieve is not a reason to reconnect IMAP */
        char err[512], *name = NULL, *script = NULL;
        status(s, o->kind == OP_SIEVE_GET ? "Reading the server's filters" : "Saving the server's filters", NULL);
        if (o->kind == OP_SIEVE_GET) {
            int r = jd_sieve_get(&s->account, s->password, &name, &script, err, sizeof err);
            if (r == 0) emit(s, JD_EV_SIEVE, script, name, 0, 0);
            else emit(s, JD_EV_SIEVE, err, NULL, 0, -1);
        } else {
            int r = jd_sieve_put(&s->account, s->password, o->b, o->a, err, sizeof err);
            emit(s, JD_EV_SIEVE_SAVED, r == 0 ? NULL : err, NULL, 0, 0);
        }
        free(name), free(script);
        return JD_IMAP_OK;
    }
    }
    return JD_IMAP_ERROR;
}

static const char *op_name(op_kind k)
{
    static const char *names[] = {"changing flags", "moving mail", "copying mail", "deleting mail",
                                  "downloading a message", "searching the server", "creating the folder",
                                  "renaming the folder", "deleting the folder", "sending", "saving the draft",
                                  "reading filters", "saving filters"};
    return names[k];
}

/* Ops run in order. A refused op is retried after a reconnect and dropped after OP_ATTEMPTS, with a full sync to
   bring back the server's state over the local guess. */
static int run_ops(jd_sync *s, int draining)
{
    while (s->nops && (draining || !quitting(s))) {
        op *o = &s->ops[0];
        if (run_op(s, o) != JD_IMAP_OK) {
            if (++o->attempts < OP_ATTEMPTS) return JD_IMAP_ERROR;
            char *why = jd_printf("The server refused %s.", op_name(o->kind));
            emit(s, JD_EV_ERROR, why, NULL, 0, 0);
            free(why);
            s->want_full = 1;
        }
        op_free(o);
        memmove(s->ops, s->ops + 1, sizeof *s->ops * (size_t)--s->nops);
    }
    return JD_IMAP_OK;
}

/* ---- the loop ---- */

enum { WOKE_TIMEOUT, WOKE_JOB, WOKE_SERVER };

static int wait_for(jd_sync *s, int seconds, int idle_fd)
{
    struct pollfd p[2] = {{s->wake[0], POLLIN, 0}, {idle_fd, POLLIN, 0}};
    int r;
    do r = poll(p, idle_fd >= 0 ? 2 : 1, seconds * 1000);
    while (r < 0 && errno == EINTR);
    if (r <= 0) return WOKE_TIMEOUT;
    if (p[0].revents) {
        drain(s);
        return WOKE_JOB;
    }
    return WOKE_SERVER;
}

static void drop_connection(jd_sync *s)
{
    jd_imap_close(s->imap);
    s->imap = NULL;
}

static void *worker(void *arg)
{
    jd_sync *s = arg;
    char err[512];
    s->store = jd_store_open(s->db_path, err, sizeof err);
    if (!s->store) {
        emit(s, JD_EV_ERROR, err, NULL, 0, 0);
        return NULL;
    }
    int backoff = 0;
    int64_t next_full = 0, next_inbox = 0;
    while (!quitting(s)) {
        jd_prefs p;
        jd_prefs_load(&p);
        s->prefetch_days = p.prefetch_days;
        s->check_minutes = p.check_minutes;
        jd_prefs_free(&p);
        apply_requests(s);
        if (!s->imap) {
            status(s, "Connecting to %s", s->account.imap.host);
            int st;
            s->imap = jd_imap_connect(&s->account, s->password, &st, err, sizeof err);
            if (!s->imap) {
                emit(s, st == JD_IMAP_AUTH ? JD_EV_AUTH : JD_EV_ERROR, err, NULL, 0, 0);
                status(s, NULL, NULL);
                if (st == JD_IMAP_AUTH) break;
                backoff = backoff ? (backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : backoff * 2) : BACKOFF_MIN;
                wait_for(s, backoff, -1);
                continue;
            }
            backoff = 0;
            next_full = 0;
        }
        int r = run_ops(s, 0);
        pthread_mutex_lock(&s->lock);
        if (s->sync_now || s->want_full) next_full = 0;
        s->sync_now = s->want_full = 0;
        pthread_mutex_unlock(&s->lock);

        int64_t now = jd_now();
        if (r == JD_IMAP_OK && now >= next_full) {
            r = sync_all(s);
            if (r == JD_IMAP_OK) {
                s->first_sync_done = 1;
                next_full = now + s->check_minutes * 60;
                next_inbox = now + INBOX_POLL_EVERY;
            }
        } else if (r == JD_IMAP_OK && now >= next_inbox && s->inbox) {
            r = sync_folder(s, s->inbox, "INBOX");
            next_inbox = now + INBOX_POLL_EVERY;
        }
        int64_t send_at = next_scheduled(s);
        if (r == JD_IMAP_OK && send_at && send_at <= jd_now()) {
            r = send_due(s);
            send_at = next_scheduled(s);
        }
        if (r != JD_IMAP_OK) {
            emit(s, JD_EV_ERROR, "Connection lost, reconnecting.", NULL, 0, 0);
            drop_connection(s);
            continue;
        }
        status(s, NULL, NULL);

        pthread_mutex_lock(&s->lock);
        int pending = s->nrequests > 0;
        pthread_mutex_unlock(&s->lock);
        if (pending) continue;
        now = jd_now();
        int64_t wake_at = next_full < next_inbox ? next_full : next_inbox;
        if (send_at && send_at < wake_at) wake_at = send_at;
        int until = (int)(wake_at - now);
        if (until < 1) until = 1;
        jd_selected sel;
        jd_folder_state st;
        int selected = s->inbox && jd_imap_can_idle(s->imap) && jd_imap_select(s->imap, "INBOX", &sel) == JD_IMAP_OK;
        /* IDLE reports only what happens after it starts: mail that came in since the last look is synced first */
        if (selected && jd_folder_state_get(s->store, s->inbox, &st) == 0 &&
            (st.uidnext != sel.uidnext || (uint32_t)st.count != sel.exists)) {
            next_inbox = 0;
            continue;
        }
        if (selected && jd_imap_idle_start(s->imap) == JD_IMAP_OK) {
            int woke = wait_for(s, until < IDLE_RESTART ? until : IDLE_RESTART, jd_imap_idle_fd(s->imap));
            if (jd_imap_idle_done(s->imap) != JD_IMAP_OK) {
                drop_connection(s);
                continue;
            }
            if (woke == WOKE_SERVER) next_inbox = 0;
        } else {
            wait_for(s, until, -1);
        }
    }
    /* changes made just before quitting (flags, a draft saved on close) still reach the server */
    apply_requests(s);
    if (s->imap && s->nops) {
        status(s, "Saving changes", NULL);
        run_ops(s, 1);
    }
    drop_connection(s);
    jd_store_close(s->store);
    s->store = NULL;
    return NULL;
}

jd_sync *jd_sync_start(const jd_account *a, const char *password, const char *account_dir, const char *db_path)
{
    jd_sync *s = jd_alloc(sizeof *s);
    s->account = (jd_account){jd_strdup(a->name), jd_strdup(a->address), jd_strdup(a->username),
                              {jd_strdup(a->imap.host), a->imap.port, a->imap.security},
                              {jd_strdup(a->smtp.host), a->smtp.port, a->smtp.security}, a->sieve_port};
    s->password = jd_strdup(password);
    s->dir = jd_strdup(account_dir);
    s->db_path = jd_strdup(db_path);
    s->send_later_uuid = jd_send_later_uuid(a->address);
    /* what a crash or a lost connection left in the outbox goes after the first sync */
    s->outbox_at = 1;
    pthread_mutex_init(&s->lock, NULL);
    if (pipe2(s->wake, O_NONBLOCK | O_CLOEXEC) != 0 || pthread_create(&s->thread, NULL, worker, s) != 0) abort();
    return s;
}

void jd_sync_stop(jd_sync *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->quit = 1;
    pthread_mutex_unlock(&s->lock);
    poke(s);
    pthread_join(s->thread, NULL);
    close(s->wake[0]), close(s->wake[1]);
    for (int i = 0; i < s->nevents; i++) jd_event_free(&s->events[i]);
    for (int i = 0; i < s->nrequests; i++) {
        request *rq = &s->requests[i];
        for (int k = 0; k < rq->nrcpts; k++) free(rq->rcpts[k]);
        free(rq->ids), free(rq->a), free(rq->b), free(rq->rcpts);
    }
    for (int i = 0; i < s->nops; i++) op_free(&s->ops[i]);
    free(s->events), free(s->requests), free(s->ops);
    for (int i = 0; i < s->nheld; i++) free(s->held[i]);
    free(s->held);
    if (s->password) jd_wipe(s->password, strlen(s->password));
    free(s->password), free(s->dir), free(s->db_path), free(s->send_later_uuid);
    jd_account_free(&s->account);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void jd_sync_now(jd_sync *s)
{
    pthread_mutex_lock(&s->lock);
    s->sync_now = 1;
    pthread_mutex_unlock(&s->lock);
    poke(s);
}

void jd_sync_hold(jd_sync *s, const char *msgid, int on)
{
    if (!s || !msgid) return;
    pthread_mutex_lock(&s->lock);
    if (on) {
        s->held = jd_realloc(s->held, sizeof *s->held * (size_t)(s->nheld + 1));
        s->held[s->nheld++] = jd_strdup(msgid);
    } else {
        for (int i = 0; i < s->nheld; i++)
            if (strcmp(s->held[i], msgid) == 0) {
                free(s->held[i]);
                s->held[i] = s->held[--s->nheld];
                break;
            }
    }
    pthread_mutex_unlock(&s->lock);
    /* a draft that came due while open goes now */
    if (!on) poke(s);
}

static void request_add(jd_sync *s, request rq, const int64_t *ids, int n)
{
    if (n) {
        rq.ids = jd_alloc(sizeof *rq.ids * (size_t)n);
        memcpy(rq.ids, ids, sizeof *ids * (size_t)n);
        rq.n = n;
    }
    pthread_mutex_lock(&s->lock);
    s->requests = jd_realloc(s->requests, sizeof *s->requests * (size_t)(s->nrequests + 1));
    s->requests[s->nrequests++] = rq;
    pthread_mutex_unlock(&s->lock);
    poke(s);
}

void jd_sync_fetch_body(jd_sync *s, int64_t id)
{
    request_add(s, (request){.kind = OP_BODY}, &id, 1);
}

void jd_sync_set_flag(jd_sync *s, const int64_t *ids, int n, int flag, int on)
{
    if (n > 0) request_add(s, (request){.kind = OP_FLAG, .flag = flag, .on = on}, ids, n);
}

void jd_sync_move(jd_sync *s, const int64_t *ids, int n, const char *to, int copy)
{
    if (n > 0) request_add(s, (request){.kind = copy ? OP_COPY : OP_MOVE, .a = jd_strdup(to)}, ids, n);
}

void jd_sync_destroy(jd_sync *s, const int64_t *ids, int n)
{
    if (n > 0) request_add(s, (request){.kind = OP_DESTROY}, ids, n);
}

void jd_sync_search(jd_sync *s, const char *words, const char *folder_name, int64_t serial)
{
    request_add(s, (request){.kind = OP_SEARCH, .a = jd_strdup(words), .b = jd_strdup(folder_name), .serial = serial},
                NULL, 0);
}

void jd_sync_send(jd_sync *s, const char *msg, size_t len, char **rcpts, int nrcpts, const char *msgid,
                  int64_t replied_to, int64_t serial)
{
    request rq = {.kind = OP_SEND, .a = jd_strndup(msg, len), .len = len, .b = jd_strdup(msgid), .serial = serial,
                  .replied = replied_to, .nrcpts = nrcpts, .rcpts = jd_alloc(sizeof(char *) * (size_t)(nrcpts + 1))};
    for (int i = 0; i < nrcpts; i++) rq.rcpts[i] = jd_strdup(rcpts[i]);
    request_add(s, rq, NULL, 0);
}

void jd_sync_save_draft(jd_sync *s, const char *msg, size_t len, const char *msgid, int64_t serial)
{
    request_add(s, (request){.kind = OP_DRAFT, .a = jd_strndup(msg, len), .len = len, .b = jd_strdup(msgid), .serial = serial},
                NULL, 0);
}

void jd_sync_sieve_get(jd_sync *s)
{
    request_add(s, (request){.kind = OP_SIEVE_GET}, NULL, 0);
}

void jd_sync_sieve_put(jd_sync *s, const char *name, const char *script)
{
    request_add(s, (request){.kind = OP_SIEVE_PUT, .a = jd_strdup(script), .b = jd_strdup(name)}, NULL, 0);
}

void jd_sync_create_folder(jd_sync *s, const char *name)
{
    request_add(s, (request){.kind = OP_CREATE, .a = jd_strdup(name)}, NULL, 0);
}

void jd_sync_rename_folder(jd_sync *s, const char *from, const char *to)
{
    request_add(s, (request){.kind = OP_RENAME, .a = jd_strdup(from), .b = jd_strdup(to)}, NULL, 0);
}

void jd_sync_delete_folder(jd_sync *s, const char *name)
{
    request_add(s, (request){.kind = OP_DELETE_FOLDER, .a = jd_strdup(name)}, NULL, 0);
}

int jd_sync_poll(jd_sync *s, jd_event *ev)
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

void jd_event_free(jd_event *ev)
{
    free(ev->text), free(ev->text2), free(ev->ids);
    *ev = (jd_event){0};
}
