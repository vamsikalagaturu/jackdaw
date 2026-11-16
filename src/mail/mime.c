#define _GNU_SOURCE
#include "mime.h"

#include <ctype.h>
#include <libetpan/libetpan.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/util.h"

char *jd_mime_header(const char *raw)
{
    if (!raw) return NULL;
    size_t idx = 0;
    char *out = NULL;
    if (mailmime_encoded_phrase_parse("utf-8", raw, strlen(raw), &idx, "utf-8", &out) != MAILIMF_NO_ERROR || !out)
        return jd_strdup(raw);
    char *s = jd_strdup(out);
    free(out);
    return s;
}

int64_t jd_mime_date(const char *raw)
{
    if (!raw) return 0;
    size_t idx = 0;
    struct mailimf_date_time *dt = NULL;
    if (mailimf_date_time_parse(raw, strlen(raw), &idx, &dt) != MAILIMF_NO_ERROR) return 0;
    struct tm tm = {.tm_year = dt->dt_year - 1900, .tm_mon = dt->dt_month - 1, .tm_mday = dt->dt_day,
                    .tm_hour = dt->dt_hour, .tm_min = dt->dt_min, .tm_sec = dt->dt_sec};
    int zone = dt->dt_zone;
    mailimf_date_time_free(dt);
    return (int64_t)timegm(&tm) - (zone / 100 * 60 + zone % 100) * 60;
}

char *jd_mime_msgid(const char *raw)
{
    if (!raw) return NULL;
    const char *s = strchr(raw, '<'), *e = s ? strchr(s, '>') : NULL;
    if (s && e && e > s + 1) return jd_strndup(s + 1, (size_t)(e - s - 1));
    while (*raw == ' ') raw++;
    return *raw ? jd_strdup(raw) : NULL;
}

char **jd_mime_references(const char *headers, size_t len, int *n)
{
    *n = 0;
    size_t idx = 0;
    struct mailimf_fields *fields = NULL;
    if (!headers || mailimf_fields_parse(headers, len, &idx, &fields) != MAILIMF_NO_ERROR) return NULL;
    char **out = NULL;
    for (clistiter *it = clist_begin(fields->fld_list); it; it = clist_next(it)) {
        struct mailimf_field *f = clist_content(it);
        if (f->fld_type != MAILIMF_FIELD_REFERENCES) continue;
        for (clistiter *r = clist_begin(f->fld_data.fld_references->mid_list); r; r = clist_next(r)) {
            out = jd_realloc(out, sizeof *out * (size_t)(*n + 1));
            out[(*n)++] = jd_strdup(clist_content(r));
        }
    }
    mailimf_fields_free(fields);
    return out;
}

static char *address_list(struct mailimf_address_list *al)
{
    jd_buf b = {0};
    for (clistiter *it = al ? clist_begin(al->ad_list) : NULL; it; it = clist_next(it)) {
        struct mailimf_address *ad = clist_content(it);
        if (ad->ad_type != MAILIMF_ADDRESS_MAILBOX) continue;
        struct mailimf_mailbox *mb = ad->ad_data.ad_mailbox;
        char *name = mb->mb_display_name ? jd_mime_header(mb->mb_display_name) : NULL;
        if (name && name[0]) jd_buf_addf(&b, "%s%s <%s>", b.len ? ", " : "", name, mb->mb_addr_spec);
        else jd_buf_addf(&b, "%s%s", b.len ? ", " : "", mb->mb_addr_spec);
        free(name);
    }
    return b.s;
}

static char *msgid_list(clist *ids)
{
    jd_buf b = {0};
    for (clistiter *it = ids ? clist_begin(ids) : NULL; it; it = clist_next(it))
        jd_buf_addf(&b, "%s%s", b.len ? " " : "", (char *)clist_content(it));
    return b.s;
}

void jd_mime_draft_headers(const char *raw, size_t len, jd_draft_headers *out)
{
    *out = (jd_draft_headers){0};
    size_t idx = 0;
    struct mailimf_fields *fields = NULL;
    if (mailimf_envelope_and_optional_fields_parse(raw, len, &idx, &fields) != MAILIMF_NO_ERROR) return;
    for (clistiter *it = clist_begin(fields->fld_list); it; it = clist_next(it)) {
        struct mailimf_field *f = clist_content(it);
        switch (f->fld_type) {
        case MAILIMF_FIELD_TO: out->to = address_list(f->fld_data.fld_to->to_addr_list); break;
        case MAILIMF_FIELD_CC: out->cc = address_list(f->fld_data.fld_cc->cc_addr_list); break;
        case MAILIMF_FIELD_BCC: out->bcc = address_list(f->fld_data.fld_bcc->bcc_addr_list); break;
        case MAILIMF_FIELD_SUBJECT: out->subject = jd_mime_header(f->fld_data.fld_subject->sbj_value); break;
        case MAILIMF_FIELD_IN_REPLY_TO: out->in_reply_to = msgid_list(f->fld_data.fld_in_reply_to->mid_list); break;
        case MAILIMF_FIELD_REFERENCES: out->references = msgid_list(f->fld_data.fld_references->mid_list); break;
        }
    }
    mailimf_fields_free(fields);
}

void jd_draft_headers_free(jd_draft_headers *h)
{
    free(h->to), free(h->cc), free(h->bcc), free(h->subject), free(h->in_reply_to), free(h->references);
    *h = (jd_draft_headers){0};
}

/* the tag at s ("<p ...", "</div>") ends a line of text: block elements and <br> */
static int block_tag(const char *s)
{
    static const char *const BLOCKS[] = {"br", "p", "div", "tr", "td", "th", "li", "h1", "h2", "h3", "h4", "h5",
                                         "h6", "table", "blockquote", "ul", "ol", "hr"};
    s += 1 + (s[1] == '/');
    for (size_t i = 0; i < sizeof BLOCKS / sizeof *BLOCKS; i++) {
        size_t k = strlen(BLOCKS[i]);
        if (!strncasecmp(s, BLOCKS[i], k) && !isalnum((unsigned char)s[k])) return 1;
    }
    return 0;
}

/* the character reference at s ("&amp;", "&#228;", "&#xe4;") as UTF-8 into out; its length, 0 when there is none */
static size_t entity(const char *s, size_t n, jd_buf *out)
{
    static const struct {
        const char *name, *utf8;
    } NAMED[] = {{"nbsp", " "}, {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"#39", "'"}};
    const char *semi = memchr(s, ';', n < 12 ? n : 12);
    if (!semi) return 0;
    size_t len = (size_t)(semi - s) + 1;
    for (size_t i = 0; i < sizeof NAMED / sizeof *NAMED; i++)
        if (len == strlen(NAMED[i].name) + 2 && !strncmp(s + 1, NAMED[i].name, len - 2)) {
            jd_buf_adds(out, NAMED[i].utf8);
            return len;
        }
    if (s[1] != '#') return 0;
    char *end;
    unsigned long cp = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, &end, 16) : strtoul(s + 2, &end, 10);
    if (end != semi || !cp || cp > 0x10ffff) return 0;
    char u[4];
    int k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (k == 1) u[0] = (char)cp;
    else {
        for (int j = k - 1; j > 0; j--) u[j] = (char)(0x80 | (cp & 0x3f)), cp >>= 6;
        u[0] = (char)((k == 2 ? 0xc0 : k == 3 ? 0xe0 : 0xf0) | cp);
    }
    jd_buf_add(out, u, (size_t)k);
    return len;
}

/* HTML as text: block elements end lines, character references decoded, runs of spaces and of blank lines made one.
   Left out: the head (its title is no text of the mail), comments (Outlook's <!--[if mso]> settings), style and
   script, and blocks of link text alone, a newsletter's chrome ("View in browser", a menu, the sender's site). */
static void strip_html(jd_buf *out, const char *s, size_t n)
{
    int in_tag = 0, skip = 0, in_link = 0, plain = 0;
    size_t block = 0;
    jd_buf t = {0};
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '<' && !strncmp(s + i, "<!--", 4)) {
            size_t j = i + 4;
            while (j + 2 < n && strncmp(s + j, "-->", 3)) j++;
            i = j + 2;
        } else if (s[i] == '<') {
            in_tag = 1;
            static const char *const HIDDEN[] = {"head", "style", "script", "title"};
            int close = s[i + 1] == '/';
            const char *name = s + i + 1 + close;
            for (size_t h = 0; h < sizeof HIDDEN / sizeof *HIDDEN; h++) {
                size_t k = strlen(HIDDEN[h]);
                if (!strncasecmp(name, HIDDEN[h], k) && !isalnum((unsigned char)name[k])) skip = !close;
            }
            if (!strncasecmp(name, "a", 1) && !isalnum((unsigned char)name[1])) in_link = !close;
            if (block_tag(s + i)) {
                if (!plain) t.len = block;
                jd_buf_adds(&t, "\n");
                block = t.len, plain = 0;
            } else {
                jd_buf_adds(&t, " ");
            }
        } else if (s[i] == '>') {
            in_tag = 0;
        } else if (!in_tag && !skip) {
            size_t was = t.len, k = s[i] == '&' ? entity(s + i, n - i, &t) : 0;
            if (k) i += k - 1;
            else jd_buf_add(&t, s + i, 1);
            for (size_t j = was; j < t.len && !in_link; j++)
                if (!isspace((unsigned char)t.s[j])) plain = 1;
        }
    }
    if (!plain) t.len = block;
    int newlines = 2, space = 0;
    for (size_t i = 0; i < t.len; i++) {
        char ch = t.s[i] == '\t' || t.s[i] == '\r' ? ' ' : t.s[i];
        if (ch == '\n') newlines++, space = 0;
        else if (ch == ' ') space = 1;
        else {
            if (newlines) jd_buf_adds(out, newlines > 1 && out->len ? "\n\n" : out->len ? "\n" : "");
            else if (space) jd_buf_adds(out, " ");
            jd_buf_add(out, &ch, 1);
            newlines = space = 0;
        }
    }
    jd_buf_free(&t);
}

static void add_part(jd_buf *out, struct mailmime *mime, int want_html)
{
    struct mailmime_single_fields fields;
    mailmime_single_fields_init(&fields, mime->mm_mime_fields, mime->mm_content_type);
    if (fields.fld_disposition && fields.fld_disposition->dsp_type &&
        fields.fld_disposition->dsp_type->dsp_type == MAILMIME_DISPOSITION_TYPE_ATTACHMENT)
        return;
    struct mailmime_content *ct = mime->mm_content_type;
    if (!ct || !ct->ct_type || ct->ct_type->tp_type != MAILMIME_TYPE_DISCRETE_TYPE ||
        ct->ct_type->tp_data.tp_discrete_type->dt_type != MAILMIME_DISCRETE_TYPE_TEXT)
        return;
    if (strcasecmp(ct->ct_subtype, want_html ? "html" : "plain") != 0) return;
    struct mailmime_data *data = mime->mm_data.mm_single;
    if (!data || data->dt_type != MAILMIME_DATA_TEXT) return;

    int encoding = fields.fld_encoding ? fields.fld_encoding->enc_type : MAILMIME_MECHANISM_8BIT;
    size_t idx = 0, dlen = 0;
    char *decoded = NULL;
    if (mailmime_part_parse(data->dt_data.dt_text.dt_data, data->dt_data.dt_text.dt_length, &idx, encoding, &decoded,
                            &dlen) != MAILIMF_NO_ERROR)
        return;
    const char *charset = fields.fld_content_charset ? fields.fld_content_charset : "us-ascii";
    char *utf8 = NULL;
    size_t ulen = 0;
    int converted = charconv_buffer("utf-8", charset, decoded, dlen, &utf8, &ulen) == MAIL_CHARCONV_NO_ERROR;
    const char *text = converted ? utf8 : decoded;
    size_t tlen = converted ? ulen : dlen;
    if (out->len) jd_buf_adds(out, "\n");
    if (want_html) strip_html(out, text, tlen);
    else jd_buf_add(out, text, tlen);
    if (converted) charconv_buffer_free(utf8);
    mmap_string_unref(decoded);
}

static void walk(jd_buf *out, struct mailmime *mime, int want_html)
{
    switch (mime->mm_type) {
    case MAILMIME_SINGLE:
        add_part(out, mime, want_html);
        break;
    case MAILMIME_MULTIPLE:
        for (clistiter *it = clist_begin(mime->mm_data.mm_multipart.mm_mp_list); it; it = clist_next(it))
            walk(out, clist_content(it), want_html);
        break;
    case MAILMIME_MESSAGE:
        if (mime->mm_data.mm_message.mm_msg_mime) walk(out, mime->mm_data.mm_message.mm_msg_mime, want_html);
        break;
    }
}

static char *decode_part(struct mailmime *mime, size_t *out_len)
{
    struct mailmime_single_fields fields;
    mailmime_single_fields_init(&fields, mime->mm_mime_fields, mime->mm_content_type);
    struct mailmime_data *data = mime->mm_data.mm_single;
    if (!data || data->dt_type != MAILMIME_DATA_TEXT) return NULL;
    int encoding = fields.fld_encoding ? fields.fld_encoding->enc_type : MAILMIME_MECHANISM_8BIT;
    size_t idx = 0, dlen = 0;
    char *decoded = NULL;
    if (mailmime_part_parse(data->dt_data.dt_text.dt_data, data->dt_data.dt_text.dt_length, &idx, encoding, &decoded,
                            &dlen) != MAILIMF_NO_ERROR)
        return NULL;
    char *out = jd_alloc(dlen + 1);
    memcpy(out, decoded, dlen);
    mmap_string_unref(decoded);
    *out_len = dlen;
    return out;
}

/* RFC 2231 extended parameters: name*=charset'lang'%XX..., or split as name*0*=..., name*1*=... in order */
static char *rfc2231(clist *params, const char *name)
{
    size_t n = strlen(name);
    jd_buf raw = {0};
    char *charset = NULL;
    for (int part = -1; part < 100; part++) {
        int found = 0;
        for (clistiter *it = params ? clist_begin(params) : NULL; it; it = clist_next(it)) {
            struct mailmime_parameter *p = clist_content(it);
            if (!p->pa_name || strncasecmp(p->pa_name, name, n) != 0 || p->pa_name[n] != '*') continue;
            const char *rest = p->pa_name + n + 1;
            int idx = *rest ? atoi(rest) : -1;
            if (idx != part) continue;
            found = 1;
            const char *v = p->pa_value;
            int encoded = !*rest || rest[strlen(rest) - 1] == '*';
            if (part <= 0 && encoded) {
                const char *q1 = strchr(v, '\''), *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
                if (q1 && q2) {
                    charset = jd_strndup(v, (size_t)(q1 - v));
                    v = q2 + 1;
                }
            }
            for (; *v; v++) {
                if (encoded && *v == '%' && v[1] && v[2]) {
                    char hex[3] = {v[1], v[2], 0}, c = (char)strtol(hex, NULL, 16);
                    jd_buf_add(&raw, &c, 1);
                    v += 2;
                } else {
                    jd_buf_add(&raw, v, 1);
                }
            }
        }
        if (!found && part > 0) break;
    }
    if (!raw.s) {
        free(charset);
        return NULL;
    }
    char *out = NULL;
    size_t olen;
    if (charset && charset[0] && charconv_buffer("utf-8", charset, raw.s, raw.len, &out, &olen) == MAIL_CHARCONV_NO_ERROR) {
        char *copy = jd_strndup(out, olen);
        charconv_buffer_free(out);
        out = copy;
        jd_buf_free(&raw);
    } else {
        out = raw.s;
    }
    free(charset);
    return out;
}

static clist *disposition_params(struct mailmime *mime)
{
    clist *out = clist_new();
    struct mailmime_fields *f = mime->mm_mime_fields;
    for (clistiter *it = f ? clist_begin(f->fld_list) : NULL; it; it = clist_next(it)) {
        struct mailmime_field *field = clist_content(it);
        if (field->fld_type != MAILMIME_FIELD_DISPOSITION || !field->fld_data.fld_disposition) continue;
        for (clistiter *p = clist_begin(field->fld_data.fld_disposition->dsp_parms); p; p = clist_next(p)) {
            struct mailmime_disposition_parm *dp = clist_content(p);
            if (dp->pa_type == MAILMIME_DISPOSITION_PARM_PARAMETER) clist_append(out, dp->pa_data.pa_parameter);
        }
    }
    return out;
}

static int is_attachment(struct mailmime *mime, char **name)
{
    struct mailmime_single_fields fields;
    mailmime_single_fields_init(&fields, mime->mm_mime_fields, mime->mm_content_type);
    clist *dparams = disposition_params(mime);
    char *extended = rfc2231(dparams, "filename");
    clist_free(dparams);
    if (!extended && mime->mm_content_type) extended = rfc2231(mime->mm_content_type->ct_parameters, "name");
    const char *fname = extended ? extended
                        : fields.fld_disposition_filename ? fields.fld_disposition_filename : fields.fld_content_name;
    int attached = fields.fld_disposition && fields.fld_disposition->dsp_type &&
                   fields.fld_disposition->dsp_type->dsp_type == MAILMIME_DISPOSITION_TYPE_ATTACHMENT;
    struct mailmime_content *ct = mime->mm_content_type;
    int text = ct && ct->ct_type && ct->ct_type->tp_type == MAILMIME_TYPE_DISCRETE_TYPE &&
               ct->ct_type->tp_data.tp_discrete_type->dt_type == MAILMIME_DISCRETE_TYPE_TEXT;
    if (!attached && (text || !fname)) {
        free(extended);
        return 0;
    }
    *name = extended ? extended : fname ? jd_mime_header(fname) : jd_strdup("attachment");
    return 1;
}

static char *type_of(struct mailmime *mime)
{
    struct mailmime_content *ct = mime->mm_content_type;
    if (!ct || !ct->ct_type) return jd_strdup("application/octet-stream");
    const char *major = "application";
    if (ct->ct_type->tp_type == MAILMIME_TYPE_DISCRETE_TYPE) {
        static const char *names[] = {"", "text", "image", "audio", "video", "application"};
        int t = ct->ct_type->tp_data.tp_discrete_type->dt_type;
        if (t > 0 && t < 6) major = names[t];
    }
    return jd_printf("%s/%s", major, ct->ct_subtype ? ct->ct_subtype : "octet-stream");
}

typedef struct {
    jd_attachment *list;
    int n, want;
    char *data;
    size_t data_len;
} att_walk;

static void walk_attachments(struct mailmime *mime, att_walk *w)
{
    if (mime->mm_type == MAILMIME_MULTIPLE) {
        for (clistiter *it = clist_begin(mime->mm_data.mm_multipart.mm_mp_list); it; it = clist_next(it))
            walk_attachments(clist_content(it), w);
        return;
    }
    if (mime->mm_type == MAILMIME_MESSAGE) {
        if (mime->mm_data.mm_message.mm_msg_mime) walk_attachments(mime->mm_data.mm_message.mm_msg_mime, w);
        return;
    }
    char *name;
    if (!is_attachment(mime, &name)) return;
    if (w->want == w->n) w->data = decode_part(mime, &w->data_len);
    if (w->want < 0) {
        size_t size = 0;
        free(decode_part(mime, &size));
        w->list = jd_realloc(w->list, sizeof *w->list * (size_t)(w->n + 1));
        w->list[w->n] = (jd_attachment){name, type_of(mime), size};
    } else {
        free(name);
    }
    w->n++;
}

static void walk_inline(struct mailmime *mime, jd_inline_part **out, int *n)
{
    if (mime->mm_type == MAILMIME_MULTIPLE) {
        for (clistiter *it = clist_begin(mime->mm_data.mm_multipart.mm_mp_list); it; it = clist_next(it))
            walk_inline(clist_content(it), out, n);
        return;
    }
    if (mime->mm_type == MAILMIME_MESSAGE) {
        if (mime->mm_data.mm_message.mm_msg_mime) walk_inline(mime->mm_data.mm_message.mm_msg_mime, out, n);
        return;
    }
    struct mailmime_single_fields fields;
    mailmime_single_fields_init(&fields, mime->mm_mime_fields, mime->mm_content_type);
    if (!fields.fld_id) return;
    size_t dlen;
    char *data = decode_part(mime, &dlen);
    if (!data) return;
    *out = jd_realloc(*out, sizeof **out * (size_t)(*n + 1));
    (*out)[(*n)++] = (jd_inline_part){jd_strdup(fields.fld_id), type_of(mime), data, dlen};
}

jd_inline_part *jd_mime_inline_parts(const char *raw, size_t len, int *n)
{
    size_t idx = 0;
    struct mailmime *mime = NULL;
    *n = 0;
    if (mailmime_parse(raw, len, &idx, &mime) != MAILIMF_NO_ERROR) return NULL;
    jd_inline_part *out = NULL;
    walk_inline(mime, &out, n);
    mailmime_free(mime);
    return out;
}

void jd_inline_parts_free(jd_inline_part *p, int n)
{
    for (int i = 0; i < n; i++) free(p[i].cid), free(p[i].type), free(p[i].data);
    free(p);
}

jd_attachment *jd_mime_attachments(const char *raw, size_t len, int *n)
{
    size_t idx = 0;
    struct mailmime *mime = NULL;
    *n = 0;
    if (mailmime_parse(raw, len, &idx, &mime) != MAILIMF_NO_ERROR) return NULL;
    att_walk w = {.want = -1};
    walk_attachments(mime, &w);
    mailmime_free(mime);
    *n = w.n;
    return w.list;
}

void jd_attachments_free(jd_attachment *a, int n)
{
    for (int i = 0; i < n; i++) free(a[i].name), free(a[i].type);
    free(a);
}

char *jd_mime_attachment_data(const char *raw, size_t len, int index, size_t *out_len)
{
    size_t idx = 0;
    struct mailmime *mime = NULL;
    if (mailmime_parse(raw, len, &idx, &mime) != MAILIMF_NO_ERROR) return NULL;
    att_walk w = {.want = index};
    walk_attachments(mime, &w);
    mailmime_free(mime);
    *out_len = w.data_len;
    return w.data;
}

/* the message itself is the top MAILMIME_MESSAGE node; message/rfc822 parts inside it (forwards) are not searched */
static struct mailmime *find_html(struct mailmime *mime, int top)
{
    if (mime->mm_type == MAILMIME_MULTIPLE) {
        for (clistiter *it = clist_begin(mime->mm_data.mm_multipart.mm_mp_list); it; it = clist_next(it)) {
            struct mailmime *found = find_html(clist_content(it), 0);
            if (found) return found;
        }
        return NULL;
    }
    if (mime->mm_type == MAILMIME_MESSAGE)
        return top && mime->mm_data.mm_message.mm_msg_mime ? find_html(mime->mm_data.mm_message.mm_msg_mime, 0) : NULL;
    char *name;
    if (is_attachment(mime, &name)) {
        free(name);
        return NULL;
    }
    struct mailmime_content *ct = mime->mm_content_type;
    if (ct && ct->ct_type && ct->ct_type->tp_type == MAILMIME_TYPE_DISCRETE_TYPE &&
        ct->ct_type->tp_data.tp_discrete_type->dt_type == MAILMIME_DISCRETE_TYPE_TEXT && ct->ct_subtype &&
        strcasecmp(ct->ct_subtype, "html") == 0)
        return mime;
    return NULL;
}

char *jd_mime_body_html(const char *raw, size_t len)
{
    size_t idx = 0;
    struct mailmime *mime = NULL;
    if (mailmime_parse(raw, len, &idx, &mime) != MAILIMF_NO_ERROR) return NULL;
    struct mailmime *part = find_html(mime, 1);
    char *out = NULL;
    if (part) {
        struct mailmime_single_fields fields;
        mailmime_single_fields_init(&fields, part->mm_mime_fields, part->mm_content_type);
        size_t dlen;
        char *decoded = decode_part(part, &dlen);
        char *utf8 = NULL;
        size_t ulen;
        const char *charset = fields.fld_content_charset ? fields.fld_content_charset : "utf-8";
        if (decoded && charconv_buffer("utf-8", charset, decoded, dlen, &utf8, &ulen) == MAIL_CHARCONV_NO_ERROR) {
            out = jd_strndup(utf8, ulen);
            charconv_buffer_free(utf8);
            free(decoded);
        } else {
            out = decoded;
        }
    }
    mailmime_free(mime);
    return out;
}

/* text with HTML tags in it ('<', an optional '/', a name, then space, '/' or '>'), as some mail sends for its
   text/plain part; two or more, so one stray <b> or "a < b" does not count. <user@host> and <https://...> are no
   tags: their name ends at '@' or ':'. */
static int looks_like_html(const char *s)
{
    int tags = 0;
    for (const char *p = strchr(s, '<'); p && tags < 2; p = strchr(p + 1, '<')) {
        const char *q = p + 1 + (p[1] == '/'), *name = q;
        while (isalnum((unsigned char)*q)) q++;
        if (q > name && isalpha((unsigned char)*name) && (*q == ' ' || *q == '/' || *q == '>') && strchr(q, '>')) tags++;
    }
    return tags >= 2;
}

char *jd_mime_body_text(const char *raw, size_t len)
{
    size_t idx = 0;
    struct mailmime *mime = NULL;
    if (mailmime_parse(raw, len, &idx, &mime) != MAILIMF_NO_ERROR) return jd_strdup("");
    jd_buf out = {0};
    walk(&out, mime, 0);
    /* a text part that is HTML itself: the HTML part's text reads as the sender meant */
    if (out.len && looks_like_html(out.s)) {
        jd_buf html = {0};
        walk(&html, mime, 1);
        if (html.len) jd_buf_free(&out), out = html;
        else jd_buf_free(&html);
    }
    if (!out.len) walk(&out, mime, 1);
    mailmime_free(mime);
    return out.s ? out.s : jd_strdup("");
}
