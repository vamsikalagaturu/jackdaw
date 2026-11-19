#define _GNU_SOURCE
#include "caldav.h"

#include <curl/curl.h>
#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"

#define NS_DAV "DAV: "
#define NS_CALDAV "urn:ietf:params:xml:ns:caldav "
#define NS_CS "http://calendarserver.org/ns/ "
#define NS_APPLE "http://apple.com/ns/ical/ "

enum { MAX_BODY = 64 * 1024 * 1024, MULTIGET_BATCH = 100 };

struct jd_dav {
    CURL *curl;
    char *user, *password, *bearer;
    char error[512];
    int auto_schedule;
    /* the last answer */
    long status;
    jd_buf body;
    char *etag, *location;
    char *effective; /* the URL that answered, after redirects */
};

jd_dav *jd_dav_new(const char *user, const char *password)
{
    jd_dav *d = jd_alloc(sizeof *d);
    d->curl = curl_easy_init();
    d->user = jd_strdup(user);
    d->password = jd_strdup(password);
    return d;
}

void jd_dav_set_bearer(jd_dav *d, const char *token)
{
    free(d->bearer);
    d->bearer = jd_strdup(token);
}

void jd_dav_free(jd_dav *d)
{
    if (!d) return;
    if (d->curl) curl_easy_cleanup(d->curl);
    if (d->password) jd_wipe(d->password, strlen(d->password));
    if (d->bearer) jd_wipe(d->bearer, strlen(d->bearer));
    free(d->user), free(d->password), free(d->bearer), free(d->etag), free(d->location), free(d->effective);
    jd_buf_free(&d->body);
    free(d);
}

const char *jd_dav_error(jd_dav *d)
{
    return d->error;
}

int jd_dav_auto_schedule(jd_dav *d)
{
    return d->auto_schedule;
}

/* ---- URLs ---- */

char *jd_dav_url(const char *base, const char *href)
{
    if (!href || !href[0]) return jd_strdup(base);
    if (strncasecmp(href, "http://", 7) == 0 || strncasecmp(href, "https://", 8) == 0) return jd_strdup(href);
    const char *scheme_end = strstr(base, "://");
    const char *host_end = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    size_t origin = host_end ? (size_t)(host_end - base) : strlen(base);
    if (href[0] == '/') return jd_printf("%.*s%s", (int)origin, base, href);
    const char *q = strpbrk(base + origin, "?#");
    size_t path_end = q ? (size_t)(q - base) : strlen(base);
    size_t dir = path_end;
    while (dir > origin && base[dir - 1] != '/') dir--;
    if (dir == origin) return jd_printf("%.*s/%s", (int)origin, base, href);
    return jd_printf("%.*s%s", (int)dir, base, href);
}

const char *jd_dav_path(const char *url)
{
    const char *s = strstr(url, "://");
    if (!s) return url;
    const char *p = strchr(s + 3, '/');
    return p ? p : "/";
}

static int same_resource(const char *a, const char *b)
{
    const char *pa = jd_dav_path(a), *pb = jd_dav_path(b);
    size_t la = strlen(pa), lb = strlen(pb);
    while (la > 1 && pa[la - 1] == '/') la--;
    while (lb > 1 && pb[lb - 1] == '/') lb--;
    return la == lb && strncmp(pa, pb, la) == 0;
}

/* ---- XML ---- */

typedef struct node {
    char *name;      /* "namespace local", as expat joins them */
    char *name_attr; /* its name="" attribute: <C:comp name="VEVENT"/> */
    jd_buf text;
    struct node **kids;
    int nkids;
    struct node *up;
} node;

static void node_free(node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids), free(n->name), free(n->name_attr);
    jd_buf_free(&n->text);
    free(n);
}

typedef struct {
    node *root, *at;
} tree;

static void on_start(void *data, const XML_Char *name, const XML_Char **attrs)
{
    tree *t = data;
    node *n = jd_alloc(sizeof *n);
    n->name = jd_strdup(name);
    for (int i = 0; attrs[i] && attrs[i + 1]; i += 2)
        if (strcmp(attrs[i], "name") == 0) n->name_attr = jd_strdup(attrs[i + 1]);
    n->up = t->at;
    if (t->at) {
        t->at->kids = jd_realloc(t->at->kids, sizeof *t->at->kids * (size_t)(t->at->nkids + 1));
        t->at->kids[t->at->nkids++] = n;
    } else if (!t->root) {
        t->root = n;
    } else {
        node_free(n);
        return;
    }
    t->at = n;
}

static void on_end(void *data, const XML_Char *name)
{
    tree *t = data;
    if (t->at) t->at = t->at->up;
}

static void on_text(void *data, const XML_Char *s, int len)
{
    tree *t = data;
    if (t->at) jd_buf_add(&t->at->text, s, (size_t)len);
}

static node *xml_parse(const char *s, size_t len)
{
    XML_Parser p = XML_ParserCreateNS(NULL, ' ');
    if (!p) return NULL;
    tree t = {0};
    XML_SetUserData(p, &t);
    XML_SetElementHandler(p, on_start, on_end);
    XML_SetCharacterDataHandler(p, on_text);
    int ok = XML_Parse(p, s, (int)len, 1) == XML_STATUS_OK;
    XML_ParserFree(p);
    if (!ok) {
        node_free(t.root);
        return NULL;
    }
    return t.root;
}

static node *child(const node *n, const char *name)
{
    for (int i = 0; n && i < n->nkids; i++)
        if (strcmp(n->kids[i]->name, name) == 0) return n->kids[i];
    return NULL;
}

static char *text_of(const node *n)
{
    if (!n || !n->text.s) return NULL;
    const char *s = n->text.s;
    size_t len = n->text.len;
    while (len && strchr(" \t\r\n", *s)) s++, len--;
    while (len && strchr(" \t\r\n", s[len - 1])) len--;
    return len ? jd_strndup(s, len) : NULL;
}

static long status_code(const node *status)
{
    char *s = text_of(status);
    long code = 0;
    const char *sp = s ? strchr(s, ' ') : NULL;
    if (sp) code = strtol(sp + 1, NULL, 10);
    free(s);
    return code;
}

/* a response's properties that came with 200 */
static node *ok_props(const node *response)
{
    for (int i = 0; i < response->nkids; i++) {
        node *ps = response->kids[i];
        if (strcmp(ps->name, NS_DAV "propstat") != 0) continue;
        long code = status_code(child(ps, NS_DAV "status"));
        if (code >= 200 && code < 300) return child(ps, NS_DAV "prop");
    }
    return NULL;
}

static void xml_escape(jd_buf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '&') jd_buf_adds(b, "&amp;");
        else if (*s == '<') jd_buf_adds(b, "&lt;");
        else if (*s == '>') jd_buf_adds(b, "&gt;");
        else jd_buf_add(b, s, 1);
    }
}

/* ---- HTTP ---- */

static size_t on_body(char *p, size_t size, size_t n, void *data)
{
    jd_dav *d = data;
    if (d->body.len + size * n > MAX_BODY) return 0;
    jd_buf_add(&d->body, p, size * n);
    return size * n;
}

static char *header_value(const char *line, size_t len, const char *name)
{
    size_t nl = strlen(name);
    if (len <= nl || strncasecmp(line, name, nl) != 0 || line[nl] != ':') return NULL;
    const char *v = line + nl + 1;
    size_t vl = len - nl - 1;
    while (vl && (*v == ' ' || *v == '\t')) v++, vl--;
    while (vl && strchr(" \t\r\n", v[vl - 1])) vl--;
    return jd_strndup(v, vl);
}

static size_t on_header(char *p, size_t size, size_t n, void *data)
{
    jd_dav *d = data;
    size_t len = size * n;
    /* a redirect's or a 100's headers come first: only the final answer's count */
    if (len > 5 && strncmp(p, "HTTP/", 5) == 0) {
        free(d->etag), free(d->location);
        d->etag = d->location = NULL;
    }
    char *v;
    if ((v = header_value(p, len, "ETag"))) free(d->etag), d->etag = v;
    else if ((v = header_value(p, len, "Location"))) free(d->location), d->location = v;
    else if ((v = header_value(p, len, "DAV"))) {
        if (strcasestr(v, "calendar-auto-schedule")) d->auto_schedule = 1;
        free(v);
    }
    return len;
}

/* method on url with body (may be NULL) and the extra headers; the answer's status in d->status, its body in
   d->body. JD_DAV_AUTH for 401, JD_DAV_ERROR when no answer came. */
static int request(jd_dav *d, const char *method, const char *url, const char *depth, const char *type,
                   const char *body, const char *const *headers)
{
    CURL *c = d->curl;
    curl_easy_reset(c);
    jd_buf_free(&d->body);
    free(d->etag), free(d->location), free(d->effective);
    d->etag = d->location = d->effective = NULL;
    d->status = 0;
    struct curl_slist *h = NULL;
    char line[256];
    if (depth) {
        snprintf(line, sizeof line, "Depth: %s", depth);
        h = curl_slist_append(h, line);
    }
    if (type) {
        snprintf(line, sizeof line, "Content-Type: %s", type);
        h = curl_slist_append(h, line);
    }
    for (int i = 0; headers && headers[i]; i++) h = curl_slist_append(h, headers[i]);
    char *auth = NULL;
    if (d->bearer) {
        auth = jd_printf("Authorization: Bearer %s", d->bearer);
        h = curl_slist_append(h, auth);
    } else if (d->user) {
        curl_easy_setopt(c, CURLOPT_USERNAME, d->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, d->password ? d->password : "");
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)(CURLAUTH_BASIC | CURLAUTH_DIGEST));
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    /* a redirect never leaves https for http; a plain-http server (on a LAN) may redirect within http */
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, strncasecmp(url, "http://", 7) == 0 ? "http,https" : "https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    /* a PROPFIND sent to /.well-known/caldav goes on, with its body, to where that points */
    curl_easy_setopt(c, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "jackdaw");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    const char *file = getenv("SSL_CERT_FILE"), *dir = getenv("SSL_CERT_DIR");
    if (file && file[0]) curl_easy_setopt(c, CURLOPT_CAINFO, file);
    if (dir && dir[0]) curl_easy_setopt(c, CURLOPT_CAPATH, dir);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    } else if (strcmp(method, "GET") != 0 && strcmp(method, "DELETE") != 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, d);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, d);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(h);
    if (auth) {
        jd_wipe(auth, strlen(auth));
        free(auth);
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &d->status);
    char *eff = NULL;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    d->effective = jd_strdup(eff ? eff : url);
    if (r != CURLE_OK) {
        snprintf(d->error, sizeof d->error, "%s: %s", url, curl_easy_strerror(r));
        return JD_DAV_ERROR;
    }
    if (d->status == 401) {
        snprintf(d->error, sizeof d->error, "%s refused the sign-in (401)", url);
        return JD_DAV_AUTH;
    }
    if (d->status >= 400 || d->status < 200) {
        snprintf(d->error, sizeof d->error, "%s %s: HTTP %ld", method, url, d->status);
        return JD_DAV_ERROR;
    }
    return JD_DAV_OK;
}

/* a 207 Multi-Status body, parsed */
static int multistatus(jd_dav *d, const char *method, const char *url, const char *depth, const char *xml, node **out)
{
    *out = NULL;
    int r = request(d, method, url, depth, "application/xml; charset=utf-8", xml, NULL);
    if (r != JD_DAV_OK) return r;
    node *root = d->body.s ? xml_parse(d->body.s, d->body.len) : NULL;
    if (!root || strcmp(root->name, NS_DAV "multistatus") != 0) {
        node_free(root);
        snprintf(d->error, sizeof d->error, "%s %s: no WebDAV answer (HTTP %ld)", method, url, d->status);
        return JD_DAV_ERROR;
    }
    *out = root;
    return JD_DAV_OK;
}

/* ---- discovery ---- */

static const char *props_xml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
    "<d:current-user-principal/><c:calendar-home-set/><d:resourcetype/></d:prop></d:propfind>";

static const char *list_xml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:cs=\"http://calendarserver.org/ns/\""
    " xmlns:a=\"http://apple.com/ns/ical/\"><d:prop><d:resourcetype/><d:displayname/><a:calendar-color/><cs:getctag/>"
    "<d:sync-token/><c:supported-calendar-component-set/><d:current-user-privilege-set/></d:prop></d:propfind>";

/* the href inside a property (current-user-principal, calendar-home-set) of url's own response */
static char *prop_href(node *ms, const char *url, const char *prop)
{
    for (int i = 0; i < ms->nkids; i++) {
        node *resp = ms->kids[i];
        if (strcmp(resp->name, NS_DAV "response") != 0) continue;
        node *p = ok_props(resp);
        node *v = p ? child(p, prop) : NULL;
        node *href = v ? child(v, NS_DAV "href") : NULL;
        char *s = text_of(href);
        if (s) {
            char *abs = jd_dav_url(url, s);
            free(s);
            return abs;
        }
    }
    return NULL;
}

static int is_calendar(node *props)
{
    node *rt = props ? child(props, NS_DAV "resourcetype") : NULL;
    return rt && child(rt, NS_CALDAV "calendar");
}

static int writable(node *props)
{
    node *set = child(props, NS_DAV "current-user-privilege-set");
    if (!set) return 1;
    for (int i = 0; i < set->nkids; i++) {
        node *priv = child(set->kids[i], NS_DAV "privilege");
        if (!priv && strcmp(set->kids[i]->name, NS_DAV "privilege") == 0) priv = set->kids[i];
        if (priv && (child(priv, NS_DAV "all") || child(priv, NS_DAV "write") || child(priv, NS_DAV "write-content")))
            return 1;
    }
    return 0;
}

/* a calendar that names no components takes any (RFC 4791 5.2.3) */
static int holds_events(node *props)
{
    node *set = child(props, NS_CALDAV "supported-calendar-component-set");
    if (!set || !set->nkids) return 1;
    for (int i = 0; i < set->nkids; i++)
        if (set->kids[i]->name_attr && strcasecmp(set->kids[i]->name_attr, "VEVENT") == 0) return 1;
    return 0;
}

static void list_calendars(node *ms, const char *home, jd_dav_calendar **out, int *n)
{
    for (int i = 0; i < ms->nkids; i++) {
        node *resp = ms->kids[i];
        if (strcmp(resp->name, NS_DAV "response") != 0) continue;
        node *props = ok_props(resp);
        if (!is_calendar(props) || !holds_events(props)) continue;
        char *href = text_of(child(resp, NS_DAV "href"));
        if (!href) continue;
        char *color = text_of(child(props, NS_APPLE "calendar-color"));
        /* "#rrggbbaa" from Apple and Nextcloud: the alpha goes */
        if (color && strlen(color) == 9 && color[0] == '#') color[7] = 0;
        *out = jd_realloc(*out, sizeof **out * (size_t)(*n + 1));
        (*out)[(*n)++] = (jd_dav_calendar){jd_dav_url(home, href), text_of(child(props, NS_DAV "displayname")), color,
                                           text_of(child(props, NS_CS "getctag")), text_of(child(props, NS_DAV "sync-token")),
                                           !writable(props)};
        free(href);
    }
}

static int list_home(jd_dav *d, const char *home, jd_dav_calendar **out, int *n)
{
    node *ms = NULL;
    int r = multistatus(d, "PROPFIND", home, "1", list_xml, &ms);
    if (r != JD_DAV_OK) return r;
    list_calendars(ms, d->effective, out, n);
    node_free(ms);
    return JD_DAV_OK;
}

static char *origin_of(const char *url)
{
    const char *s = strstr(url, "://");
    const char *p = s ? strchr(s + 3, '/') : NULL;
    return p ? jd_strndup(url, (size_t)(p - url)) : jd_strdup(url);
}

int jd_dav_discover(jd_dav *d, const char *url, jd_dav_calendar **out, int *n)
{
    *out = NULL, *n = 0;
    char *start = strstr(url, "://") ? jd_strdup(url) : jd_printf("https://%s", url);
    char *origin = origin_of(start);
    char *well_known = jd_printf("%s/.well-known/caldav", origin);
    int bare = strcmp(jd_dav_path(start), "/") == 0;
    node *ms = NULL;
    int r = multistatus(d, "PROPFIND", bare ? well_known : start, "0", props_xml, &ms);
    /* a URL that is not DAV itself (a web page of the server): its well-known one */
    if (r == JD_DAV_ERROR && !bare) r = multistatus(d, "PROPFIND", well_known, "0", props_xml, &ms);
    if (r != JD_DAV_OK) goto done;
    char *here = jd_strdup(d->effective);
    char *home = prop_href(ms, here, NS_CALDAV "calendar-home-set");
    char *principal = home ? NULL : prop_href(ms, here, NS_DAV "current-user-principal");
    int itself = 0;
    for (int i = 0; i < ms->nkids && !itself; i++)
        if (strcmp(ms->kids[i]->name, NS_DAV "response") == 0) itself = is_calendar(ok_props(ms->kids[i]));
    node_free(ms);
    ms = NULL;
    if (!home && principal) {
        r = multistatus(d, "PROPFIND", principal, "0", props_xml, &ms);
        if (r == JD_DAV_OK) home = prop_href(ms, d->effective, NS_CALDAV "calendar-home-set");
        node_free(ms);
        ms = NULL;
    }
    if (home) {
        r = list_home(d, home, out, n);
    } else if (itself) {
        /* a calendar's own URL: it alone (its parent may be the home, but need not be readable) */
        r = list_home(d, here, out, n);
        for (int i = *n - 1; i >= 0; i--)
            if (!same_resource((*out)[i].href, here)) {
                jd_dav_calendar *c = &(*out)[i];
                free(c->href), free(c->name), free(c->color), free(c->ctag), free(c->sync_token);
                memmove(c, c + 1, sizeof *c * (size_t)(*n - i - 1));
                (*n)--;
            }
    } else if (r == JD_DAV_OK) {
        snprintf(d->error, sizeof d->error, "%s names no calendar home (RFC 4791 6.2.1)", here);
        r = JD_DAV_ERROR;
    }
    free(here), free(home), free(principal);
done:
    free(start), free(origin), free(well_known);
    return r;
}

void jd_dav_calendars_free(jd_dav_calendar *c, int n)
{
    for (int i = 0; i < n; i++) free(c[i].href), free(c[i].name), free(c[i].color), free(c[i].ctag), free(c[i].sync_token);
    free(c);
}

/* ---- events ---- */

void jd_dav_items_free(jd_dav_item *it, int n)
{
    for (int i = 0; i < n; i++) free(it[i].href), free(it[i].etag), free(it[i].data);
    free(it);
}

/* the members of a multistatus, the collection itself left out */
static void items(node *ms, const char *cal, jd_dav_item **out, int *n)
{
    for (int i = 0; i < ms->nkids; i++) {
        node *resp = ms->kids[i];
        if (strcmp(resp->name, NS_DAV "response") != 0) continue;
        char *href = text_of(child(resp, NS_DAV "href"));
        if (!href) continue;
        char *abs = jd_dav_url(cal, href);
        int self = same_resource(abs, cal);
        free(abs);
        if (self) {
            free(href);
            continue;
        }
        long code = status_code(child(resp, NS_DAV "status"));
        node *props = ok_props(resp);
        jd_dav_item it = {.href = jd_strdup(jd_dav_path(href)), .gone = code == 404};
        free(href);
        if (props) {
            it.etag = text_of(child(props, NS_DAV "getetag"));
            it.data = text_of(child(props, NS_CALDAV "calendar-data"));
        } else if (!it.gone) {
            free(it.href);
            continue;
        }
        *out = jd_realloc(*out, sizeof **out * (size_t)(*n + 1));
        (*out)[(*n)++] = it;
    }
}

/* RFC 6578 3.6: a truncated answer says 507 for the collection itself; the rest comes for the token it carries */
static int truncated(node *ms, const char *cal)
{
    for (int i = 0; i < ms->nkids; i++) {
        node *resp = ms->kids[i];
        if (strcmp(resp->name, NS_DAV "response") != 0) continue;
        char *href = text_of(child(resp, NS_DAV "href"));
        char *abs = href ? jd_dav_url(cal, href) : NULL;
        int self = abs && same_resource(abs, cal);
        free(href), free(abs);
        if (self && status_code(child(resp, NS_DAV "status")) == 507) return 1;
    }
    return 0;
}

int jd_dav_sync(jd_dav *d, const char *cal, const char *token, jd_dav_item **out, int *n, char **next)
{
    *out = NULL, *n = 0, *next = NULL;
    char *at = jd_strdup(token);
    /* a server answers in pages (Google: 250 events each): the pages are asked for until one is not cut short */
    for (int page = 0; page < 1000; page++) {
        jd_buf x = {0};
        jd_buf_adds(&x, "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>");
        if (at) xml_escape(&x, at);
        jd_buf_adds(&x, "</d:sync-token><d:sync-level>1</d:sync-level><d:prop><d:getetag/></d:prop></d:sync-collection>");
        node *ms = NULL;
        int r = multistatus(d, "REPORT", cal, "1", x.s, &ms);
        jd_buf_free(&x);
        if (r != JD_DAV_OK) {
            free(at);
            jd_dav_items_free(*out, *n);
            *out = NULL, *n = 0;
            free(*next);
            *next = NULL;
            return r == JD_DAV_ERROR && d->status >= 400 && d->status != 404 ? JD_DAV_RESYNC : r;
        }
        items(ms, cal, out, n);
        free(*next);
        *next = text_of(child(ms, NS_DAV "sync-token"));
        int more = truncated(ms, cal);
        node_free(ms);
        if (!more || !*next || (at && strcmp(*next, at) == 0)) break;
        free(at);
        at = jd_strdup(*next);
    }
    free(at);
    return JD_DAV_OK;
}

int jd_dav_etags(jd_dav *d, const char *cal, jd_dav_item **out, int *n)
{
    *out = NULL, *n = 0;
    static const char *q = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                           "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                           "<d:prop><d:getetag/></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
                           "<c:comp-filter name=\"VEVENT\"/></c:comp-filter></c:filter></c:calendar-query>";
    node *ms = NULL;
    int r = multistatus(d, "REPORT", cal, "1", q, &ms);
    if (r != JD_DAV_OK) return r;
    items(ms, cal, out, n);
    node_free(ms);
    return JD_DAV_OK;
}

int jd_dav_multiget(jd_dav *d, const char *cal, char **hrefs, int nhrefs, jd_dav_item **out, int *n)
{
    *out = NULL, *n = 0;
    for (int at = 0; at < nhrefs; at += MULTIGET_BATCH) {
        jd_buf x = {0};
        jd_buf_adds(&x, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                        "<c:calendar-multiget xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                        "<d:prop><d:getetag/><c:calendar-data/></d:prop>");
        for (int i = at; i < nhrefs && i < at + MULTIGET_BATCH; i++) {
            jd_buf_adds(&x, "<d:href>");
            xml_escape(&x, hrefs[i]);
            jd_buf_adds(&x, "</d:href>");
        }
        jd_buf_adds(&x, "</c:calendar-multiget>");
        node *ms = NULL;
        int r = multistatus(d, "REPORT", cal, "1", x.s, &ms);
        jd_buf_free(&x);
        if (r != JD_DAV_OK) return r;
        items(ms, cal, out, n);
        node_free(ms);
    }
    return JD_DAV_OK;
}

int jd_dav_state(jd_dav *d, const char *cal, char **ctag, char **token)
{
    *ctag = *token = NULL;
    static const char *q = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                           "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop>"
                           "<cs:getctag/><d:sync-token/></d:prop></d:propfind>";
    node *ms = NULL;
    int r = multistatus(d, "PROPFIND", cal, "0", q, &ms);
    if (r != JD_DAV_OK) return r;
    node *resp = child(ms, NS_DAV "response");
    node *props = resp ? ok_props(resp) : NULL;
    *ctag = text_of(child(props, NS_CS "getctag"));
    *token = text_of(child(props, NS_DAV "sync-token"));
    node_free(ms);
    return JD_DAV_OK;
}

int jd_dav_put(jd_dav *d, const char *cal, const char *href, const char *ics, const char *etag, char **new_etag)
{
    *new_etag = NULL;
    char *url = jd_dav_url(cal, href);
    char cond[600];
    if (etag) snprintf(cond, sizeof cond, "If-Match: %s", etag);
    else snprintf(cond, sizeof cond, "If-None-Match: *");
    const char *headers[] = {cond, NULL};
    int r = request(d, "PUT", url, NULL, "text/calendar; charset=utf-8", ics, headers);
    free(url);
    if (r == JD_DAV_ERROR && d->status == 412) {
        snprintf(d->error, sizeof d->error, "The event changed on the server meanwhile.");
        return JD_DAV_CONFLICT;
    }
    if (r == JD_DAV_OK && d->etag && strncmp(d->etag, "W/", 2) != 0) *new_etag = jd_strdup(d->etag);
    return r;
}

int jd_dav_options(jd_dav *d, const char *url)
{
    return request(d, "OPTIONS", url, NULL, NULL, NULL, NULL);
}

int jd_dav_delete(jd_dav *d, const char *cal, const char *href, const char *etag)
{
    char *url = jd_dav_url(cal, href);
    char cond[600];
    snprintf(cond, sizeof cond, "If-Match: %s", etag ? etag : "");
    const char *headers[] = {etag ? cond : NULL, NULL};
    int r = request(d, "DELETE", url, NULL, NULL, NULL, headers);
    free(url);
    if (r == JD_DAV_ERROR && (d->status == 404 || d->status == 410)) return JD_DAV_OK;
    if (r == JD_DAV_ERROR && d->status == 412) {
        snprintf(d->error, sizeof d->error, "The event changed on the server meanwhile.");
        return JD_DAV_CONFLICT;
    }
    return r;
}
