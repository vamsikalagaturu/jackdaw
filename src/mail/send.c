#define _GNU_SOURCE
#include "send.h"

#include <fcntl.h>
#include <libetpan/libetpan.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "core/util.h"
#include "mail/imap.h"

static void random_hex(char *out, int bytes)
{
    unsigned char buf[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, buf, (size_t)bytes) != bytes) abort();
    close(fd);
    for (int i = 0; i < bytes; i++) sprintf(out + 2 * i, "%02x", buf[i]);
}

char *jd_message_id_new(const char *address)
{
    char hex[33];
    random_hex(hex, 16);
    const char *at = address ? strrchr(address, '@') : NULL;
    return jd_printf("%s@%s", hex, at && at[1] ? at + 1 : "jackdaw.invalid");
}

static int ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(jd_buf *b, const unsigned char *s, size_t n, int wrap)
{
    int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        char q[4] = {b64[v >> 18 & 63], b64[v >> 12 & 63], i + 1 < n ? b64[v >> 6 & 63] : '=', i + 2 < n ? b64[v & 63] : '='};
        jd_buf_add(b, q, 4);
        if (wrap && (col += 4) >= 76) {
            jd_buf_adds(b, "\r\n");
            col = 0;
        }
    }
    if (wrap && col) jd_buf_adds(b, "\r\n");
}

/* RFC 2047 encoded words, split on character boundaries so each word stays under 75 characters */
static void encoded_words(jd_buf *b, const char *s)
{
    if (ascii(s)) {
        jd_buf_adds(b, s);
        return;
    }
    size_t n = strlen(s), i = 0;
    while (i < n) {
        size_t j = i, take = 0;
        while (j < n && j - i < 45) {
            j++;
            while (j < n && ((unsigned char)s[j] & 0xc0) == 0x80) j++;
            take = j - i;
        }
        if (i) jd_buf_adds(b, "\r\n ");
        jd_buf_adds(b, "=?UTF-8?B?");
        base64(b, (const unsigned char *)s + i, take, 0);
        jd_buf_adds(b, "?=");
        i += take;
    }
}

/* one address list header; also collects the addr-specs for the envelope */
static int address_header(jd_buf *b, const char *field, const char *list, char ***rcpts, int *n, char *err, int errlen,
                          int write)
{
    if (!list) return 0;
    while (*list == ' ' || *list == ',') list++;
    if (!*list) return 0;
    size_t idx = 0;
    struct mailimf_address_list *al = NULL;
    if (mailimf_address_list_parse(list, strlen(list), &idx, &al) != MAILIMF_NO_ERROR) {
        snprintf(err, (size_t)errlen, "%s: not a list of addresses: %s", field, list);
        return -1;
    }
    int first = 1;
    if (write) jd_buf_addf(b, "%s: ", field);
    for (clistiter *it = clist_begin(al->ad_list); it; it = clist_next(it)) {
        struct mailimf_address *ad = clist_content(it);
        if (ad->ad_type != MAILIMF_ADDRESS_MAILBOX) continue;
        struct mailimf_mailbox *mb = ad->ad_data.ad_mailbox;
        char *name = mb->mb_display_name ? jd_strdup(mb->mb_display_name) : NULL;
        if (write) {
            jd_buf_adds(b, first ? "" : ",\r\n ");
            if (name && name[0]) {
                if (ascii(name)) {
                    jd_buf_adds(b, "\"");
                    for (const char *c = name; *c; c++) {
                        if (*c == '"' || *c == '\\') jd_buf_adds(b, "\\");
                        jd_buf_add(b, c, 1);
                    }
                    jd_buf_adds(b, "\" ");
                } else {
                    encoded_words(b, name);
                    jd_buf_adds(b, " ");
                }
            }
            jd_buf_addf(b, "<%s>", mb->mb_addr_spec);
        }
        first = 0;
        *rcpts = jd_realloc(*rcpts, sizeof **rcpts * (size_t)(*n + 1));
        (*rcpts)[(*n)++] = jd_strdup(mb->mb_addr_spec);
        free(name);
    }
    if (write) jd_buf_adds(b, "\r\n");
    mailimf_address_list_free(al);
    return 0;
}

/* RFC 2045 section 6.7, lines CRLF, soft breaks before 76 */
static void quoted_printable(jd_buf *b, const char *s)
{
    int col = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n') {
            jd_buf_adds(b, "\r\n");
            col = 0;
            continue;
        }
        if (*p == '\r') continue;
        char enc[4];
        int ws_end = (*p == ' ' || *p == '\t') && (p[1] == '\n' || p[1] == '\r' || !p[1]);
        int plain = (*p >= 33 && *p <= 126 && *p != '=') || ((*p == ' ' || *p == '\t') && !ws_end);
        /* a line starting "From " or "." is encoded so no mail system rewrites it */
        if (col == 0 && (*p == '.' || (*p == 'F' && strncmp((const char *)p, "From ", 5) == 0))) plain = 0;
        int len = plain ? 1 : 3;
        if (plain) enc[0] = (char)*p;
        else snprintf(enc, sizeof enc, "=%02X", *p);
        if (col + len > 75) {
            jd_buf_adds(b, "=\r\n");
            col = 0;
        }
        jd_buf_add(b, enc, (size_t)len);
        col += len;
    }
    jd_buf_adds(b, "\r\n");
}

static void date_header(jd_buf *b)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    long off = tm.tm_gmtoff / 60;
    jd_buf_addf(b, "Date: %s, %02d %s %d %02d:%02d:%02d %c%02ld%02ld\r\n", days[tm.tm_wday], tm.tm_mday,
                months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+',
                labs(off) / 60, labs(off) % 60);
}

/* RFC 2231 filename*=UTF-8''... next to a plain fallback */
static void filename_params(jd_buf *b, const char *name)
{
    jd_buf_adds(b, "; filename=\"");
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        jd_buf_add(b, *c >= 0x80 || *c == '"' || *c == '\\' ? "_" : (const char *)c, 1);
    jd_buf_adds(b, "\"");
    if (ascii(name)) return;
    jd_buf_adds(b, ";\r\n filename*=UTF-8''");
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-._~", *c))
            jd_buf_add(b, (const char *)c, 1);
        else
            jd_buf_addf(b, "%%%02X", *c);
    }
}

char *jd_message_build(const jd_account *from, const jd_outgoing *o, size_t *len, char ***rcpts, int *nrcpts,
                       char *err, int errlen)
{
    jd_buf b = {0};
    *rcpts = NULL;
    *nrcpts = 0;
    char from_line[1024];
    snprintf(from_line, sizeof from_line, "%s <%s>", from->name && !jd_streq(from->name, from->address) ? from->name : "",
             from->address);
    char **self = NULL;
    int nself = 0;
    if (address_header(&b, "From", from_line, &self, &nself, err, errlen, 1) ||
        address_header(&b, "To", o->to, rcpts, nrcpts, err, errlen, 1) ||
        address_header(&b, "Cc", o->cc, rcpts, nrcpts, err, errlen, 1) ||
        address_header(&b, "Bcc", o->bcc, rcpts, nrcpts, err, errlen, o->draft)) {
        jd_rcpts_free(self, nself);
        jd_rcpts_free(*rcpts, *nrcpts);
        *rcpts = NULL;
        *nrcpts = 0;
        jd_buf_free(&b);
        return NULL;
    }
    jd_rcpts_free(self, nself);
    jd_buf_adds(&b, "Subject: ");
    encoded_words(&b, o->subject ? o->subject : "");
    jd_buf_adds(&b, "\r\n");
    date_header(&b);
    char *mid = o->message_id ? jd_strdup(o->message_id) : jd_message_id_new(from->address);
    jd_buf_addf(&b, "Message-ID: <%s>\r\n", mid);
    free(mid);
    if (o->in_reply_to && o->in_reply_to[0]) jd_buf_addf(&b, "In-Reply-To: <%s>\r\n", o->in_reply_to);
    if (o->references && o->references[0]) {
        jd_buf_adds(&b, "References:");
        char *refs = jd_strdup(o->references), *save = NULL;
        for (char *r = strtok_r(refs, " ", &save); r; r = strtok_r(NULL, " ", &save)) jd_buf_addf(&b, "\r\n <%s>", r);
        free(refs);
        jd_buf_adds(&b, "\r\n");
    }
    jd_buf_adds(&b, "User-Agent: jackdaw\r\nMIME-Version: 1.0\r\n");
    char boundary[40] = "=_jd_";
    random_hex(boundary + 5, 12);
    if (o->nparts) {
        jd_buf_addf(&b, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n", boundary);
        jd_buf_addf(&b, "--%s\r\n", boundary);
    }
    char alt[40] = "=_jd_alt_";
    if (o->html) {
        random_hex(alt + 9, 12);
        jd_buf_addf(&b, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n--%s\r\n", alt, alt);
    }
    jd_buf_adds(&b, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
    quoted_printable(&b, o->body ? o->body : "");
    if (o->html) {
        char rel[40] = "=_jd_rel_";
        int related = o->nrelated > 0;
        jd_buf_addf(&b, "--%s\r\n", alt);
        if (related) {
            random_hex(rel + 9, 12);
            jd_buf_addf(&b, "Content-Type: multipart/related; type=\"text/html\"; boundary=\"%s\"\r\n\r\n--%s\r\n", rel,
                        rel);
        }
        jd_buf_adds(&b, "Content-Type: text/html; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
        quoted_printable(&b, o->html);
        for (int i = 0; related && i < o->nrelated; i++) {
            const jd_part *p = &o->related[i];
            jd_buf_addf(&b, "--%s\r\nContent-Type: %s\r\nContent-Transfer-Encoding: base64\r\nContent-ID: <%s>\r\n"
                            "Content-Disposition: inline\r\n\r\n",
                        rel, p->type ? p->type : "application/octet-stream", p->cid);
            base64(&b, (const unsigned char *)p->data, p->len, 1);
        }
        if (related) jd_buf_addf(&b, "--%s--\r\n", rel);
        jd_buf_addf(&b, "--%s--\r\n", alt);
    }
    for (int i = 0; i < o->nparts; i++) {
        const jd_part *p = &o->parts[i];
        jd_buf_addf(&b, "--%s\r\nContent-Type: %s\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment",
                    boundary, p->type ? p->type : jd_mime_type_of(p->name));
        filename_params(&b, p->name);
        jd_buf_adds(&b, "\r\n\r\n");
        base64(&b, (const unsigned char *)p->data, p->len, 1);
    }
    if (o->nparts) jd_buf_addf(&b, "--%s--\r\n", boundary);
    *len = b.len;
    return b.s;
}

void jd_rcpts_free(char **r, int n)
{
    for (int i = 0; i < n; i++) free(r[i]);
    free(r);
}

const char *jd_mime_type_of(const char *name)
{
    static const struct {
        const char *ext, *type;
    } types[] = {{"pdf", "application/pdf"}, {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
                 {"gif", "image/gif"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"}, {"txt", "text/plain"},
                 {"md", "text/markdown"}, {"csv", "text/csv"}, {"html", "text/html"}, {"zip", "application/zip"},
                 {"gz", "application/gzip"}, {"tar", "application/x-tar"}, {"ics", "text/calendar"},
                 {"doc", "application/msword"},
                 {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
                 {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
                 {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
                 {"odt", "application/vnd.oasis.opendocument.text"}, {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"},
                 {NULL, NULL}};
    const char *dot = name ? strrchr(name, '.') : NULL;
    for (int i = 0; dot && types[i].ext; i++)
        if (strcasecmp(dot + 1, types[i].ext) == 0) return types[i].type;
    return "application/octet-stream";
}

static void tls_setup(struct mailstream_ssl_context *ctx, void *data)
{
    const char *host = data;
    mailstream_ssl_set_server_name(ctx, (char *)host);
    SSL_CTX *ssl = mailstream_ssl_get_openssl_ssl_ctx(ctx);
    SSL_CTX_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    jd_tls_trust(ssl);
    SSL_CTX_set_min_proto_version(ssl, TLS1_2_VERSION);
    X509_VERIFY_PARAM *param = SSL_CTX_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set1_host(param, host, 0);
}

static int smtp_fail(mailsmtp *s, int r, const char *what, char *err, int errlen)
{
    const char *why = s && s->response ? s->response : mailsmtp_strerror(r);
    snprintf(err, (size_t)errlen, "%s: %s", what, why);
    return -1;
}

int jd_smtp_send(const jd_account *a, const char *password, char **rcpts, int nrcpts, const char *msg, size_t len,
                 char *err, int errlen)
{
    if (!nrcpts) {
        snprintf(err, (size_t)errlen, "No recipients.");
        return -1;
    }
    mailsmtp *s = mailsmtp_new(0, NULL);
    mailsmtp_set_timeout(s, 60);
    int r, ret = -1;
    if (a->smtp.security == JD_SEC_TLS)
        r = mailsmtp_ssl_connect_with_callback(s, a->smtp.host, (uint16_t)a->smtp.port, tls_setup, a->smtp.host);
    else
        r = mailsmtp_socket_connect(s, a->smtp.host, (uint16_t)a->smtp.port);
    if (r != MAILSMTP_NO_ERROR) {
        snprintf(err, (size_t)errlen, "cannot reach %s:%d securely (%s)", a->smtp.host, a->smtp.port, mailsmtp_strerror(r));
        goto done;
    }
    if ((r = mailsmtp_init(s)) != MAILSMTP_NO_ERROR) {
        smtp_fail(s, r, "greeting", err, errlen);
        goto done;
    }
    if (a->smtp.security == JD_SEC_STARTTLS) {
        if ((r = mailsmtp_socket_starttls_with_callback(s, tls_setup, a->smtp.host)) != MAILSMTP_NO_ERROR ||
            (r = mailsmtp_init(s)) != MAILSMTP_NO_ERROR) {
            smtp_fail(s, r, "STARTTLS", err, errlen);
            goto done;
        }
    }
    if ((r = mailsmtp_auth(s, a->username, password)) != MAILSMTP_NO_ERROR) {
        smtp_fail(s, r, "sign-in refused", err, errlen);
        goto done;
    }
    if ((r = mailsmtp_mail(s, a->address)) != MAILSMTP_NO_ERROR) {
        smtp_fail(s, r, "sender refused", err, errlen);
        goto done;
    }
    for (int i = 0; i < nrcpts; i++)
        if ((r = mailsmtp_rcpt(s, rcpts[i])) != MAILSMTP_NO_ERROR) {
            char what[300];
            snprintf(what, sizeof what, "recipient %s refused", rcpts[i]);
            smtp_fail(s, r, what, err, errlen);
            goto done;
        }
    if ((r = mailsmtp_data(s)) != MAILSMTP_NO_ERROR || (r = mailsmtp_data_message(s, msg, len)) != MAILSMTP_NO_ERROR) {
        smtp_fail(s, r, "message refused", err, errlen);
        goto done;
    }
    ret = 0;
    mailsmtp_quit(s);
done:
    mailsmtp_free(s);
    return ret;
}
