#define _GNU_SOURCE
#include "sieve.h"

#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/util.h"

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    char buf[8192];
    int len, pos;
} conn;

static int raw_read(conn *c, char *out, int n)
{
    return c->ssl ? SSL_read(c->ssl, out, n) : (int)read(c->fd, out, (size_t)n);
}

static int send_all(conn *c, const char *s, size_t n)
{
    while (n) {
        int w = c->ssl ? SSL_write(c->ssl, s, (int)n) : (int)write(c->fd, s, n);
        if (w <= 0) return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

static int getc_(conn *c)
{
    if (c->pos == c->len) {
        c->len = raw_read(c, c->buf, sizeof c->buf);
        c->pos = 0;
        if (c->len <= 0) {
            c->len = 0;
            return -1;
        }
    }
    return (unsigned char)c->buf[c->pos++];
}

/* one CRLF-terminated line, without the CRLF */
static char *read_line(conn *c)
{
    jd_buf b = {0};
    for (int ch; (ch = getc_(c)) >= 0;) {
        if (ch == '\n') {
            if (b.len && b.s[b.len - 1] == '\r') b.s[--b.len] = 0;
            return b.s ? b.s : jd_strdup("");
        }
        char k = (char)ch;
        jd_buf_add(&b, &k, 1);
    }
    jd_buf_free(&b);
    return NULL;
}

static char *read_bytes(conn *c, long n)
{
    char *out = jd_alloc((size_t)n + 1);
    for (long i = 0; i < n; i++) {
        int ch = getc_(c);
        if (ch < 0) {
            free(out);
            return NULL;
        }
        out[i] = (char)ch;
    }
    return out;
}

/* a quoted string or {n} literal starting at *p (of line), reading the literal from the connection */
static char *string_at(conn *c, const char **p)
{
    while (**p == ' ') (*p)++;
    if (**p == '"') {
        jd_buf b = {0};
        for ((*p)++; **p && **p != '"'; (*p)++) {
            if (**p == '\\' && (*p)[1]) (*p)++;
            jd_buf_add(&b, *p, 1);
        }
        if (**p == '"') (*p)++;
        return b.s ? b.s : jd_strdup("");
    }
    if (**p == '{') {
        long n = strtol(*p + 1, NULL, 10);
        *p += strlen(*p);
        return read_bytes(c, n);
    }
    return NULL;
}

/* reads to the OK/NO/BYE line; lines before it go to each (data lines), the reason of NO/BYE to err */
static int response(conn *c, void (*each)(conn *, const char *, void *), void *ctx, char *err, int errlen)
{
    for (;;) {
        char *line = read_line(c);
        if (!line) {
            snprintf(err, (size_t)errlen, "the Sieve server closed the connection");
            return -1;
        }
        int ok = strncasecmp(line, "OK", 2) == 0, no = strncasecmp(line, "NO", 2) == 0 || strncasecmp(line, "BYE", 3) == 0;
        if (ok || no) {
            if (no) {
                const char *p = line + (line[0] == 'B' || line[0] == 'b' ? 3 : 2);
                while (*p == ' ') p++;
                if (*p == '(') p = strchr(p, ')') ? strchr(p, ')') + 1 : p;
                char *why = string_at(c, &p);
                snprintf(err, (size_t)errlen, "%s", why ? why : line);
                free(why);
            }
            free(line);
            return ok ? 0 : -1;
        }
        if (each) each(c, line, ctx);
        free(line);
    }
}

static int has_starttls;
static void caps(conn *c, const char *line, void *ctx)
{
    if (strncasecmp(line, "\"STARTTLS\"", 10) == 0) has_starttls = 1;
}

static void hangup(conn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
}

static int open_conn(conn *c, const jd_account *a, const char *password, char *err, int errlen)
{
    *c = (conn){.fd = -1};
    char port[16];
    snprintf(port, sizeof port, "%d", a->sieve_port ? a->sieve_port : 4190);
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(a->imap.host, port, &hints, &res) != 0) {
        snprintf(err, (size_t)errlen, "cannot find %s", a->imap.host);
        return -1;
    }
    for (struct addrinfo *r = res; r && c->fd < 0; r = r->ai_next) {
        c->fd = socket(r->ai_family, r->ai_socktype | SOCK_CLOEXEC, r->ai_protocol);
        struct timeval tv = {30, 0};
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (c->fd >= 0 && connect(c->fd, r->ai_addr, r->ai_addrlen) != 0) close(c->fd), c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0) {
        snprintf(err, (size_t)errlen, "no Sieve server at %s:%s", a->imap.host, port);
        return -1;
    }
    has_starttls = 0;
    if (response(c, caps, NULL, err, errlen) != 0) return -1;
    if (!has_starttls) {
        snprintf(err, (size_t)errlen, "the Sieve server offers no STARTTLS; not sending the password in plain text");
        return -1;
    }
    if (send_all(c, "STARTTLS\r\n", 10) != 0 || response(c, NULL, NULL, err, errlen) != 0) return -1;
    c->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(c->ctx);
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    c->ssl = SSL_new(c->ctx);
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, a->imap.host);
    SSL_set_hostflags(c->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set1_host(c->ssl, a->imap.host);
    c->len = c->pos = 0;
    if (SSL_connect(c->ssl) != 1) {
        snprintf(err, (size_t)errlen, "the Sieve server's certificate was not accepted");
        return -1;
    }
    /* RFC 5804 section 2.2: the server repeats its capabilities after STARTTLS */
    if (response(c, NULL, NULL, err, errlen) != 0) return -1;

    jd_buf plain = {0};
    jd_buf_add(&plain, "", 1);
    jd_buf_adds(&plain, a->username);
    jd_buf_add(&plain, "", 1);
    jd_buf_adds(&plain, password);
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    jd_buf cmd = {0};
    jd_buf_adds(&cmd, "AUTHENTICATE \"PLAIN\" \"");
    const unsigned char *s = (const unsigned char *)plain.s;
    for (size_t i = 0; i < plain.len; i += 3) {
        unsigned v = (unsigned)s[i] << 16 | (i + 1 < plain.len ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < plain.len ? s[i + 2] : 0);
        char q[4] = {b64[v >> 18 & 63], b64[v >> 12 & 63], i + 1 < plain.len ? b64[v >> 6 & 63] : '=',
                     i + 2 < plain.len ? b64[v & 63] : '='};
        jd_buf_add(&cmd, q, 4);
    }
    jd_buf_adds(&cmd, "\"\r\n");
    explicit_bzero(plain.s, plain.len);
    jd_buf_free(&plain);
    int r = send_all(c, cmd.s, cmd.len) == 0 ? response(c, NULL, NULL, err, errlen) : -1;
    explicit_bzero(cmd.s, cmd.len);
    jd_buf_free(&cmd);
    if (r != 0) {
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, (size_t)errlen, "Sieve sign-in refused: %s", why);
    }
    return r;
}

typedef struct {
    char *active, *first;
} listing;

static void on_script(conn *c, const char *line, void *ctx)
{
    listing *l = ctx;
    const char *p = line;
    char *name = string_at(c, &p);
    if (!name) return;
    if (strcasestr(p, "ACTIVE") && !l->active) l->active = jd_strdup(name);
    if (!l->first) l->first = jd_strdup(name);
    free(name);
}

typedef struct {
    char *text;
} script_body;

static void on_body(conn *c, const char *line, void *ctx)
{
    script_body *b = ctx;
    const char *p = line;
    if (!b->text) b->text = string_at(c, &p);
}

static void quoted(jd_buf *b, const char *s)
{
    jd_buf_adds(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') jd_buf_adds(b, "\\");
        jd_buf_add(b, s, 1);
    }
    jd_buf_adds(b, "\"");
}

int jd_sieve_get(const jd_account *a, const char *password, char **name, char **script, char *err, int errlen)
{
    *name = NULL;
    *script = NULL;
    conn c;
    int r = open_conn(&c, a, password, err, errlen);
    listing l = {0};
    if (r == 0) r = send_all(&c, "LISTSCRIPTS\r\n", 13) == 0 ? response(&c, on_script, &l, err, errlen) : -1;
    const char *pick = l.active ? l.active : l.first;
    if (r == 0 && pick) {
        jd_buf cmd = {0};
        jd_buf_adds(&cmd, "GETSCRIPT ");
        quoted(&cmd, pick);
        jd_buf_adds(&cmd, "\r\n");
        script_body body = {0};
        r = send_all(&c, cmd.s, cmd.len) == 0 ? response(&c, on_body, &body, err, errlen) : -1;
        jd_buf_free(&cmd);
        *script = body.text;
        *name = jd_strdup(pick);
    }
    if (r == 0 && !*script) *script = jd_strdup("");
    if (c.fd >= 0 && r == 0) send_all(&c, "LOGOUT\r\n", 8);
    free(l.active), free(l.first);
    hangup(&c);
    return r;
}

int jd_sieve_put(const jd_account *a, const char *password, const char *name, const char *script, char *err,
                 int errlen)
{
    conn c;
    int r = open_conn(&c, a, password, err, errlen);
    size_t n = strlen(script);
    if (r == 0) {
        jd_buf cmd = {0};
        jd_buf_addf(&cmd, "CHECKSCRIPT {%zu+}\r\n", n);
        jd_buf_add(&cmd, script, n);
        jd_buf_adds(&cmd, "\r\n");
        r = send_all(&c, cmd.s, cmd.len) == 0 ? response(&c, NULL, NULL, err, errlen) : -1;
        jd_buf_free(&cmd);
    }
    if (r == 0) {
        jd_buf cmd = {0};
        jd_buf_adds(&cmd, "PUTSCRIPT ");
        quoted(&cmd, name);
        jd_buf_addf(&cmd, " {%zu+}\r\n", n);
        jd_buf_add(&cmd, script, n);
        jd_buf_adds(&cmd, "\r\nSETACTIVE ");
        quoted(&cmd, name);
        jd_buf_adds(&cmd, "\r\n");
        r = send_all(&c, cmd.s, cmd.len) == 0 ? response(&c, NULL, NULL, err, errlen) : -1;
        if (r == 0) r = response(&c, NULL, NULL, err, errlen);
        jd_buf_free(&cmd);
    }
    if (c.fd >= 0 && r == 0) send_all(&c, "LOGOUT\r\n", 8);
    hangup(&c);
    return r;
}
