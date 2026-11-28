#pragma once

typedef struct jd_toml jd_toml;
typedef enum { JD_SEC_TLS, JD_SEC_STARTTLS } jd_security;

/* the colour roles of the desktop theme's colors.toml, as [theme] in settings.toml names them (ui/theme.c) */
#define JD_THEME_ROLES 13
extern const char *const jd_theme_roles[JD_THEME_ROLES];

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

/* a calendar account, [[calendar]] in settings.toml */
typedef struct {
    int provider;      /* provider = "caldav" (1) | "google" (2) */
    char *url;         /* url: where CalDAV discovery starts (a server, principal or calendar); unused for Google */
    char *username;    /* username: the CalDAV login, or the Google address */
    int check_minutes; /* check_minutes, default 15 */
} jd_cal_account;

/* what the account's calendars are filed under in the store and its keystore entry: "google:<address>" or
   "caldav:<username>@<url>"; malloc'd */
char *jd_cal_account_id(const jd_cal_account *a);

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
    int undo_seconds;    /* [compose] undo_seconds: a sent message waits this long for Undo; 0 to 10, 5 by default */
    int mono_plain;      /* [fonts] mono_plain: plain-text mail in the monospace font */
    int sidebar_width, list_width, list_height; /* [view], pixels; 0 = default */
    int light;           /* the theme in use: scheme, with "system" resolved (apply_prefs) */
    int scheme;          /* [theme] scheme = "dark" (0) | "light" (1) | "system" (2); older files: [view] theme */
    /* [theme] overrides of the desktop's colors.toml roles (jd_theme_roles), "#rrggbb"; NULL keeps */
    char *theme_roles[JD_THEME_ROLES];
    int scale;           /* [view] scale, percent of the normal size, 80-160 */
    int compact;         /* [view] density = "comfortable" | "compact" */
    int snippets;        /* [view] snippets, default true */
    int effects;         /* [view] effects: the paper plane, the burn, frosted and rising dialogs; default true */
    int prefer_plain;    /* [reading] format = "html" | "plain" */
    int remote_images;   /* [reading] remote_images = "ask" | "always" */
    char *image_senders; /* [reading] images_from: addresses whose remote images load, comma-separated */
    int mark_read;       /* [reading] mark_read = true: opening a message marks it read */
    int notify;          /* [notifications] enabled, default true */
    int notify_focused;  /* [notifications] when_focused: also while jackdaw is in front, default true */
    int push;            /* [notifications] push: Android keeps IMAP IDLE open while closed, default true */
    int update_check;    /* [updates] check: look for a new release once a day, default true */
    int prefetch_days;   /* [sync] prefetch_days, default 30; 0 downloads bodies only when opened, -1 every one */
    int prefetch_all;    /* [sync] prefetch_folders = "all": every folder kept offline, else "inbox" */
    int check_minutes;   /* [sync] check_minutes, default 5: all folders; the Inbox waits in IDLE */
    int share;           /* [sync] settings, default true: the JD_SHARE_* groups shared through the mail account */
    char **new_exclude;  /* [home] exclude ([new] before): full folder names kept out of Home and its notifications */
    int nnew_exclude;
    char *signature_new, *signature_reply;      /* [signature] new, reply */
    jd_cal_account *cals; /* [[calendar]], one per calendar account; an older file's single [calendar] is the first */
    int ncals;
    int cal_reminders;   /* reminders (written in every [[calendar]]): notifications for events' alarms, default true */
    int cal_view;        /* [view] calendar = "agenda" (0) | "day" (1) | "week" (2) | "month" (3); absent -1: the
                            device's (week on a desktop, agenda on a phone) */
    jd_rule *rules;      /* [[rule]] */
    int nrules;
} jd_prefs;

/* [account] in settings.toml; 0 when every field is present and valid */
int jd_account_load(jd_account *a, char *err, int errlen);
void jd_account_free(jd_account *a);
void jd_prefs_load(jd_prefs *p);
void jd_prefs_free(jd_prefs *p);
void jd_prefs_add_rule(jd_prefs *p, const char *match, const char *action);
/* the account with the same id is replaced; returns its index */
int jd_prefs_add_calendar(jd_prefs *p, int provider, const char *url, const char *username, int check_minutes);
void jd_prefs_remove_calendar(jd_prefs *p, int i);
int jd_prefs_trusts_images(const jd_prefs *p, const char *address);
void jd_prefs_trust_images(jd_prefs *p, const char *address, int trust);
/* writes the whole file; an account without an address leaves [account] out */
/* [[account]], one per mail account, the first first; an older file's single [account] is read as the only one */
int jd_settings_save(const jd_account *const *accounts, int naccounts, const jd_prefs *p);
/* every complete account, 0 when there is at least one */
int jd_accounts_load(jd_account **out, int *n, char *err, int errlen);
void jd_accounts_free(jd_account *a, int n);

/* The settings an account's jackdaws share ([sync] settings): groups compared and merged each on its own, the
   newer change winning. Layout, sizes, fonts, push and the sync timers stay per device. */
enum {
    JD_SHARE_THEME, JD_SHARE_READING, JD_SHARE_NOTIFY, JD_SHARE_COMPOSE, JD_SHARE_SIGNATURE, JD_SHARE_RULES,
    JD_SHARE_CALVIEW, JD_SHARE_CALENDARS, JD_SHARE_CHOICES, JD_SHARE_COUNT
};
/* the group's key in the copy's [stamps] */
extern const char *const jd_share_names[JD_SHARE_COUNT];
/* group g of p as TOML, the same text for the same settings; NULL for JD_SHARE_CHOICES, which the store holds */
char *jd_share_text(const jd_prefs *p, int g);
/* group g of a shared copy into p */
void jd_share_apply(jd_prefs *p, int g, const jd_toml *t);
const char *jd_settings_path(void);

/* $XDG_DATA_HOME/jackdaw/<address>, created on demand */
char *jd_account_dir(const jd_account *a);

int jd_default_port(int imap, jd_security security);
const char *jd_security_name(jd_security s);
