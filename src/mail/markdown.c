#include "mail/markdown.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

typedef struct {
    const char *s;
    int n;
} line;

static void escape(jd_buf *b, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': jd_buf_adds(b, "&amp;"); break;
        case '<': jd_buf_adds(b, "&lt;"); break;
        case '>': jd_buf_adds(b, "&gt;"); break;
        case '"': jd_buf_adds(b, "&quot;"); break;
        default: jd_buf_add(b, s + i, 1);
        }
    }
}

static int find(const char *s, int from, int n, const char *what)
{
    int k = (int)strlen(what);
    for (int i = from; i + k <= n; i++)
        if (!memcmp(s + i, what, (size_t)k)) return i;
    return -1;
}

static int url_char_end(const char *s, int i, int n)
{
    int j = i;
    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '<' && s[j] != '>') j++;
    while (j > i && strchr(".,;:!?)'\"", s[j - 1])) j--;
    return j;
}

static void link(jd_buf *b, const char *url, int un, const char *text, int tn);

static void inlines(jd_buf *b, const char *s, int n)
{
    for (int i = 0; i < n;) {
        char c = s[i];
        int prev_word = i > 0 && isalnum((unsigned char)s[i - 1]);
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            escape(b, s + i + 1, 1);
            i += 2;
            continue;
        }
        if (c == '`') {
            int end = find(s, i + 1, n, "`");
            if (end > i + 1) {
                jd_buf_adds(b, "<code>");
                escape(b, s + i + 1, end - i - 1);
                jd_buf_adds(b, "</code>");
                i = end + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && !(c == '_' && prev_word)) {
            int dbl = i + 1 < n && s[i + 1] == c;
            const char *close = dbl ? (c == '*' ? "**" : "__") : (c == '*' ? "*" : "_");
            int open = i + (dbl ? 2 : 1);
            int end = open < n && !isspace((unsigned char)s[open]) ? find(s, open + 1, n, close) : -1;
            /* single _ must close at a word end; skip a closing that is the start of a double marker */
            while (end > 0 && !dbl && ((c == '_' && end + 1 < n && isalnum((unsigned char)s[end + 1])) ||
                                       (end + 1 < n && s[end + 1] == c)))
                end = find(s, end + 2, n, close);
            if (end > open && !isspace((unsigned char)s[end - 1])) {
                jd_buf_adds(b, dbl ? "<strong>" : "<em>");
                inlines(b, s + open, end - open);
                jd_buf_adds(b, dbl ? "</strong>" : "</em>");
                i = end + (dbl ? 2 : 1);
                continue;
            }
        }
        if (c == '[') {
            int mid = find(s, i + 1, n, "](");
            int end = mid > 0 ? find(s, mid + 2, n, ")") : -1;
            if (end > mid) {
                link(b, s + mid + 2, end - mid - 2, s + i + 1, mid - i - 1);
                i = end + 1;
                continue;
            }
        }
        if (c == '<' && (find(s, i, n, "<http://") == i || find(s, i, n, "<https://") == i || find(s, i, n, "<mailto:") == i)) {
            int end = find(s, i, n, ">");
            if (end > 0) {
                link(b, s + i + 1, end - i - 1, NULL, 0);
                i = end + 1;
                continue;
            }
        }
        if (!prev_word && (find(s, i, n, "http://") == i || find(s, i, n, "https://") == i)) {
            int end = url_char_end(s, i, n);
            if (end > i + 8) {
                link(b, s + i, end - i, NULL, 0);
                i = end;
                continue;
            }
        }
        escape(b, s + i, 1);
        i++;
    }
}

static void link(jd_buf *b, const char *url, int un, const char *text, int tn)
{
    jd_buf_adds(b, "<a href=\"");
    escape(b, url, un);
    jd_buf_adds(b, "\">");
    if (text) inlines(b, text, tn);
    else escape(b, url, un);
    jd_buf_adds(b, "</a>");
}

static int blank(line l)
{
    for (int i = 0; i < l.n; i++)
        if (!isspace((unsigned char)l.s[i])) return 0;
    return 1;
}

static int indent(line l)
{
    int i = 0;
    while (i < l.n && l.s[i] == ' ') i++;
    return i;
}

static int heading(line l)
{
    int i = 0;
    while (i < l.n && l.s[i] == '#') i++;
    return i >= 1 && i <= 6 && (i == l.n || l.s[i] == ' ') ? i : 0;
}

static int rule(line l)
{
    char m = 0;
    int k = 0;
    for (int i = 0; i < l.n; i++) {
        if (l.s[i] == ' ') continue;
        if (!strchr("-*_", l.s[i]) || (m && l.s[i] != m)) return 0;
        m = l.s[i];
        k++;
    }
    return k >= 3;
}

/* the width of a list marker at the start of l ("- ", "1. "), 0 when there is none; *ordered says which */
static int item(line l, int *ordered)
{
    int i = indent(l);
    if (i + 1 < l.n && strchr("-*+", l.s[i]) && l.s[i + 1] == ' ') {
        *ordered = 0;
        return i + 2;
    }
    int d = i;
    while (d < l.n && isdigit((unsigned char)l.s[d])) d++;
    if (d > i && d - i < 10 && d + 1 < l.n && (l.s[d] == '.' || l.s[d] == ')') && l.s[d + 1] == ' ') {
        *ordered = 1;
        return d + 2;
    }
    return 0;
}

static int fence(line l)
{
    int i = indent(l);
    return l.n - i >= 3 && !strncmp(l.s + i, "```", 3);
}

static int signature(line l)
{
    return l.n == 3 && !strncmp(l.s, "-- ", 3);
}

static void blocks(jd_buf *b, line *ls, int n);

static void paragraph(jd_buf *b, line *ls, int n)
{
    jd_buf_adds(b, "<p>");
    for (int i = 0; i < n; i++) {
        if (i) jd_buf_adds(b, "<br>\n");
        line l = ls[i];
        while (l.n && l.s[l.n - 1] == ' ') l.n--;
        inlines(b, l.s, l.n);
    }
    jd_buf_adds(b, "</p>\n");
}

static void blocks(jd_buf *b, line *ls, int n)
{
    int i = 0;
    while (i < n) {
        line l = ls[i];
        int ordered;
        if (blank(l)) {
            i++;
        } else if (signature(l)) {
            jd_buf_adds(b, "<div style=\"color:#6b7280\">-- <br>\n");
            for (i++; i < n; i++) {
                escape(b, ls[i].s, ls[i].n);
                jd_buf_adds(b, "<br>\n");
            }
            jd_buf_adds(b, "</div>\n");
        } else if (fence(l)) {
            jd_buf_adds(b, "<pre style=\"background:#f3f4f6;padding:8px;border-radius:4px\"><code>");
            for (i++; i < n && !fence(ls[i]); i++) {
                escape(b, ls[i].s, ls[i].n);
                jd_buf_adds(b, "\n");
            }
            jd_buf_adds(b, "</code></pre>\n");
            i++;
        } else if (heading(l)) {
            int h = heading(l), s = h;
            while (s < l.n && l.s[s] == ' ') s++;
            int e = l.n;
            while (e > s && (l.s[e - 1] == '#' || l.s[e - 1] == ' ')) e--;
            jd_buf_addf(b, "<h%d>", h);
            inlines(b, l.s + s, e - s);
            jd_buf_addf(b, "</h%d>\n", h);
            i++;
        } else if (rule(l)) {
            jd_buf_adds(b, "<hr>\n");
            i++;
        } else if (indent(l) < 4 && l.s[indent(l)] == '>') {
            int j = i;
            line *inner = jd_alloc(sizeof *inner * (size_t)n);
            int k = 0;
            for (; j < n && !blank(ls[j]) && ls[j].s[indent(ls[j])] == '>'; j++) {
                int s = indent(ls[j]) + 1;
                if (s < ls[j].n && ls[j].s[s] == ' ') s++;
                inner[k++] = (line){ls[j].s + s, ls[j].n - s};
            }
            jd_buf_adds(b, "<blockquote style=\"margin:0 0 0 .8ex;border-left:2px solid #ccc;padding-left:1ex;color:#555\">\n");
            blocks(b, inner, k);
            jd_buf_adds(b, "</blockquote>\n");
            free(inner);
            i = j;
        } else if (item(l, &ordered)) {
            int kind = ordered, o;
            jd_buf_adds(b, kind ? "<ol>\n" : "<ul>\n");
            while (i < n && item(ls[i], &o) && o == kind) {
                int w = item(ls[i], &o);
                jd_buf_adds(b, "<li>");
                inlines(b, ls[i].s + w, ls[i].n - w);
                for (i++; i < n && !blank(ls[i]) && indent(ls[i]) >= 2 && !item(ls[i], &o); i++) {
                    jd_buf_adds(b, "<br>\n");
                    int s = indent(ls[i]);
                    inlines(b, ls[i].s + s, ls[i].n - s);
                }
                jd_buf_adds(b, "</li>\n");
            }
            jd_buf_adds(b, kind ? "</ol>\n" : "</ul>\n");
        } else {
            int j = i + 1;
            while (j < n && !blank(ls[j]) && !signature(ls[j]) && !fence(ls[j]) && !heading(ls[j]) && !rule(ls[j]) &&
                   ls[j].s[indent(ls[j])] != '>' && !item(ls[j], &ordered))
                j++;
            paragraph(b, ls + i, j - i);
            i = j;
        }
    }
}

char *jd_markdown_html(const char *text)
{
    if (!text) text = "";
    int n = 0, cap = 64;
    line *ls = jd_alloc(sizeof *ls * (size_t)cap);
    for (const char *p = text;;) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (n == cap) ls = jd_realloc(ls, sizeof *ls * (size_t)(cap *= 2));
        ls[n++] = (line){p, len};
        if (!nl) break;
        p = nl + 1;
    }
    jd_buf b = {0};
    jd_buf_adds(&b, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head>\n"
                    "<body style=\"font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;font-size:14px;"
                    "line-height:1.5\">\n");
    blocks(&b, ls, n);
    jd_buf_adds(&b, "</body></html>\n");
    free(ls);
    return b.s;
}
