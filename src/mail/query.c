#define _GNU_SOURCE
#include "query.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/util.h"

static const char *next_token(const char *p, char **key, char **value)
{
    while (*p == ' ') p++;
    *key = *value = NULL;
    if (!*p) return NULL;
    const char *start = p, *colon = NULL;
    while (*p && *p != ' ' && *p != '"') {
        if (*p == ':' && !colon) colon = p;
        p++;
    }
    if (colon && colon > start) {
        *key = jd_strndup(start, (size_t)(colon - start));
        start = colon + 1;
    } else {
        p = start;
    }
    jd_buf b = {0};
    if (*p == '"' || (colon && *start == '"')) {
        p = (*p == '"' ? p : start) + 1;
        while (*p && *p != '"') jd_buf_add(&b, p++, 1);
        if (*p == '"') p++;
    } else {
        if (colon) p = start;
        while (*p && *p != ' ') jd_buf_add(&b, p++, 1);
    }
    *value = b.s ? b.s : jd_strdup("");
    return p;
}

/* DD-MM-YYYY, or ISO 8601 YYYY-MM-DD */
static int64_t day_of(const char *s)
{
    struct tm tm = {0};
    const char *end = strptime(s, "%d-%m-%Y", &tm);
    if (!end || *end) {
        tm = (struct tm){0};
        end = strptime(s, "%Y-%m-%d", &tm);
    }
    if (!end || *end) return 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static void set(char **field, char *value)
{
    free(*field);
    *field = value;
}

void jd_search_parse(const char *text, jd_search *out)
{
    *out = (jd_search){0};
    jd_buf words = {0};
    char *key, *value;
    for (const char *p = text; (p = next_token(p, &key, &value));) {
        int used = 1;
        if (!key) used = 0;
        else if (!strcasecmp(key, "from")) set(&out->q.from, jd_strdup(value));
        else if (!strcasecmp(key, "to")) set(&out->q.to, jd_strdup(value));
        else if (!strcasecmp(key, "subject")) set(&out->q.subject, jd_strdup(value));
        else if (!strcasecmp(key, "in")) set(&out->folder_name, jd_strdup(value));
        else if (!strcasecmp(key, "is") && !strcasecmp(value, "unread")) out->q.unread = 1;
        else if (!strcasecmp(key, "is") && !strcasecmp(value, "read")) out->q.read = 1;
        else if (!strcasecmp(key, "is") && !strcasecmp(value, "flagged")) out->q.flagged = 1;
        else if (!strcasecmp(key, "has") && !strncasecmp(value, "attachment", 10)) out->q.attachment = 1;
        else if (!strcasecmp(key, "after") && day_of(value)) out->q.after = day_of(value);
        else if (!strcasecmp(key, "before") && day_of(value)) out->q.before = day_of(value);
        else used = 0;
        if (!used) {
            if (key) jd_buf_addf(&words, "%s%s:%s", words.len ? " " : "", key, value);
            else if (value[0]) jd_buf_addf(&words, "%s%s", words.len ? " " : "", value);
        }
        free(key), free(value);
    }
    out->q.text = words.s;
}

void jd_search_free(jd_search *s)
{
    free(s->q.from), free(s->q.to), free(s->q.subject), free(s->q.text), free(s->folder_name);
    *s = (jd_search){0};
}
