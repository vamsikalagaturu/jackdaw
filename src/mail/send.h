#pragma once
#include <stddef.h>

#include "mail/account.h"

typedef struct {
    char *name, *type;
    char *data;
    size_t len;
    char *cid; /* a part the HTML shows (cid: URL, RFC 2392), without <>; NULL for an attachment */
} jd_part;

/* What the compose view hands over. to, cc, bcc are address lists as typed ("Ada <ada@x>, bob@y"). */
typedef struct {
    const char *to, *cc, *bcc, *subject, *body;
    const char *html; /* NULL: text/plain only; else sent beside body in multipart/alternative (RFC 2046 5.1.4) */
    const char *in_reply_to, *references; /* msg-ids without <>, references space-separated */
    const char *message_id;               /* kept across draft saves; made when NULL */
    const jd_part *parts;
    int nparts;
    /* images the HTML shows by cid: (a quoted original's): beside it in multipart/related (RFC 2387) */
    const jd_part *related;
    int nrelated;
    int draft; /* Bcc stays in the headers of a draft, so it survives reopening */
} jd_outgoing;

/* RFC 5322 + MIME (RFC 2045-2047, 2231): UTF-8 text as quoted-printable, attachments base64 in multipart/mixed.
   rcpts gets the envelope recipients (to, cc, bcc). NULL with err set when an address list does not parse. */
char *jd_message_build(const jd_account *from, const jd_outgoing *o, size_t *len, char ***rcpts, int *nrcpts,
                       char *err, int errlen);
void jd_rcpts_free(char **r, int n);
/* a fresh <random@domain-of-address> id, without the brackets */
char *jd_message_id_new(const char *address);

/* SMTP submission (RFC 6409) with AUTH, implicit TLS or STARTTLS, certificates checked as for IMAP */
int jd_smtp_send(const jd_account *a, const char *password, char **rcpts, int nrcpts, const char *msg, size_t len,
                 char *err, int errlen);

/* type for a file name's extension, application/octet-stream when unknown */
const char *jd_mime_type_of(const char *name);
