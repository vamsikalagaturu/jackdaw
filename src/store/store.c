#include "store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"
#include "sqlite3.h"

struct jd_store {
    sqlite3 *db;
};

static const char *schema_v1 =
    "CREATE TABLE folders(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, delim TEXT, special_use TEXT,"
    " uidvalidity INTEGER NOT NULL DEFAULT 0, uidnext INTEGER NOT NULL DEFAULT 0,"
    " highestmodseq INTEGER NOT NULL DEFAULT 0, present INTEGER NOT NULL DEFAULT 1);"
    "CREATE TABLE threads(id INTEGER PRIMARY KEY, root_msgid TEXT, subject_norm TEXT,"
    " last_date INTEGER NOT NULL DEFAULT 0, unread INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE messages(id INTEGER PRIMARY KEY,"
    " folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE, uid INTEGER NOT NULL,"
    " modseq INTEGER NOT NULL DEFAULT 0, flags INTEGER NOT NULL DEFAULT 0, keywords TEXT,"
    " internaldate INTEGER, date INTEGER, size INTEGER, msgid TEXT, in_reply_to TEXT, subject TEXT,"
    " thread_id INTEGER REFERENCES threads(id), has_attachment INTEGER NOT NULL DEFAULT 0,"
    " body_cached INTEGER NOT NULL DEFAULT 0, UNIQUE(folder_id, uid));"
    "CREATE INDEX messages_msgid ON messages(msgid);"
    "CREATE INDEX messages_thread ON messages(thread_id);"
    "CREATE TABLE addresses(message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
    " role INTEGER NOT NULL, position INTEGER NOT NULL, name TEXT, addr TEXT);"
    "CREATE INDEX addresses_message ON addresses(message_id);"
    "CREATE TABLE refs(message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
    " position INTEGER NOT NULL, ref_msgid TEXT NOT NULL);"
    "CREATE INDEX refs_msgid ON refs(ref_msgid);"
    "CREATE INDEX refs_message ON refs(message_id);"
    "CREATE VIRTUAL TABLE search USING fts5(subject, sender, recipients, body,"
    " tokenize='unicode61 remove_diacritics 2');"
    "CREATE TRIGGER messages_unsearch AFTER DELETE ON messages BEGIN"
    " DELETE FROM search WHERE rowid = old.id; END;"
    "PRAGMA user_version = 1;";

static sqlite3_stmt *prep(jd_store *s, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK)
        fprintf(stderr, "jackdaw: sql: %s: %s\n", sqlite3_errmsg(s->db), sql);
    return st;
}

static void bind_text(sqlite3_stmt *st, int i, const char *v)
{
    if (v) sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, i);
}

static int64_t run(sqlite3_stmt *st)
{
    int64_t v = 0;
    if (st && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void exec(jd_store *s, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "jackdaw: sql: %s\n", err);
        sqlite3_free(err);
    }
}

/* the opening of a message for the list: quoted lines, "... wrote:" lines and the signature left out,
   whitespace folded, at most about 200 bytes cut on a character boundary */
static char *make_snippet(const char *text)
{
    jd_buf b = {0};
    const char *p = text ? text : "";
    while (*p && b.len < 200) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= 3 && strncmp(p, "-- ", 3) == 0 && n == 3) break;
        int skip = p[0] == '>' || (n > 6 && strncmp(p + n - 6, "wrote:", 6) == 0) ||
                   (n > 7 && strncmp(p + n - 7, "schrieb:", 7) == 0);
        if (!skip) {
            for (size_t i = 0; i < n; i++) {
                char c = p[i] == '\t' || p[i] == '\r' ? ' ' : p[i];
                if (c == ' ' && (!b.len || b.s[b.len - 1] == ' ')) continue;
                jd_buf_add(&b, &c, 1);
            }
            if (b.len && b.s[b.len - 1] != ' ') jd_buf_add(&b, " ", 1);
        }
        p += n + (nl ? 1 : 0);
    }
    if (b.len > 200) {
        size_t cut = 200;
        while (cut > 0 && ((unsigned char)b.s[cut] & 0xc0) == 0x80) cut--;
        b.s[cut] = 0;
        b.len = cut;
    }
    while (b.len && b.s[b.len - 1] == ' ') b.s[--b.len] = 0;
    return b.s ? b.s : jd_strdup("");
}

jd_store *jd_store_open(const char *path, char *err, int errlen)
{
    jd_store *s = jd_alloc(sizeof *s);
    if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK)
        goto bad;
    sqlite3_busy_timeout(s->db, 5000);
    exec(s, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;");
    if (run(prep(s, "PRAGMA user_version")) == 0) {
        char *msg = NULL;
        exec(s, "BEGIN IMMEDIATE");
        if (run(prep(s, "PRAGMA user_version")) == 0 && sqlite3_exec(s->db, schema_v1, NULL, NULL, &msg) != SQLITE_OK) {
            snprintf(err, (size_t)errlen, "%s: %s", path, msg);
            sqlite3_free(msg);
            exec(s, "ROLLBACK");
            jd_store_close(s);
            return NULL;
        }
        exec(s, "COMMIT");
    }
    if (run(prep(s, "PRAGMA user_version")) == 1) {
        exec(s, "BEGIN IMMEDIATE");
        if (run(prep(s, "PRAGMA user_version")) == 1)
            exec(s, "ALTER TABLE folders ADD COLUMN collapsed INTEGER NOT NULL DEFAULT 0; PRAGMA user_version = 2;");
        exec(s, "COMMIT");
    }
    if (run(prep(s, "PRAGMA user_version")) == 2) {
        exec(s, "BEGIN IMMEDIATE");
        if (run(prep(s, "PRAGMA user_version")) == 2) {
            exec(s, "ALTER TABLE messages ADD COLUMN snippet TEXT");
            sqlite3_stmt *st = prep(s, "SELECT rowid, body FROM search WHERE body != ''");
            while (st && sqlite3_step(st) == SQLITE_ROW) {
                char *snippet = make_snippet((const char *)sqlite3_column_text(st, 1));
                sqlite3_stmt *up = prep(s, "UPDATE messages SET snippet = ? WHERE id = ?");
                bind_text(up, 1, snippet);
                sqlite3_bind_int64(up, 2, sqlite3_column_int64(st, 0));
                run(up);
                free(snippet);
            }
            sqlite3_finalize(st);
            exec(s, "PRAGMA user_version = 3");
        }
        exec(s, "COMMIT");
    }
    if (run(prep(s, "PRAGMA user_version")) == 3) {
        exec(s, "BEGIN IMMEDIATE");
        if (run(prep(s, "PRAGMA user_version")) == 3) {
            exec(s, "ALTER TABLE folders ADD COLUMN headers_stale INTEGER NOT NULL DEFAULT 0");
#ifdef __ANDROID__
            /* bionic's iconv decoded no 8-bit charset: subjects, names and snippets stored before are wrong */
            exec(s, "UPDATE folders SET headers_stale = 1");
#endif
            exec(s, "PRAGMA user_version = 4");
        }
        exec(s, "COMMIT");
    }
    return s;
bad:
    snprintf(err, (size_t)errlen, "%s: %s", path, s->db ? sqlite3_errmsg(s->db) : "cannot open");
    jd_store_close(s);
    return NULL;
}

void jd_store_close(jd_store *s)
{
    if (!s) return;
    sqlite3_close(s->db);
    free(s);
}

int jd_store_begin(jd_store *s)
{
    return sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int jd_store_commit(jd_store *s)
{
    return sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

void jd_header_free(jd_header *h)
{
    free(h->keywords), free(h->msgid), free(h->in_reply_to), free(h->subject);
    for (int r = 0; r < JD_ROLE_COUNT; r++) {
        for (int i = 0; i < h->naddrs[r]; i++) free(h->addrs[r][i].name), free(h->addrs[r][i].addr);
        free(h->addrs[r]);
    }
    for (int i = 0; i < h->nrefs; i++) free(h->refs[i]);
    free(h->refs);
    *h = (jd_header){0};
}

void jd_folders_mark_absent(jd_store *s)
{
    exec(s, "UPDATE folders SET present = 0");
}

int64_t jd_folder_upsert(jd_store *s, const char *name, char delim, const char *special_use)
{
    char d[2] = {delim, 0};
    sqlite3_stmt *st = prep(s, "INSERT INTO folders(name, delim, special_use) VALUES(?1, ?2, ?3)"
                               " ON CONFLICT(name) DO UPDATE SET delim = ?2, special_use = ?3, present = 1"
                               " RETURNING id");
    bind_text(st, 1, name);
    bind_text(st, 2, delim ? d : NULL);
    bind_text(st, 3, special_use);
    return run(st);
}

void jd_folders_sweep_absent(jd_store *s)
{
    exec(s, "DELETE FROM folders WHERE present = 0");
    exec(s, "DELETE FROM threads WHERE id NOT IN (SELECT thread_id FROM messages WHERE thread_id IS NOT NULL)");
}

int jd_folder_state_get(jd_store *s, int64_t folder, jd_folder_state *out)
{
    sqlite3_stmt *st = prep(s, "SELECT uidvalidity, uidnext, highestmodseq,"
                               " (SELECT count(*) FROM messages WHERE folder_id = ?1) FROM folders WHERE id = ?1");
    sqlite3_bind_int64(st, 1, folder);
    int found = sqlite3_step(st) == SQLITE_ROW;
    if (found)
        *out = (jd_folder_state){folder, (uint32_t)sqlite3_column_int64(st, 0), (uint32_t)sqlite3_column_int64(st, 1),
                                 (uint64_t)sqlite3_column_int64(st, 2), sqlite3_column_int(st, 3)};
    sqlite3_finalize(st);
    return found ? 0 : -1;
}

void jd_folder_state_set(jd_store *s, int64_t folder, uint32_t uidvalidity, uint32_t uidnext, uint64_t modseq)
{
    sqlite3_stmt *st = prep(s, "UPDATE folders SET uidvalidity = ?, uidnext = ?, highestmodseq = ? WHERE id = ?");
    sqlite3_bind_int64(st, 1, uidvalidity);
    sqlite3_bind_int64(st, 2, uidnext);
    sqlite3_bind_int64(st, 3, (int64_t)modseq);
    sqlite3_bind_int64(st, 4, folder);
    run(st);
}

void jd_folder_clear(jd_store *s, int64_t folder)
{
    sqlite3_stmt *st = prep(s, "DELETE FROM messages WHERE folder_id = ?");
    sqlite3_bind_int64(st, 1, folder);
    run(st);
    jd_threads_refresh(s, folder);
}

static uint32_t *uid_list(sqlite3_stmt *st, int *n)
{
    uint32_t *out = NULL;
    int cap = 0;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) out = jd_realloc(out, sizeof *out * (size_t)(cap = cap ? cap * 2 : 256));
        out[(*n)++] = (uint32_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

uint32_t *jd_folder_uids(jd_store *s, int64_t folder, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT uid FROM messages WHERE folder_id = ? ORDER BY uid");
    sqlite3_bind_int64(st, 1, folder);
    return uid_list(st, n);
}

uint32_t *jd_folder_uncached(jd_store *s, int64_t folder, int64_t since, int64_t max_size, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT uid FROM messages WHERE folder_id = ? AND body_cached = 0"
                               " AND internaldate >= ? AND size <= ? ORDER BY uid DESC");
    sqlite3_bind_int64(st, 1, folder);
    sqlite3_bind_int64(st, 2, since);
    sqlite3_bind_int64(st, 3, max_size);
    return uid_list(st, n);
}

int64_t jd_message_id(jd_store *s, int64_t folder, uint32_t uid)
{
    sqlite3_stmt *st = prep(s, "SELECT id FROM messages WHERE folder_id = ? AND uid = ?");
    sqlite3_bind_int64(st, 1, folder);
    sqlite3_bind_int64(st, 2, uid);
    return run(st);
}

int64_t jd_message_thread(jd_store *s, int64_t id)
{
    sqlite3_stmt *st = prep(s, "SELECT thread_id FROM messages WHERE id = ?");
    sqlite3_bind_int64(st, 1, id);
    return run(st);
}

/* RFC 5256 section 2.1 base subject, reduced: reply/forward prefixes and list tags dropped, case folded */
static char *base_subject(const char *subject)
{
    const char *p = subject ? subject : "";
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (e) {
                p = e + 1;
                continue;
            }
        }
        static const char *prefixes[] = {"re", "fwd", "fw", "aw", "sv", "wg", NULL};
        int hit = 0;
        for (int i = 0; prefixes[i]; i++) {
            size_t l = strlen(prefixes[i]);
            if (strncasecmp(p, prefixes[i], l) == 0) {
                const char *q = p + l;
                if (*q == '[' && strchr(q, ']')) q = strchr(q, ']') + 1;
                if (*q == ':') {
                    p = q + 1;
                    hit = 1;
                    break;
                }
            }
        }
        if (!hit) break;
    }
    char *out = jd_strdup(p);
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
    return out;
}

/* JWZ threading (https://www.jwz.org/doc/threading.html), done incrementally: a message joins every thread that
   holds a message it references, that references it, or that has the same Message-ID, and those threads merge.
   The subject-grouping step is left out: it joins unrelated mail that shares a subject. */
static void thread_message(jd_store *s, int64_t id, const jd_header *h)
{
    sqlite3_stmt *st = prep(s, "SELECT DISTINCT thread_id FROM messages WHERE thread_id IS NOT NULL AND id != ?1 AND ("
                               " msgid IN (SELECT ref_msgid FROM refs WHERE message_id = ?1)"
                               " OR (msgid IS NOT NULL AND msgid = ?2)"
                               " OR id IN (SELECT message_id FROM refs WHERE ref_msgid = ?2))"
                               " ORDER BY thread_id");
    sqlite3_bind_int64(st, 1, id);
    bind_text(st, 2, h->msgid);
    int64_t keep = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t t = sqlite3_column_int64(st, 0);
        if (!keep) {
            keep = t;
            continue;
        }
        sqlite3_stmt *m = prep(s, "UPDATE messages SET thread_id = ? WHERE thread_id = ?");
        sqlite3_bind_int64(m, 1, keep);
        sqlite3_bind_int64(m, 2, t);
        run(m);
        m = prep(s, "DELETE FROM threads WHERE id = ?");
        sqlite3_bind_int64(m, 1, t);
        run(m);
    }
    sqlite3_finalize(st);
    if (!keep) {
        char *subj = base_subject(h->subject);
        st = prep(s, "INSERT INTO threads(root_msgid, subject_norm, last_date) VALUES(?, ?, ?) RETURNING id");
        bind_text(st, 1, h->nrefs ? h->refs[0] : h->msgid);
        bind_text(st, 2, subj);
        sqlite3_bind_int64(st, 3, h->date);
        keep = run(st);
        free(subj);
    }
    st = prep(s, "UPDATE messages SET thread_id = ? WHERE id = ?");
    sqlite3_bind_int64(st, 1, keep);
    sqlite3_bind_int64(st, 2, id);
    run(st);
}

static void join_addrs(char **out, const jd_header *h, int from, int to)
{
    jd_buf b = {0};
    for (int r = from; r <= to; r++)
        for (int i = 0; i < h->naddrs[r]; i++)
            jd_buf_addf(&b, "%s%s <%s>", b.len ? ", " : "", h->addrs[r][i].name ? h->addrs[r][i].name : "",
                        h->addrs[r][i].addr ? h->addrs[r][i].addr : "");
    *out = b.s ? b.s : jd_strdup("");
}

int64_t jd_message_put(jd_store *s, int64_t folder, const jd_header *h, int *is_new)
{
    int64_t id = jd_message_id(s, folder, h->uid);
    *is_new = id == 0;
    if (id) {
        jd_message_set_flags(s, folder, h->uid, h->flags, h->keywords, h->modseq);
        return id;
    }
    sqlite3_stmt *st = prep(s, "INSERT INTO messages(folder_id, uid, modseq, flags, keywords, internaldate, date, size,"
                               " msgid, in_reply_to, subject, has_attachment)"
                               " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id");
    sqlite3_bind_int64(st, 1, folder);
    sqlite3_bind_int64(st, 2, h->uid);
    sqlite3_bind_int64(st, 3, (int64_t)h->modseq);
    sqlite3_bind_int(st, 4, h->flags);
    bind_text(st, 5, h->keywords);
    sqlite3_bind_int64(st, 6, h->internaldate);
    sqlite3_bind_int64(st, 7, h->date ? h->date : h->internaldate);
    sqlite3_bind_int64(st, 8, h->size);
    bind_text(st, 9, h->msgid);
    bind_text(st, 10, h->in_reply_to);
    bind_text(st, 11, h->subject);
    sqlite3_bind_int(st, 12, h->has_attachment);
    id = run(st);
    if (!id) return 0;

    for (int r = 0; r < JD_ROLE_COUNT; r++)
        for (int i = 0; i < h->naddrs[r]; i++) {
            st = prep(s, "INSERT INTO addresses(message_id, role, position, name, addr) VALUES(?, ?, ?, ?, ?)");
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_int(st, 2, r);
            sqlite3_bind_int(st, 3, i);
            bind_text(st, 4, h->addrs[r][i].name);
            bind_text(st, 5, h->addrs[r][i].addr);
            run(st);
        }
    for (int i = 0; i < h->nrefs; i++) {
        st = prep(s, "INSERT INTO refs(message_id, position, ref_msgid) VALUES(?, ?, ?)");
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int(st, 2, i);
        bind_text(st, 3, h->refs[i]);
        run(st);
    }
    char *sender, *recipients;
    join_addrs(&sender, h, JD_ROLE_FROM, JD_ROLE_FROM);
    join_addrs(&recipients, h, JD_ROLE_TO, JD_ROLE_CC);
    st = prep(s, "INSERT INTO search(rowid, subject, sender, recipients, body) VALUES(?, ?, ?, ?, '')");
    sqlite3_bind_int64(st, 1, id);
    bind_text(st, 2, h->subject);
    bind_text(st, 3, sender);
    bind_text(st, 4, recipients);
    run(st);
    free(sender), free(recipients);
    thread_message(s, id, h);
    return id;
}

int64_t jd_message_refresh(jd_store *s, int64_t folder, const jd_header *h)
{
    int64_t id = jd_message_id(s, folder, h->uid);
    if (!id) return 0;
    sqlite3_stmt *st = prep(s, "UPDATE messages SET subject = ? WHERE id = ?");
    bind_text(st, 1, h->subject);
    sqlite3_bind_int64(st, 2, id);
    run(st);
    st = prep(s, "DELETE FROM addresses WHERE message_id = ?");
    sqlite3_bind_int64(st, 1, id);
    run(st);
    for (int r = 0; r < JD_ROLE_COUNT; r++)
        for (int i = 0; i < h->naddrs[r]; i++) {
            st = prep(s, "INSERT INTO addresses(message_id, role, position, name, addr) VALUES(?, ?, ?, ?, ?)");
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_int(st, 2, r);
            sqlite3_bind_int(st, 3, i);
            bind_text(st, 4, h->addrs[r][i].name);
            bind_text(st, 5, h->addrs[r][i].addr);
            run(st);
        }
    char *sender, *recipients;
    join_addrs(&sender, h, JD_ROLE_FROM, JD_ROLE_FROM);
    join_addrs(&recipients, h, JD_ROLE_TO, JD_ROLE_CC);
    st = prep(s, "UPDATE search SET subject = ?, sender = ?, recipients = ? WHERE rowid = ?");
    bind_text(st, 1, h->subject);
    bind_text(st, 2, sender);
    bind_text(st, 3, recipients);
    sqlite3_bind_int64(st, 4, id);
    run(st);
    free(sender), free(recipients);
    return id;
}

int jd_folder_headers_stale(jd_store *s, int64_t folder)
{
    sqlite3_stmt *st = prep(s, "SELECT headers_stale FROM folders WHERE id = ?");
    sqlite3_bind_int64(st, 1, folder);
    return run(st) != 0;
}

void jd_folder_headers_done(jd_store *s, int64_t folder)
{
    sqlite3_stmt *st = prep(s, "UPDATE folders SET headers_stale = 0 WHERE id = ?");
    sqlite3_bind_int64(st, 1, folder);
    run(st);
}

void jd_message_set_flags(jd_store *s, int64_t folder, uint32_t uid, int flags, const char *keywords, uint64_t modseq)
{
    sqlite3_stmt *st = prep(s, "UPDATE messages SET flags = ?, keywords = ?, modseq = max(modseq, ?)"
                               " WHERE folder_id = ? AND uid = ?");
    sqlite3_bind_int(st, 1, flags);
    bind_text(st, 2, keywords);
    sqlite3_bind_int64(st, 3, (int64_t)modseq);
    sqlite3_bind_int64(st, 4, folder);
    sqlite3_bind_int64(st, 5, uid);
    run(st);
}

void jd_message_delete(jd_store *s, int64_t folder, uint32_t uid)
{
    sqlite3_stmt *st = prep(s, "DELETE FROM messages WHERE folder_id = ? AND uid = ?");
    sqlite3_bind_int64(st, 1, folder);
    sqlite3_bind_int64(st, 2, uid);
    run(st);
}

void jd_message_set_body(jd_store *s, int64_t id, const char *text)
{
    sqlite3_stmt *st = prep(s, "UPDATE search SET body = ? WHERE rowid = ?");
    bind_text(st, 1, text ? text : "");
    sqlite3_bind_int64(st, 2, id);
    run(st);
    char *snippet = make_snippet(text);
    st = prep(s, "UPDATE messages SET body_cached = 1, snippet = ? WHERE id = ?");
    bind_text(st, 1, snippet);
    sqlite3_bind_int64(st, 2, id);
    run(st);
    free(snippet);
}

int jd_message_locate(jd_store *s, int64_t id, int64_t *folder, char **folder_name, uint32_t *uid, int64_t *size)
{
    sqlite3_stmt *st = prep(s, "SELECT m.folder_id, f.name, m.uid, m.size FROM messages m"
                               " JOIN folders f ON f.id = m.folder_id WHERE m.id = ?");
    sqlite3_bind_int64(st, 1, id);
    int found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        *folder = sqlite3_column_int64(st, 0);
        *folder_name = jd_strdup((const char *)sqlite3_column_text(st, 1));
        *uid = (uint32_t)sqlite3_column_int64(st, 2);
        *size = sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    return found ? 0 : -1;
}

void jd_threads_refresh(jd_store *s, int64_t folder)
{
    sqlite3_stmt *st = prep(s, "UPDATE threads SET"
                               " last_date = (SELECT coalesce(max(date), 0) FROM messages WHERE thread_id = threads.id),"
                               " unread = (SELECT count(*) FROM messages WHERE thread_id = threads.id AND flags & 1 = 0)"
                               " WHERE id IN (SELECT thread_id FROM messages WHERE folder_id = ?)");
    sqlite3_bind_int64(st, 1, folder);
    run(st);
    exec(s, "DELETE FROM threads WHERE id NOT IN (SELECT thread_id FROM messages WHERE thread_id IS NOT NULL)");
}

char *jd_body_path(const char *account_dir, int64_t folder, uint32_t uidvalidity, uint32_t uid)
{
    return jd_printf("%s/bodies/%lld/%u-%u.eml", account_dir, (long long)folder, uidvalidity, uid);
}

jd_folder_row *jd_store_folders(jd_store *s, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT f.id, f.name, f.special_use, f.delim, f.collapsed,"
                               " (SELECT count(*) FROM messages m WHERE m.folder_id = f.id),"
                               " (SELECT count(*) FROM messages m WHERE m.folder_id = f.id AND m.flags & 1 = 0)"
                               " FROM folders f ORDER BY"
                               " CASE WHEN upper(f.name) = 'INBOX' THEN 0 WHEN f.special_use = 'Drafts' THEN 1"
                               " WHEN f.special_use = 'Sent' THEN 2 WHEN f.special_use = 'Archive' THEN 3"
                               " WHEN f.special_use = 'Junk' THEN 4 WHEN f.special_use = 'Trash' THEN 5 ELSE 6 END,"
                               " f.name COLLATE NOCASE");
    jd_folder_row *rows = NULL;
    int cap = 0;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) rows = jd_realloc(rows, sizeof *rows * (size_t)(cap = cap ? cap * 2 : 16));
        const char *d = (const char *)sqlite3_column_text(st, 3);
        rows[(*n)++] = (jd_folder_row){
            .id = sqlite3_column_int64(st, 0),
            .name = jd_strdup((const char *)sqlite3_column_text(st, 1)),
            .special_use = jd_strdup((const char *)sqlite3_column_text(st, 2)),
            .delim = d ? d[0] : 0,
            .collapsed = sqlite3_column_int(st, 4),
            .total = sqlite3_column_int(st, 5),
            .unread = sqlite3_column_int(st, 6),
        };
    }
    sqlite3_finalize(st);
    return rows;
}

void jd_folder_set_collapsed(jd_store *s, int64_t folder, int collapsed)
{
    sqlite3_stmt *st = prep(s, "UPDATE folders SET collapsed = ? WHERE id = ?");
    sqlite3_bind_int(st, 1, collapsed);
    sqlite3_bind_int64(st, 2, folder);
    run(st);
}

void jd_folder_rows_free(jd_folder_row *rows, int n)
{
    for (int i = 0; i < n; i++) free(rows[i].name), free(rows[i].special_use);
    free(rows);
}

static char *col_dup(sqlite3_stmt *st, int i)
{
    return jd_strdup((const char *)sqlite3_column_text(st, i));
}

/* "Ada, Bob" from the distinct senders of the thread's messages in the folder, by first appearance */
static char *thread_senders(jd_store *s, int64_t thread, int64_t folder)
{
    sqlite3_stmt *st = prep(s, "SELECT coalesce(nullif(a.name, ''), a.addr) FROM messages m"
                               " JOIN addresses a ON a.message_id = m.id AND a.role = 0 AND a.position = 0"
                               " WHERE m.thread_id = ?1 AND (?2 = 0 OR m.folder_id = ?2) GROUP BY a.addr ORDER BY min(m.date)");
    sqlite3_bind_int64(st, 1, thread);
    sqlite3_bind_int64(st, 2, folder);
    jd_buf b = {0};
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        if (!name) continue;
        if (n++ == 3) {
            jd_buf_adds(&b, ", ...");
            break;
        }
        jd_buf_addf(&b, "%s%s", b.len ? ", " : "", name);
    }
    sqlite3_finalize(st);
    return b.s ? b.s : jd_strdup("Unknown sender");
}

static char *message_sender(jd_store *s, int64_t id)
{
    sqlite3_stmt *st = prep(s, "SELECT coalesce(nullif(name, ''), addr) FROM addresses"
                               " WHERE message_id = ? AND role = 0 ORDER BY position LIMIT 1");
    sqlite3_bind_int64(st, 1, id);
    char *out = sqlite3_step(st) == SQLITE_ROW ? col_dup(st, 0) : NULL;
    sqlite3_finalize(st);
    return out ? out : jd_strdup("Unknown sender");
}

/* FTS5 query from free words: each word quoted (no operator injection), all required, the last one as a prefix */
static char *fts_query(const char *text)
{
    jd_buf b = {0};
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        if (p == w) break;
        jd_buf_adds(&b, b.len ? " \"" : "\"");
        for (const char *c = w; c < p; c++) jd_buf_add(&b, *c == '"' ? "\"\"" : c, *c == '"' ? 2 : 1);
        jd_buf_adds(&b, *p ? "\"" : "\"*");
    }
    return b.s;
}

static char *like_of(const char *s)
{
    jd_buf b = {0};
    jd_buf_adds(&b, "%");
    for (; *s; s++) {
        if (*s == '%' || *s == '_' || *s == '\\') jd_buf_adds(&b, "\\");
        jd_buf_add(&b, s, 1);
    }
    jd_buf_adds(&b, "%");
    return b.s;
}

/* q's conditions ANDed, its parameters named :<field><p>; an OR group's alternatives get p plus their place */
static void where_of(jd_buf *w, const jd_list_query *q, const char *p)
{
    jd_buf_adds(w, "1");
    if (q->folder) jd_buf_addf(w, " AND m.folder_id = :folder%s", p);
    if (q->unread) jd_buf_adds(w, " AND m.flags & 1 = 0");
    if (q->read) jd_buf_adds(w, " AND m.flags & 1 != 0");
    if (q->flagged) jd_buf_adds(w, " AND m.flags & 4 != 0");
    if (q->attachment) jd_buf_adds(w, " AND m.has_attachment");
    if (q->after) jd_buf_addf(w, " AND m.date >= :after%s", p);
    if (q->before) jd_buf_addf(w, " AND m.date < :before%s", p);
    if (q->subject) jd_buf_addf(w, " AND m.subject LIKE :subject%s ESCAPE '\\'", p);
    if (q->from)
        jd_buf_addf(w, " AND m.id IN (SELECT message_id FROM addresses WHERE role = 0"
                       " AND (addr LIKE :from%s ESCAPE '\\' OR name LIKE :from%s ESCAPE '\\'))", p, p);
    if (q->to)
        jd_buf_addf(w, " AND m.id IN (SELECT message_id FROM addresses WHERE role IN (1, 2)"
                       " AND (addr LIKE :to%s ESCAPE '\\' OR name LIKE :to%s ESCAPE '\\'))", p, p);
    if (q->only) jd_buf_addf(w, " AND m.id IN (SELECT value FROM json_each(:only%s))", p);
    int words = q->text && q->text[strspn(q->text, " ")];
    if (words || q->nextra) {
        jd_buf_adds(w, " AND (0");
        if (words) jd_buf_addf(w, " OR m.id IN (SELECT rowid FROM search WHERE search MATCH :fts%s)", p);
        if (q->nextra) jd_buf_addf(w, " OR m.id IN (SELECT value FROM json_each(:extra%s))", p);
        jd_buf_adds(w, ")");
    }
    for (int g = 0; g < q->nany; g++) {
        jd_buf_adds(w, " AND (0");
        for (int a = 0; a < q->any[g].n; a++) {
            char *sub = jd_printf("%s_%d_%d", p, g, a);
            jd_buf_adds(w, " OR (");
            where_of(w, &q->any[g].alt[a], sub);
            jd_buf_adds(w, ")");
            free(sub);
        }
        jd_buf_adds(w, ")");
    }
}

static void bind_named(sqlite3_stmt *st, const char *field, const char *p, const char *text)
{
    char *name = jd_printf(":%s%s", field, p);
    bind_text(st, sqlite3_bind_parameter_index(st, name), text);
    free(name);
}

static void bind_named_int(sqlite3_stmt *st, const char *field, const char *p, int64_t v)
{
    char *name = jd_printf(":%s%s", field, p);
    int i = sqlite3_bind_parameter_index(st, name);
    if (i) sqlite3_bind_int64(st, i, v);
    free(name);
}

static void bind_ids(sqlite3_stmt *st, const char *field, const char *p, const int64_t *ids, int n)
{
    jd_buf b = {0};
    jd_buf_adds(&b, "[");
    for (int i = 0; ids && i < n; i++) jd_buf_addf(&b, "%s%lld", i ? "," : "", (long long)ids[i]);
    jd_buf_adds(&b, "]");
    bind_named(st, field, p, b.s);
    jd_buf_free(&b);
}

static void bind_query(sqlite3_stmt *st, const jd_list_query *q, const char *p)
{
    bind_named_int(st, "folder", p, q->folder);
    bind_named_int(st, "after", p, q->after);
    bind_named_int(st, "before", p, q->before);
    char *like;
    if (q->subject) bind_named(st, "subject", p, like = like_of(q->subject)), free(like);
    if (q->from) bind_named(st, "from", p, like = like_of(q->from)), free(like);
    if (q->to) bind_named(st, "to", p, like = like_of(q->to)), free(like);
    char *fts = q->text ? fts_query(q->text) : NULL;
    bind_named(st, "fts", p, fts);
    free(fts);
    if (q->nextra) bind_ids(st, "extra", p, q->extra, q->nextra);
    if (q->only) bind_ids(st, "only", p, q->only, q->nonly);
    for (int g = 0; g < q->nany; g++)
        for (int a = 0; a < q->any[g].n; a++) {
            char *sub = jd_printf("%s_%d_%d", p, g, a);
            bind_query(st, &q->any[g].alt[a], sub);
            free(sub);
        }
}

jd_thread_row *jd_store_list(jd_store *s, const jd_list_query *q, int *n)
{
    jd_buf w = {0};
    jd_buf_adds(&w, "m.thread_id IS NOT NULL AND ");
    where_of(&w, q, "");
    char *sql = q->grouped
        ? jd_printf("WITH hit AS (SELECT m.*, f.name AS fname FROM messages m JOIN folders f ON f.id = m.folder_id"
                    " WHERE %s)"
                    " SELECT thread_id, count(*), sum(flags & 1 = 0), sum(flags & 4 != 0), max(date), max(has_attachment),"
                    " (SELECT id FROM hit h WHERE h.thread_id = hit.thread_id ORDER BY date DESC LIMIT 1),"
                    " group_concat(DISTINCT fname)"
                    " FROM hit GROUP BY thread_id ORDER BY max(date) DESC LIMIT 5000", w.s)
        : jd_printf("SELECT m.thread_id, 1, m.flags & 1 = 0, m.flags & 4 != 0, m.date, m.has_attachment, m.id, f.name"
                    " FROM messages m JOIN folders f ON f.id = m.folder_id WHERE %s ORDER BY m.date DESC LIMIT 5000", w.s);
    sqlite3_stmt *st = prep(s, sql);
    free(sql);
    jd_buf_free(&w);
    if (st) bind_query(st, q, "");

    jd_thread_row *rows = NULL;
    int cap = 0;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) rows = jd_realloc(rows, sizeof *rows * (size_t)(cap = cap ? cap * 2 : 64));
        rows[(*n)++] = (jd_thread_row){
            .thread = sqlite3_column_int64(st, 0),
            .count = sqlite3_column_int(st, 1),
            .unread = sqlite3_column_int(st, 2),
            .flagged = sqlite3_column_int(st, 3),
            .date = sqlite3_column_int64(st, 4),
            .has_attachment = sqlite3_column_int(st, 5),
            .latest = sqlite3_column_int64(st, 6),
            .folders = col_dup(st, 7),
        };
    }
    sqlite3_finalize(st);
    for (int i = 0; i < *n; i++) {
        st = prep(s, "SELECT m.subject, m.snippet, (SELECT addr FROM addresses WHERE message_id = m.id AND role = 0"
                     " ORDER BY position LIMIT 1) FROM messages m WHERE m.id = ?");
        sqlite3_bind_int64(st, 1, rows[i].latest);
        if (sqlite3_step(st) == SQLITE_ROW) {
            rows[i].subject = col_dup(st, 0);
            rows[i].snippet = col_dup(st, 1);
            rows[i].sender_addr = col_dup(st, 2);
        }
        sqlite3_finalize(st);
        if (!rows[i].subject || !rows[i].subject[0]) free(rows[i].subject), rows[i].subject = jd_strdup("(no subject)");
        rows[i].senders = q->grouped ? thread_senders(s, rows[i].thread, q->folder) : message_sender(s, rows[i].latest);
    }
    return rows;
}

int64_t jd_store_special(jd_store *s, const char *use, char **name)
{
    sqlite3_stmt *st = prep(s, "SELECT id, name FROM folders WHERE special_use = ? ORDER BY id LIMIT 1");
    bind_text(st, 1, use);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        if (name) *name = col_dup(st, 1);
    }
    sqlite3_finalize(st);
    return id;
}

int64_t *jd_store_by_msgid(jd_store *s, int64_t folder, const char *msgid, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT id FROM messages WHERE folder_id = ? AND msgid = ?");
    sqlite3_bind_int64(st, 1, folder);
    bind_text(st, 2, msgid);
    int64_t *out = NULL;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        out = jd_realloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

int jd_store_reply_ids(jd_store *s, int64_t id, char **msgid, char **refs)
{
    sqlite3_stmt *st = prep(s, "SELECT msgid FROM messages WHERE id = ?");
    sqlite3_bind_int64(st, 1, id);
    *msgid = sqlite3_step(st) == SQLITE_ROW ? col_dup(st, 0) : NULL;
    sqlite3_finalize(st);
    st = prep(s, "SELECT ref_msgid FROM refs WHERE message_id = ? ORDER BY position");
    sqlite3_bind_int64(st, 1, id);
    jd_buf b = {0};
    while (sqlite3_step(st) == SQLITE_ROW) jd_buf_addf(&b, "%s%s", b.len ? " " : "", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    *refs = b.s;
    return *msgid ? 0 : -1;
}

jd_contact *jd_store_contacts(jd_store *s, const char *prefix, int limit, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT a.addr, max(nullif(a.name, '')),"
                               " count(*) + 4 * sum(f.special_use = 'Sent' AND a.role IN (1, 2)) AS score"
                               " FROM addresses a JOIN messages m ON m.id = a.message_id JOIN folders f ON f.id = m.folder_id"
                               " WHERE a.addr LIKE ?1 ESCAPE '\\' OR a.name LIKE ?1 ESCAPE '\\' OR a.name LIKE ?2 ESCAPE '\\'"
                               " GROUP BY lower(a.addr) ORDER BY score DESC LIMIT ?3");
    jd_buf p = {0}, w = {0};
    for (const char *c = prefix; *c; c++) {
        if (*c == '%' || *c == '_' || *c == '\\') jd_buf_adds(&p, "\\");
        jd_buf_add(&p, c, 1);
    }
    jd_buf_adds(&p, "%");
    jd_buf_addf(&w, "%% %s", p.s);
    bind_text(st, 1, p.s);
    bind_text(st, 2, w.s);
    sqlite3_bind_int(st, 3, limit);
    jd_buf_free(&p), jd_buf_free(&w);
    jd_contact *out = NULL;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        out = jd_realloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = (jd_contact){col_dup(st, 1), col_dup(st, 0)};
    }
    sqlite3_finalize(st);
    return out;
}

void jd_contacts_free(jd_contact *c, int n)
{
    for (int i = 0; i < n; i++) free(c[i].name), free(c[i].addr);
    free(c);
}

int64_t jd_store_folder_by_name(jd_store *s, const char *name)
{
    sqlite3_stmt *st = prep(s, "SELECT id FROM folders WHERE name = ?1 COLLATE NOCASE"
                               " OR (delim IS NOT NULL AND name LIKE '%' || delim || ?1 COLLATE NOCASE)"
                               " OR special_use = ?1 COLLATE NOCASE ORDER BY name = ?1 COLLATE NOCASE DESC LIMIT 1");
    bind_text(st, 1, name);
    return run(st);
}

void jd_thread_rows_free(jd_thread_row *rows, int n)
{
    for (int i = 0; i < n; i++)
        free(rows[i].subject), free(rows[i].senders), free(rows[i].snippet), free(rows[i].sender_addr), free(rows[i].folders);
    free(rows);
}

static char *addresses_of(jd_store *s, int64_t id, int role)
{
    sqlite3_stmt *st = prep(s, "SELECT name, addr FROM addresses WHERE message_id = ? AND role = ? ORDER BY position");
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int(st, 2, role);
    jd_buf b = {0};
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0), *addr = (const char *)sqlite3_column_text(st, 1);
        if (name && name[0]) jd_buf_addf(&b, "%s%s <%s>", b.len ? ", " : "", name, addr ? addr : "");
        else jd_buf_addf(&b, "%s%s", b.len ? ", " : "", addr ? addr : "");
    }
    sqlite3_finalize(st);
    return b.s;
}

jd_message_row *jd_store_conversation(jd_store *s, int64_t thread, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT m.id, m.folder_id, m.date, m.uid, f.uidvalidity, m.flags, m.body_cached,"
                               " m.has_attachment, m.subject, f.name, f.special_use FROM messages m"
                               " JOIN folders f ON f.id = m.folder_id WHERE m.thread_id = ? ORDER BY m.date, m.id");
    sqlite3_bind_int64(st, 1, thread);
    jd_message_row *rows = NULL;
    int cap = 0;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) rows = jd_realloc(rows, sizeof *rows * (size_t)(cap = cap ? cap * 2 : 8));
        rows[(*n)++] = (jd_message_row){
            .id = sqlite3_column_int64(st, 0),
            .folder = sqlite3_column_int64(st, 1),
            .date = sqlite3_column_int64(st, 2),
            .uid = (uint32_t)sqlite3_column_int64(st, 3),
            .uidvalidity = (uint32_t)sqlite3_column_int64(st, 4),
            .flags = sqlite3_column_int(st, 5),
            .body_cached = sqlite3_column_int(st, 6),
            .has_attachment = sqlite3_column_int(st, 7),
            .subject = col_dup(st, 8),
            .folder_name = col_dup(st, 9),
            .special_use = col_dup(st, 10),
        };
    }
    sqlite3_finalize(st);
    for (int i = 0; i < *n; i++) {
        rows[i].from = addresses_of(s, rows[i].id, JD_ROLE_FROM);
        rows[i].to = addresses_of(s, rows[i].id, JD_ROLE_TO);
        rows[i].cc = addresses_of(s, rows[i].id, JD_ROLE_CC);
        rows[i].reply_to = addresses_of(s, rows[i].id, JD_ROLE_REPLY_TO);
    }
    return rows;
}

void jd_message_row_clear(jd_message_row *r)
{
    free(r->subject), free(r->from), free(r->to), free(r->cc), free(r->reply_to), free(r->folder_name);
    free(r->special_use);
    *r = (jd_message_row){0};
}

int64_t *jd_store_thread_ids(jd_store *s, int64_t thread, int64_t folder, int *n)
{
    sqlite3_stmt *st = prep(s, "SELECT id FROM messages WHERE thread_id = ?1 AND (?2 = 0 OR folder_id = ?2)");
    sqlite3_bind_int64(st, 1, thread);
    sqlite3_bind_int64(st, 2, folder);
    int64_t *out = NULL;
    int cap = 0;
    *n = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) out = jd_realloc(out, sizeof *out * (size_t)(cap = cap ? cap * 2 : 8));
        out[(*n)++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

void jd_message_rows_free(jd_message_row *rows, int n)
{
    for (int i = 0; i < n; i++) jd_message_row_clear(&rows[i]);
    free(rows);
}

void jd_store_set_flag(jd_store *s, int64_t id, int flag, int on)
{
    sqlite3_stmt *st = prep(s, on ? "UPDATE messages SET flags = flags | ?1 WHERE id = ?2"
                                  : "UPDATE messages SET flags = flags & ~?1 WHERE id = ?2");
    sqlite3_bind_int(st, 1, flag);
    sqlite3_bind_int64(st, 2, id);
    run(st);
    st = prep(s, "UPDATE threads SET unread = (SELECT count(*) FROM messages WHERE thread_id = threads.id"
                 " AND flags & 1 = 0) WHERE id = (SELECT thread_id FROM messages WHERE id = ?)");
    sqlite3_bind_int64(st, 1, id);
    run(st);
}

void jd_message_delete_id(jd_store *s, int64_t id)
{
    sqlite3_stmt *st = prep(s, "DELETE FROM messages WHERE id = ?");
    sqlite3_bind_int64(st, 1, id);
    run(st);
}
