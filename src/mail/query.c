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

/* one term into q; free words gather in words */
static void apply(jd_list_query *q, char **folder, const char *key, const char *value, jd_buf *words)
{
    int used = 1;
    if (!key) used = 0;
    else if (!strcasecmp(key, "from")) set(&q->from, jd_strdup(value));
    else if (!strcasecmp(key, "to")) set(&q->to, jd_strdup(value));
    else if (!strcasecmp(key, "subject")) set(&q->subject, jd_strdup(value));
    else if (!strcasecmp(key, "in")) set(folder, jd_strdup(value));
    else if (!strcasecmp(key, "is") && !strcasecmp(value, "unread")) q->unread = 1;
    else if (!strcasecmp(key, "is") && !strcasecmp(value, "read")) q->read = 1;
    else if (!strcasecmp(key, "is") && !strcasecmp(value, "flagged")) q->flagged = 1;
    else if (!strcasecmp(key, "has") && !strncasecmp(value, "attachment", 10)) q->attachment = 1;
    else if (!strcasecmp(key, "after") && day_of(value)) q->after = day_of(value);
    else if (!strcasecmp(key, "before") && day_of(value)) q->before = day_of(value);
    else used = 0;
    if (!used) {
        if (key) jd_buf_addf(words, "%s%s:%s", words->len ? " " : "", key, value);
        else if (value[0]) jd_buf_addf(words, "%s%s", words->len ? " " : "", value);
    }
}

typedef struct {
    char *key, *value;
} term;

static int is_or(const term *t) { return !t->key && strcmp(t->value, "OR") == 0; }

void jd_search_parse(const char *text, jd_search *out)
{
    *out = (jd_search){0};
    term *t = NULL;
    int n = 0, cap = 0;
    char *key, *value;
    for (const char *p = text; (p = next_token(p, &key, &value));) {
        if (n == cap) t = jd_realloc(t, sizeof *t * (size_t)(cap = cap ? cap * 2 : 16));
        t[n++] = (term){key, value};
    }
    jd_buf words = {0};
    for (int i = 0; i < n;) {
        int end = i + 1;
        while (end + 1 < n && is_or(&t[end]) && !is_or(&t[end + 1])) end += 2;
        if (end == i + 1) {
            if (!is_or(&t[i])) apply(&out->q, &out->folder_name, t[i].key, t[i].value, &words);
            i = end;
            continue;
        }
        jd_query_any any = {.alt = jd_alloc(sizeof *any.alt * (size_t)((end - i + 1) / 2))};
        for (int k = i; k < end; k += 2) {
            jd_list_query *alt = &any.alt[any.n++];
            *alt = (jd_list_query){0};
            jd_buf w = {0};
            apply(alt, &out->folder_name, t[k].key, t[k].value, &w);
            alt->text = w.s;
        }
        out->q.any = jd_realloc(out->q.any, sizeof *out->q.any * (size_t)(out->q.nany + 1));
        out->q.any[out->q.nany++] = any;
        i = end;
    }
    for (int i = 0; i < n; i++) free(t[i].key), free(t[i].value);
    free(t);
    out->q.text = words.s;
}

static void query_free(jd_list_query *q)
{
    free(q->from), free(q->to), free(q->subject), free(q->text);
    for (int g = 0; g < q->nany; g++) {
        for (int a = 0; a < q->any[g].n; a++) query_free(&q->any[g].alt[a]);
        free(q->any[g].alt);
    }
    free(q->any);
}

void jd_search_free(jd_search *s)
{
    query_free(&s->q);
    free(s->folder_name);
    *s = (jd_search){0};
}
