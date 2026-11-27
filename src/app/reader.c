#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "core/zip.h"
#include "mail/fetch.h"
#include "mail/mime.h"
#include "platform.h"
#include "ui/fx.h"
#include "ui/html.h"
#include "ui/icons.h"
#include <rlgl.h>

/* the message text size of the settings ([view] reader_text), and its line height; set by text_size().
   Everything else in the reader (subject, headers, attachments) follows it: K is that size against the default 15. */
static float TEXT_SIZE = 15, LINE_H = 21, K = 1;

/* keyboard visual mode: the conversation and message its caret is in; seg 0 is the body, else the id of the
   header line (From, To, Cc, In) the caret is on, with its caret and anchor as byte offsets in that line */
static struct {
    int active, msg;
    jd_conv *conv;
    int seg, hc, ha;
    int selecting; /* 0: a caret moving freely (v starts a selection at it); 1: moving selects */
} rv;
static void visual_end(jd_conv *c);

/* Header text (subject, sender, date, From, To, Cc, In) can be selected too: a drag over one of them selects
   part of it. One selection at a time, shared with the bodies through clear_selections and selected_text. */
static struct {
    jd_conv *conv;
    int id, dragging, anchor;
    int from, to;
    char *text; /* the selected part, copied: the label itself is gone after the frame */
} hl;

/* the header lines of the message visual mode is in, as drawn last frame: id, text, and where its pieces (each
   address, which wraps as a whole) were drawn, so the caret can move between the wrapped rows */
enum { HREG_PIECES = 64 };
static struct {
    int id;
    char text[1024];
    float y, size;
    int npieces;
    int start[HREG_PIECES];
    float px[HREG_PIECES], py[HREG_PIECES];
} hreg[8];
static int nhreg;

/* returns the registered line, NULL when visual mode is not in this message */
static void *header_register(jd_conv *c, int id, const char *text, float y, float size)
{
    if (!rv.active || rv.conv != c || id / 16 != rv.msg || nhreg >= 8) return NULL;
    hreg[nhreg].id = id;
    snprintf(hreg[nhreg].text, sizeof hreg[nhreg].text, "%s", text);
    hreg[nhreg].y = y;
    hreg[nhreg].size = size;
    hreg[nhreg].npieces = 0;
    return &hreg[nhreg++];
}

static void header_piece(int line, int start, float x, float y)
{
    if (line < 0 || hreg[line].npieces >= HREG_PIECES) return;
    int k = hreg[line].npieces++;
    hreg[line].start[k] = start, hreg[line].px[k] = x, hreg[line].py[k] = y;
}

static int header_caret_here(jd_conv *c, int id)
{
    return rv.active && rv.conv == c && rv.seg == id;
}

/* the reader's zoom for this session, on top of Settings > Message text; not saved */
static const float zooms[] = {0.5f, 0.67f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3};
enum { ZOOM_NONE = 4, ZOOMS = sizeof zooms / sizeof *zooms };
static int zoom_at = ZOOM_NONE;

int reader_zoom_percent(void) { return (int)(zooms[zoom_at] * 100 + 0.5f); }

void reader_zoom(jd_app *app, int dir)
{
    int to = dir ? zoom_at + dir : ZOOM_NONE;
    if (to < 0 || to >= ZOOMS || to == zoom_at) return;
    zoom_at = to;
    /* the open messages laid out again at the new size, as a new Message text size does (apply_prefs) */
    for (int t = 0; t < app->ntabs; t++)
        if (app->tabs[t].conv.thread) conv_open(app, &app->tabs[t].conv, app->tabs[t].conv.thread, app->tabs[t].conv.message);
    notify_user(app, "Message zoom %d%%", reader_zoom_percent());
}

static void text_size(jd_app *app)
{
    TEXT_SIZE = roundf((float)(app->prefs.reader_text ? app->prefs.reader_text : 15) * zooms[zoom_at]);
    LINE_H = (float)(int)(TEXT_SIZE * 1.4f + 0.5f);
    K = TEXT_SIZE / 15.0f;
}

jd_conv *preview(jd_app *app)
{
    return &app->tabs[0].conv;
}

static void free_thumbs(jd_reading *r)
{
    for (int i = 0; r->thumbs && i < r->nfiles; i++)
        if (r->thumbs[i].id) UnloadTexture(r->thumbs[i]);
    free(r->thumbs);
    r->thumbs = NULL;
}

static void free_reading(jd_reading *r)
{
    free(r->text), free(r->line_starts), free(r->invite);
    free_thumbs(r);
    jd_attachments_free(r->files, r->nfiles);
    if (r->html) jd_html_free(r->html);
    jd_message_row_clear(&r->row);
}

static void forget_conv(jd_conv *c);

void conv_close(jd_conv *c)
{
    visual_end(c);
    forget_conv(c);
    for (int i = 0; i < c->n; i++) free_reading(&c->reading[i]);
    free(c->reading);
    *c = (jd_conv){0};
}

static char *read_raw(jd_app *app, const jd_message_row *row, size_t *len)
{
    if (!row->body_cached) return NULL;
    char *path = jd_body_path(app->dir, row->folder, row->uidvalidity, row->uid);
    char *raw = jd_read_file(path, len);
    free(path);
    return raw;
}

static int attribution(const char *line, size_t n)
{
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\r')) n--;
    return (n > 6 && strncmp(line + n - 6, "wrote:", 6) == 0) || (n > 8 && strncmp(line + n - 8, "schrieb:", 8) == 0) ||
           (n > 7 && strncmp(line + n - 7, "écrit :", 7) == 0);
}

/* The quoted history a top-posted reply carries: from the attribution line ("On ..., X wrote:") or the first
   quoted line, when only quoted, blank and attribution lines follow. -1 for inline replies and plain mail. */
static int trailing_quote(const char *text)
{
    if (!text) return -1;
    int start = -1;
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        int blank = 1;
        for (size_t i = 0; i < n; i++) blank &= p[i] == ' ' || p[i] == '\r';
        int quoted = p[0] == '>', attr = attribution(p, n);
        if (quoted || attr) {
            if (start < 0) start = (int)(p - text);
        } else if (!blank) {
            start = -1;
        }
        p += n + (nl ? 1 : 0);
    }
    return start > 0 ? start : -1;
}

static void load_text(jd_app *app, jd_reading *r)
{
    free(r->text), free(r->line_starts), free(r->invite);
    free_thumbs(r);
    jd_attachments_free(r->files, r->nfiles);
    if (r->html) jd_html_free(r->html);
    r->invite = NULL;
    r->text = NULL;
    r->line_starts = NULL;
    r->files = NULL;
    r->html = NULL;
    r->nfiles = 0;
    r->wrapped_for = r->html_w = 0;
    r->loaded = 1;
    size_t len;
    char *raw = read_raw(app, &r->row, &len);
    if (!raw) return;
    r->text = jd_mime_body_text(raw, len);
    r->quote_at = trailing_quote(r->text);
    r->files = jd_mime_attachments(raw, len, &r->nfiles);
    r->invite = jd_mime_calendar(raw, len);
    r->thumbs = jd_alloc(sizeof *r->thumbs * (size_t)(r->nfiles + 1));
    for (int i = 0; i < r->nfiles; i++) {
        if (strncmp(r->files[i].type, "image/", 6) != 0) continue;
        size_t dlen;
        char *data = jd_mime_attachment_data(raw, len, i, &dlen);
        const char *ext = strstr(r->files[i].type, "jpeg") ? ".jpg" : strstr(r->files[i].type, "gif") ? ".gif" : ".png";
        Image im = data ? LoadImageFromMemory(ext, (const unsigned char *)data, (int)dlen) : (Image){0};
        if (im.data) {
            r->thumbs[i] = LoadTextureFromImage(im);
            SetTextureFilter(r->thumbs[i], TEXTURE_FILTER_BILINEAR);
            UnloadImage(im);
        }
        free(data);
    }
    char *html = jd_mime_body_html(raw, len);
    if (html) {
        text_size(app);
        r->html = jd_html_new(html, &app->ui.reader, TEXT_SIZE);
        int n;
        jd_inline_part *parts = r->html ? jd_mime_inline_parts(raw, len, &n) : NULL;
        for (int i = 0; parts && i < n; i++) {
            char *src = jd_printf("cid:%s", parts[i].cid);
            jd_html_add_image(r->html, src, parts[i].data, parts[i].len);
            free(src);
        }
        jd_inline_parts_free(parts, parts ? n : 0);
    }
    free(html), free(raw);
}

/* a link in a mail (HTML or plain text): mailto: opens a compose tab, web links go to the system's browser */
static void follow_link(jd_app *app, const char *url)
{
    if (strncasecmp(url, "mailto:", 7) == 0) {
        compose_open(app, COMPOSE_NEW, 0);
        jd_tab *t = &app->tabs[app->tab];
        if (t->kind == TAB_COMPOSE) compose_set_to(t->compose, url + 7);
        return;
    }
    free(app->error);
    app->error = NULL;
    if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
        app->error = jd_printf("Not opening %s: only web and mail links open.", url);
        return;
    }
    if (plat_open_uri(url) != 0) app->error = jd_printf("No browser to open %s", url);
    else {
        notify_user(app, "Opened %s", url);
    }
}

static void take_row(jd_message_row *row)
{
    jd_message_row_clear(row);
}

/* Every unread message and the newest open expanded; read ones show as a header line, a click expands them.
   Unread messages are marked read. */
void conv_open(jd_app *app, jd_conv *c, int64_t thread, int64_t only)
{
    conv_close(c);
    c->thread = thread;
    c->message = only;
    int n;
    jd_message_row *rows = jd_store_conversation(app->view, thread, &n);
    c->reading = jd_alloc(sizeof *c->reading * (size_t)(n + 1));
    int64_t *unread = jd_alloc(sizeof *unread * (size_t)(n + 1));
    int nunread = 0;
    for (int i = 0; i < n; i++) {
        if (only && rows[i].id != only) {
            take_row(&rows[i]);
            continue;
        }
        jd_reading *r = &c->reading[c->n++];
        *r = (jd_reading){.row = rows[i]};
        if (!r->row.body_cached && app->sync) jd_sync_fetch_body(app->sync, r->row.id);
        if (!(r->row.flags & JD_SEEN)) unread[nunread++] = r->row.id;
    }
    free(rows);
    /* only what opens expanded is read now: a long thread's collapsed cards show their row's snippet */
    for (int i = 0; i < c->n; i++) {
        c->reading[i].expanded = !(c->reading[i].row.flags & JD_SEEN) || i == c->n - 1;
        c->reading[i].open = (float)c->reading[i].expanded;
        if (c->reading[i].expanded) load_text(app, &c->reading[i]);
    }
    if (nunread && app->sync && app->prefs.mark_read) jd_sync_set_flag(app->sync, unread, nunread, JD_SEEN, 1);
    c->plain = app->prefs.prefer_plain;
    c->opened_at = GetTime();
    free(unread);
}

/* After a sync: replies that arrived join, moved-away messages leave; texts and expanded states stay. */
void conv_refresh(jd_app *app, jd_conv *c)
{
    if (!c->thread || !app->view) return;
    int n;
    jd_message_row *rows = jd_store_conversation(app->view, c->thread, &n);
    jd_reading *next = jd_alloc(sizeof *next * (size_t)(n + 1));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (c->message && rows[i].id != c->message) {
            take_row(&rows[i]);
            continue;
        }
        jd_reading *old = NULL;
        for (int j = 0; j < c->n; j++)
            if (c->reading[j].row.id == rows[i].id) old = &c->reading[j];
        jd_reading *r = &next[k++];
        if (old) {
            int had_body = old->row.body_cached;
            take_row(&old->row);
            *r = *old;
            *old = (jd_reading){0};
            r->row = rows[i];
            rows[i] = (jd_message_row){0};
            if (!had_body && r->row.body_cached && r->loaded) load_text(app, r);
        } else {
            *r = (jd_reading){.row = rows[i], .expanded = 1};
            load_text(app, r);
        }
    }
    free(rows);
    for (int j = 0; j < c->n; j++) free_reading(&c->reading[j]);
    free(c->reading);
    c->reading = next;
    c->n = k;
}

void conv_load_all(jd_app *app, jd_conv *c)
{
    for (int i = 0; i < c->n; i++)
        if (!c->reading[i].loaded) load_text(app, &c->reading[i]);
}

void conv_body_ready(jd_app *app, jd_conv *c, int64_t id)
{
    for (int i = 0; i < c->n; i++)
        if (c->reading[i].row.id == id) {
            c->reading[i].row.body_cached = 1;
            if (c->reading[i].loaded) load_text(app, &c->reading[i]);
        }
}

/* the attachment written to a new file in dir; its path, NULL (and app->error set) when that failed */
static char *write_attachment(jd_app *app, jd_reading *r, int index, const char *dir)
{
    size_t len, dlen = 0;
    char *raw = read_raw(app, &r->row, &len);
    char *data = raw ? jd_mime_attachment_data(raw, len, index, &dlen) : NULL;
    free(raw);
    free(app->error);
    app->error = NULL;
    if (!data) {
        app->error = jd_strdup("That attachment could not be read.");
        return NULL;
    }
    jd_mkdirs(dir);
    char *path = jd_unique_path(dir, r->files[index].name);
    if (jd_write_file(path, data, dlen, 0600) != 0) {
        app->error = jd_printf("Cannot write %s", path);
        free(path);
        path = NULL;
    }
    free(data);
    return path;
}

static void save_message(jd_app *app, jd_reading *r);

/* the last message of the conversation as it came from the server (RFC 5322), named after its subject */
void conv_save_message(jd_app *app, jd_conv *c)
{
    if (c->n) save_message(app, &c->reading[c->n - 1]);
}

static void save_message(jd_app *app, jd_reading *r)
{
    size_t len;
    char *raw = read_raw(app, &r->row, &len);
    if (!raw) {
        notify_user(app, "The message is still downloading.");
        return;
    }
    char name[200];
    snprintf(name, sizeof name, "%.180s.eml", r->row.subject && r->row.subject[0] ? r->row.subject : "message");
    for (char *p = name; *p; p++)
        if (*p == '/') *p = '-';
    jd_mkdirs(jd_download_dir());
    char *path = jd_unique_path(jd_download_dir(), name);
    free(app->error);
    app->error = NULL;
    if (jd_write_file(path, raw, len, 0600) == 0) notify_user(app, "Saved %s", path);
    else app->error = jd_printf("Cannot write %s", path);
    free(path), free(raw);
}

/* every attachment of r in one .zip in Downloads; names that repeat get " (2)" */
static void save_all_zip(jd_app *app, jd_reading *r)
{
    size_t len;
    char *raw = read_raw(app, &r->row, &len);
    free(app->error);
    app->error = NULL;
    if (!raw) {
        notify_user(app, "The message is still downloading.");
        return;
    }
    jd_zip_entry *e = jd_alloc(sizeof *e * (size_t)r->nfiles);
    char **names = jd_alloc(sizeof *names * (size_t)r->nfiles);
    int n = 0;
    for (int i = 0; i < r->nfiles; i++) {
        size_t dlen;
        char *data = jd_mime_attachment_data(raw, len, i, &dlen);
        if (!data) continue;
        const char *base = r->files[i].name && r->files[i].name[0] ? r->files[i].name : "attachment";
        char *name = jd_strdup(base);
        for (int k = 2;; k++) {
            int dup = 0;
            for (int j = 0; j < n && !dup; j++) dup = !strcmp(names[j], name);
            if (!dup) break;
            free(name);
            const char *dot = strrchr(base, '.');
            int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
            name = jd_printf("%.*s (%d)%s", stem, base, k, base + stem);
        }
        names[n] = name;
        e[n++] = (jd_zip_entry){name, data, dlen};
    }
    free(raw);
    char zname[200];
    snprintf(zname, sizeof zname, "%.170s attachments.zip", r->row.subject && r->row.subject[0] ? r->row.subject : "Mail");
    for (char *p = zname; *p; p++)
        if (*p == '/') *p = '-';
    jd_mkdirs(jd_download_dir());
    char *path = jd_unique_path(jd_download_dir(), zname);
    if (!n) app->error = jd_strdup("The attachments could not be read.");
    else if (jd_zip_write(path, e, n) == 0) notify_user(app, "Saved %d files to %s", n, path);
    else app->error = jd_printf("Cannot write %s", path);
    for (int i = 0; i < n; i++) free(names[i]), free((char *)e[i].data);
    free(names), free(e), free(path);
}

static void save_attachment(jd_app *app, jd_reading *r, int index)
{
    char *path = write_attachment(app, r, index, jd_download_dir());
    if (path) notify_user(app, "Saved %s", path);
    free(path);
}

/* opened from the session's runtime directory (removed at logout), so opening leaves nothing in Downloads */
static void open_attachment(jd_app *app, jd_reading *r, int index)
{
    const char *run = getenv("XDG_RUNTIME_DIR");
    char *dir = run && run[0] ? jd_printf("%s/" JD_APP_NAME "/open", run) : jd_printf("%s/open", app->dir);
    char *path = write_attachment(app, r, index, dir);
    if (path && plat_open_file(path) != 0) app->error = jd_printf("No application to open %s", r->files[index].name);
    else if (path) notify_user(app, "Opening %s", r->files[index].name);
    free(path), free(dir);
}

static int copy_selection(jd_app *app, jd_conv *c);
static char *selected_text(jd_conv *c);
static void clear_selections(jd_conv *c);
static float span(jd_ui *ui, const char *text, int from, int to);
/* the right-click menu over a message's text: the body's, or a header text's (menu_label, its id in menu_label_id,
   which Select all selects whole) */
static jd_popup reader_menu;
static jd_conv *menu_conv;
static int menu_msg, menu_label_id;
static char *menu_link, *menu_label;

/* the wrapped line of plain text m holding byte pos, among the lines shown */
static int plain_line(const jd_reading *m, int pos)
{
    int l = 0;
    while (l + 1 < m->body_lines && m->line_starts[l + 1] <= pos) l++;
    return l;
}

static int plain_line_end(const jd_reading *m, int l)
{
    int end = l + 1 < m->nlines ? m->line_starts[l + 1] : (int)strlen(m->text);
    while (end > m->line_starts[l] && (m->text[end - 1] == '\n' || m->text[end - 1] == ' ')) end--;
    return end;
}

static int word_char(char ch)
{
    return (unsigned char)ch >= 0x80 || isalnum((unsigned char)ch) || ch == '_';
}

/* a vim motion of the caret in plain text m (see jd_html_caret_move) */
static void plain_move(jd_app *app, jd_conv *c, jd_reading *m, int motion)
{
    jd_ui *ui = &app->ui;
    jd_ui_use(ui, app->prefs.mono_plain ? &ui->mono : &ui->reader);
    int last = m->body_lines - 1, end = plain_line_end(m, last), p = m->caret, l = plain_line(m, p);
    switch (motion) {
    case 'h':
        if (p > 0) p--;
        while (p > 0 && ((unsigned char)m->text[p] & 0xc0) == 0x80) p--;
        break;
    case 'l':
        if (p < end) p++;
        while (p < end && ((unsigned char)m->text[p] & 0xc0) == 0x80) p++;
        break;
    case 'w':
        while (p < end && word_char(m->text[p])) p++;
        while (p < end && !word_char(m->text[p])) p++;
        break;
    case 'b':
        while (p > 0 && !word_char(m->text[p - 1])) p--;
        while (p > 0 && word_char(m->text[p - 1])) p--;
        break;
    case '0': p = m->line_starts[l]; break;
    case '$': p = plain_line_end(m, l); break;
    case 'g': p = 0; break;
    case 'G': p = end; break;
    case 'j': case 'k': {
        int to = l + (motion == 'j' ? 1 : -1);
        if (to < 0 || to > last) break;
        float x = span(ui, m->text, m->line_starts[l], p);
        int q = m->line_starts[to], stop = plain_line_end(m, to);
        while (q < stop) {
            int next = q + 1;
            while (next < stop && ((unsigned char)m->text[next] & 0xc0) == 0x80) next++;
            if ((span(ui, m->text, m->line_starts[to], q) + span(ui, m->text, m->line_starts[to], next)) / 2 > x) break;
            q = next;
        }
        p = q;
        break;
    }
    default: break;
    }
    jd_ui_use(ui, NULL);
    m->caret = p;
    m->sel_from = m->anchor < p ? m->anchor : p;
    m->sel_to = m->anchor < p ? p : m->anchor;
    (void)c;
}

/* v: visual mode from the first line on screen of the first open message in view */
static void visual_begin(jd_conv *c)
{
    for (int i = 0; i < c->n; i++) {
        jd_reading *m = &c->reading[i];
        if (!m->expanded || m->body_y < 0) continue;
        int html = m->html && !c->plain;
        float height = html ? m->html_h : (float)m->body_lines * LINE_H;
        if (m->body_y + height <= c->top) continue;
        if (!html && (!m->text || !m->body_lines)) continue;
        clear_selections(c);
        rv.active = 1, rv.conv = c, rv.msg = i, rv.seg = 0, rv.selecting = 0;
        nhreg = 0;
        float into = c->top - m->body_y;
        if (html) jd_html_caret_begin(m->html, into > 0 ? into : 0);
        else {
            int l = into > 0 ? (int)(into / LINE_H) + 1 : 0;
            if (l >= m->body_lines) l = m->body_lines - 1;
            m->caret = m->anchor = m->line_starts[l];
        }
        return;
    }
}

/* the selection in a header line follows its caret; it stays within the line */
static void header_select(jd_conv *c, int k)
{
    hl.conv = c, hl.id = rv.seg, hl.dragging = 0;
    hl.from = rv.ha < rv.hc ? rv.ha : rv.hc;
    hl.to = rv.ha < rv.hc ? rv.hc : rv.ha;
    free(hl.text);
    hl.text = hl.to > hl.from ? jd_strndup(hreg[k].text + hl.from, (size_t)(hl.to - hl.from)) : NULL;
}

/* from the body's first line up to the last header line */
static float header_enter(jd_conv *c)
{
    jd_reading *m = &c->reading[rv.msg];
    clear_selections(c);
    if (m->html) jd_html_caret_end(m->html);
    rv.seg = hreg[nhreg - 1].id;
    rv.hc = rv.ha = 0;
    header_select(c, nhreg - 1);
    return hreg[nhreg - 1].y;
}

static int label_hit(jd_ui *ui, const char *text, float x, float size, int bold);
static float label_x(jd_ui *ui, const char *text, int at, float size, int bold);

/* in header line k: the piece holding byte p */
static int piece_of(int k, int p)
{
    int i = 0;
    while (i + 1 < hreg[k].npieces && hreg[k].start[i + 1] <= p) i++;
    return i;
}

/* j or k between the wrapped rows of header line k, keeping the caret's x; -1 when there is no row that way */
static int header_row_move(jd_ui *ui, int k, int p, int dir)
{
    if (!hreg[k].npieces) return -1;
    const char *t = hreg[k].text;
    float ts = hreg[k].size;
    int i = piece_of(k, p);
    float x = hreg[k].px[i] + label_x(ui, t + hreg[k].start[i], p - hreg[k].start[i], ts, 0), row = hreg[k].py[i];
    /* the nearest row above or below */
    float to = 0;
    int found = 0;
    for (int j = 0; j < hreg[k].npieces; j++) {
        float py = hreg[k].py[j];
        if ((dir > 0 ? py > row : py < row) && (!found || (dir > 0 ? py < to : py > to))) to = py, found = 1;
    }
    if (!found) return -1;
    int best = -1;
    for (int j = 0; j < hreg[k].npieces; j++) {
        if (hreg[k].py[j] != to) continue;
        if (best < 0 || hreg[k].px[j] <= x) best = j;
    }
    int end = best + 1 < hreg[k].npieces ? hreg[k].start[best + 1] : (int)strlen(t);
    char piece[1024];
    snprintf(piece, sizeof piece, "%.*s", end - hreg[k].start[best], t + hreg[k].start[best]);
    float into = x - hreg[k].px[best];
    return hreg[k].start[best] + (into <= 0 ? 0 : label_hit(ui, piece, into, ts, 0));
}

/* a motion of the caret in a header line; j past the last one goes back into the body. Returns the caret's y. */
static float header_move(jd_app *app, jd_conv *c, int motion)
{
    int k = 0;
    while (k < nhreg && hreg[k].id != rv.seg) k++;
    if (k == nhreg) return c->top;
    const char *t = hreg[k].text;
    int len = (int)strlen(t), p = rv.hc;
    switch (motion) {
    case 'h':
        if (p > 0) p--;
        while (p > 0 && ((unsigned char)t[p] & 0xc0) == 0x80) p--;
        break;
    case 'l':
        if (p < len) p++;
        while (p < len && ((unsigned char)t[p] & 0xc0) == 0x80) p++;
        break;
    case 'w':
        while (p < len && word_char(t[p])) p++;
        while (p < len && !word_char(t[p])) p++;
        break;
    case 'b':
        while (p > 0 && !word_char(t[p - 1])) p--;
        while (p > 0 && word_char(t[p - 1])) p--;
        break;
    case '0': case 'g': p = 0; break;
    case '$': case 'G': p = len; break;
    case 'k':
    case 'j': {
        /* first between the rows the line wrapped onto; the selection carries on */
        int q = header_row_move(&app->ui, k, p, motion == 'j' ? 1 : -1);
        if (q >= 0) {
            rv.hc = q;
            header_select(c, k);
            return hreg[k].py[piece_of(k, q)];
        }
        int to = k + (motion == 'j' ? 1 : -1);
        if (to < 0) return hreg[k].y;
        if (to >= nhreg) {
            /* into the body, at its start */
            jd_reading *m = &c->reading[rv.msg];
            clear_selections(c);
            rv.seg = 0;
            if (m->html && !c->plain) jd_html_caret_begin(m->html, 0);
            else m->caret = m->anchor = 0;
            return m->body_y;
        }
        rv.seg = hreg[to].id;
        int tl = (int)strlen(hreg[to].text);
        rv.hc = rv.ha = p < tl ? p : tl;
        header_select(c, to);
        return hreg[to].y;
    }
    default: break;
    }
    rv.hc = p;
    header_select(c, k);
    return hreg[k].y;
}

/* nothing selected: the selection, when it starts, starts at the caret */
static void anchor_at_caret(jd_conv *c)
{
    jd_reading *m = &c->reading[rv.msg];
    if (rv.seg) {
        rv.ha = rv.hc;
        int k = 0;
        while (k < nhreg && hreg[k].id != rv.seg) k++;
        if (k < nhreg) header_select(c, k);
    } else if (m->html && !c->plain) jd_html_caret_anchor(m->html);
    else m->anchor = m->caret, m->sel_from = m->sel_to = 0;
}

/* the keys of visual mode; 1 when one was used */
static int visual_keys(jd_app *app, jd_conv *c)
{
    jd_reading *m = &c->reading[rv.msg];
    int html = m->html && !c->plain, shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    static int pending_g;
    int g = pending_g;
    if (jd_key_went_down()) pending_g = 0;
    /* v starts or ends selecting at the caret; Esc ends selecting, then the caret */
    if ((IsKeyPressed(KEY_V) && !shift) || (IsKeyPressed(KEY_ESCAPE) && rv.selecting)) {
        rv.selecting = IsKeyPressed(KEY_V) ? !rv.selecting : 0;
        anchor_at_caret(c);
        return 1;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        visual_end(c);
        clear_selections(c);
        return 1;
    }
    if (IsKeyPressed(KEY_Y)) {
        copy_selection(app, c);
        rv.selecting = 0;
        anchor_at_caret(c);
        return 1;
    }
    int motion = 0;
    static const struct {
        int key, motion;
    } keys[] = {{KEY_H, 'h'}, {KEY_LEFT, 'h'}, {KEY_L, 'l'}, {KEY_RIGHT, 'l'}, {KEY_J, 'j'}, {KEY_DOWN, 'j'},
                {KEY_K, 'k'}, {KEY_UP, 'k'}, {KEY_W, 'w'}, {KEY_B, 'b'}, {KEY_ZERO, '0'}, {KEY_HOME, '0'}, {KEY_END, '$'}};
    for (size_t k = 0; k < sizeof keys / sizeof *keys && !motion; k++)
        if (IsKeyPressed(keys[k].key) || IsKeyPressedRepeat(keys[k].key)) motion = keys[k].motion;
    if (IsKeyPressed(KEY_FOUR) && shift) motion = '$';
    if (IsKeyPressed(KEY_G)) {
        if (shift) motion = 'G';
        else if (g) motion = 'g';
        else pending_g = 1;
    }
    if (!motion) return IsKeyPressed(KEY_G);
    float caret_y;
    if (rv.seg) {
        caret_y = header_move(app, c, motion);
    } else if (html) {
        float before = jd_html_caret_y(m->html);
        jd_html_caret_move(m->html, motion);
        caret_y = m->body_y + jd_html_caret_y(m->html);
        /* k on the first line goes up into the header lines */
        if (motion == 'k' && jd_html_caret_y(m->html) == before && nhreg) caret_y = header_enter(c);
    } else {
        int before = plain_line(m, m->caret);
        plain_move(app, c, m, motion);
        caret_y = m->body_y + (float)plain_line(m, m->caret) * LINE_H;
        if (motion == 'k' && before == 0 && nhreg) caret_y = header_enter(c);
    }
    /* not selecting: the caret only moves */
    if (!rv.selecting) anchor_at_caret(c);
    /* the caret stays in view */
    if (caret_y < c->top + 20) c->scroll -= c->top + 20 - caret_y;
    if (caret_y + LINE_H > c->top + c->height - 20) c->scroll += caret_y + LINE_H - (c->top + c->height - 20);
    return 1;
}

int conv_keys(jd_app *app, jd_conv *c)
{
    if (!c->n) return 0;
    if (reader_menu.open) return 1;
    int ctrl_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_C) && ctrl_down) return copy_selection(app, c);
    if (rv.active && rv.conv == c && rv.msg < c->n) return visual_keys(app, c);
    /* Esc only drops a selection; leaving the reader is h, Left or Tab */
    if (IsKeyPressed(KEY_ESCAPE)) {
        char *sel = selected_text(c);
        if (sel) clear_selections(c);
        free(sel);
        return sel != NULL;
    }
    if (IsKeyPressed(KEY_V) && !ctrl_down && !(IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
        visual_begin(c);
        return 1;
    }
    text_size(app);
    float page = c->height - 60;
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_J) || IsKeyPressedRepeat(KEY_J) || IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
        c->scroll += LINE_H * 2;
    else if (IsKeyPressed(KEY_K) || IsKeyPressedRepeat(KEY_K) || IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
        c->scroll -= LINE_H * 2;
    else if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_PAGE_DOWN)) c->scroll += shift ? -page : page;
    else if (IsKeyPressed(KEY_PAGE_UP)) c->scroll -= page;
    else if (IsKeyPressed(KEY_O))
        for (int i = 0; i < c->n; i++) c->reading[i].expanded = 1;
    else if (IsKeyPressed(KEY_P) && shift && !ctrl_down) clear_selections(c), c->plain = !c->plain;
    else if (IsKeyPressed(KEY_L) && shift && !ctrl_down) app->page_flip = !app->page_flip;
    else if (IsKeyPressed(KEY_S) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) conv_save_message(app, c);
    else if (IsKeyPressed(KEY_P) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)))
        app->print_tab = (int)(c == preview(app) ? 0 : app->tab) + 1;
    else if (IsKeyPressed(KEY_A) && shift) {
        for (int i = 0; i < c->n; i++) {
            if (!c->reading[i].loaded && c->reading[i].row.has_attachment) load_text(app, &c->reading[i]);
            if (c->reading[i].nfiles > 1) save_all_zip(app, &c->reading[i]);
            else if (c->reading[i].nfiles) save_attachment(app, &c->reading[i], 0);
        }
    } else return 0;
    return 1;
}

static int label_hit(jd_ui *ui, const char *text, float x, float size, int bold)
{
    int n = (int)strlen(text), i = 0;
    char buf[1024];
    while (i < n && i < (int)sizeof buf - 8) {
        int next = i + 1;
        while (next < n && ((unsigned char)text[next] & 0xc0) == 0x80) next++;
        memcpy(buf, text, (size_t)i);
        buf[i] = 0;
        float a = jd_text_width(ui, buf, size, bold);
        memcpy(buf, text, (size_t)next);
        buf[next] = 0;
        if ((a + jd_text_width(ui, buf, size, bold)) / 2 > x) break;
        i = next;
    }
    return i;
}

static float label_x(jd_ui *ui, const char *text, int at, float size, int bold)
{
    char buf[1024];
    if (at > (int)sizeof buf - 1) at = (int)sizeof buf - 1;
    memcpy(buf, text, (size_t)at);
    buf[at] = 0;
    return jd_text_width(ui, buf, size, bold);
}

static void clear_selections(jd_conv *c);

/* a header text's selection after a press at byte at: the word there on a double click, the whole text on a
   triple one, else none yet (a drag makes it) */
static void label_press(const char *text, int at)
{
    int clicks = jd_click_count(), a = at, b = at;
    if (clicks == 2) {
        while (a > 0 && word_char(text[a - 1])) a--;
        while (text[b] && word_char(text[b])) b++;
    } else if (clicks == 3) {
        a = 0, b = (int)strlen(text);
    }
    hl.anchor = hl.from = a, hl.to = b;
    hl.dragging = clicks == 1;
    free(hl.text);
    hl.text = b > a ? jd_strndup(text + a, (size_t)(b - a)) : NULL;
}

/* the reader's menu over header text id; the subject's (ids from 100000) acts on the newest message */
static void label_menu_open(jd_conv *c, int id, const char *text)
{
    menu_conv = c;
    menu_msg = id >= 100000 ? c->n - 1 : id / 16;
    free(menu_link);
    menu_link = NULL;
    free(menu_label);
    menu_label = jd_strdup(text);
    menu_label_id = id;
    jd_popup_open(&reader_menu);
}

/* text drawn at (x, y) in at most w, made selectable as id */
static void sel_label(jd_app *app, jd_conv *c, int id, const char *text, float x, float y, float w, float size, int bold,
                      Rectangle clip)
{
    if (!text || !text[0]) return;
    jd_ui *ui = &app->ui;
    float tw = jd_text_width(ui, text, size, bold);
    Rectangle box = {x, y - 2, tw < w ? tw : w, size + 4};
    Vector2 mp = GetMousePosition();
    int over = CheckCollisionPointRec(mp, box) && CheckCollisionPointRec(mp, clip) && !app->dialog.kind &&
               !app->menu.open && !reader_menu.open && !on_floating();
    if (over) SetMouseCursor(MOUSE_CURSOR_IBEAM);
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        clear_selections(c);
        hl.conv = c, hl.id = id;
        label_press(text, label_hit(ui, text, mp.x - x, size, bold));
    }
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) label_menu_open(c, id, text);
    if (hl.conv != c || hl.id != id) return;
    if (hl.dragging) {
        int at = label_hit(ui, text, mp.x - x, size, bold);
        hl.from = at < hl.anchor ? at : hl.anchor;
        hl.to = at < hl.anchor ? hl.anchor : at;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) hl.dragging = 0;
        free(hl.text);
        hl.text = hl.to > hl.from ? jd_strndup(text + hl.from, (size_t)(hl.to - hl.from)) : NULL;
    }
    if (hl.to > hl.from) {
        float x0 = label_x(ui, text, hl.from, size, bold), x1 = label_x(ui, text, hl.to, size, bold);
        if (x1 > w) x1 = w;
        DrawRectangleV((Vector2){x + x0, y + size * 0.5f - size * 0.65f}, (Vector2){x1 - x0, size * 1.3f},
                       ColorAlpha(JD_NORD10, 0.45f));
    }
}

/* "Ada Lovelace <ada@example>", "ada@example" or "\"Lovelace, Ada\" <ada@example>" split into name and address */
static void split_address(const char *s, int n, char *name, size_t name_cap, char *addr, size_t addr_cap)
{
    while (n && (*s == ' ' || *s == '\t')) s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    const char *lt = memchr(s, '<', (size_t)n);
    if (!lt) {
        snprintf(name, name_cap, "%s", "");
        snprintf(addr, addr_cap, "%.*s", n, s);
        return;
    }
    const char *gt = memchr(lt, '>', (size_t)(n - (lt - s)));
    int an = gt ? (int)(gt - lt - 1) : n - (int)(lt - s) - 1;
    snprintf(addr, addr_cap, "%.*s", an, lt + 1);
    int nn = (int)(lt - s);
    while (nn && s[nn - 1] == ' ') nn--;
    if (nn >= 2 && s[0] == '"' && s[nn - 1] == '"') s++, nn -= 2;
    snprintf(name, name_cap, "%.*s", nn, s);
}

/* the address chip menu: what was right-clicked */
static jd_popup chip_menu;
static jd_conv *chip_conv;
static char chip_name[256], chip_addr[256];

/* From, To and Cc as text: "Name <address>, …" wrapping onto more lines (an address is never split), selectable
   by dragging like the rest of the message; the address under the mouse lights up, and a right click on it
   offers what can be done with that one address */
static float address_line(jd_app *app, jd_conv *c, int id, const char *label, const char *list, float x, float y,
                          float w, Rectangle clip)
{
    if (!list || !list[0]) return y;
    jd_ui *ui = &app->ui;
    float ts = 13 * K, lh = 20 * K, lx = x + 46 * K, right = x + w;
    jd_text(ui, label, x, y, ts, JD_MUTED, 1);

    /* the text shown, and where each address sits in it and on screen */
    enum { MAX_ADDR = 64 };
    struct {
        char name[256], addr[256];
        int start, end;
    } a[MAX_ADDR];
    int n = 0;
    jd_buf shown = {0};
    int quoted = 0, angle = 0;
    const char *start = list;
    for (const char *p = list; n < MAX_ADDR; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == '<') angle = 1;
        else if (*p == '>') angle = 0;
        if (*p && (*p != ',' || quoted || angle)) continue;
        split_address(start, (int)(p - start), a[n].name, sizeof a[n].name, a[n].addr, sizeof a[n].addr);
        if (a[n].addr[0]) {
            if (shown.len) jd_buf_adds(&shown, ", ");
            a[n].start = (int)shown.len;
            if (a[n].name[0]) jd_buf_addf(&shown, "%s <%s>", a[n].name, a[n].addr);
            else jd_buf_adds(&shown, a[n].addr);
            a[n].end = (int)shown.len;
            n++;
        }
        if (!*p) break;
        start = p + 1;
    }
    if (!n) return y + lh;
    const char *text = shown.s;
    int reg = header_register(c, id, text, y, ts) ? nhreg - 1 : -1;
    /* Laid out as pieces of the text: each address with its comma, wrapping before one that does not fit; one too
       wide for a row of its own breaks after its name, its <address> going to the next row. */
    struct {
        int addr, start, end; /* end is before the comma */
        int part;             /* 0 the whole address, 1 its name, 2 its <address> */
        float x, y;
    } pc[MAX_ADDR * 2];
    int npc = 0;
    float cx = lx, cy = y, space = label_x(ui, " ", 1, ts, 0);
    for (int i = 0; i < n; i++) {
        int comma = i + 1 < n;
        float aw = label_x(ui, text + a[i].start, a[i].end - a[i].start + comma, ts, 0);
        if (cx > lx && cx + aw > right) cx = lx, cy += lh;
        if (cx + aw > right && a[i].name[0]) {
            int name_end = a[i].start + (int)strlen(a[i].name) + 1;
            pc[npc++] = (__typeof__(pc[0])){i, a[i].start, name_end, 1, cx, cy};
            header_piece(reg, a[i].start, cx, cy);
            cx = lx, cy += lh;
            pc[npc++] = (__typeof__(pc[0])){i, name_end, a[i].end, 2, cx, cy};
            header_piece(reg, name_end, cx, cy);
            cx += label_x(ui, text + name_end, a[i].end - name_end + comma, ts, 0) + space;
            continue;
        }
        pc[npc++] = (__typeof__(pc[0])){i, a[i].start, a[i].end, 0, cx, cy};
        header_piece(reg, a[i].start, cx, cy);
        cx += aw + space;
    }

    Vector2 mp = GetMousePosition();
    int free_mouse = CheckCollisionPointRec(mp, clip) && !app->dialog.kind && !app->menu.open && !chip_menu.open &&
                     !reader_menu.open && !on_floating();
    int under = -1;
    for (int k = 0; k < npc && free_mouse; k++) {
        float pw = label_x(ui, text + pc[k].start, pc[k].end - pc[k].start, ts, 0);
        if (CheckCollisionPointRec(mp, (Rectangle){pc[k].x, pc[k].y - 2, pw, ts + 4})) under = pc[k].addr;
    }
    /* the place in the text under the mouse, for selecting */
    int at = -1;
    if (free_mouse && mp.y >= y - 2 && mp.y < cy + lh && mp.x >= lx - 4 && mp.x < right) {
        int row_first = -1;
        for (int k = 0; k < npc; k++)
            if (mp.y >= pc[k].y - 2 && mp.y < pc[k].y + lh - 2) {
                if (row_first < 0) row_first = k;
                if (mp.x >= pc[k].x || k == row_first) {
                    int local = mp.x < pc[k].x ? 0 : label_hit(ui, text + pc[k].start, mp.x - pc[k].x, ts, 0);
                    at = pc[k].start + local;
                }
            }
        SetMouseCursor(MOUSE_CURSOR_IBEAM);
    }
    if (at >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        clear_selections(c);
        hl.conv = c, hl.id = id;
        label_press(text, at);
    }
    if (hl.conv == c && hl.id == id && hl.dragging) {
        if (at >= 0) {
            hl.from = at < hl.anchor ? at : hl.anchor;
            hl.to = at < hl.anchor ? hl.anchor : at;
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) hl.dragging = 0;
        free(hl.text);
        hl.text = hl.to > hl.from ? jd_strndup(text + hl.from, (size_t)(hl.to - hl.from)) : NULL;
    }
    if (under >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        snprintf(chip_name, sizeof chip_name, "%s", a[under].name);
        snprintf(chip_addr, sizeof chip_addr, "%s", a[under].addr);
        chip_conv = c;
        jd_popup_open(&chip_menu);
    } else if (at >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        label_menu_open(c, id, text);
    }

    int sel = hl.conv == c && hl.id == id && hl.to > hl.from;
    for (int k = 0; k < npc; k++) {
        int i = pc[k].addr, last = pc[k].part != 1, comma = last && i + 1 < n;
        int s = pc[k].start, e = pc[k].end + comma;
        if (sel && hl.from < e && hl.to > s) {
            int f = hl.from > s ? hl.from : s, t = hl.to < e ? hl.to : e;
            float x0 = label_x(ui, text + s, f - s, ts, 0), x1 = label_x(ui, text + s, t - s, ts, 0);
            DrawRectangleV((Vector2){pc[k].x + x0, pc[k].y + ts * 0.5f - ts * 0.65f}, (Vector2){x1 - x0, ts * 1.3f},
                           ColorAlpha(JD_NORD10, 0.55f));
        }
        float px = pc[k].x;
        int lit = i == under;
        char part[300];
        /* the name in the text colour, the <address> muted */
        if (a[i].name[0] && pc[k].part != 2) {
            snprintf(part, sizeof part, "%s ", a[i].name);
            jd_text(ui, part, px, pc[k].y, ts, lit ? JD_NORD8 : JD_NORD6, 0);
            px += label_x(ui, part, (int)strlen(part), ts, 0);
        }
        if (pc[k].part != 1) {
            if (a[i].name[0]) snprintf(part, sizeof part, "<%s>", a[i].addr);
            else snprintf(part, sizeof part, "%s", a[i].addr);
            jd_text(ui, part, px, pc[k].y, ts, lit ? JD_NORD8 : a[i].name[0] ? JD_MUTED : JD_NORD6, 0);
            px += label_x(ui, part, (int)strlen(part), ts, 0);
        }
        if (comma) jd_text(ui, ",", px, pc[k].y, ts, JD_MUTED, 0);
    }
    if (header_caret_here(c, id)) {
        int k = 0;
        while (k + 1 < npc && pc[k + 1].start <= rv.hc) k++;
        float px = pc[k].x + label_x(ui, text + pc[k].start, rv.hc - pc[k].start, ts, 0);
        DrawRectangleV((Vector2){px - 1, pc[k].y + ts * 0.5f - ts * 0.625f}, (Vector2){2, ts * 1.25f}, JD_NORD8);
    }
    jd_buf_free(&shown);
    return cy + lh;
}

static void chip_menu_draw(jd_app *app, jd_conv *c)
{
    if (chip_conv != c || !chip_menu.open) return;
    char *sel = selected_text(c);
    jd_popup_item items[] = {
        {"Copy address", "copy", NULL, 0},
        {"Copy name and address", "user", NULL, !chip_name[0]},
        {"Copy selection", "text-cursor-input", "Ctrl+C", !sel},
        {"New message to", "pencil", NULL, 0},
    };
    free(sel);
    switch (jd_popup_draw(&app->ui, &chip_menu, items, 4)) {
    case 0:
        SetClipboardText(chip_addr);
        notify_user(app, "Copied %s", chip_addr);
        break;
    case 1: {
        char *both = jd_printf("%s <%s>", chip_name, chip_addr);
        SetClipboardText(both);
        notify_user(app, "Copied %s", both);
        free(both);
        break;
    }
    case 2: copy_selection(app, c); break;
    case 3:
        compose_open(app, COMPOSE_NEW, 0);
        if (app->tabs[app->tab].kind == TAB_COMPOSE) compose_set_to(app->tabs[app->tab].compose, chip_addr);
        break;
    default: break;
    }
}

static float header_line(jd_app *app, jd_conv *c, int id, const char *label, const char *value, float x, float y, float w,
                         Rectangle clip)
{
    if (!value || !value[0]) return y;
    jd_text(&app->ui, label, x, y, 13 * K, JD_MUTED, 1);
    jd_text_fit(&app->ui, value, x + 46 * K, y, w - 46 * K, 13 * K, JD_NORD4, 0);
    sel_label(app, c, id, value, x + 46 * K, y, w - 46 * K, 13 * K, 0, clip);
    if (header_register(c, id, value, y, 13 * K)) header_piece(nhreg - 1, 0, x + 46 * K, y);
    if (header_caret_here(c, id)) {
        float ts = 13 * K, px = x + 46 * K + label_x(&app->ui, value, rv.hc, ts, 0);
        DrawRectangleV((Vector2){px - 1, y + ts * 0.5f - ts * 0.625f}, (Vector2){2, ts * 1.25f}, JD_NORD8);
    }
    return y + 20 * K;
}

static int clicked(Rectangle box, Rectangle clip, jd_app *app)
{
    return IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), box) &&
           CheckCollisionPointRec(GetMousePosition(), clip) && !app->dialog.kind && !on_floating();
}

static char *sender_address(const jd_reading *m);

/* the addresses of a list as typed ("a <x>, \"b, c\" <y>"): how many, and the first one's name (else address) */
static int count_addresses(const char *list, char *first, size_t first_cap)
{
    int n = 0, quoted = 0, angle = 0;
    const char *start = list;
    first[0] = 0;
    for (const char *p = list; list; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == '<') angle = 1;
        else if (*p == '>') angle = 0;
        if (*p && (*p != ',' || quoted || angle)) continue;
        char name[256], addr[256];
        split_address(start, (int)(p - start), name, sizeof name, addr, sizeof addr);
        if (addr[0]) {
            if (!n++) snprintf(first, first_cap, "%s", name[0] ? name : addr);
        }
        if (!*p) break;
        start = p + 1;
    }
    return n;
}

/* One line in place of From, To, Cc and In: the sender's address, the first recipient and how many more, the
   folder. A click shows them all. */
static float address_summary(jd_app *app, jd_reading *m, float x, float y, float w, Rectangle clip)
{
    jd_ui *ui = &app->ui;
    float ts = 13 * K, lh = 20 * K;
    char first[256], first_cc[256], line[1024];
    int nto = count_addresses(m->row.to, first, sizeof first), ncc = count_addresses(m->row.cc, first_cc, sizeof first_cc);
    char *from = sender_address(m);
    int more = nto + ncc - 1;
    char others[32] = "";
    if (more > 0) snprintf(others, sizeof others, ", +%d", more);
    snprintf(line, sizeof line, "%s  to  %s%s  ·  %s", from, nto ? first : first_cc[0] ? first_cc : "nobody", others,
             m->row.folder_name ? m->row.folder_name : "");
    free(from);
    Rectangle row = {x, y - 2, w, lh};
    int hover = CheckCollisionPointRec(GetMousePosition(), row) && CheckCollisionPointRec(GetMousePosition(), clip);
    if (hover) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    jd_icon("chevron-right", x - 2, y, ts, hover ? JD_NORD8 : JD_MUTED);
    jd_text_fit(ui, line, x + 18 * K, y, w - 18 * K, ts, hover ? JD_NORD4 : JD_MUTED, 0);
    if (clicked(row, clip, app)) m->addresses = 1;
    return y + lh;
}

static void size_text(size_t bytes, char *out, int len)
{
    if (bytes < 1024) snprintf(out, (size_t)len, "%zu B", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, (size_t)len, "%.0f kB", bytes / 1024.0);
    else snprintf(out, (size_t)len, "%.1f MB", bytes / 1048576.0);
}

/* attachment chips; returns the y below them */
static char *sender_address(const jd_reading *m)
{
    const char *from = m->row.from ? m->row.from : "";
    const char *lt = strrchr(from, '<');
    return lt ? jd_strndup(lt + 1, strcspn(lt + 1, ">")) : jd_strdup(from);
}

/* one image fetched (or found in the cache): into every open message that refers to it */
void images_arrived(jd_app *app, const char *url, int ok)
{
    if (!ok || !app->fetcher) return;
    char *path = jd_fetch_cache_path(app->fetcher, url);
    size_t len;
    char *data = jd_read_file(path, &len);
    free(path);
    if (!data) return;
    for (int t = 0; t < app->ntabs; t++)
        for (int i = 0; i < app->tabs[t].conv.n; i++) {
            jd_reading *m = &app->tabs[t].conv.reading[i];
            if (!m->html) continue;
            for (int k = 0; k < jd_html_remote_count(m->html); k++)
                if (strcmp(jd_html_remote_url(m->html, k), url) == 0) {
                    jd_html_add_image(m->html, url, data, len);
                    m->html_w = 0;
                }
        }
    free(data);
}

static void request_images(jd_app *app, jd_reading *m)
{
    m->images_asked = 1;
    for (int k = 0; app->fetcher && k < jd_html_remote_count(m->html); k++) jd_fetch(app->fetcher, jd_html_remote_url(m->html, k));
}

/* Remote images load when allowed (always, from a trusted sender, or this once); otherwise a bar asks. */
static float remote_images(jd_app *app, jd_reading *m, float x, float y, float w, Rectangle clip)
{
    int n = jd_html_remote_count(m->html);
    if (!n) return y;
    char *addr = sender_address(m);
    int allowed = app->prefs.remote_images || m->load_images || jd_prefs_trusts_images(&app->prefs, addr);
    if (allowed) {
        if (!m->images_asked) request_images(app, m);
        free(addr);
        return y;
    }
    jd_ui *ui = &app->ui;
    char line[96];
    snprintf(line, sizeof line, "%d remote image%s not loaded", n, n == 1 ? "" : "s");
    float bx = x + 24 + 20 * K + jd_text_width(ui, line, 13 * K, 0);
    const char *once = "Load images", *always = "Always from this sender";
    float ow = jd_text_width(ui, once, 15, 1) + 20, aw = jd_text_width(ui, always, 15, 1) + 20;
    /* a narrow pane (a phone) puts the buttons on a row of their own under the line */
    int below = bx + ow > x + w - 8;
    if (below) bx = x + 12;
    if (bx + ow + aw + 8 > x + w - 8) aw = 0;
    float bar_h = below ? 40 * K + 36 : 40 * K;
    Rectangle bar = {x, y, w, bar_h};
    DrawRectangleRounded(bar, 0.2f / (below ? 2 : 1), 6, JD_NORD0);
    jd_icon("eye-off", x + 12, y + 12 * K, 16 * K, JD_NORD13);
    jd_text(ui, line, x + 18 + 20 * K, y + 12 * K, 13 * K, JD_NORD6, 0);
    int hover_clip = CheckCollisionPointRec(GetMousePosition(), clip);
    float by = below ? y + 40 * K : y + (40 * K - 28) / 2;
    if (jd_button(ui, once, (Rectangle){bx, by, ow, 28}, 0) && hover_clip) {
        m->load_images = 1;
        request_images(app, m);
    }
    if (aw && jd_button(ui, always, (Rectangle){bx + ow + 8, by, aw, 28}, 0) && hover_clip) {
        jd_prefs_trust_images(&app->prefs, addr, 1);
        save_prefs(app);
        m->load_images = 1;
        request_images(app, m);
        notify_user(app, "Images from %s load from now on (Settings, Reading).", addr);
    }
    free(addr);
    return y + bar_h + 10;
}

static float draw_files(jd_app *app, jd_reading *m, float x, float y, float w, Rectangle clip)
{
    jd_ui *ui = &app->ui;
    float ch = 28 * K, ts = 13 * K, is = 14 * K, ty = (ch - ts) / 2, iy = (ch - is) / 2;
    /* more than one row of them (several files, or an image preview) folds into one chip until opened */
    int thumbs = 0;
    int64_t total = 0;
    for (int f = 0; f < m->nfiles; f++) thumbs += m->thumbs && m->thumbs[f].id, total += m->files[f].size;
    if (m->nfiles > 1 || thumbs) {
        char size[32], label[96];
        size_text(total, size, sizeof size);
        snprintf(label, sizeof label, "%d attachment%s · %s", m->nfiles, m->nfiles == 1 ? "" : "s", size);
        float lw = jd_text_width(ui, label, ts, 0), cw = lw + is + is + 36;
        Rectangle chip = {x, y, cw, ch};
        int hover = CheckCollisionPointRec(GetMousePosition(), chip) && CheckCollisionPointRec(GetMousePosition(), clip);
        DrawRectangleRounded(chip, 0.3f, 6, hover ? JD_NORD3 : JD_NORD2);
        jd_icon("paperclip", x + 8, y + iy, is, JD_NORD4);
        jd_text(ui, label, x + 14 + is, y + ty, ts, JD_NORD6, 0);
        jd_icon(m->files_open ? "chevron-down" : "chevron-right", x + cw - is - 10, y + iy, is, hover ? JD_NORD8 : JD_NORD4);
        if (clicked(chip, clip, app)) m->files_open = !m->files_open;
        y += ch + 8;
        if (!m->files_open) return y + 4;
    }
    /* image previews first, up to 240x160 each, a click saves the image */
    float tx = x, row_h = 0;
    for (int f = 0; m->thumbs && f < m->nfiles; f++) {
        Texture2D t = m->thumbs[f];
        if (!t.id) continue;
        float s = 240.0f / (float)t.width;
        if ((float)t.height * s > 160) s = 160.0f / (float)t.height;
        if (s > 1) s = 1;
        float tw = (float)t.width * s, th = (float)t.height * s;
        if (tx + tw > x + w && tx > x) tx = x, y += row_h + 8, row_h = 0;
        Rectangle box = {tx, y, tw, th};
        DrawTexturePro(t, (Rectangle){0, 0, (float)t.width, (float)t.height}, box, (Vector2){0, 0}, 0, WHITE);
        int hover = CheckCollisionPointRec(GetMousePosition(), box) && CheckCollisionPointRec(GetMousePosition(), clip);
        DrawRectangleLinesEx(box, 1, hover ? JD_NORD8 : JD_NORD2);
        Rectangle dl = {tx + tw - 30, y + 4, 26, 26};
        if (hover) {
            DrawRectangleRounded(dl, 0.3f, 6, ColorAlpha(JD_NORD0, 0.85f));
            jd_icon("download", dl.x + 5, dl.y + 5, 16, CheckCollisionPointRec(GetMousePosition(), dl) ? JD_NORD8 : JD_NORD4);
        }
        if (clicked(dl, clip, app)) save_attachment(app, m, f);
        else if (clicked(box, clip, app)) open_attachment(app, m, f);
        if (th > row_h) row_h = th;
        tx += tw + 8;
    }
    if (row_h) y += row_h + 10;
    float cx = x;
    for (int f = 0; f < m->nfiles; f++) {
        char size[32], label[300];
        size_text(m->files[f].size, size, sizeof size);
        snprintf(label, sizeof label, "%s  %s", m->files[f].name, size);
        float lw = jd_text_width(ui, label, ts, 0);
        if (lw > w - 4 * ch) lw = w - 4 * ch;
        float cw = lw + 2 * ch;
        if (cx + cw > x + w && cx > x) cx = x, y += ch + 6;
        Rectangle chip = {cx, y, cw, ch};
        int hover = CheckCollisionPointRec(GetMousePosition(), chip) && CheckCollisionPointRec(GetMousePosition(), clip);
        DrawRectangleRounded(chip, 0.3f, 6, hover ? JD_NORD3 : JD_NORD2);
        jd_icon("paperclip", cx + 8, y + iy, is, JD_NORD4);
        jd_text_fit(ui, label, cx + 14 + is, y + ty, lw, ts, JD_NORD6, 0);
        Rectangle dl = {cx + cw - ch, y, ch, ch};
        int on_dl = hover && CheckCollisionPointRec(GetMousePosition(), dl);
        if (on_dl) DrawRectangleRounded(dl, 0.3f, 6, JD_NORD1);
        jd_icon("download", dl.x + iy, dl.y + iy, is, on_dl ? JD_NORD8 : hover ? JD_NORD4 : JD_MUTED);
        if (clicked(dl, clip, app)) save_attachment(app, m, f);
        else if (clicked(chip, clip, app)) open_attachment(app, m, f);
        if (hover && !plat_touch()) jd_tooltip(on_dl ? "Save to Downloads" : "Open", on_dl ? dl.x : cx, y + ch + 4);
        cx += cw + 8;
    }
    if (m->nfiles > 1) {
        const char *all = "Download all (.zip)";
        float cw = jd_text_width(ui, all, ts, 1) + is + 26;
        if (cx + cw > x + w && cx > x) cx = x, y += ch + 6;
        Rectangle chip = {cx, y, cw, ch};
        int hover = CheckCollisionPointRec(GetMousePosition(), chip) && CheckCollisionPointRec(GetMousePosition(), clip);
        DrawRectangleRounded(chip, 0.3f, 6, hover ? JD_NORD3 : JD_NORD2);
        jd_icon("download", cx + 10, y + iy, is, JD_NORD8);
        jd_text(ui, all, cx + 16 + is, y + ty, ts, JD_NORD6, 1);
        if (clicked(chip, clip, app)) save_all_zip(app, m);
    }
    return m->nfiles ? y + ch + 12 : y;
}

/* Selecting text in the reader: a drag in a message's body selects (HTML through litehtml's laid-out text,
   plain text by its wrapped lines); a click without dragging follows a link. One selection at a time. */
static struct {
    int active, moved, msg;
    jd_conv *conv;
    Vector2 start; /* relative to the body's top left */
} drag;

int reader_visual(void)
{
    return rv.active && rv.selecting;
}

int reader_caret(void)
{
    return rv.active && !rv.selecting;
}

static void visual_end(jd_conv *c)
{
    if (rv.active && rv.conv == c && rv.msg < c->n && c->reading[rv.msg].html) jd_html_caret_end(c->reading[rv.msg].html);
    if (rv.conv == c) rv.active = 0;
}

static float span(jd_ui *ui, const char *text, int from, int to)
{
    if (to <= from) return 0;
    char stack[512], *buf = to - from < (int)sizeof stack ? stack : malloc((size_t)(to - from + 1));
    memcpy(buf, text + from, (size_t)(to - from));
    buf[to - from] = 0;
    float w = jd_text_width(ui, buf, TEXT_SIZE, 0);
    if (buf != stack) free(buf);
    return w;
}

/* the link [*a, *b) covering byte at (one wrapped onto this line included), or the next starting before limit */
static int url_from(const char *t, int at, int limit, int *a, int *b)
{
    int s = at;
    while (s > 0 && at - s < 2048 && !isspace((unsigned char)t[s - 1]) && !strchr("<>\"'", t[s - 1])) s--;
    for (int i = s; i < limit; i++) {
        int e = jd_url_end(t, i);
        if (e > i && e > at) {
            *a = i, *b = e;
            return 1;
        }
        if (e > i) i = e - 1;
    }
    return 0;
}

/* the byte of plain text m under (x, y), relative to the text's top left; lines past shown are folded away */
static int plain_hit(jd_ui *ui, const jd_reading *m, float x, float y, int shown)
{
    if (!shown) return 0;
    int l = y < 0 ? 0 : (int)(y / LINE_H);
    if (l >= shown) l = shown - 1;
    int tlen = (int)strlen(m->text), from = m->line_starts[l], to = l + 1 < m->nlines ? m->line_starts[l + 1] : tlen;
    if (y >= (float)shown * LINE_H) return to;
    while (to > from && (m->text[to - 1] == '\n' || m->text[to - 1] == ' ')) to--;
    int i = from;
    while (i < to) {
        int next = i + 1;
        while (next < to && ((unsigned char)m->text[next] & 0xc0) == 0x80) next++;
        if ((span(ui, m->text, from, i) + span(ui, m->text, from, next)) / 2 > x) break;
        i = next;
    }
    return i;
}

/* the link under (x, y) in plain text m, malloc'd; NULL when there is none (past a line's end is none) */
static char *plain_link_at(jd_ui *ui, const jd_reading *m, float x, float y, int shown)
{
    if (!shown || y < 0 || y >= (float)shown * LINE_H || x < 0) return NULL;
    int l = (int)(y / LINE_H), tlen = (int)strlen(m->text);
    int from = m->line_starts[l], to = l + 1 < m->nlines ? m->line_starts[l + 1] : tlen;
    if (x > span(ui, m->text, from, to)) return NULL;
    int at = plain_hit(ui, m, x, y, shown), a, b;
    if (at > from && at == to) at--;
    if (!url_from(m->text, at, at + 1, &a, &b) || at < a || at >= b) return NULL;
    return jd_printf("%.*s", b - a, m->text + a);
}

static void clear_selections(jd_conv *c)
{
    for (int i = 0; i < c->n; i++) {
        c->reading[i].sel_from = c->reading[i].sel_to = 0;
        if (c->reading[i].html) jd_html_select_none(c->reading[i].html);
    }
    if (hl.conv == c) {
        hl.conv = NULL, hl.from = hl.to = 0;
        free(hl.text);
        hl.text = NULL;
    }
}

/* the selected text of c, or NULL; free() it */
static char *selected_text(jd_conv *c)
{
    if (hl.conv == c && hl.text) return jd_strdup(hl.text);
    for (int i = 0; i < c->n; i++) {
        jd_reading *m = &c->reading[i];
        if (m->html && !c->plain && jd_html_has_selection(m->html)) return jd_html_selection(m->html);
        if (m->text && m->sel_to > m->sel_from) return jd_strndup(m->text + m->sel_from, (size_t)(m->sel_to - m->sel_from));
    }
    return NULL;
}

static int copy_selection(jd_app *app, jd_conv *c)
{
    char *s = selected_text(c);
    if (!s) return 0;
    SetClipboardText(s);
    notify_user(app, "Copied.");
    free(s);
    return 1;
}

/* Selecting by touch, as Android does: a long press selects the word under it and puts a handle at each end; a
   finger put on a handle drags that end while the other stays; a bar by the selection copies, selects all or
   shares. Positions are relative to the body's text, its top left at (ox, oy) on the screen. */
#define HANDLE_R 11.0f
static struct {
    int active, msg, html, drag; /* drag: the end the finger moves, 1 start or 2 end; 0 none */
    int was_down;
    jd_conv *conv;
    float ox, oy;
    Vector2 a, b; /* the ends: x and the top of their line */
    float lh;
    Vector2 fixed; /* the end not dragged, mid-line */
    Vector2 grab;  /* finger minus the dragged end, mid-line */
    Vector2 press; /* where the long press was: the word stays selected until the finger leaves it */
    int moved;
    Rectangle bar;
} ts;

/* the system's actions on text (Translate, ...), read again for each selection, behind the bar's last button */
static struct {
    char *labels;
    const char *label[16];
    int n;
    jd_popup menu;
} text_actions;

static void text_actions_load(void)
{
    free(text_actions.labels);
    text_actions.labels = plat_text_actions();
    text_actions.n = 0;
    for (char *p = text_actions.labels, *nl; p && *p && text_actions.n < 16; p = nl + 1) {
        nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        text_actions.label[text_actions.n++] = p;
    }
}

static int touch_ends(jd_app *app, jd_reading *m, int html)
{
    if (html) return jd_html_selection_ends(m->html, &ts.a.x, &ts.a.y, &ts.b.x, &ts.b.y, &ts.lh);
    if (m->sel_to <= m->sel_from || !m->line_starts) return 0;
    ts.lh = TEXT_SIZE * 1.25f;
    int la = plain_line(m, m->sel_from), lb = plain_line(m, m->sel_to);
    float top = TEXT_SIZE * 0.5f - ts.lh / 2;
    ts.a = (Vector2){span(&app->ui, m->text, m->line_starts[la], m->sel_from), (float)la * LINE_H + top};
    ts.b = (Vector2){span(&app->ui, m->text, m->line_starts[lb], m->sel_to), (float)lb * LINE_H + top};
    return 1;
}

static void touch_select_between(jd_app *app, jd_reading *m, int html, int shown, Vector2 p, Vector2 q)
{
    if (html) {
        jd_html_select(m->html, p.x, p.y, q.x, q.y);
        return;
    }
    int a = plain_hit(&app->ui, m, p.x, p.y, shown), b = plain_hit(&app->ui, m, q.x, q.y, shown);
    m->sel_from = a < b ? a : b;
    m->sel_to = a < b ? b : a;
}

static int select_word(jd_app *app, jd_reading *m, int html, int shown, Vector2 at)
{
    if (html) return jd_html_select_word(m->html, at.x, at.y);
    if (at.y < 0 || at.y >= (float)shown * LINE_H) return 0;
    int k = plain_hit(&app->ui, m, at.x, at.y, shown), a = k, b = k;
    while (a > 0 && word_char(m->text[a - 1])) a--;
    while (m->text[b] && word_char(m->text[b])) b++;
    if (a == b) return 0;
    m->sel_from = a, m->sel_to = b;
    return 1;
}

/* the line under at: HTML's line as laid out, plain text's up to its line breaks */
static int select_line(jd_app *app, jd_reading *m, int html, int shown, Vector2 at)
{
    if (html) return jd_html_select_line(m->html, at.x, at.y);
    if (at.y < 0 || at.y >= (float)shown * LINE_H) return 0;
    int k = plain_hit(&app->ui, m, at.x, at.y, shown), a = k, b = k;
    while (a > 0 && m->text[a - 1] != '\n') a--;
    while (m->text[b] && m->text[b] != '\n' && m->text[b] != '\r') b++;
    if (a == b) return 0;
    m->sel_from = a, m->sel_to = b;
    return 1;
}

/* the knob of end e (1 start, 2 end) on the screen: it hangs below the line, outward from the selection */
static Vector2 touch_knob(int e)
{
    Vector2 p = e == 1 ? ts.a : ts.b;
    return (Vector2){ts.ox + p.x + (e == 1 ? -HANDLE_R : HANDLE_R), ts.oy + p.y + ts.lh + HANDLE_R};
}

static void touch_mouse(jd_app *app, jd_conv *c, int i, Rectangle clip, float ox, float oy, int html, int shown)
{
    jd_reading *m = &c->reading[i];
    if (!ts.active || ts.conv != c || ts.msg != i) return;
    ts.ox = ox, ts.oy = oy;
    if (!touch_ends(app, m, html)) {
        ts.active = 0;
        return;
    }
    float fx, fy;
    int down = plat_finger(&fx, &fy);
    if (down && !ts.was_down && !ts.drag)
        for (int e = 1; e <= 2; e++) {
            Vector2 k = touch_knob(e);
            if (hypotf(fx - k.x, fy - k.y) > HANDLE_R * 2.2f) continue;
            plat_finger_claim();
            Vector2 mine = e == 1 ? ts.a : ts.b, other = e == 1 ? ts.b : ts.a;
            ts.drag = e, ts.moved = 1;
            ts.fixed = (Vector2){other.x, other.y + ts.lh / 2};
            ts.grab = (Vector2){fx - ox - mine.x, fy - oy - mine.y - ts.lh / 2};
            break;
        }
    ts.was_down = down;
    if (!down) {
        ts.drag = 0;
        return;
    }
    if (!ts.drag) return;
    if (!ts.moved && hypotf(fx - ts.press.x, fy - ts.press.y) < HANDLE_R) return;
    ts.moved = 1;
    touch_select_between(app, m, html, shown, ts.fixed, (Vector2){fx - ts.grab.x - ox, fy - ts.grab.y - oy});
    if (fy < clip.y + 3 * HANDLE_R) c->scroll -= 6;
    else if (fy > clip.y + clip.height - 3 * HANDLE_R) c->scroll += 6;
    touch_ends(app, m, html);
}

static void touch_handles_draw(jd_app *app, jd_conv *c, Rectangle r)
{
    if (!ts.active || ts.conv != c) return;
    jd_ui *ui = &app->ui;
    jd_scissor(r.x, r.y, r.width, r.height);
    for (int e = 1; e <= 2; e++) {
        Vector2 k = touch_knob(e);
        DrawCircleV(k, HANDLE_R, JD_NORD8);
        DrawRectangleV((Vector2){e == 1 ? k.x : k.x - HANDLE_R, k.y - HANDLE_R}, (Vector2){HANDLE_R, HANDLE_R}, JD_NORD8);
    }
    jd_scissor_end();
    ts.bar = (Rectangle){0};
    if (ts.drag) return;
    static const char *labels[] = {"Copy", "Select all", "Share", NULL};
    int nb = text_actions.n ? 4 : 3;
    float size = 14, pad = 14, h = 40, w = 0, lw[4];
    for (int k = 0; k < nb; k++) w += lw[k] = labels[k] ? jd_text_width(ui, labels[k], size, 1) + 2 * pad : h;
    float ay = ts.oy + ts.a.y, by = ts.oy + ts.b.y + ts.lh + 2 * HANDLE_R;
    float y = ay - h - 10 >= r.y + 4 ? ay - h - 10 : by + 10;
    if (y + h > r.y + r.height - 4) y = r.y + 4;
    float x = ts.ox + ts.a.x - w / 2;
    if (x + w > r.x + r.width - 8) x = r.x + r.width - 8 - w;
    if (x < r.x + 8) x = r.x + 8;
    ts.bar = (Rectangle){x, y, w, h};
    DrawRectangleRounded((Rectangle){x, y + 2, w, h}, 0.3f, 8, ColorAlpha(BLACK, 0.25f));
    DrawRectangleRounded(ts.bar, 0.3f, 8, JD_NORD2);
    int choice = -1;
    for (int k = 0; k < nb; k++) {
        Rectangle b = {x, y, lw[k], h};
        int over = !text_actions.menu.open && CheckCollisionPointRec(GetMousePosition(), b);
        if (over && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) DrawRectangleRounded(b, 0.3f, 8, JD_NORD3);
        if (labels[k]) jd_text(ui, labels[k], x + pad, y + (h - size) / 2, size, JD_NORD6, 1);
        else jd_icon("ellipsis-vertical", x + (h - 18) / 2, y + (h - 18) / 2, 18, JD_NORD6);
        if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) choice = k;
        x += lw[k];
    }
    if (text_actions.menu.open) {
        jd_popup_item items[16];
        for (int k = 0; k < text_actions.n; k++) items[k] = (jd_popup_item){text_actions.label[k], NULL, NULL, 0};
        int pick = jd_popup_draw(ui, &text_actions.menu, items, text_actions.n);
        if (pick >= 0) {
            char *s = selected_text(c);
            if (s && plat_text_action(pick, s) != 0) notify_user(app, "That app did not open.");
            free(s);
        }
    }
    if (choice < 0) return;
    if (choice == 3) {
        jd_popup_open(&text_actions.menu);
        return;
    }
    jd_reading *m = &c->reading[ts.msg];
    if (choice == 0) {
        copy_selection(app, c);
        clear_selections(c);
        ts.active = 0;
    } else if (choice == 1) {
        clear_selections(c);
        if (ts.html) jd_html_select_all(m->html);
        else if (m->text) m->sel_from = 0, m->sel_to = (int)strlen(m->text);
    } else {
        char *s = selected_text(c);
        if (s && plat_share_text(s) != 0) notify_user(app, "Nothing here can share text.");
        free(s);
    }
}

/* the mouse over message i's body (area, its text's top left at ox, oy): select, follow links, open the menu */
static void text_mouse(jd_app *app, jd_conv *c, int i, Rectangle area, Rectangle clip, float ox, float oy, int html,
                       int shown)
{
    jd_reading *m = &c->reading[i];
    Vector2 mp = GetMousePosition(), at = {mp.x - ox, mp.y - oy};
    int free_mouse = !app->dialog.kind && !app->menu.open && !reader_menu.open && !on_floating();
    int over = free_mouse && CheckCollisionPointRec(mp, area) && CheckCollisionPointRec(mp, clip);
    if (app->ui.touch) {
        touch_mouse(app, c, i, clip, ox, oy, html, shown);
        if (ts.active && (CheckCollisionPointRec(mp, ts.bar) || text_actions.menu.open)) over = 0;
        if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            char *link = html ? jd_html_link_at(m->html, at.x, at.y) : NULL;
            int on_link = link != NULL;
            free(link);
            if (!on_link) {
                visual_end(c);
                clear_selections(c);
                if (select_word(app, m, html, shown, at)) {
                    plat_finger_claim();
                    ts = (typeof(ts)){.active = 1, .msg = i, .html = html, .drag = 2, .was_down = 1, .conv = c,
                                      .ox = ox, .oy = oy, .press = mp};
                    touch_ends(app, m, html);
                    ts.fixed = (Vector2){ts.a.x, ts.a.y + ts.lh / 2};
                    text_actions_load();
                    return;
                }
            }
        }
    }
    /* Ctrl + click opens a link (a plain click selects, as on any text); a phone's tap opens it on release */
    int ctrl = !app->ui.touch && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
    char *link = over && ctrl ? (html ? jd_html_link_at(m->html, at.x, at.y) : plain_link_at(&app->ui, m, at.x, at.y, shown)) : NULL;
    if (over) SetMouseCursor(link ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_IBEAM);
    if (link && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        follow_link(app, link);
        free(link);
        return;
    }
    free(link);
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        visual_end(c);
        clear_selections(c);
        int clicks = jd_click_count();
        if (clicks == 2 && select_word(app, m, html, shown, at)) return;
        if (clicks == 3 && select_line(app, m, html, shown, at)) return;
        drag.active = 1, drag.moved = 0, drag.msg = i, drag.conv = c, drag.start = at;
    }
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        menu_conv = c, menu_msg = i;
        free(menu_link);
        menu_link = html ? jd_html_link_at(m->html, at.x, at.y) : plain_link_at(&app->ui, m, at.x, at.y, shown);
        free(menu_label);
        menu_label = NULL;
        jd_popup_open(&reader_menu);
    }
    if (!drag.active || drag.conv != c || drag.msg != i) return;
    if (fabsf(at.x - drag.start.x) + fabsf(at.y - drag.start.y) > 4) drag.moved = 1;
    if (drag.moved) {
        if (html) jd_html_select(m->html, drag.start.x, drag.start.y, at.x, at.y);
        else {
            int a = plain_hit(&app->ui, m, drag.start.x, drag.start.y, shown), b = plain_hit(&app->ui, m, at.x, at.y, shown);
            m->sel_from = a < b ? a : b;
            m->sel_to = a < b ? b : a;
        }
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!drag.moved && app->ui.touch) {
            char *url = html ? jd_html_link_at(m->html, drag.start.x, drag.start.y)
                             : plain_link_at(&app->ui, m, drag.start.x, drag.start.y, shown);
            if (url) follow_link(app, url);
            free(url);
        }
        drag.active = 0;
    }
}

/* the right-click menu over a message's body */
static void reader_menu_draw(jd_app *app, jd_conv *c)
{
    if (menu_conv != c || !reader_menu.open) return;
    if (menu_msg >= c->n) {
        reader_menu.open = 0;
        return;
    }
    char *sel = selected_text(c);
    jd_popup_item items[] = {
        {"Copy", "copy", "Ctrl+C", !sel},
        {"Select all", "text-cursor-input", NULL, 0},
        {NULL, NULL, NULL, 0},
        {"Open link", "external-link", NULL, !menu_link},
        {"Copy link address", "link", NULL, !menu_link},
        {NULL, NULL, NULL, 0},
        {"Reply", "reply", "r", 0},
        {"Reply all", "reply-all", "R", 0},
        {"Forward", "forward", "F", 0},
    };
    free(sel);
    int n = (int)(sizeof items / sizeof *items);
    int choice = jd_popup_draw(&app->ui, &reader_menu, items, n);
    jd_reading *m = &c->reading[menu_msg];
    switch (choice) {
    case 0: copy_selection(app, c); break;
    case 1:
        clear_selections(c);
        if (menu_label) {
            hl.conv = c, hl.id = menu_label_id, hl.dragging = 0;
            hl.anchor = hl.from = 0, hl.to = (int)strlen(menu_label);
            free(hl.text);
            hl.text = jd_strdup(menu_label);
        } else if (m->html && !c->plain) jd_html_select_all(m->html);
        else if (m->text) m->sel_from = 0, m->sel_to = (int)strlen(m->text);
        break;
    case 3: follow_link(app, menu_link); break;
    case 4:
        SetClipboardText(menu_link);
        notify_user(app, "Copied the link.");
        break;
    case 6: compose_open(app, COMPOSE_REPLY, m->row.id); break;
    case 7: compose_open(app, COMPOSE_REPLY_ALL, m->row.id); break;
    case 8: compose_open(app, COMPOSE_FORWARD, m->row.id); break;
    default: break;
    }
}

/* An open card's actions, on its message alone (the toolbar acts on the conversation): icons on the header, the
   rest behind its ⋮ */
enum { CA_REPLY, CA_REPLY_ALL, CA_FORWARD, CA_READ, CA_FLAG, CA_DELETE, CA_ARCHIVE, CA_MOVE, CA_COPY, CA_TAB, CA_SAVE,
       CA_PRINT, CA_COUNT };
static jd_popup card_menu;
static jd_conv *card_conv;
static int card_msg, card_more[CA_COUNT], ncard_more;
static unsigned card_frames, card_opened_at;
/* a click's action, done once the conversation is drawn: it may close it, or open a tab that moves it */
static int card_pending = -1, card_pending_msg;

/* c is closed, or opens another conversation in its place (the preview does): a selection, drag or menu made in
   it is keyed by its address and a message index or label id, and would land on the next one's messages */
static void forget_conv(jd_conv *c)
{
    clear_selections(c);
    if (drag.conv == c) drag.active = 0;
    if (ts.conv == c) ts.active = ts.drag = 0;
    if (menu_conv == c) {
        reader_menu.open = 0;
        menu_conv = NULL;
    }
    if (chip_conv == c) {
        chip_menu.open = 0;
        chip_conv = NULL;
    }
    if (card_conv == c) {
        card_menu.open = 0;
        card_conv = NULL;
        card_pending = -1;
    }
}

static const char *card_icon(const jd_reading *m, int a)
{
    static const char *icons[CA_COUNT] = {"reply", "reply-all", "forward", "mail", "star", "trash-2", "archive",
                                          "folder-input", "copy", "external-link", "download", "printer"};
    if (a == CA_READ && !(m->row.flags & JD_SEEN)) return "mail-open";
    return icons[a];
}

static const char *card_label(const jd_reading *m, int a)
{
    static const char *labels[CA_COUNT] = {"Reply", "Reply all", "Forward", "Mark unread", "Flag", "Delete",
                                           "Archive", "Move to…", "Copy to…", "Open in a tab", "Save as .eml", "Print"};
    if (a == CA_READ && !(m->row.flags & JD_SEEN)) return "Mark read";
    if (a == CA_FLAG && (m->row.flags & JD_FLAGGED)) return "Unflag";
    return labels[a];
}

static void card_act(jd_app *app, jd_conv *c, int i, int a)
{
    jd_reading *m = &c->reading[i];
    int64_t id = m->row.id, thread = c->thread;
    switch (a) {
    case CA_REPLY: compose_open(app, COMPOSE_REPLY, id); break;
    case CA_REPLY_ALL: compose_open(app, COMPOSE_REPLY_ALL, id); break;
    case CA_FORWARD: compose_open(app, COMPOSE_FORWARD, id); break;
    case CA_READ:
    case CA_FLAG: {
        int flag = a == CA_READ ? JD_SEEN : JD_FLAGGED, on = !(m->row.flags & flag);
        if (app->sync) jd_sync_set_flag(app->sync, &id, 1, flag, on);
        m->row.flags = on ? m->row.flags | flag : m->row.flags & ~flag;
        break;
    }
    case CA_DELETE: message_delete(app, id, m->row.folder); break;
    case CA_ARCHIVE: message_archive(app, id); break;
    case CA_MOVE: message_move_ask(app, id, 0); break;
    case CA_COPY: message_move_ask(app, id, 1); break;
    case CA_TAB: {
        /* tab_add may move the tabs, c among them: nothing of c is used after it */
        jd_tab *t = tab_add(app, TAB_CONV);
        conv_open(app, &t->conv, thread, id);
        break;
    }
    case CA_SAVE: save_message(app, m); break;
    case CA_PRINT: app->print_message = id; break;
    default: break;
    }
}

/* the icons right-aligned to right, centred on cy; returns their width. narrow: Reply and ⋮ only. */
static float card_icons(jd_app *app, jd_conv *c, int i, float right, float cy, Rectangle clip, int narrow, int draw)
{
    static const int wide[] = {CA_REPLY, CA_REPLY_ALL, CA_FORWARD, CA_READ, CA_FLAG, CA_DELETE};
    int nshown = narrow ? 1 : (int)(sizeof wide / sizeof *wide);
    float bw = 30, w = bw * (float)(nshown + 1);
    if (!draw) return w;
    jd_reading *m = &c->reading[i];
    int free_mouse = !app->dialog.kind && !app->menu.open && !card_menu.open && !reader_menu.open && !on_floating();
    const char *tip = NULL;
    float tip_x = 0, x = right - w;
    for (int k = 0; k <= nshown; k++, x += bw) {
        int a = k < nshown ? wide[k] : -1;
        Rectangle b = {x, cy - 14, 28, 28};
        int hover = free_mouse && CheckCollisionPointRec(GetMousePosition(), b) && CheckCollisionPointRec(GetMousePosition(), clip);
        if (hover || (a < 0 && card_menu.open && card_conv == c && card_msg == i)) DrawRectangleRounded(b, 0.3f, 6, JD_NORD2);
        Color tint = a == CA_FLAG && (m->row.flags & JD_FLAGGED) ? JD_NORD13 : hover ? JD_NORD8 : JD_NORD4;
        jd_icon(a < 0 ? "ellipsis-vertical" : card_icon(m, a), x + 6, cy - 8, 16, tint);
        if (hover && !app->ui.touch) tip = a < 0 ? "More" : card_label(m, a), tip_x = x;
        if (!hover || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) continue;
        if (a >= 0) {
            card_pending = a, card_pending_msg = i;
            continue;
        }
        card_conv = c, card_msg = i, ncard_more = 0;
        for (int e = narrow ? CA_REPLY_ALL : CA_ARCHIVE; e < CA_COUNT; e++) card_more[ncard_more++] = e;
        jd_popup_open(&card_menu);
        card_menu.at = (Vector2){x + 28 - 240, cy + 16};
        card_opened_at = card_frames;
    }
    if (tip && !plat_touch()) jd_tooltip(tip, tip_x, cy + 16);
    return w;
}

/* last in conv_draw: the menu, then the action a click chose */
static void card_menu_draw(jd_app *app, jd_conv *c)
{
    card_frames++;
    if (card_menu.open && card_conv == c && card_msg >= c->n) card_menu.open = 0;
    /* from the next frame: the press that opened it lies outside it, and would close it at once */
    if (card_menu.open && card_conv == c && card_frames > card_opened_at + 1) {
        jd_reading *m = &c->reading[card_msg];
        jd_popup_item items[CA_COUNT];
        for (int k = 0; k < ncard_more; k++)
            items[k] = (jd_popup_item){.label = card_label(m, card_more[k]), .icon = card_icon(m, card_more[k])};
        int k = jd_popup_draw(&app->ui, &card_menu, items, ncard_more);
        if (k >= 0) card_pending = card_more[k], card_pending_msg = card_msg;
    }
    int a = card_pending;
    card_pending = -1;
    if (a >= 0 && card_pending_msg < c->n) card_act(app, c, card_pending_msg, a);
}

/* A conversation's entrance, as the list's rows come in: the subject, then each card on screen, 30 ms apart, from
   16 px to the right, fading in over 0.24 s. Between enter_begin and enter_end the drawing is shifted; how far in
   it has come is returned. */
static float enter_begin(int entering, double since, int k)
{
    if (!entering) return 1;
    float u = fminf(1, fmaxf(0, (float)(since - 0.03 * k) / 0.24f));
    float came = 1 - (1 - u) * (1 - u) * (1 - u);
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlTranslatef(roundf(16 * (1 - came)), 0, 0);
    return came;
}

static void enter_end(int entering, float came, Rectangle box)
{
    if (!entering) return;
    rlDrawRenderBatchActive();
    rlPopMatrix();
    if (came < 1) DrawRectangleRec(box, ColorAlpha(JD_NORD0, 1 - came));
}

void conv_draw(jd_app *app, jd_conv *c, Rectangle r, int focused)
{
    jd_ui *ui = &app->ui;
    text_size(app);
    c->height = r.height;
    c->top = r.y;
    if (rv.active && rv.conv == c) nhreg = 0;
    for (int i = 0; i < c->n; i++) c->reading[i].body_y = -1;
    DrawRectangleRec(r, JD_NORD0);
    if (!c->n) {
        jd_text(ui, !app->nrows ? "" : plat_touch() ? "Tap a conversation to read it." : "Enter opens the selected conversation.",
                r.x + 28, r.y + 24, 14, JD_MUTED, 0);
        return;
    }
    const int narrow = r.width < 560;
    float margin = narrow ? 6 : 16, x = r.x + margin, w = r.width - 2 * margin;
    if (CheckCollisionPointRec(GetMousePosition(), r) && !app->dialog.kind && !app->menu.open)
        c->scroll -= jd_wheel_y(LINE_H * 3);
    /* clamped before drawing, by the height the last frame measured: clamping afterwards drew a frame past the
       end and snapped back on the next one */
    if (c->content > 0 && c->scroll > c->content - r.height) c->scroll = c->content - r.height;
    if (c->scroll < 0) c->scroll = 0;

    jd_scissor(r.x, r.y, r.width, r.height);
    float y = r.y + 20 - c->scroll;
    double since = GetTime() - c->opened_at;
    int entering = fx_enabled() && since < 1.5, entered = 0;
    float came = enter_begin(entering, since, entered++);
    const char *subject = c->reading[c->n - 1].row.subject;
    if (!subject || !subject[0]) subject = "(no subject)";
    float ss = 21 * K;
    int nsub;
    int *sub = jd_wrap(ui, subject, w, ss, &nsub);
    for (int l = 0; l < nsub; l++, y += ss * 1.3f) {
        int end = l + 1 < nsub ? sub[l + 1] : (int)strlen(subject);
        jd_text_range(ui, subject, sub[l], end, x, y, ss, focused ? JD_NORD6 : JD_NORD4, 1);
        char *line = jd_strndup(subject + sub[l], (size_t)(end - sub[l]));
        sel_label(app, c, 100000 + l, line, x, y, w, ss, 1, r);
        free(line);
    }
    free(sub);
    if (c->n > 1) {
        char count[48];
        snprintf(count, sizeof count, "%d messages", c->n);
        jd_text(ui, count, x, y + 2, 13 * K, JD_MUTED, 0);
        y += 20 * K;
    }
    enter_end(entering, came, (Rectangle){r.x, r.y, r.width, y - r.y});
    y += 14;
    float newest_y = y;
    /* each message is a card: a collapsed one shows its sender, date and first line; a click opens or closes it */
    for (int i = 0; i < c->n; i++) {
        jd_reading *m = &c->reading[i];
        const float pad = narrow ? 8 : 16, ar = 16 * K;
        float card_y = y, ix = x + pad, iw = w - 2 * pad;
        /* counted among those on screen only: a long thread opens at its end */
        int on_screen = card_y < r.y + r.height && card_y + fmaxf(m->card_h, 1) > r.y;
        float card_came = enter_begin(entering && on_screen, since, entered);
        entered += on_screen;
        newest_y = card_y;
        float head_h = 2 * ar;
        /* opening and closing ease over OPEN_S: the open card is drawn throughout, cut to a height between the
           closed one's and its whole height (smoothstep), and what follows moves with it */
        const float OPEN_S = 0.18f;
        float step = GetFrameTime() / OPEN_S;
        m->open = m->expanded ? fminf(1, m->open + step) : fmaxf(0, m->open - step);
        int shown = m->expanded || m->open > 0;
        if (shown && !m->loaded) load_text(app, m);
        int moving = m->open > 0 && m->open < 1;
        float closed_h = head_h + 2 * pad, ease = m->open * m->open * (3 - 2 * m->open);
        float moving_h = closed_h + (fmaxf(m->full_h, closed_h) - closed_h) * ease;
        if (moving) {
            m->card_h = moving_h;
            jd_scissor(x, card_y, w, moving_h);
        }
        /* a collapsed card is one target as a whole; an open one closes from its header */
        Rectangle head = shown ? (Rectangle){x, card_y, w, pad + head_h + 6}
                               : (Rectangle){x, card_y, w, head_h + 2 * pad};
        int on_head = CheckCollisionPointRec(GetMousePosition(), head) && CheckCollisionPointRec(GetMousePosition(), r) &&
                      !app->dialog.kind && !app->menu.open && !card_menu.open;
        if (on_head) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        if (m->card_h > 0 && card_y < r.y + r.height && card_y + m->card_h > r.y)
            DrawRectangleRounded((Rectangle){x, card_y, w, m->card_h}, 16.0f / fminf(w, m->card_h), 8,
                                 on_head && !shown && !app->ui.touch ? JD_NORD2 : JD_NORD1);
        y += pad;
        char date[32];
        format_date(m->row.date, date, sizeof date);
        float dw = jd_text_width(ui, date, 13 * K, 0);
        float tx = ix + 2 * ar + 12, tw = iw - (tx - ix);
        float name_y = shown ? y + ar - 15 * K * 0.6f : y + 1;
        /* the name alone: the address is in the From chip of an open card */
        char from_name[256], from_addr[256];
        split_address(m->row.from ? m->row.from : "", m->row.from ? (int)strlen(m->row.from) : 0, from_name,
                      sizeof from_name, from_addr, sizeof from_addr);
        const char *from = from_name[0] ? from_name : from_addr[0] ? from_addr : "Unknown sender";
        /* in an open card the sender and date are text to select; a click elsewhere on the header closes it */
        float icons_right = tx + tw - dw - 26 * K, icons_cy = y + ar;
        float icons_w = shown ? card_icons(app, c, i, icons_right, icons_cy, r, narrow, 0) : 0;
        /* before its body is downloaded and read only the server knows a message has attachments, not how many */
        int clip = !shown && (m->nfiles > 0 || ((!m->row.body_cached || !m->loaded) && m->row.has_attachment));
        char nfiles[16] = "";
        if (clip && m->nfiles) snprintf(nfiles, sizeof nfiles, "%d", m->nfiles);
        float clip_w = clip ? 14 * K + (nfiles[0] ? 3 + jd_text_width(ui, nfiles, 12 * K, 0) : 0) + 8 * K : 0;
        float nw = jd_text_width(ui, from, 15 * K, 1), nmax = tw - dw - 40 * K - icons_w - clip_w;
        Rectangle name_box = {tx, name_y - 2, nw < nmax ? nw : nmax, 15 * K + 4};
        Rectangle date_box = {tx + tw - dw, name_y, dw, 13 * K + 4};
        Rectangle icons_box = {icons_right - icons_w, icons_cy - 14, icons_w, 28};
        int on_text = shown && (CheckCollisionPointRec(GetMousePosition(), name_box) ||
                                      CheckCollisionPointRec(GetMousePosition(), date_box) ||
                                      CheckCollisionPointRec(GetMousePosition(), icons_box));
        if (!on_text && clicked(head, r, app)) m->expanded = !m->expanded;
        char *addr = sender_address(m);
        sender_avatar(app, m->row.from && m->row.from[0] ? m->row.from : "?", addr, ix + ar, y + ar, ar);
        free(addr);
        jd_text_fit(ui, from, tx, name_y, nmax, 15 * K, JD_NORD6, 1);
        if (shown) {
            sel_label(app, c, i * 16 + 1, from, tx, name_y, nmax, 15 * K, 1, r);
            sel_label(app, c, i * 16 + 2, date, tx + tw - dw, name_y + 2 * K, dw, 13 * K, 0, r);
        }
        if ((m->row.flags & JD_FLAGGED) && !shown) jd_icon("star", tx + tw - dw - 22 * K, name_y + 1, 14 * K, JD_NORD13);
        if (clip) {
            float cx = tx + tw - dw - ((m->row.flags & JD_FLAGGED) ? 22 * K : 0) - clip_w;
            jd_icon("paperclip", cx, name_y + 1, 14 * K, JD_NORD4);
            if (nfiles[0]) jd_text(ui, nfiles, cx + 14 * K + 3, name_y + 2 * K, 12 * K, JD_NORD4, 0);
        }
        jd_text(ui, date, tx + tw - dw, name_y + 2 * K, 13 * K, JD_NORD4, 0);
        if (shown) card_icons(app, c, i, icons_right, icons_cy, r, narrow, 1);
        if (!shown) {
            /* the first line of what was written, quotes left out; the stored snippet while it is not read */
            char line[256] = "";
            if (!m->loaded && m->row.snippet) snprintf(line, sizeof line, "%s", m->row.snippet);
            for (const char *p = m->loaded ? m->text : NULL; p && *p;) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                while (len && (*p == ' ' || *p == '\t')) p++, len--;
                while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
                if (len && *p != '>') {
                    snprintf(line, sizeof line, "%.*s", len < 199 ? len : 199, p);
                    break;
                }
                p = nl ? nl + 1 : p + len;
            }
            jd_text_fit(ui, line[0] ? line : m->row.body_cached ? "" : "Not downloaded yet", tx, y + 20 * K, tw, 13 * K,
                        JD_MUTED, 0);
            y += head_h + pad;
            m->card_h = y - card_y;
            enter_end(entering && on_screen, card_came, (Rectangle){r.x, card_y, r.width, m->card_h});
            y += 8;
            continue;
        }
        y += head_h + 10;
        if (!m->addresses) y = address_summary(app, m, ix, y, iw, r);
        else {
            /* the chevron stays where the one line's is, turned down; the lines move beside it */
            Rectangle fold = {ix - 2, y - 2, 18 * K, 20 * K};
            int on_fold = CheckCollisionPointRec(GetMousePosition(), fold) && CheckCollisionPointRec(GetMousePosition(), r);
            if (on_fold) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            jd_icon("chevron-down", ix - 2, y, 13 * K, on_fold ? JD_NORD8 : JD_MUTED);
            if (clicked(fold, r, app)) m->addresses = 0;
            float ax = ix + 18 * K, aw = iw - 18 * K;
            y = address_line(app, c, i * 16 + 6, "From", m->row.from, ax, y, aw, r);
            y = address_line(app, c, i * 16 + 3, "To", m->row.to, ax, y, aw, r);
            y = address_line(app, c, i * 16 + 4, "Cc", m->row.cc, ax, y, aw, r);
            y = header_line(app, c, i * 16 + 5, "In", m->row.folder_name, ax, y, aw, r);
        }
        y += 10;
        float x0 = x, w0 = w;
        x = ix, w = iw;
        if (m->invite) y = invite_card(app, m->invite, x, y, w, r);
        y = draw_files(app, m, x, y, w, r);
        if (m->html && !c->plain) y = remote_images(app, m, x, y, w, r);
        if (m->html && !c->plain) {
            /* edge to edge: the mail's body margin (8 px, as in a browser) is its only inset, as in Thunderbird */
            if (m->html_w != w0) {
                m->html_h = jd_html_layout(m->html, w0);
                m->html_w = w0;
            }
            Rectangle paper = {x0, y, w0, m->html_h};
            if (paper.y < r.y + r.height && paper.y + paper.height > r.y) {
                int dark = mail_page_dark(app);
                DrawRectangleRec(paper, jd_html_page(dark));
                /* mail laid out wider than the page (fixed-width tables) is cut at the page's edge */
                jd_scissor(paper.x, paper.y, paper.width, paper.height);
                jd_html_set_dark(m->html, dark);
                jd_html_draw(m->html, x0, y, r.y, r.y + r.height);
                jd_scissor_end();
            }
            text_mouse(app, c, i, paper, r, x0, y, 1, 0);
            m->body_y = y;
            y += paper.height;
        } else if (!m->text) {
            jd_text(ui, m->row.body_cached ? "This message has no readable text." : "Downloading...", x, y, TEXT_SIZE, JD_NORD4, 0);
            y += LINE_H;
        } else {
            jd_ui_use(ui, app->prefs.mono_plain ? &ui->mono : &ui->reader);
            if (m->wrapped_for != w || m->wrapped_size != TEXT_SIZE) {
                free(m->line_starts);
                m->line_starts = jd_wrap(ui, m->text, w, TEXT_SIZE, &m->nlines);
                m->wrapped_for = w;
                m->wrapped_size = TEXT_SIZE;
            }
            int tlen = (int)strlen(m->text);
            int folded = m->quote_at > 0 && !m->show_quote;
            float text_top = y;
            int shown = 0;
            while (shown < m->nlines && !(folded && m->line_starts[shown] >= m->quote_at)) shown++;
            text_mouse(app, c, i, (Rectangle){x, text_top, w, (float)shown * LINE_H}, r, x, text_top, 0, shown);
            m->body_y = text_top;
            m->body_lines = shown;
            int caret_here = rv.active && rv.conv == c && rv.msg == i && !rv.seg;
            for (int l = 0; l < m->nlines; l++, y += LINE_H) {
                int from = m->line_starts[l], to = l + 1 < m->nlines ? m->line_starts[l + 1] : tlen;
                if (folded && from >= m->quote_at) break;
                if (y + LINE_H < r.y || y > r.y + r.height) continue;
                /* raylib fits a font's ascent to descent into the size, so the letters sit around half the size
                   below the top: highlight and caret are centred there, not on the taller line */
                float band_h = TEXT_SIZE * 1.25f, band = y + TEXT_SIZE * 0.5f - band_h / 2;
                if (m->sel_to > m->sel_from && m->sel_from < to && m->sel_to > from) {
                    int a = m->sel_from > from ? m->sel_from : from, b = m->sel_to < to ? m->sel_to : to;
                    float x0 = span(ui, m->text, from, a), x1 = span(ui, m->text, from, b);
                    if (m->sel_to > to) x1 += 6;
                    DrawRectangleV((Vector2){x + x0, band}, (Vector2){x1 - x0, band_h}, ColorAlpha(JD_NORD10, 0.55f));
                }
                if (caret_here && m->caret >= from && (m->caret < to || l + 1 == shown))
                    DrawRectangleV((Vector2){x + span(ui, m->text, from, m->caret) - 1, band}, (Vector2){2, band_h}, JD_NORD8);
                /* a wrapped quote keeps the quote colour on its continuation lines */
                int src = from;
                while (src > 0 && m->text[src - 1] != '\n') src--;
                Color plain = m->text[src] == '>' ? JD_NORD9 : JD_NORD4;
                /* links in the accent colour, underlined; a link wrapped from the line above goes on here */
                int p = from, a, b;
                while (p < to && url_from(m->text, p, to, &a, &b)) {
                    if (a < p) a = p;
                    if (b > to) b = to;
                    jd_text_range(ui, m->text, p, a, x + span(ui, m->text, from, p), y, TEXT_SIZE, plain, 0);
                    float lx = x + span(ui, m->text, from, a), lw = span(ui, m->text, a, b);
                    jd_text_range(ui, m->text, a, b, lx, y, TEXT_SIZE, JD_NORD8, 0);
                    DrawRectangleV((Vector2){lx, y + TEXT_SIZE + 1}, (Vector2){lw, 1}, ColorAlpha(JD_NORD8, 0.6f));
                    p = b;
                }
                jd_text_range(ui, m->text, p, to, x + span(ui, m->text, from, p), y, TEXT_SIZE, plain, 0);
            }
            jd_ui_use(ui, NULL);
            if (m->quote_at > 0) {
                const char *label = m->show_quote ? "Hide quoted text" : "Show quoted text";
                float lw = jd_text_width(ui, label, 13 * K, 1) + 24;
                Rectangle pill = {x, y + 4, lw, 26 * K};
                int hover = CheckCollisionPointRec(GetMousePosition(), pill) && CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRounded(pill, 0.5f, 6, hover ? JD_NORD3 : JD_NORD2);
                jd_text(ui, label, x + 12, y + 4 + 6.5f * K, 13 * K, JD_NORD9, 1);
                if (clicked(pill, r, app)) m->show_quote = !m->show_quote;
                y += 26 * K + 10;
            }
        }
        x = x0, w = w0;
        y += pad;
        m->full_h = y - card_y;
        if (moving) {
            jd_scissor_end();
            y = card_y + moving_h;
        }
        m->card_h = y - card_y;
        enter_end(entering && on_screen, card_came, (Rectangle){r.x, card_y, r.width, m->card_h});
        y += 8;
    }
    jd_scissor_end();
    /* a conversation opens at its newest message (the last card), once the first frame has measured the cards */
    int opening = c->content == 0;
    c->content = y + c->scroll - r.y + 40;
    if (opening && c->n > 1) c->scroll += newest_y - r.y - 8;
    if (!app->dialog.kind && !app->menu.open) jd_scrollbar(r, c->content, &c->scroll);
    touch_handles_draw(app, c, r);
    reader_menu_draw(app, c);
    chip_menu_draw(app, c);
    card_menu_draw(app, c);
}

int reader_step(jd_app *app, jd_conv *c, int dir, jd_conv *keep)
{
    int at = -1;
    for (int k = 0; k < app->nrows && at < 0; k++)
        if (app->grouped ? app->rows[k].thread == c->thread : app->rows[k].latest == c->message) at = k;
    int to = at + dir;
    if (at < 0 || to < 0 || to >= app->nrows) {
        notify_user(app, dir > 0 ? "This is the last message in the list." : "This is the first message in the list.");
        return 0;
    }
    app->row = to;
    if (keep) {
        conv_close(keep);
        *keep = *c;
        *c = (jd_conv){.plain = keep->plain};
    }
    conv_open(app, c, app->rows[to].thread, app->grouped ? 0 : app->rows[to].latest);
    c->scroll = 0;
    return 1;
}

void reader_open(jd_app *app, int in_tab)
{
    if (!app->nrows) return;
    const jd_thread_row *t = &app->rows[app->row];
    int64_t only = app->grouped ? 0 : t->latest;
    if (in_tab) {
        jd_tab *tab = tab_add(app, TAB_CONV);
        conv_open(app, &tab->conv, t->thread, only);
        return;
    }
    conv_open(app, preview(app), t->thread, only);
    app->pane = PANE_READER;
}

void reader_close(jd_app *app)
{
    conv_close(preview(app));
    if (app->pane == PANE_READER) app->pane = PANE_LIST;
}
