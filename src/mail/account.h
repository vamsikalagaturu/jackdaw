#pragma once

typedef enum { JD_SEC_TLS, JD_SEC_STARTTLS } jd_security;

typedef struct {
    char *host;
    int port;
    jd_security security;
} jd_server;

typedef struct {
    char *name, *address, *username;
    jd_server imap, smtp;
    int sieve_port; /* ManageSieve on the IMAP host; [account] sieve_port, default 4190 */
} jd_account;

/* A filter run on new Inbox mail: match is search syntax (see mail/query.h); action is "read", "flag",
   "delete", "move:<folder>" or "copy:<folder>". */
typedef struct {
    char *match, *action;
} jd_rule;

/* everything in settings.toml besides [account] */
typedef struct {
    int conversations;   /* [view] conversations, default true */
    int reader_pos;      /* [view] reader = "right" (0) | "bottom" (1) | "off" (2: messages open in a tab) */
    int sidebar_rail;    /* [view] sidebar = "full" | "rail" (icons only) */
    int tabs_side;       /* [view] tabs = "top" | "sidebar" */
    int list_text, reader_text; /* [view] list_text, reader_text: text size in pixels, default 15 */
    char *font_ui, *font_list, *font_reader, *font_mono; /* [fonts] ui, list, reader, mono: family names, "" = Inter */
    int attachment_list; /* [compose] attachment_list: a numbered list of the attached files in the body */
    int markdown;        /* [compose] markdown: the body is also sent as HTML, rendered from Markdown */
    int mono_plain;      /* [fonts] mono_plain: plain-text mail in the monospace font */
    int sidebar_width, list_width, list_height; /* [view], pixels; 0 = default */
    int light;           /* [view] theme = "dark" | "light" */
    int scale;           /* [view] scale, percent of the normal size, 80-160 */
    int compact;         /* [view] density = "comfortable" | "compact" */
    int snippets;        /* [view] snippets, default true */
    int prefer_plain;    /* [reading] format = "html" | "plain" */
    int remote_images;   /* [reading] remote_images = "ask" | "always" */
    char *image_senders; /* [reading] images_from: addresses whose remote images load, comma-separated */
    int mark_read;       /* [reading] mark_read = true: opening a message marks it read */
    int notify;          /* [notifications] enabled, default true */
    int notify_focused;  /* [notifications] when_focused: also while jackdaw is in front, default true */
    int push;            /* [notifications] push: Android keeps IMAP IDLE open while closed, default true */
    int prefetch_days;   /* [sync] prefetch_days, default 30; 0 downloads bodies only when opened */
    int check_minutes;   /* [sync] check_minutes, default 5: all folders; the Inbox waits in IDLE */
    char *signature_new, *signature_reply;      /* [signature] new, reply */
    jd_rule *rules;      /* [[rule]] */
    int nrules;
} jd_prefs;

/* [account] in settings.toml; 0 when every field is present and valid */
int jd_account_load(jd_account *a, char *err, int errlen);
void jd_account_free(jd_account *a);
void jd_prefs_load(jd_prefs *p);
void jd_prefs_free(jd_prefs *p);
void jd_prefs_add_rule(jd_prefs *p, const char *match, const char *action);
int jd_prefs_trusts_images(const jd_prefs *p, const char *address);
void jd_prefs_trust_images(jd_prefs *p, const char *address, int trust);
/* writes the whole file; an account without an address leaves [account] out */
int jd_settings_save(const jd_account *a, const jd_prefs *p);
const char *jd_settings_path(void);

/* $XDG_DATA_HOME/jackdaw/<address>, created on demand */
char *jd_account_dir(const jd_account *a);

int jd_default_port(int imap, jd_security security);
const char *jd_security_name(jd_security s);
