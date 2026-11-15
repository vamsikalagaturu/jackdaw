#include "toml.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *table, *key, *value;
    int is_string;
} entry;

struct jd_toml {
    entry *e;
    int n;
};

static void skip_space(const char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
}

static int bare_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static void put_utf8(jd_buf *b, unsigned long cp)
{
    char out[4];
    int n;
    if (cp < 0x80) out[0] = (char)cp, n = 1;
    else if (cp < 0x800) out[0] = (char)(0xc0 | cp >> 6), out[1] = (char)(0x80 | (cp & 0x3f)), n = 2;
    else if (cp < 0x10000)
        out[0] = (char)(0xe0 | cp >> 12), out[1] = (char)(0x80 | (cp >> 6 & 0x3f)), out[2] = (char)(0x80 | (cp & 0x3f)), n = 3;
    else
        out[0] = (char)(0xf0 | cp >> 18), out[1] = (char)(0x80 | (cp >> 12 & 0x3f)), out[2] = (char)(0x80 | (cp >> 6 & 0x3f)),
        out[3] = (char)(0x80 | (cp & 0x3f)), n = 4;
    jd_buf_add(b, out, (size_t)n);
}

static char *parse_string(const char **p)
{
    jd_buf b = {0};
    (*p)++;
    while (**p && **p != '"' && **p != '\n') {
        if (**p != '\\') {
            jd_buf_add(&b, (*p)++, 1);
            continue;
        }
        (*p)++;
        char c = *(*p)++;
        switch (c) {
        case 'n': jd_buf_adds(&b, "\n"); break;
        case 't': jd_buf_adds(&b, "\t"); break;
        case 'r': jd_buf_adds(&b, "\r"); break;
        case '"': jd_buf_adds(&b, "\""); break;
        case '\\': jd_buf_adds(&b, "\\"); break;
        case 'u':
        case 'U': {
            int len = c == 'u' ? 4 : 8;
            char hex[9] = {0};
            for (int i = 0; i < len; i++) {
                if (!isxdigit((unsigned char)**p)) goto bad;
                hex[i] = *(*p)++;
            }
            put_utf8(&b, strtoul(hex, NULL, 16));
            break;
        }
        default: goto bad;
        }
    }
    if (**p != '"') goto bad;
    (*p)++;
    if (!b.s) b.s = jd_strdup("");
    return b.s;
bad:
    jd_buf_free(&b);
    return NULL;
}

/* entries of [[name]] are tables "name.0", "name.1", ... */
static int next_index(jd_toml *t, const char *name)
{
    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s.", name);
    int next = 0;
    for (int i = 0; i < t->n; i++)
        if (strncmp(t->e[i].table, prefix, strlen(prefix)) == 0) {
            int k = atoi(t->e[i].table + strlen(prefix));
            if (k + 1 > next) next = k + 1;
        }
    return next;
}

jd_toml *jd_toml_parse(const char *text, char *err, size_t errlen)
{
    jd_toml *t = jd_alloc(sizeof *t);
    char *table = jd_strdup("");
    int line = 1;
    int pending_index = -1;
    char *pending_name = NULL;
    for (const char *p = text; *p; line++) {
        skip_space(&p);
        if (*p == '[') {
            int array = p[1] == '[';
            p += array ? 2 : 1;
            const char *s = p;
            while (bare_char(*p) || *p == '.') p++;
            if (*p != ']' || p == s || (array && p[1] != ']')) goto bad;
            free(table);
            table = jd_strndup(s, (size_t)(p - s));
            p += array ? 2 : 1;
            if (array) {
                /* an entry without keys still counts, so the next [[name]] gets the next index */
                if (pending_name && jd_streq(pending_name, table)) pending_index++;
                else pending_index = next_index(t, table);
                free(pending_name);
                pending_name = jd_strdup(table);
                char *indexed = jd_printf("%s.%d", table, pending_index);
                free(table);
                table = indexed;
            }
        } else if (bare_char(*p)) {
            const char *s = p;
            while (bare_char(*p)) p++;
            entry e = {.table = jd_strdup(table), .key = jd_strndup(s, (size_t)(p - s))};
            skip_space(&p);
            if (*p++ != '=') goto bad_entry;
            skip_space(&p);
            if (*p == '"') {
                e.value = parse_string(&p);
                e.is_string = 1;
            } else {
                const char *v = p;
                while (*p && !isspace((unsigned char)*p) && *p != '#') p++;
                if (p > v) e.value = jd_strndup(v, (size_t)(p - v));
            }
            if (!e.value) goto bad_entry;
            t->e = jd_realloc(t->e, sizeof(entry) * (size_t)(t->n + 1));
            t->e[t->n++] = e;
            goto rest;
        bad_entry:
            free(e.table), free(e.key), free(e.value);
            goto bad;
        }
    rest:
        skip_space(&p);
        if (*p == '#')
            while (*p && *p != '\n') p++;
        if (*p == '\r') p++;
        if (*p && *p != '\n') goto bad;
        if (*p) p++;
    }
    free(table), free(pending_name);
    return t;
bad:
    if (err) snprintf(err, errlen, "line %d: not understood", line);
    free(table), free(pending_name);
    jd_toml_free(t);
    return NULL;
}

int jd_toml_bool(const jd_toml *t, const char *table, const char *key, int fallback)
{
    const char *v = NULL;
    for (int i = t ? t->n - 1 : -1; i >= 0 && !v; i--)
        if (!t->e[i].is_string && jd_streq(t->e[i].table, table) && jd_streq(t->e[i].key, key)) v = t->e[i].value;
    return jd_streq(v, "true") ? 1 : jd_streq(v, "false") ? 0 : fallback;
}

int jd_toml_count(const jd_toml *t, const char *array)
{
    return t ? next_index((jd_toml *)t, array) : 0;
}

jd_toml *jd_toml_load(const char *path, char *err, size_t errlen)
{
    char *text = jd_read_file(path, NULL);
    if (!text) {
        if (err) snprintf(err, errlen, "cannot read %s", path);
        return NULL;
    }
    jd_toml *t = jd_toml_parse(text, err, errlen);
    free(text);
    return t;
}

void jd_toml_free(jd_toml *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(t->e[i].table), free(t->e[i].key), free(t->e[i].value);
    free(t->e);
    free(t);
}

static const entry *find(const jd_toml *t, const char *table, const char *key)
{
    for (int i = t ? t->n - 1 : -1; i >= 0; i--)
        if (jd_streq(t->e[i].table, table) && jd_streq(t->e[i].key, key)) return &t->e[i];
    return NULL;
}

const char *jd_toml_str(const jd_toml *t, const char *table, const char *key)
{
    const entry *e = find(t, table, key);
    return e && e->is_string ? e->value : NULL;
}

int jd_toml_int(const jd_toml *t, const char *table, const char *key, long long *out)
{
    const entry *e = find(t, table, key);
    if (!e || e->is_string) return -1;
    char *end;
    long long v = strtoll(e->value, &end, 10);
    if (*end) return -1;
    *out = v;
    return 0;
}

void jd_toml_quote(jd_buf *b, const char *s)
{
    jd_buf_adds(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') jd_buf_addf(b, "\\%c", c);
        else if (c == '\n') jd_buf_adds(b, "\\n");
        else if (c == '\t') jd_buf_adds(b, "\\t");
        else if (c < 0x20 || c == 0x7f) jd_buf_addf(b, "\\u%04x", c);
        else jd_buf_add(b, s, 1);
    }
    jd_buf_adds(b, "\"");
}
