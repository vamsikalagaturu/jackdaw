#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "core/util.h"
#include "mail/markdown.h"
#include "mail/mime.h"
#include "mail/send.h"
#include "platform.h"
#include "ui/html.h"
#include "ui/icons.h"

enum { C_TO, C_CC, C_BCC, C_SUBJECT, C_BODY, C_COUNT };
enum { MAX_ATTACHMENT = 25 * 1024 * 1024 };

struct jd_compose {
    compose_mode mode;
    jd_field fields[C_SUBJECT + 1];
    jd_editor body;
    int focus, show_cc;
    char *msgid, *in_reply_to, *references;
    int64_t replied;
    /* A reply: the attribution line and the "> " quote of the original. To HTML mail, with html_quote, the editor
       holds no quote: the original shows below it as it is sent, its own HTML in the HTML part (the text part
       still gets the "> " lines); without, the "> " lines are in the editor, as for plain mail. */
    char *attribution, *quote_text, *quote_html;
    int html_quote;
    jd_part *related; /* the original's cid: images, sent beside the quoted HTML */
    int nrelated;
    jd_html *quote_doc;
    float quote_w, quote_h, quote_scroll;
    jd_part *parts;
    int nparts;
    jd_contact *sugg;
    int nsugg, sugg_sel;
    char sugg_for[256];
    int64_t serial;
    int sending, dirty;
    int normal;  /* vim's normal mode: keys move (between fields, or in the body) instead of typing */
    int visual;  /* visual mode in the body: moving selects */
    int pending; /* the first key of a two-key command (gg, dd, yy) */
    jd_popup menu; /* right click on the body */
    jd_popup later; /* Send later's times */
    int64_t send_at; /* scheduled: saved as a draft that a client sends then */
    int holding;     /* the draft opened here is held back from sending (jd_sync_hold) */
    char error[512];
};

/* the save that scheduled a message, whose tab is closed by the time the server takes it */
static int64_t scheduled_serial, scheduled_for;

static void when_label(int64_t t, char *out, size_t n)
{
    time_t when = (time_t)t;
    struct tm tm;
    localtime_r(&when, &tm);
    strftime(out, n, "%a %d %b, %H:%M", &tm);
}

int compose_inserting(struct jd_compose *c)
{
    return c && !c->normal;
}

int compose_visual(struct jd_compose *c)
{
    return c && c->normal && c->visual;
}

static int64_t next_serial = 1;

/* from a mailto: link (RFC 6068): the address part, up to any ?query */
void compose_set_to(struct jd_compose *c, const char *mailto)
{
    jd_buf b = {0};
    for (const char *p = mailto; *p && *p != '?'; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            char ch = (char)strtol(hex, NULL, 16);
            jd_buf_add(&b, &ch, 1);
            p += 2;
        } else {
            jd_buf_add(&b, p, 1);
        }
    }
    jd_field_set(&c->fields[C_TO], b.s);
    jd_buf_free(&b);
    c->focus = C_SUBJECT;
}

const char *compose_title(struct jd_compose *c)
{
    return c->fields[C_SUBJECT].len ? c->fields[C_SUBJECT].text : "New message";
}

void compose_free(jd_app *app, struct jd_compose *c)
{
    if (!c) return;
    if (c->holding && app->sync) jd_sync_hold(app->sync, c->msgid, 0);
    jd_editor_free(&c->body);
    free(c->msgid), free(c->in_reply_to), free(c->references);
    free(c->quote_html), free(c->attribution), free(c->quote_text);
    for (int i = 0; i < c->nrelated; i++) free(c->related[i].type), free(c->related[i].data), free(c->related[i].cid);
    free(c->related);
    if (c->quote_doc) jd_html_free(c->quote_doc);
    for (int i = 0; i < c->nparts; i++) free(c->parts[i].name), free(c->parts[i].type), free(c->parts[i].data);
    free(c->parts);
    jd_contacts_free(c->sugg, c->nsugg);
    free(c);
}

static struct jd_compose *new_compose(jd_app *app, compose_mode mode)
{
    struct jd_compose *c = jd_alloc(sizeof *c);
    c->mode = mode;
    c->msgid = jd_message_id_new(app->account.address);
    c->fields[C_TO].placeholder = "To";
    c->fields[C_CC].placeholder = "Cc";
    c->fields[C_BCC].placeholder = "Bcc";
    c->fields[C_SUBJECT].placeholder = "Subject";
    jd_tab *t = tab_add(app, TAB_COMPOSE);
    t->compose = c;
    return c;
}

static void add_part(struct jd_compose *c, const char *name, const char *type, char *data, size_t len)
{
    c->parts = jd_realloc(c->parts, sizeof *c->parts * (size_t)(c->nparts + 1));
    c->parts[c->nparts++] = (jd_part){.name = jd_strdup(name), .type = jd_strdup(type), .data = data, .len = len};
    c->dirty = 1;
}

/* "[n] " at s */
static int list_item(const char *s)
{
    if (*s != '[' || !isdigit((unsigned char)s[1])) return 0;
    const char *p = s + 1;
    while (isdigit((unsigned char)*p)) p++;
    return p[0] == ']' && p[1] == ' ';
}

/* where the block goes: before the signature, a forwarded message or the quoted reply, else at the end */
static int list_place(const char *t, int len)
{
    static const char *marks[] = {"\n\n-- \n", "\n\n---------- Forwarded message", "\n\nOn "};
    int at = len;
    for (int i = 0; i < 3; i++) {
        const char *m = strstr(t, marks[i]);
        if (m && (int)(m - t) < at) at = (int)(m - t);
    }
    return at;
}

/* The "---" and "[1] name" lines in the body, rewritten whenever the attachments change */
static void attachment_list(jd_app *app, struct jd_compose *c)
{
    const char *t = c->body.text ? c->body.text : "";
    jd_buf out = {0};
    int len = (int)strlen(t), cursor = c->body.cursor;
    const char *old = strstr(t, "\n\n---\n[1] ");
    int from = len, to = len;
    if (old) {
        from = (int)(old - t);
        const char *p = old + 6;
        while (list_item(p)) {
            const char *nl = strchr(p, '\n');
            p = nl ? nl + 1 : t + len;
        }
        to = (int)(p - t);
        if (to > from && t[to - 1] == '\n' && to < len) to--;
    }
    jd_buf_addf(&out, "%.*s%s", from, t, t + to);
    int place = old ? from : list_place(out.s ? out.s : "", (int)out.len);
    if (cursor > to) cursor -= to - from;
    else if (cursor > from) cursor = from;
    jd_buf block = {0};
    if (app->prefs.attachment_list && c->nparts) {
        jd_buf_adds(&block, "\n\n---");
        for (int i = 0; i < c->nparts; i++) jd_buf_addf(&block, "\n[%d] %s", i + 1, c->parts[i].name);
    }
    jd_buf body = {0};
    jd_buf_addf(&body, "%.*s%s%s", place, out.s ? out.s : "", block.s ? block.s : "", out.s ? out.s + place : "");
    if (cursor > place) cursor += (int)block.len;
    float scroll = c->body.scroll;
    jd_editor_set(&c->body, body.s ? body.s : "");
    c->body.scroll = scroll;
    c->body.cursor = cursor < (int)body.len ? cursor : (int)body.len;
    jd_buf_free(&out), jd_buf_free(&block), jd_buf_free(&body);
}

void compose_attach_paths(jd_app *app, struct jd_compose *c, char **paths, int n)
{
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size > MAX_ATTACHMENT) {
            snprintf(c->error, sizeof c->error, "%s is over 25 MB; most servers refuse that.", paths[i]);
            continue;
        }
        size_t len;
        char *data = jd_read_file(paths[i], &len);
        if (!data) continue;
        const char *slash = strrchr(paths[i], '/');
        const char *name = slash ? slash + 1 : paths[i];
        add_part(c, name, jd_mime_type_of(name), data, len);
    }
    attachment_list(app, c);
}

/* the message the reply or forward is about: its row, text and raw source */
static int load_source(jd_app *app, int64_t id, jd_message_row *row, char **text, char **raw, size_t *len)
{
    *text = *raw = NULL;
    int n;
    jd_message_row *rows = jd_store_conversation(app->view, jd_message_thread(app->view, id), &n);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (rows[i].id == id) {
            *row = rows[i];
            rows[i] = (jd_message_row){0};
            found = 1;
        }
    jd_message_rows_free(rows, n);
    if (!found) return -1;
    if (row->body_cached) {
        char *path = jd_body_path(app->dir, row->folder, row->uidvalidity, row->uid);
        *raw = jd_read_file(path, len);
        free(path);
        if (*raw) *text = jd_mime_body_text(*raw, *len);
    }
    return 0;
}

static const char *bare_address(const char *s, char *out, size_t n)
{
    const char *lt = strrchr(s, '<');
    if (lt) snprintf(out, n, "%.*s", (int)strcspn(lt + 1, ">"), lt + 1);
    else snprintf(out, n, "%s", s);
    return out;
}

/* "a, b" without the account's own address and without repeats of what is already in keep */
static void add_others(jd_buf *b, const char *list, const char *self, const char *keep)
{
    if (!list) return;
    char *copy = jd_strdup(list), *next = copy;
    while (next) {
        /* a comma inside a quoted name ("Hoppe, Kirstin") does not end the address */
        char *item = next, *p = next;
        int quoted = 0;
        for (; *p && (quoted || *p != ','); p++) {
            if (*p == '\\' && quoted && p[1]) p++;
            else if (*p == '"') quoted = !quoted;
        }
        next = *p ? p + 1 : NULL;
        *p = 0;
        while (*item == ' ') item++;
        char addr[320];
        bare_address(item, addr, sizeof addr);
        if (!addr[0] || strcasecmp(addr, self) == 0 || (keep && strcasestr(keep, addr)) || (b->s && strcasestr(b->s, addr)))
            continue;
        jd_buf_addf(b, "%s%s", b->len ? ", " : "", item);
    }
    free(copy);
}

static char *prefixed(const char *subject, const char *prefix)
{
    const char *s = subject ? subject : "";
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0 ? jd_strdup(s) : jd_printf("%s %s", prefix, s);
}

static void quote_into(jd_buf *b, const char *text)
{
    const char *p = text ? text : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        /* without CRLF mail's \r: the quote reads as the editor holds it, so the HTML toggle finds it there */
        size_t shown = n && p[n - 1] == '\r' ? n - 1 : n;
        jd_buf_addf(b, ">%s%.*s\n", shown && p[0] != '>' ? " " : "", (int)shown, p);
        p += n + (nl ? 1 : 0);
    }
}

static void quote_lines(struct jd_compose *c, int in);

static void signature_into(jd_buf *b, const char *sig)
{
    /* RFC 3676 section 4.3: the signature separator is "-- " on a line of its own */
    if (sig && sig[0]) jd_buf_addf(b, "\n\n-- \n%s", sig);
}

void compose_open(jd_app *app, compose_mode mode, int64_t source)
{
    jd_message_row row = {0};
    char *text = NULL, *raw = NULL;
    size_t len = 0;
    if (mode != COMPOSE_NEW && (!source || load_source(app, source, &row, &text, &raw, &len) != 0)) {
        free(app->error);
        app->error = jd_strdup("Select a message to reply to or forward first.");
        return;
    }
    struct jd_compose *c = new_compose(app, mode);
    jd_buf body = {0};
    const char *self = app->account.address;
    if (mode == COMPOSE_NEW) {
        signature_into(&body, app->prefs.signature_new);
        c->focus = C_TO;
    } else {
        int from_self = row.from && strcasestr(row.from, self);
        if (mode != COMPOSE_FORWARD) {
            jd_buf to = {0}, cc = {0};
            /* replying to your own sent message goes to its recipients again */
            if (from_self) add_others(&to, row.to, self, NULL);
            else add_others(&to, row.reply_to && row.reply_to[0] ? row.reply_to : row.from, "", NULL);
            if (mode == COMPOSE_REPLY_ALL) {
                if (!from_self) add_others(&cc, row.to, self, to.s);
                add_others(&cc, row.cc, self, to.s);
            }
            jd_field_set(&c->fields[C_TO], to.s);
            jd_field_set(&c->fields[C_CC], cc.s);
            c->show_cc = cc.len > 0;
            jd_buf_free(&to), jd_buf_free(&cc);
            char *subject = prefixed(row.subject, "Re:");
            jd_field_set(&c->fields[C_SUBJECT], subject);
            free(subject);
            char *msgid, *refs;
            if (jd_store_reply_ids(app->view, source, &msgid, &refs) == 0) {
                c->in_reply_to = jd_strdup(msgid);
                c->references = refs && refs[0] ? jd_printf("%s %s", refs, msgid) : jd_strdup(msgid);
            }
            free(msgid), free(refs);
            c->replied = source;
            c->focus = C_BODY;
        } else {
            char *subject = prefixed(row.subject, "Fwd:");
            jd_field_set(&c->fields[C_SUBJECT], subject);
            free(subject);
            c->focus = C_TO;
        }
        signature_into(&body, app->prefs.signature_reply);
        char date[64];
        format_date(row.date, date, sizeof date);
        if (mode == COMPOSE_FORWARD) {
            jd_buf_addf(&body, "\n\n---------- Forwarded message ----------\nFrom: %s\nDate: %s\nSubject: %s\nTo: %s\n%s%s\n\n%s",
                        row.from ? row.from : "", date, row.subject ? row.subject : "", row.to ? row.to : "",
                        row.cc ? "Cc: " : "", row.cc ? row.cc : "", text ? text : "");
            int nf;
            jd_attachment *files = raw ? jd_mime_attachments(raw, len, &nf) : NULL;
            for (int i = 0; files && i < nf; i++) {
                size_t dlen;
                char *data = jd_mime_attachment_data(raw, len, i, &dlen);
                if (data) add_part(c, files[i].name, files[i].type, data, dlen);
            }
            jd_attachments_free(files, files ? nf : 0);
        } else {
            /* room to write above the quote, which goes in under this line once the editor holds it */
            c->attribution = jd_printf("On %s, %s wrote:", date, row.from ? row.from : "someone");
            jd_buf_addf(&body, "\n\n%s\n", c->attribution);
            jd_buf q = {0};
            quote_into(&q, text);
            c->quote_text = q.s ? q.s : jd_strdup("");
            c->quote_html = raw ? jd_mime_body_html(raw, len) : NULL;
            int ni;
            jd_inline_part *in = c->quote_html ? jd_mime_inline_parts(raw, len, &ni) : NULL;
            if (in && ni) {
                c->related = jd_alloc(sizeof *c->related * (size_t)ni);
                for (int i = 0; i < ni; i++, c->nrelated++) {
                    c->related[i] = (jd_part){.type = in[i].type, .data = in[i].data, .len = in[i].len, .cid = in[i].cid};
                    in[i] = (jd_inline_part){0};
                }
            }
            jd_inline_parts_free(in, in ? ni : 0);
        }
    }
    jd_editor_set(&c->body, body.s ? body.s : "");
    jd_buf_free(&body);
    /* text by default: replies mostly answer line by line; Quote: HTML is a toggle away */
    if (c->quote_text) quote_lines(c, 1);
    if (c->nparts) attachment_list(app, c);
    c->dirty = 0;
    free(text), free(raw);
    jd_message_row_clear(&row);
}

void compose_open_draft(jd_app *app, int64_t draft)
{
    jd_message_row row = {0};
    char *text = NULL, *raw = NULL;
    size_t len = 0;
    if (load_source(app, draft, &row, &text, &raw, &len) != 0 || !raw) {
        free(app->error);
        app->error = jd_strdup("The draft is not downloaded yet; try again in a moment.");
        if (app->sync) jd_sync_fetch_body(app->sync, draft);
        free(text), free(raw);
        jd_message_row_clear(&row);
        return;
    }
    struct jd_compose *c = new_compose(app, COMPOSE_NEW);
    jd_draft_headers h;
    jd_mime_draft_headers(raw, len, &h);
    jd_field_set(&c->fields[C_TO], h.to);
    jd_field_set(&c->fields[C_CC], h.cc);
    jd_field_set(&c->fields[C_BCC], h.bcc);
    jd_field_set(&c->fields[C_SUBJECT], h.subject);
    c->show_cc = (h.cc && h.cc[0]) || (h.bcc && h.bcc[0]);
    c->in_reply_to = jd_strdup(h.in_reply_to);
    c->references = jd_strdup(h.references);
    char *at = jd_mime_field(raw, len, "X-Send-Later-At"), *uuid = jd_mime_field(raw, len, "X-Send-Later-Uuid");
    char *ours = jd_send_later_uuid(app->account.address);
    if (uuid && strcasecmp(uuid, ours) == 0) c->send_at = jd_mime_date(at);
    free(at), free(uuid), free(ours);
    char *msgid, *refs;
    if (jd_store_reply_ids(app->view, draft, &msgid, &refs) == 0) {
        free(c->msgid);
        c->msgid = msgid;
    }
    free(refs);
    if (c->send_at && c->msgid && app->sync) {
        jd_sync_hold(app->sync, c->msgid, 1);
        c->holding = 1;
    }
    int nf;
    jd_attachment *files = jd_mime_attachments(raw, len, &nf);
    for (int i = 0; files && i < nf; i++) {
        size_t dlen;
        char *data = jd_mime_attachment_data(raw, len, i, &dlen);
        if (data) add_part(c, files[i].name, files[i].type, data, dlen);
    }
    jd_attachments_free(files, files ? nf : 0);
    jd_editor_set(&c->body, text ? text : "");
    jd_draft_headers_free(&h);
    c->dirty = 0;
    c->focus = C_BODY;
    free(text), free(raw);
    jd_message_row_clear(&row);
}

/* body with the "> " quote after its attribution line (at the end when that line is gone) */
static char *with_quote(struct jd_compose *c, const char *body)
{
    const char *at = strstr(body, c->attribution);
    size_t cut = strlen(body);
    if (at) {
        const char *nl = strchr(at, '\n');
        cut = nl ? (size_t)(nl + 1 - body) : cut;
    }
    jd_buf b = {0};
    jd_buf_add(&b, body, cut);
    if (!at || (cut && body[cut - 1] != '\n')) jd_buf_adds(&b, "\n");
    jd_buf_adds(&b, c->quote_text);
    jd_buf_adds(&b, body + cut);
    return b.s;
}

/* the "> " lines into the editor (Quote: text), or out of it again while they are as they were (Quote: HTML) */
static void quote_lines(struct jd_compose *c, int in)
{
    const char *body = c->body.text ? c->body.text : "";
    if (in) {
        char *s = with_quote(c, body);
        jd_editor_set(&c->body, s);
        free(s);
        c->html_quote = 0;
        return;
    }
    const char *q = c->quote_text[0] ? strstr(body, c->quote_text) : NULL;
    if (!q) {
        snprintf(c->error, sizeof c->error, "The quoted lines were edited, so they stay as text.");
        return;
    }
    jd_buf b = {0};
    jd_buf_add(&b, body, (size_t)(q - body));
    jd_buf_adds(&b, q + strlen(c->quote_text));
    jd_editor_set(&c->body, b.s ? b.s : "");
    jd_buf_free(&b);
    c->html_quote = 1;
}

static void quote_toggle(struct jd_compose *c)
{
    if (c->quote_html) quote_lines(c, c->html_quote);
}

static char *build(jd_app *app, struct jd_compose *c, int draft, size_t *len, char ***rcpts, int *n)
{
    jd_outgoing o = {
        .to = c->fields[C_TO].text, .cc = c->fields[C_CC].text, .bcc = c->fields[C_BCC].text,
        .subject = c->fields[C_SUBJECT].text, .body = c->body.text ? c->body.text : "",
        .in_reply_to = c->in_reply_to, .references = c->references, .message_id = c->msgid,
        .parts = c->parts, .nparts = c->nparts, .draft = draft, .send_at = draft ? c->send_at : 0,
    };
    /* drafts keep only the Markdown source, which is what reopening one reads back */
    char *html = NULL, *text = NULL;
    if (c->html_quote) {
        /* the text part (and a draft) quotes as "> " lines, after the attribution, as the editor would hold them */
        text = with_quote(c, o.body);
        if (!draft) {
            const char *at = strstr(o.body, c->attribution);
            char *mine = at ? jd_strndup(o.body, (size_t)(at - o.body)) : jd_strdup(o.body);
            html = jd_markdown_reply_html(mine, app->prefs.markdown, c->attribution, c->quote_html);
            free(mine);
            o.related = c->related, o.nrelated = c->nrelated;
        }
        o.body = text;
    } else if (app->prefs.markdown && !draft) html = jd_markdown_html(o.body);
    o.html = html;
    char *msg = jd_message_build(&app->account, &o, len, rcpts, n, c->error, sizeof c->error);
    free(html), free(text);
    return msg;
}

static void send(jd_app *app, struct jd_compose *c)
{
    if (c->sending || !app->sync) return;
    c->error[0] = 0;
    size_t len;
    char **rcpts;
    int n;
    char *msg = build(app, c, 0, &len, &rcpts, &n);
    if (!msg) return;
    if (!n) {
        snprintf(c->error, sizeof c->error, "Add someone to send it to.");
    } else {
        c->serial = next_serial++;
        c->sending = 1;
        jd_sync_send(app->sync, msg, len, rcpts, n, c->msgid, c->replied, c->serial);
    }
    jd_rcpts_free(rcpts, n);
    free(msg);
}

/* 1 when handed to the worker */
static int save_draft(jd_app *app, struct jd_compose *c)
{
    if (!app->sync) return 0;
    size_t len;
    char **rcpts;
    int n;
    char *msg = build(app, c, 1, &len, &rcpts, &n);
    if (!msg) return 0;
    c->serial = next_serial++;
    jd_sync_save_draft(app->sync, msg, len, c->msgid, c->serial);
    jd_rcpts_free(rcpts, n);
    free(msg);
    c->dirty = 0;
    c->body.changed = 0;
    return 1;
}

/* saved to Drafts with the time, and its tab closed */
static void schedule(jd_app *app, struct jd_compose *c, int64_t at)
{
    if (c->sending) return;
    c->error[0] = 0;
    if (at <= jd_now()) {
        snprintf(c->error, sizeof c->error, "Pick a time still to come.");
        return;
    }
    if (!c->fields[C_TO].len && !c->fields[C_CC].len && !c->fields[C_BCC].len) {
        snprintf(c->error, sizeof c->error, "Add someone to send it to.");
        return;
    }
    int64_t before = c->send_at;
    c->send_at = at;
    if (!save_draft(app, c)) {
        c->send_at = before;
        return;
    }
    scheduled_serial = c->serial, scheduled_for = at;
    for (int t = 0; t < app->ntabs; t++)
        if (app->tabs[t].kind == TAB_COMPOSE && app->tabs[t].compose == c) {
            tab_close(app, t);
            break;
        }
}

void compose_schedule_typed(jd_app *app, const char *text)
{
    if (app->tab <= 0 || app->tab >= app->ntabs || app->tabs[app->tab].kind != TAB_COMPOSE) return;
    struct jd_compose *c = app->tabs[app->tab].compose;
    struct tm tm = {.tm_isdst = -1};
    if (sscanf(text, "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) {
        snprintf(c->error, sizeof c->error, "Not a time: \"%s\"; write it as 2026-11-19 08:00.", text);
        return;
    }
    tm.tm_year -= 1900, tm.tm_mon -= 1;
    schedule(app, c, (int64_t)mktime(&tm));
}

static void later_menu(jd_app *app, struct jd_compose *c)
{
    if (!c->later.open) return;
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    struct tm tomorrow = today, monday = today;
    tomorrow.tm_mday += 1, tomorrow.tm_hour = 8, tomorrow.tm_min = tomorrow.tm_sec = 0, tomorrow.tm_isdst = -1;
    int to_monday = (8 - today.tm_wday) % 7;
    monday.tm_mday += to_monday ? to_monday : 7, monday.tm_hour = 8, monday.tm_min = monday.tm_sec = 0;
    monday.tm_isdst = -1;
    int64_t at[3] = {(now + 3600) / 60 * 60, mktime(&tomorrow), mktime(&monday)};
    char labels[3][64], when[48];
    static const char *names[] = {"In an hour", "Tomorrow morning", "Monday morning"};
    for (int i = 0; i < 3; i++) {
        when_label(at[i], when, sizeof when);
        snprintf(labels[i], sizeof labels[i], "%s (%s)", names[i], when);
    }
    jd_popup_item items[] = {{labels[0], "send", NULL, 0}, {labels[1], "calendar", NULL, 0},
                             {labels[2], "calendar", NULL, 0}, {"Pick a time...", "pencil", NULL, 0}};
    int i = jd_popup_draw(&app->ui, &c->later, items, 4);
    if (i >= 0 && i < 3) {
        schedule(app, c, at[i]);
    } else if (i == 3) {
        char typed[32];
        strftime(typed, sizeof typed, "%Y-%m-%d %H:%M", &tomorrow);
        dialog_open(app, DLG_PROMPT, ACT_SEND_LATER, "Send at (YYYY-MM-DD HH:MM)", typed);
    }
}

static struct jd_compose *by_serial(jd_app *app, int64_t serial, int *tab)
{
    for (int i = 0; i < app->ntabs; i++)
        if (app->tabs[i].kind == TAB_COMPOSE && app->tabs[i].compose->serial == serial) {
            *tab = i;
            return app->tabs[i].compose;
        }
    return NULL;
}

void compose_sent(jd_app *app, int64_t serial, const char *error)
{
    int tab;
    struct jd_compose *c = by_serial(app, serial, &tab);
    if (!c) return;
    c->sending = 0;
    if (error) {
        snprintf(c->error, sizeof c->error, "Not sent: %s", error);
        return;
    }
    c->dirty = 0;
    c->body.changed = 0;
    tab_close(app, tab);
    notify_user(app, "Sent.");
}

void compose_saved(jd_app *app, int64_t serial, const char *error)
{
    char when[48];
    if (!error && serial == scheduled_serial) {
        when_label(scheduled_for, when, sizeof when);
        notify_user(app, "Scheduled for %s.", when);
    } else {
        notify_user(app, "%s", error ? error : "Draft saved.");
    }
}

void compose_discard(jd_app *app)
{
    if (app->tab <= 0 || app->tab >= app->ntabs || app->tabs[app->tab].kind != TAB_COMPOSE) return;
    struct jd_compose *c = app->tabs[app->tab].compose;
    char *name = NULL;
    int64_t drafts = app->view ? jd_store_special(app->view, "Drafts", &name) : 0;
    free(name);
    int n = 0;
    int64_t *ids = drafts ? jd_store_by_msgid(app->view, drafts, c->msgid, &n) : NULL;
    if (n && app->sync) jd_sync_destroy(app->sync, ids, n);
    free(ids);
    c->dirty = 0, c->body.changed = 0;
    tab_close(app, app->tab);
    notify_user(app, "Discarded.");
}

int compose_may_close(jd_app *app, struct jd_compose *c, int tab)
{
    if ((c->dirty || c->body.changed) && !c->sending) save_draft(app, c);
    return 1;
}

/* the part of an address field after the last comma */
static const char *current_token(const jd_field *f)
{
    const char *comma = strrchr(f->text, ',');
    const char *t = comma ? comma + 1 : f->text;
    while (*t == ' ') t++;
    return t;
}

static void update_suggestions(jd_app *app, struct jd_compose *c)
{
    if (c->focus > C_BCC) {
        c->nsugg = 0;
        return;
    }
    const char *tok = current_token(&c->fields[c->focus]);
    if (strcmp(tok, c->sugg_for) == 0) return;
    snprintf(c->sugg_for, sizeof c->sugg_for, "%s", tok);
    jd_contacts_free(c->sugg, c->nsugg);
    c->sugg = NULL;
    c->nsugg = c->sugg_sel = 0;
    if (strlen(tok) >= 1 && !strchr(tok, '<') && app->view) c->sugg = jd_store_contacts(app->view, tok, 6, &c->nsugg);
}

static void accept_suggestion(struct jd_compose *c, int i)
{
    jd_field *f = &c->fields[c->focus];
    const jd_contact *k = &c->sugg[i];
    size_t keep = (size_t)(current_token(f) - f->text);
    char *head = jd_strndup(f->text, keep);
    char *entry = k->name && k->name[0] ? jd_printf("%s <%s>", k->name, k->addr) : jd_strdup(k->addr);
    char *value = jd_printf("%s%s%s, ", head, keep && head[keep - 1] != ' ' ? " " : "", entry);
    jd_field_set(f, value);
    free(head), free(entry), free(value);
    snprintf(c->sugg_for, sizeof c->sugg_for, "%s", current_token(f));
    c->nsugg = 0;
    c->dirty = 1;
}

/* Markdown formatting, put in as text at the cursor: marks around it (bold, italic, code, link), or a prefix at
   the start of its line (heading, lists, quote) */
enum { FMT_BOLD, FMT_ITALIC, FMT_CODE, FMT_LINK, FMT_HEADING, FMT_BULLETS, FMT_NUMBERS, FMT_QUOTE, FMT_RULE, FMT_COUNT };

/* With a selection, the marks go around it and the prefixes on each of its lines. */
static void format_apply(struct jd_compose *c, int what)
{
    jd_editor *e = &c->body;
    if (!e->text) jd_editor_insert(e, "", 0);
    static const char *const around[][2] = {{"**", "**"}, {"*", "*"}, {"`", "`"}, {"[", "](https://)"}};
    static const char *const prefix[] = {"## ", "- ", "1. ", "> "};
    int from, to, sel = jd_editor_selection(e, &from, &to);
    jd_editor_deselect(e);
    if (what <= FMT_LINK) {
        const char *open = around[what][0], *close = around[what][1];
        if (sel) {
            e->cursor = to;
            jd_editor_insert(e, close, (int)strlen(close));
            e->cursor = from;
            jd_editor_insert(e, open, (int)strlen(open));
            /* a link's cursor goes to its address, the rest after the closing mark */
            e->cursor = to + (int)strlen(open) + (what == FMT_LINK ? 2 : (int)strlen(close));
        } else {
            jd_editor_insert(e, open, (int)strlen(open));
            int back = e->cursor;
            jd_editor_insert(e, close, (int)strlen(close));
            e->cursor = back;
        }
    } else if (what == FMT_RULE) {
        int at_start = e->cursor == 0 || e->text[e->cursor - 1] == '\n';
        const char *rule = at_start ? "---\n" : "\n---\n";
        jd_editor_insert(e, rule, (int)strlen(rule));
    } else {
        int keep = e->cursor, first = jd_editor_line_start(e, sel ? from : e->cursor), last = sel ? to : e->cursor;
        int n = 1, added = 0;
        for (int start = first; start <= last + added;) {
            char p[16];
            if (what == FMT_NUMBERS) snprintf(p, sizeof p, "%d. ", n++);
            else snprintf(p, sizeof p, "%s", prefix[what - FMT_HEADING]);
            e->cursor = start;
            jd_editor_insert(e, p, (int)strlen(p));
            added += (int)strlen(p);
            if (start <= keep) keep += (int)strlen(p);
            int stop = jd_editor_line_stop(e, start);
            if (!sel || stop >= e->len) break;
            start = stop + 1;
        }
        e->cursor = keep;
    }
    c->dirty = 1;
}

/* Enter on a list item starts the next one ("- ", "* ", "> ", "2. " after "1. "); on an empty item it ends the
   list instead. 0 when the line is no list item. */
static int list_continue(struct jd_compose *c)
{
    jd_editor *e = &c->body;
    int from, to;
    if (!e->text || jd_editor_selection(e, &from, &to)) return 0;
    int ls = jd_editor_line_start(e, e->cursor), le = jd_editor_line_stop(e, e->cursor), i = ls;
    while (i < le && e->text[i] == ' ') i++;
    int indent = i - ls, num = -1;
    char delim = 0;
    if (i + 1 < le && strchr("-*+>", e->text[i]) && e->text[i + 1] == ' ') {
        delim = e->text[i];
        i += 2;
    } else {
        int d = i;
        while (d < le && isdigit((unsigned char)e->text[d])) d++;
        if (d == i || d - i > 9 || d + 1 >= le || (e->text[d] != '.' && e->text[d] != ')') || e->text[d + 1] != ' ')
            return 0;
        num = atoi(e->text + i);
        delim = e->text[d];
        i = d + 2;
    }
    if (e->cursor < i) return 0;
    if (i >= le && e->cursor == le) {
        jd_editor_delete_range(e, ls, le);
        c->dirty = 1;
        return 1;
    }
    char next[48];
    if (num >= 0) snprintf(next, sizeof next, "\n%*s%d%c ", indent, "", num + 1, delim);
    else snprintf(next, sizeof next, "\n%*s%c ", indent, "", delim);
    jd_editor_insert(e, next, (int)strlen(next));
    c->dirty = 1;
    return 1;
}

/* the formatting buttons above the body; returns the y below them */
/* the address suggestions drawn over the fields, body and format bar take the mouse this frame */
static int on_suggestions;

static float format_bar(jd_app *app, struct jd_compose *c, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    static const struct {
        const char *icon, *tip;
        int gap;
    } buttons[FMT_COUNT] = {
        {"bold", "Bold  **text**  (Ctrl+B)", 0},      {"italic", "Italic  *text*  (Ctrl+I)", 0},
        {"code", "Code  `text`", 0},                  {"link", "Link  [text](https://…)  (Ctrl+K)", 0},
        {"heading", "Heading  ## ", 1},               {"list", "Bulleted list  - ", 0},
        {"list-ordered", "Numbered list  1. ", 0},    {"text-quote", "Quote  > ", 0},
        {"minus", "Horizontal line  ---", 1},
    };
    float bx = x;
    const char *tip = NULL;
    float tip_x = 0;
    for (int i = 0; i < FMT_COUNT; i++) {
        if (buttons[i].gap) {
            DrawRectangle((int)bx + 3, (int)y + 7, 1, 18, JD_NORD2);
            bx += 8;
        }
        Rectangle b = {bx, y, 32, 32};
        int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind && !app->menu.open && !on_suggestions;
        if (hover) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2), tip = buttons[i].tip, tip_x = bx;
        jd_icon(buttons[i].icon, bx + 8, y + 8, 16, hover ? JD_NORD8 : JD_NORD4);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            format_apply(c, i);
            c->focus = C_BODY;
            c->normal = 0;
        }
        bx += 34;
    }
    if (x + w - bx - 12 > 160)
        jd_text_fit(ui, "Markdown: sent as formatted HTML and as this text", bx + 12, y + 9, x + w - bx - 12, 13, JD_MUTED, 0);
    if (tip) jd_tooltip(tip, tip_x, y + 34);
    return y + 36;
}

static int visible(struct jd_compose *c, int f)
{
    return c->show_cc || (f != C_CC && f != C_BCC);
}

static void copy_range(const jd_editor *e, int from, int to)
{
    char *s = jd_strndup(e->text + from, (size_t)(to - from));
    SetClipboardText(s);
    free(s);
}

/* The body in vim's normal and visual modes: motions (h j k l w b 0 $ gg G), i a I A o O to insert, x dd yy p,
   v and V to select; in visual mode d x y c act on the selection and the formatting keys wrap it. */
static void body_normal(jd_app *app, struct jd_compose *c, int ctrl, int shift)
{
    jd_editor *e = &c->body;
    jd_ui *ui = &app->ui;
    if (!e->text) jd_editor_insert(e, "", 0);
    int pending = c->pending;
    if (jd_key_went_down()) c->pending = 0;
    if (IsKeyPressed(KEY_ESCAPE)) {
        c->visual = 0;
        jd_editor_deselect(e);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        c->visual = 0;
        jd_editor_deselect(e);
        do c->focus = (c->focus + (shift ? C_COUNT - 1 : 1)) % C_COUNT;
        while (!visible(c, c->focus));
        return;
    }
    if (ctrl && app->prefs.markdown && (IsKeyPressed(KEY_B) || IsKeyPressed(KEY_I) || IsKeyPressed(KEY_K))) {
        format_apply(c, IsKeyPressed(KEY_B) ? FMT_BOLD : IsKeyPressed(KEY_I) ? FMT_ITALIC : FMT_LINK);
        c->visual = 0;
        return;
    }
    if (ctrl) return;
    if (IsKeyPressed(KEY_G) && !shift) {
        if (pending == 'g') jd_editor_motion(ui, e, KEY_G, 0, c->visual);
        else c->pending = 'g';
        return;
    }
    static const int motions[] = {KEY_H, KEY_J, KEY_K, KEY_L, KEY_W, KEY_B, KEY_ZERO, KEY_FOUR, KEY_G,
                                  KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END};
    for (size_t i = 0; i < sizeof motions / sizeof *motions; i++)
        if ((IsKeyPressed(motions[i]) || IsKeyPressedRepeat(motions[i])) &&
            (motions[i] != KEY_B || !shift) && jd_editor_motion(ui, e, motions[i], shift, c->visual))
            return;
    int from, to, sel = jd_editor_selection(e, &from, &to);
    if (c->visual) {
        if (IsKeyPressed(KEY_V)) {
            c->visual = 0;
            jd_editor_deselect(e);
        } else if (sel && (IsKeyPressed(KEY_D) || IsKeyPressed(KEY_X) || IsKeyPressed(KEY_C))) {
            copy_range(e, from, to);
            jd_editor_delete_range(e, from, to);
            c->dirty = 1;
            c->visual = 0;
            if (IsKeyPressed(KEY_C)) c->normal = 0;
        } else if (sel && IsKeyPressed(KEY_Y)) {
            copy_range(e, from, to);
            jd_editor_deselect(e);
            e->cursor = from;
            c->visual = 0;
            notify_user(app, "Copied %d characters.", to - from);
        }
        return;
    }
    int ls = jd_editor_line_start(e, e->cursor), le = jd_editor_line_stop(e, e->cursor);
    if (IsKeyPressed(KEY_V)) {
        c->visual = 1;
        if (shift) {
            /* V: whole lines */
            jd_editor_select_from(e, ls);
            e->cursor = le < e->len ? le + 1 : le;
        } else jd_editor_select_from(e, e->cursor);
    } else if (IsKeyPressed(KEY_I)) {
        if (shift) e->cursor = ls;
        c->normal = 0;
    } else if (IsKeyPressed(KEY_A)) {
        e->cursor = shift ? le : e->cursor < le ? e->cursor + 1 : e->cursor;
        c->normal = 0;
    } else if (IsKeyPressed(KEY_O)) {
        e->cursor = shift ? ls : le;
        jd_editor_insert(e, "\n", 1);
        if (shift) e->cursor = ls;
        c->normal = 0;
        c->dirty = 1;
    } else if (IsKeyPressed(KEY_X) && e->cursor < le) {
        int next = e->cursor + 1;
        while (next < le && ((unsigned char)e->text[next] & 0xc0) == 0x80) next++;
        jd_editor_delete_range(e, e->cursor, next);
        c->dirty = 1;
    } else if (IsKeyPressed(KEY_D) && !shift) {
        if (pending == 'd') {
            int stop = le < e->len ? le + 1 : le, start = stop == le && ls > 0 ? ls - 1 : ls;
            copy_range(e, ls, stop);
            jd_editor_delete_range(e, start, stop);
            c->dirty = 1;
        } else c->pending = 'd';
    } else if (IsKeyPressed(KEY_Y) && !shift) {
        if (pending == 'y') {
            copy_range(e, ls, le < e->len ? le + 1 : le);
            notify_user(app, "Copied the line.");
        } else c->pending = 'y';
    } else if (IsKeyPressed(KEY_P)) {
        const char *clip = GetClipboardText();
        if (clip && clip[0]) {
            if (!shift && e->cursor < le) e->cursor++;
            jd_editor_insert(e, clip, (int)strlen(clip));
            c->dirty = 1;
        }
    } else if (IsKeyPressed(KEY_ENTER)) {
        c->normal = 0;
    }
}

/* the body's right-click menu: clipboard, then formatting (with Markdown on) */
static void body_menu(jd_app *app, struct jd_compose *c)
{
    jd_editor *e = &c->body;
    int from, to, sel = jd_editor_selection(e, &from, &to);
    const char *clip = c->menu.open ? GetClipboardText() : NULL;
    jd_popup_item items[] = {
        {"Cut", "scissors", "Ctrl+X", !sel},      {"Copy", "copy", "Ctrl+C", !sel},
        {"Paste", "clipboard", "Ctrl+V", !clip || !clip[0]}, {"Select all", "text-cursor-input", "Ctrl+A", 0},
        {NULL, NULL, NULL, 0},
        {"Bold", "bold", "Ctrl+B", 0},           {"Italic", "italic", "Ctrl+I", 0},
        {"Code", "code", NULL, 0},               {"Link", "link", "Ctrl+K", 0},
        {"Bulleted list", "list", NULL, 0},      {"Numbered list", "list-ordered", NULL, 0},
        {"Quote", "text-quote", NULL, 0},
    };
    int n = app->prefs.markdown ? (int)(sizeof items / sizeof *items) : 4;
    int i = jd_popup_draw(&app->ui, &c->menu, items, n);
    if (i < 0) return;
    if (i <= 1) {
        char *s = jd_strndup(e->text + from, (size_t)(to - from));
        SetClipboardText(s);
        free(s);
        if (i == 0) jd_editor_delete_range(e, from, to), c->dirty = 1;
    } else if (i == 2 && clip) {
        jd_editor_insert(e, clip, (int)strlen(clip));
        c->dirty = 1;
    } else if (i == 3) {
        jd_editor_select_from(e, 0);
        e->cursor = e->len;
    } else if (i >= 5) {
        static const int fmt[] = {FMT_BOLD, FMT_ITALIC, FMT_CODE, FMT_LINK, FMT_BULLETS, FMT_NUMBERS, FMT_QUOTE};
        format_apply(c, fmt[i - 5]);
        c->visual = 0;
    }
}

void compose_keys(jd_app *app, struct jd_compose *c)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (c->menu.open || c->later.open) return;
    if (ctrl && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        send(app, c);
        return;
    }
    if (ctrl && IsKeyPressed(KEY_S)) {
        save_draft(app, c);
        return;
    }
    if (ctrl && shift && IsKeyPressed(KEY_Q) && c->quote_html) {
        quote_toggle(c);
        return;
    }
    if (ctrl && shift && IsKeyPressed(KEY_A)) {
        if (plat_pick_files("Attach files") != 0) snprintf(c->error, sizeof c->error, "No file chooser available.");
        return;
    }
    if (ctrl && shift && IsKeyPressed(KEY_C)) {
        c->show_cc = !c->show_cc;
        return;
    }
    if (c->nsugg && c->focus <= C_BCC) {
        if (IsKeyPressed(KEY_DOWN)) {
            c->sugg_sel = (c->sugg_sel + 1) % c->nsugg;
            return;
        }
        if (IsKeyPressed(KEY_UP)) {
            c->sugg_sel = (c->sugg_sel + c->nsugg - 1) % c->nsugg;
            return;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_TAB)) {
            accept_suggestion(c, c->sugg_sel);
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            c->nsugg = 0;
            return;
        }
    }
    if (c->normal) {
        if (c->focus == C_BODY) body_normal(app, c, ctrl, shift);
        else if (IsKeyPressed(KEY_I) || IsKeyPressed(KEY_A) || IsKeyPressed(KEY_ENTER)) c->normal = 0;
        else if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN) || (IsKeyPressed(KEY_TAB) && !shift)) {
            do c->focus = (c->focus + 1) % C_COUNT;
            while (!visible(c, c->focus));
        } else if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP) || (IsKeyPressed(KEY_TAB) && shift)) {
            do c->focus = (c->focus + C_COUNT - 1) % C_COUNT;
            while (!visible(c, c->focus));
        }
        while (GetCharPressed()) {
        }
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        /* a phone has no vim modes: Back closes the message, keeping a draft */
        if (plat_touch()) {
            for (int t = 0; t < app->ntabs; t++)
                if (app->tabs[t].kind == TAB_COMPOSE && app->tabs[t].compose == c) tab_close(app, t);
            return;
        }
        c->normal = 1;
        c->nsugg = 0;
        jd_editor_deselect(&c->body);
        return;
    }
    if (c->focus == C_BODY && !ctrl && !shift && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) &&
        list_continue(c)) {
        while (GetCharPressed()) {
        }
        return;
    }
    if (IsKeyPressed(KEY_TAB) && c->focus != C_BODY) {
        do c->focus = (c->focus + (shift ? C_COUNT - 1 : 1)) % C_COUNT;
        while (!visible(c, c->focus));
        return;
    }
    if (IsKeyPressed(KEY_TAB) && shift) {
        c->focus = C_SUBJECT;
        return;
    }
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && c->focus < C_BODY) {
        do c->focus++;
        while (!visible(c, c->focus));
        return;
    }
    if (c->focus == C_BODY && ctrl && app->prefs.markdown) {
        if (IsKeyPressed(KEY_B)) format_apply(c, FMT_BOLD);
        else if (IsKeyPressed(KEY_I)) format_apply(c, FMT_ITALIC);
        else if (IsKeyPressed(KEY_K)) format_apply(c, FMT_LINK);
        else jd_editor_input(&app->ui, &c->body);
    } else if (c->focus == C_BODY) {
        jd_editor_input(&app->ui, &c->body);
    } else {
        int before = c->fields[c->focus].len;
        jd_field_input(&c->fields[c->focus]);
        if (c->fields[c->focus].len != before) c->dirty = 1;
        update_suggestions(app, c);
    }
}

void compose_draw(jd_app *app, struct jd_compose *c, Rectangle r)
{
    jd_ui *ui = &app->ui;
    int n;
    char **picked = plat_picked_files(&n);
    if (picked) {
        compose_attach_paths(app, c, picked, n);
        for (int i = 0; i < n; i++) free(picked[i]);
        free(picked);
    }
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        compose_attach_paths(app, c, dropped.paths, (int)dropped.count);
        UnloadDroppedFiles(dropped);
    }

    const int narrow = r.width < 560;
    float margin = narrow ? 12 : 28;
    float x = r.x + margin, w = r.width - 2 * margin, y = r.y + (narrow ? 12 : 18);
    if (w > 980) {
        x += (w - 980) / 2;
        w = 980;
    }
    static const char *labels[] = {"To", "Cc", "Bcc", "Subject"};
    static const char *icons[] = {"user", "users", "eye-off", "type"};
    float label_w = narrow ? 28 : 92, cc_w = narrow ? 64 : 96;
    /* the suggestions sit under the focused field, over what follows it, which is drawn first */
    float sugg_y = 0;
    for (int f = 0, fy = (int)y; f <= C_SUBJECT; f++)
        if (visible(c, f)) {
            if (f == c->focus) sugg_y = (float)fy + 36;
            fy += 42;
        }
    int show_sugg = c->nsugg && c->focus <= C_BCC && sugg_y > 0;
    Rectangle sugg_box = {x + label_w, sugg_y, fminf(420, w - label_w), (float)c->nsugg * 30 + 8};
    on_suggestions = show_sugg && CheckCollisionPointRec(GetMousePosition(), sugg_box) && !app->dialog.kind;
    for (int f = 0; f <= C_SUBJECT; f++) {
        if (!visible(c, f)) continue;
        Color lc = c->focus == f ? JD_NORD8 : JD_NORD4;
        jd_icon(icons[f], x, y + 9, 16, lc);
        if (!narrow) jd_text(ui, labels[f], x + 24, y + 10, 13, lc, 1);
        Rectangle box = {x + label_w, y, w - label_w - (f == C_TO ? cc_w + 8 : 0), 34};
        c->fields[f].inert = on_suggestions;
        if (jd_field_draw(ui, &c->fields[f], box, c->focus == f)) c->focus = f, c->normal = 0;
        if (f == C_TO && jd_icon_button(ui, c->show_cc ? "chevron-down" : "chevron-right", narrow ? "Cc" : "Cc Bcc",
                                        (Rectangle){x + w - cc_w, y, cc_w, 34}, 0) && !on_suggestions)
            c->show_cc = !c->show_cc;
        y += 42;
    }
    if (app->prefs.markdown) y = format_bar(app, c, x, y + 2, w);
    float bottom = r.y + r.height - (narrow ? 56 : 58);
    if (!plat_touch()) bottom -= 22;
    if (c->nparts) bottom -= 36;
    if (c->error[0]) bottom -= 24;
    if (c->send_at) bottom -= 40;
    if (c->quote_html && narrow) bottom -= 44;
    Rectangle body = {x, y + 4, w, bottom - y - 4};
    if (c->html_quote) {
        /* the original below what is written, read-only, on the mail page, as the HTML part quotes it */
        float qh = fmaxf(120, body.height * 0.5f);
        body.height -= qh + 8;
        Rectangle q = {x, body.y + body.height + 8, w, qh};
        jd_text(ui, c->attribution, q.x + 2, q.y, 13, JD_MUTED, 0);
        Rectangle paper = {q.x, q.y + 20, q.width, q.height - 20};
        if (!c->quote_doc) {
            c->quote_doc = jd_html_new(c->quote_html, &ui->reader, 15);
            for (int i = 0; i < c->nrelated; i++) {
                char *src = jd_printf("cid:%s", c->related[i].cid);
                jd_html_add_image(c->quote_doc, src, c->related[i].data, c->related[i].len);
                free(src);
            }
            c->quote_w = 0;
        }
        if (c->quote_w != paper.width - 12) c->quote_h = jd_html_layout(c->quote_doc, paper.width - 12), c->quote_w = paper.width - 12;
        if (CheckCollisionPointRec(GetMousePosition(), paper) && !app->dialog.kind && !c->menu.open)
            c->quote_scroll -= jd_wheel_y(60);
        c->quote_scroll = fmaxf(0, fminf(c->quote_scroll, c->quote_h - paper.height));
        int dark = mail_page_dark(app);
        DrawRectangleRec(paper, jd_html_page(dark));
        DrawRectangle((int)paper.x, (int)paper.y, 3, (int)paper.height, JD_NORD9);
        jd_scissor(paper.x, paper.y, paper.width, paper.height);
        jd_html_set_dark(c->quote_doc, dark);
        jd_html_draw(c->quote_doc, paper.x + 12, paper.y - c->quote_scroll, paper.y, paper.y + paper.height);
        jd_scissor_end();
        if (c->quote_h > paper.height) jd_scrollbar(paper, c->quote_h, &c->quote_scroll);
    }
    c->body.inert = c->menu.open || c->later.open || on_suggestions;
    if (jd_editor_draw(ui, &c->body, body, c->focus == C_BODY)) c->focus = C_BODY, c->normal = 0;
    if (!c->menu.open && !on_suggestions && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && CheckCollisionPointRec(GetMousePosition(), body) &&
        !app->dialog.kind) {
        c->focus = C_BODY;
        jd_popup_open(&c->menu);
    }
    y = bottom + 8;
    if (c->nparts) {
        float cx = x;
        for (int i = 0; i < c->nparts; i++) {
            char label[300];
            snprintf(label, sizeof label, "%s", c->parts[i].name);
            float cw = jd_text_width(ui, label, 13, 0) + 56;
            if (cw > 300) cw = 300;
            Rectangle chip = {cx, y, cw, 28};
            DrawRectangleRounded(chip, 0.3f, 6, JD_NORD1);
            jd_icon("paperclip", cx + 8, y + 7, 14, JD_NORD4);
            jd_text_fit(ui, label, cx + 28, y + 7, cw - 56, 13, JD_NORD6, 0);
            Rectangle x_box = {cx + cw - 24, y + 5, 18, 18};
            jd_icon("x", x_box.x + 2, x_box.y + 2, 14, CheckCollisionPointRec(GetMousePosition(), x_box) ? JD_NORD11 : JD_NORD3);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), x_box)) {
                free(c->parts[i].name), free(c->parts[i].type), free(c->parts[i].data);
                memmove(c->parts + i, c->parts + i + 1, sizeof *c->parts * (size_t)(c->nparts - i - 1));
                c->nparts--;
                c->dirty = 1;
                attachment_list(app, c);
                break;
            }
            cx += cw + 8;
        }
        y += 36;
    }
    if (c->error[0]) {
        jd_text_fit(ui, c->error, x, y, w, 13, JD_NORD11, 0);
        y += 24;
    }
    if (c->send_at) {
        char when[48], line[80];
        when_label(c->send_at, when, sizeof when);
        if (c->send_at <= jd_now() && c->holding) snprintf(line, sizeof line, "Due; sends when closed");
        else snprintf(line, sizeof line, "Scheduled for %s", when);
        jd_icon("calendar", x, y + 8, 16, JD_NORD8);
        jd_text_fit(ui, line, x + 24, y + 9, w - 24 - 140, 13, JD_NORD4, 0);
        if (jd_icon_button(ui, "x", "Unschedule", (Rectangle){x + w - 132, y, 132, 32}, 0)) {
            c->send_at = 0;
            save_draft(app, c);
        }
        y += 40;
    }
    Rectangle bsend = narrow ? (Rectangle){x, y, w - 4 * (48 + 8), 40} : (Rectangle){x, y, 130, 36};
    Rectangle blater = narrow ? (Rectangle){x + w - 216, y, 48, 40} : (Rectangle){x + 140, y, 118, 36};
    Rectangle battach = narrow ? (Rectangle){x + w - 160, y, 48, 40} : (Rectangle){x + 268, y, 110, 36};
    Rectangle bdraft = narrow ? (Rectangle){x + w - 104, y, 48, 40} : (Rectangle){x + 388, y, 136, 36};
    Rectangle bdiscard = narrow ? (Rectangle){x + w - 48, y, 48, 40} : (Rectangle){x + w - 116, y, 116, 36};
    if (jd_icon_button(ui, "send", c->sending ? "Sending..." : "Send", bsend, 1)) send(app, c);
    if (jd_icon_button(ui, "calendar", narrow ? NULL : "Later", blater, 0) && !c->sending) jd_popup_open(&c->later);
    if (jd_icon_button(ui, "paperclip", narrow ? NULL : "Attach", battach, 0) && plat_pick_files("Attach files") != 0)
        snprintf(c->error, sizeof c->error, "No file chooser available; drop files on the window instead.");
    if (jd_icon_button(ui, "file-pen", narrow ? NULL : "Save draft", bdraft, 0)) save_draft(app, c);
    if (jd_icon_button(ui, "trash-2", narrow ? NULL : "Discard", bdiscard, 0) && !c->sending)
        dialog_open(app, DLG_CONFIRM, ACT_DISCARD, "Discard this message? Its saved draft goes too.", NULL);
    if (c->quote_html) {
        /* narrow, the row has no room left: the toggle sits above it */
        Rectangle bq = narrow ? (Rectangle){x, y - 44, 150, 36} : (Rectangle){x + 534, y, 150, 36};
        if (jd_icon_button(ui, "text-quote", c->html_quote ? "Quote: HTML" : "Quote: text", bq, 0))
            quote_toggle(c);
    }
    if (!plat_touch())
        jd_text_fit(ui, "Ctrl+Enter sends · Ctrl+S saves · Ctrl+Shift+Q quote as HTML or text · Ctrl+W closes",
                    x, y + (narrow ? 40 : 36) + 8, w, 13, JD_MUTED, 0);

    if (show_sugg && c->nsugg && c->focus <= C_BCC) {
        Rectangle box = sugg_box;
        DrawRectangleRounded(box, 0.06f, 6, JD_NORD2);
        for (int i = 0; i < c->nsugg; i++) {
            Rectangle row = {box.x + 4, box.y + 4 + (float)i * 30, box.width - 8, 28};
            Vector2 moved = GetMouseDelta();
            if ((moved.x || moved.y) && CheckCollisionPointRec(GetMousePosition(), row)) c->sugg_sel = i;
            if (i == c->sugg_sel) DrawRectangleRounded(row, 0.2f, 6, JD_NORD3);
            char line[400];
            if (c->sugg[i].name) snprintf(line, sizeof line, "%s  <%s>", c->sugg[i].name, c->sugg[i].addr);
            else snprintf(line, sizeof line, "%s", c->sugg[i].addr);
            jd_text_fit(ui, line, row.x + 10, row.y + 7, row.width - 20, 14, JD_NORD6, 0);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), row)) {
                accept_suggestion(c, i);
                break;
            }
        }
    }
    body_menu(app, c);
    later_menu(app, c);
}
