#pragma once
#include <stddef.h>
#include <stdint.h>

/* RFC 2047 encoded words to UTF-8; undecodable input comes back as it was */
char *jd_mime_header(const char *raw);
/* RFC 5322 date-time to Unix time, 0 when unparsable */
int64_t jd_mime_date(const char *raw);
/* "<id>" to "id" */
char *jd_mime_msgid(const char *raw);
/* the value of the first field called name (a non-standard one, such as X-Send-Later-At) in a header block, or in a
   whole message's headers; trimmed, NULL when absent */
char *jd_mime_field(const char *headers, size_t len, const char *name);
/* the msg-ids of a References header field block, in order */
char **jd_mime_references(const char *headers, size_t len, int *n);
/* the text of a raw RFC 5322 message for the search index: text/plain parts, else text/html without markup */
char *jd_mime_body_text(const char *raw, size_t len);
/* the first text/html part, decoded to UTF-8; NULL when the message has none */
char *jd_mime_body_html(const char *raw, size_t len);

/* the headers a draft is reopened with; address lists come back as "Name <addr>, ..." */
typedef struct {
    char *to, *cc, *bcc, *subject, *in_reply_to, *references;
} jd_draft_headers;

void jd_mime_draft_headers(const char *raw, size_t len, jd_draft_headers *out);
void jd_draft_headers_free(jd_draft_headers *h);

/* Parts with Content-Disposition attachment, or non-text parts that carry a file name */
typedef struct {
    char *name, *type;
    size_t size; /* decoded bytes */
} jd_attachment;

jd_attachment *jd_mime_attachments(const char *raw, size_t len, int *n);

/* parts with a Content-ID (RFC 2392 cid: targets), decoded */
typedef struct {
    char *cid, *type;
    char *data;
    size_t len;
} jd_inline_part;

jd_inline_part *jd_mime_inline_parts(const char *raw, size_t len, int *n);
void jd_inline_parts_free(jd_inline_part *p, int n);
void jd_attachments_free(jd_attachment *a, int n);
/* attachment index of jd_mime_attachments, decoded; free with free() */
char *jd_mime_attachment_data(const char *raw, size_t len, int index, size_t *out_len);
