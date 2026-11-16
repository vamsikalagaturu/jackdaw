#define _GNU_SOURCE
#include "account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/toml.h"

const char *const jd_theme_roles[JD_THEME_ROLES] = {"accent", "background", "dark_background", "lighter_background",
                                                    "foreground", "bright_foreground", "red", "yellow", "green",
                                                    "blue", "magenta", "cyan", "orange"};

const char *jd_settings_path(void)
{
    static char *path;
    if (!path) path = jd_printf("%s/settings.toml", jd_config_dir());
    return path;
}

const char *jd_security_name(jd_security s)
{
    return s == JD_SEC_STARTTLS ? "starttls" : "tls";
}

/* RFC 8314 section 7 (implicit TLS) and RFC 6409 / RFC 3501 (STARTTLS) */
int jd_default_port(int imap, jd_security security)
{
    if (imap) return security == JD_SEC_TLS ? 993 : 143;
    return security == JD_SEC_TLS ? 465 : 587;
}

static int load_server(const jd_toml *t, const char *prefix, jd_server *s, int imap)
{
    char key[32];
    snprintf(key, sizeof key, "%s_host", prefix);
    s->host = jd_strdup(jd_toml_str(t, "account", key));
    snprintf(key, sizeof key, "%s_security", prefix);
    const char *sec = jd_toml_str(t, "account", key);
    if (!sec || jd_streq(sec, "tls")) s->security = JD_SEC_TLS;
    else if (jd_streq(sec, "starttls")) s->security = JD_SEC_STARTTLS;
    else return -1;
    snprintf(key, sizeof key, "%s_port", prefix);
    long long port;
    s->port = jd_toml_int(t, "account", key, &port) == 0 ? (int)port : jd_default_port(imap, s->security);
    return s->host && s->host[0] && s->port > 0 && s->port < 65536 ? 0 : -1;
}

int jd_account_load(jd_account *a, char *err, int errlen)
{
    *a = (jd_account){0};
    jd_toml *t = jd_toml_load(jd_settings_path(), err, (size_t)errlen);
    if (!t) return -1;
    a->name = jd_strdup(jd_toml_str(t, "account", "name"));
    a->address = jd_strdup(jd_toml_str(t, "account", "address"));
    a->username = jd_strdup(jd_toml_str(t, "account", "username"));
    if (!a->username) a->username = jd_strdup(a->address);
    int bad = load_server(t, "imap", &a->imap, 1) | load_server(t, "smtp", &a->smtp, 0);
    long long sieve;
    a->sieve_port = jd_toml_int(t, "account", "sieve_port", &sieve) == 0 && sieve > 0 && sieve < 65536 ? (int)sieve : 4190;
    jd_toml_free(t);
    if (bad || !a->address || !strchr(a->address, '@')) {
        if (err) snprintf(err, (size_t)errlen, "incomplete [account] in %s", jd_settings_path());
        return -1;
    }
    if (!a->name) a->name = jd_strdup(a->address);
    return 0;
}

static void put(jd_buf *b, const char *key, const char *value)
{
    jd_buf_addf(b, "%s = ", key);
    jd_toml_quote(b, value);
    jd_buf_adds(b, "\n");
}

void jd_prefs_load(jd_prefs *p)
{
    *p = (jd_prefs){.conversations = 1, .scale = 100, .snippets = 1, .mark_read = 1, .notify = 1, .notify_focused = 1, .push = 1, .update_check = 1,
                    .prefetch_days = 30, .check_minutes = 5, .list_text = 15, .reader_text = 15,
                    .attachment_list = 1, .markdown = 1};
    jd_toml *t = jd_toml_load(jd_settings_path(), NULL, 0);
    if (!t) return;
    p->attachment_list = jd_toml_bool(t, "compose", "attachment_list", 1);
    p->markdown = jd_toml_bool(t, "compose", "markdown", 1);
    p->conversations = jd_toml_bool(t, "view", "conversations", 1);
    const char *reader = jd_toml_str(t, "view", "reader");
    p->reader_pos = jd_streq(reader, "bottom") ? 1 : jd_streq(reader, "off") ? 2 : 0;
    p->sidebar_rail = jd_streq(jd_toml_str(t, "view", "sidebar"), "rail");
    p->tabs_side = jd_streq(jd_toml_str(t, "view", "tabs"), "sidebar");
    p->font_ui = jd_strdup(jd_toml_str(t, "fonts", "ui"));
    p->font_list = jd_strdup(jd_toml_str(t, "fonts", "list"));
    p->font_reader = jd_strdup(jd_toml_str(t, "fonts", "reader"));
    p->font_mono = jd_strdup(jd_toml_str(t, "fonts", "mono"));
    p->mono_plain = jd_toml_bool(t, "fonts", "mono_plain", 0);
    const char *scheme = jd_toml_str(t, "theme", "scheme");
    if (!scheme) scheme = jd_toml_str(t, "view", "theme");
    p->scheme = jd_streq(scheme, "light") ? 1 : jd_streq(scheme, "system") ? 2 : 0;
    for (int i = 0; i < 13; i++) p->theme_roles[i] = jd_strdup(jd_toml_str(t, "theme", jd_theme_roles[i]));
    p->compact = jd_streq(jd_toml_str(t, "view", "density"), "compact");
    p->snippets = jd_toml_bool(t, "view", "snippets", 1);
    p->prefer_plain = jd_streq(jd_toml_str(t, "reading", "format"), "plain");
    p->mark_read = jd_toml_bool(t, "reading", "mark_read", 1);
    p->notify = jd_toml_bool(t, "notifications", "enabled", 1);
    p->notify_focused = jd_toml_bool(t, "notifications", "when_focused", 1);
    p->push = jd_toml_bool(t, "notifications", "push", 1);
    p->update_check = jd_toml_bool(t, "updates", "check", 1);
    p->remote_images = jd_streq(jd_toml_str(t, "reading", "remote_images"), "always");
    p->image_senders = jd_strdup(jd_toml_str(t, "reading", "images_from"));
    long long v;
    if (jd_toml_int(t, "view", "sidebar_width", &v) == 0) p->sidebar_width = (int)v;
    if (jd_toml_int(t, "view", "list_width", &v) == 0) p->list_width = (int)v;
    if (jd_toml_int(t, "view", "list_height", &v) == 0) p->list_height = (int)v;
    if (jd_toml_int(t, "view", "scale", &v) == 0 && v >= 50 && v <= 200) p->scale = (int)v;
    if (jd_toml_int(t, "view", "list_text", &v) == 0 && v >= 8 && v <= 40) p->list_text = (int)v;
    if (jd_toml_int(t, "view", "reader_text", &v) == 0 && v >= 8 && v <= 40) p->reader_text = (int)v;
    if (jd_toml_int(t, "sync", "prefetch_days", &v) == 0 && v >= 0 && v <= 3650) p->prefetch_days = (int)v;
    if (jd_toml_int(t, "sync", "check_minutes", &v) == 0 && v >= 1 && v <= 1440) p->check_minutes = (int)v;
    p->signature_new = jd_strdup(jd_toml_str(t, "signature", "new"));
    p->signature_reply = jd_strdup(jd_toml_str(t, "signature", "reply"));
    int n = jd_toml_count(t, "rule");
    for (int i = 0; i < n; i++) {
        char table[32];
        snprintf(table, sizeof table, "rule.%d", i);
        const char *match = jd_toml_str(t, table, "match"), *action = jd_toml_str(t, table, "action");
        if (match && action) jd_prefs_add_rule(p, match, action);
    }
    jd_toml_free(t);
}

void jd_prefs_add_rule(jd_prefs *p, const char *match, const char *action)
{
    p->rules = jd_realloc(p->rules, sizeof *p->rules * (size_t)(p->nrules + 1));
    p->rules[p->nrules++] = (jd_rule){jd_strdup(match), jd_strdup(action)};
}

int jd_prefs_trusts_images(const jd_prefs *p, const char *address)
{
    if (!address || !address[0] || !p->image_senders) return 0;
    size_t n = strlen(address);
    for (const char *s = p->image_senders; (s = strcasestr(s, address)); s += n) {
        int start = s == p->image_senders || s[-1] == ',' || s[-1] == ' ';
        int end = !s[n] || s[n] == ',' || s[n] == ' ';
        if (start && end) return 1;
    }
    return 0;
}

void jd_prefs_trust_images(jd_prefs *p, const char *address, int trust)
{
    if (!address || !address[0] || jd_prefs_trusts_images(p, address) == trust) return;
    jd_buf b = {0};
    char *copy = jd_strdup(p->image_senders ? p->image_senders : ""), *save = NULL;
    for (char *a = strtok_r(copy, ", ", &save); a; a = strtok_r(NULL, ", ", &save))
        if (trust || strcasecmp(a, address) != 0) jd_buf_addf(&b, "%s%s", b.len ? ", " : "", a);
    if (trust) jd_buf_addf(&b, "%s%s", b.len ? ", " : "", address);
    free(copy);
    free(p->image_senders);
    p->image_senders = b.s ? b.s : jd_strdup("");
}

void jd_prefs_free(jd_prefs *p)
{
    for (int i = 0; i < p->nrules; i++) free(p->rules[i].match), free(p->rules[i].action);
    free(p->rules), free(p->signature_new), free(p->signature_reply), free(p->image_senders);
    free(p->font_ui), free(p->font_list), free(p->font_reader), free(p->font_mono);
    for (int i = 0; i < 13; i++) free(p->theme_roles[i]);
    *p = (jd_prefs){0};
}

int jd_settings_save(const jd_account *a, const jd_prefs *p)
{
    jd_buf b = {0};
    jd_buf_adds(&b, "# jackdaw settings. The password is kept in the system keystore, not here.\n");
    if (a && a->address) {
        jd_buf_adds(&b, "\n[account]\n");
        put(&b, "name", a->name);
        put(&b, "address", a->address);
        put(&b, "username", a->username);
        put(&b, "imap_host", a->imap.host);
        jd_buf_addf(&b, "imap_port = %d\n", a->imap.port);
        put(&b, "imap_security", jd_security_name(a->imap.security));
        put(&b, "smtp_host", a->smtp.host);
        jd_buf_addf(&b, "smtp_port = %d\n", a->smtp.port);
        put(&b, "smtp_security", jd_security_name(a->smtp.security));
        jd_buf_addf(&b, "sieve_port = %d\n", a->sieve_port ? a->sieve_port : 4190);
    }
    static const char *readers[] = {"right", "bottom", "off"};
    static const char *schemes[] = {"dark", "light", "system"};
    jd_buf_addf(&b, "\n[theme]\nscheme = \"%s\"\n", schemes[p->scheme % 3]);
    for (int i = 0; i < 13; i++)
        if (p->theme_roles[i] && p->theme_roles[i][0]) put(&b, jd_theme_roles[i], p->theme_roles[i]);
    jd_buf_addf(&b, "\n[view]\nscale = %d\ndensity = \"%s\"\nsnippets = %s\nconversations = %s\n"
                    "reader = \"%s\"\nsidebar = \"%s\"\ntabs = \"%s\"\nlist_text = %d\nreader_text = %d\n",
                p->scale ? p->scale : 100, p->compact ? "compact" : "comfortable",
                p->snippets ? "true" : "false", p->conversations ? "true" : "false", readers[p->reader_pos % 3],
                p->sidebar_rail ? "rail" : "full", p->tabs_side ? "sidebar" : "top", p->list_text ? p->list_text : 15,
                p->reader_text ? p->reader_text : 15);
    if (p->sidebar_width) jd_buf_addf(&b, "sidebar_width = %d\n", p->sidebar_width);
    if (p->list_width) jd_buf_addf(&b, "list_width = %d\n", p->list_width);
    if (p->list_height) jd_buf_addf(&b, "list_height = %d\n", p->list_height);
    jd_buf_addf(&b, "\n[reading]\nformat = \"%s\"\nmark_read = %s\nremote_images = \"%s\"\n", p->prefer_plain ? "plain" : "html",
                p->mark_read ? "true" : "false", p->remote_images ? "always" : "ask");
    put(&b, "images_from", p->image_senders ? p->image_senders : "");
    jd_buf_addf(&b, "\n[notifications]\nenabled = %s\nwhen_focused = %s\npush = %s\n", p->notify ? "true" : "false",
                p->notify_focused ? "true" : "false", p->push ? "true" : "false");
    jd_buf_addf(&b, "\n[updates]\ncheck = %s\n", p->update_check ? "true" : "false");
    jd_buf_adds(&b, "\n# Font families as fontconfig knows them (fc-list : family); empty is the built-in Inter.\n[fonts]\n");
    put(&b, "ui", p->font_ui ? p->font_ui : "");
    put(&b, "list", p->font_list ? p->font_list : "");
    put(&b, "reader", p->font_reader ? p->font_reader : "");
    put(&b, "mono", p->font_mono ? p->font_mono : "");
    jd_buf_addf(&b, "mono_plain = %s\n", p->mono_plain ? "true" : "false");
    jd_buf_addf(&b, "\n[compose]\nattachment_list = %s\nmarkdown = %s\n", p->attachment_list ? "true" : "false",
                p->markdown ? "true" : "false");
    jd_buf_addf(&b, "\n[sync]\nprefetch_days = %d\ncheck_minutes = %d\n", p->prefetch_days,
                p->check_minutes ? p->check_minutes : 5);
    jd_buf_adds(&b, "\n# Added below what you write: new to new messages, reply to replies and forwards.\n[signature]\n");
    put(&b, "new", p->signature_new ? p->signature_new : "");
    put(&b, "reply", p->signature_reply ? p->signature_reply : "");
    if (p->nrules)
        jd_buf_adds(&b, "\n# Rules run on new Inbox mail, top to bottom. match uses the search syntax;\n"
                        "# action is read, flag, delete, move:<folder> or copy:<folder>.\n");
    for (int i = 0; i < p->nrules; i++) {
        jd_buf_adds(&b, "\n[[rule]]\n");
        put(&b, "match", p->rules[i].match);
        put(&b, "action", p->rules[i].action);
    }
    int r = jd_mkdirs(jd_config_dir()) == 0 ? jd_write_file(jd_settings_path(), b.s, b.len, 0600) : -1;
    jd_buf_free(&b);
    return r;
}

void jd_account_free(jd_account *a)
{
    free(a->name), free(a->address), free(a->username), free(a->imap.host), free(a->smtp.host);
    *a = (jd_account){0};
}

char *jd_account_dir(const jd_account *a)
{
    char *safe = jd_strdup(a->address);
    for (char *c = safe; *c; c++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-+", *c)) *c = '_';
    char *dir = jd_printf("%s/%s", jd_data_dir(), safe);
    free(safe);
    jd_mkdirs(dir);
    return dir;
}
