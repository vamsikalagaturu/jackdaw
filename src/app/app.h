#pragma once
#include "mail/account.h"
#include "mail/mime.h"
#include "mail/query.h"
#include "mail/sync.h"
#include "store/store.h"
#include "ui/widgets.h"

enum {
    F_NAME, F_ADDRESS, F_USERNAME, F_IMAP_HOST, F_IMAP_PORT, F_SMTP_HOST, F_SMTP_PORT, F_PASSWORD, F_COUNT
};

enum { PANE_FOLDERS, PANE_LIST, PANE_READER };

typedef enum { DLG_NONE, DLG_PROMPT, DLG_CONFIRM, DLG_PICK_FOLDER, DLG_RULES } dialog_kind;
typedef enum {
    ACT_NONE, ACT_CREATE_FOLDER, ACT_RENAME_FOLDER, ACT_DELETE_FOLDER, ACT_MOVE, ACT_COPY, ACT_DESTROY, ACT_ADD_RULE, ACT_LOGOUT,
    ACT_DISCARD, ACT_SEND_LATER, ACT_DELETE_EVENT, ACT_USE_SHARED
} action;

typedef struct {
    dialog_kind kind;
    action act;
    char title[256];
    jd_field field;
    int selected;
    int64_t *ids; /* the messages a move/copy/destroy is for */
    int nids;
} jd_dialog;

/* a message in an open conversation, with its text wrapped for the view width */
typedef struct {
    jd_message_row row;
    char *text;
    int *line_starts; /* byte offsets into text */
    int nlines;
    float wrapped_for;
    int expanded;
    jd_attachment *files;
    int nfiles;
    char *invite;      /* the iCalendar object of an invitation or an answer to one (RFC 6047), NULL for other mail */
    Texture2D *thumbs; /* per file: a preview for images, id 0 for the rest */
    int quote_at;      /* where the trailing quoted history starts in text, -1 when there is none */
    int show_quote;
    int load_images;   /* remote images allowed for this message; asked for once */
    int images_asked;
    struct jd_html *html; /* the text/html part, when there is one */
    float html_w, html_h;
    float wrapped_size;   /* the text size line_starts were wrapped for */
    int sel_from, sel_to; /* selected plain text (byte offsets into text), none while equal; HTML keeps its own */
    int caret, anchor;    /* visual mode in plain text: the caret and where the selection started */
    float body_y;         /* where its text started on screen last frame, -1 when it was not drawn */
    int body_lines;       /* plain text: the wrapped lines shown (folded quotes left out) */
    float card_h;         /* the height of its card, as drawn last frame */
    float open;           /* 0 collapsed to 1 expanded, eased toward expanded: the card opens and closes smoothly */
    float full_h;         /* the open card's whole height, as last measured */
    int addresses;        /* From, To, Cc and In shown in full; else one line naming them */
} jd_reading;

/* A conversation (or one message when message != 0) as the preview pane or a tab shows it */
typedef struct {
    int64_t thread, message;
    jd_reading *reading;
    int n;
    float scroll, height;
    float content; /* the height drawn last frame, 0 before the first */
    float top;     /* the screen y of the view's top, last frame */
    int plain; /* show the plain text even where there is HTML */
} jd_conv;

typedef enum { TAB_MAIL, TAB_CONV, TAB_COMPOSE, TAB_FILTERS, TAB_SETTINGS, TAB_CALENDAR } tab_kind;

/* a move or delete that waits a few seconds before it goes to the server, so it can be undone */
typedef struct {
    int64_t *ids;
    int n;
    char *to;      /* folder name, NULL: delete for good */
    double due;    /* GetTime() when it is sent */
    char label[160];
} jd_pending;

/* right-click menu */
typedef struct {
    int open;
    Vector2 at;
    int on_folder; /* else on the list row */
    int index;
} jd_menu;

/* the server's Sieve script, edited in a tab */
typedef struct {
    jd_editor script;
    jd_field name;
    int state; /* 0 loading, 1 ready, -1 failed */
    int saving, editing_name;
    char message[512];
} jd_filters;

struct jd_compose;

typedef struct {
    tab_kind kind;
    jd_conv conv;                 /* TAB_MAIL: the preview pane; TAB_CONV: the conversation */
    struct jd_compose *compose;   /* TAB_COMPOSE */
} jd_tab;

typedef struct {
    jd_ui ui;
    int show_form;

    jd_account account;
    jd_prefs prefs;
    struct jd_fetcher *fetcher; /* remote images */
    int have_account;
    char *password;
    char *dir;
    jd_sync *sync;
    jd_store *view;

    jd_field fields[F_COUNT];
    jd_security imap_sec, smtp_sec;
    int focus;
    char form_error[512];

    /* sidebar */
    jd_folder_row *folders;
    int nfolders, folder;

    /* list */
    int pane;
    int grouped;
    int filter_unread, filter_flagged, filter_attachment;
    char *filter_from;
    int searching;    /* the search field has the keyboard */
    int search_here;  /* the search covers the folder on show, else every folder */
    jd_field search;  /* what is typed after the pills */
    char **pills;     /* typed operators ("from:ada") turned into pills; scope and quick filters are pills too,
                         but live in search_here and filter_* */
    int npills;
    char *query;      /* the search in effect, NULL for the folder view */
    int64_t search_serial;
    int64_t *server_hits;
    int nserver_hits;
    int server_searching;
    jd_thread_row *rows;
    int nrows, row;
    float list_scroll;
    float side_scroll; /* the sidebar's folder rows (full and rail) */
    int side_followed; /* the folder the sidebar last scrolled to */
    int followed_row; /* the selected row the list last scrolled to; it follows only when the selection moves */
    int pending_g, pending_f, pending_d;
    int64_t *picked; /* multi-select: thread ids (grouped) or message ids */
    int npicked, anchor;
    jd_pending pending;
    jd_menu menu;
    int help;
    int settings_section, settings_focus;
    float settings_scroll;
    jd_editor sig_new, sig_reply;

    /* tabs: 0 is the mail view, whose conv is the preview pane */
    jd_tab *tabs;
    int ntabs, tab;

    /* layout, in [view] of settings.toml */
    float side_w, list_w, list_h;
    int visual;          /* visual mode: moving extends the selection from anchor */
    char *fonts_loaded;  /* the families the fonts were loaded for */
    int print_tab;       /* 1 + the tab whose conversation prints after this frame, 0 none */
    int64_t print_message; /* a message alone (a reader card's Print), after this frame; 0 none */
    int page_flip;       /* Shift+L: the mail page against the theme; not saved */
    int dragging; /* 0, 1 sidebar edge, 2 list edge */

    jd_filters filters;

    jd_dialog dialog;
    char *status, *error;   /* what the worker is doing; the last failure */
    char notice[300];       /* a one-off confirmation, shown for a few seconds */
    double notice_until;
} jd_app;

void notify_user(jd_app *app, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef enum { MODE_NORMAL, MODE_VISUAL, MODE_INSERT } jd_mode;
jd_mode current_mode(jd_app *app);
/* reading pane right, bottom, off */
void cycle_reader(jd_app *app);
void fonts_release(jd_app *app);
int compose_inserting(struct jd_compose *c);
int compose_visual(struct jd_compose *c);

void filters_open(jd_app *app);
void filters_loaded(jd_app *app, const jd_event *ev);
void filters_saved(jd_app *app, const char *error);
void filters_keys(jd_app *app);
void filters_draw(jd_app *app, Rectangle r);

void form_open(jd_app *app, const char *error);
void form_fill(jd_app *app);
int form_submit(jd_app *app);
float form_fields_draw(jd_app *app, float x, float y, float w, int *focus);
void form_frame(jd_app *app);
/* the keyboard reference from y, only the lines filter (may be NULL) finds; returns the y below it */
float help_content(jd_app *app, float x, float y, float w, const char *filter);

void mail_frame(jd_app *app);
void mail_reload(jd_app *app);
void mail_body_ready(jd_app *app, int64_t id);
void mail_server_results(jd_app *app, int64_t serial, const int64_t *ids, int n);
void mail_select_inbox(jd_app *app);
/* a tapped notification: the Inbox, with that message open */
void mail_open_message(jd_app *app, int64_t id);

void sidebar_draw(jd_app *app, Rectangle r);
int sidebar_keys(jd_app *app);
const char *folder_label(const jd_folder_row *f);
/* shows the folder with this special use ("Sent", "Drafts", ...) or "INBOX"; 0 when there is none */
int folder_go(jd_app *app, const char *use);
int folder_hidden(jd_app *app, int i);

void list_reload(jd_app *app);
/* the coloured circle with initials the list and the reader show for a sender */
void sender_avatar(jd_app *app, const char *name, const char *addr, float cx, float cy, float r);
/* a suggestion for the word being typed in a query (search, rules): what replaces it, and what is shown */
typedef struct {
    char insert[300]; /* a trailing space makes an operator a pill at once */
    char label[300], note[80];
} jd_hint;
/* the operators or values (contacts, folders, flags, dates) that fit word, at most max */
int query_hints(jd_app *app, const char *word, jd_hint *out, int max);
/* the icon and colour of a filter ("from:…", "in:…", "is:…"), as pills and hints show them */
void filter_kind(const char *text, const char **icon, Color *tint);

/* A query typed as in the search box but for rules: complete operators become pills, hints under it */
typedef struct {
    jd_field text;
    char **pills;
    int npills;
    jd_hint hints[10];
    int nhints, sel;
    char hints_for[300];
    Rectangle hints_box;
} jd_qfield;
void qfield_clear(jd_qfield *q);
/* the pills and the typed words as one query; malloc'd, NULL when empty */
char *qfield_query(const jd_qfield *q);
/* typing, Backspace into the last pill, hints with Up/Down/Tab; 1 when Enter was pressed with no hint picked */
int qfield_keys(jd_app *app, jd_qfield *q);
/* the field in box; returns 1 when clicked. Its hints are drawn by qfield_hints_draw, over what follows. */
int qfield_draw(jd_app *app, jd_qfield *q, Rectangle box, int focused);
void qfield_hints_draw(jd_app *app, jd_qfield *q, int focused);
/* a query's tokens as they show in a pill field, without editing: returns the width used */
float query_pills_draw(jd_app *app, const char *query, float x, float y, float w);

/* the search as one query: scope (with_scope), quick filters, pills and typed words; NULL when empty. free() it */
char *search_full_query(jd_app *app, int with_scope);
void list_draw(jd_app *app, Rectangle r);
int list_keys(jd_app *app);
/* gg and g + letter (folders by role), before any pane's keys */
int go_keys(jd_app *app);
/* the actions on the selection (the picked rows, else the current one) */
void act_archive(jd_app *app);
void act_delete(jd_app *app);
void act_toggle_read(jd_app *app);
void act_toggle_flag(jd_app *app);
void act_move_to(jd_app *app, const char *folder, int copy);
/* takes ids; single: one message of a conversation, not the list's selection */
void act_move_ids(jd_app *app, int64_t *ids, int n, const char *folder, int copy, int single);
/* a reader card's actions: on that one message, whatever the list has selected */
void message_archive(jd_app *app, int64_t id);
/* folder: where it is, so a message already in Trash is deleted for good (after asking) */
void message_delete(jd_app *app, int64_t id, int64_t folder);
void message_move_ask(jd_app *app, int64_t id, int copy);
/* sends a pending move/delete now (when due, or before quitting); undo drops it */
void pending_tick(jd_app *app, int force);
void pending_undo(jd_app *app);
void pending_settled(jd_app *app);
void menu_draw(jd_app *app);
void help_draw(jd_app *app);

void settings_open(jd_app *app);
/* Settings, at its Calendar section */
void settings_show_calendar(jd_app *app);
void settings_keys(jd_app *app);
void settings_draw(jd_app *app, Rectangle r);
/* the theme, scale and layout of prefs, on screen now */
void apply_prefs(jd_app *app);

jd_conv *preview(jd_app *app);
void conv_open(jd_app *app, jd_conv *c, int64_t thread, int64_t only_message);
/* c shows the list's next (dir 1) or previous (-1) conversation; 0 at the list's ends. keep, if given, takes the
   conversation c showed; conv_close it after */
int reader_step(jd_app *app, jd_conv *c, int dir, jd_conv *keep);
/* the list's selection becomes c's conversation, for the act_* functions; 0 (and a notice) when the list lacks it */
int list_select_conv(jd_app *app, const jd_conv *c);
void list_refresh(jd_app *app);

/* generated by CMakeLists.txt: "0.2.1" or "0.2.1-dev+abc1234[.dirty]", and the configure day */
extern const char jd_version[], jd_build_date[];

/* updates (update.c): update_tick every frame; update_bar draws the daily check's offer in the list, update_action
   a Settings check's (Update, its progress, Restart); both return the height used */
void update_tick(jd_app *app);
void update_check(jd_app *app, int manual);
float update_bar(jd_app *app, float x, float y, float w);
float update_action(jd_app *app, float x, float y, float w);
const char *update_status(void);
int update_checking(void);
void conv_close(jd_conv *c);
void conv_refresh(jd_app *app, jd_conv *c);
void conv_body_ready(jd_app *app, jd_conv *c, int64_t id);
/* a fetched remote image (ok 0: it failed) for every open message that shows it */
void images_arrived(jd_app *app, const char *url, int ok);
void conv_draw(jd_app *app, jd_conv *c, Rectangle r, int focused);
static inline int mail_page_dark(const jd_app *app) { return !app->prefs.light != !!app->page_flip; }
/* the newest message of c as an .eml file in Downloads */
void conv_save_message(jd_app *app, jd_conv *c);
/* c as an A4 PDF, handed to the system print dialog; draws off screen, so it runs between frames */
void print_conv(jd_app *app, jd_conv *c);
int conv_keys(jd_app *app, jd_conv *c);
/* a keyboard caret in a reader: moving freely (reader_caret), or selecting (reader_visual) */
int reader_visual(void);
int reader_caret(void);
/* the selected list row into the preview pane, or into a new tab */
void reader_open(jd_app *app, int in_tab);
void reader_close(jd_app *app);

typedef enum { COMPOSE_NEW, COMPOSE_REPLY, COMPOSE_REPLY_ALL, COMPOSE_FORWARD } compose_mode;

/* a compose tab: new mail, a reply or forward of source (a message id), or the draft with id draft */
void compose_open(jd_app *app, compose_mode mode, int64_t source);
void compose_open_draft(jd_app *app, int64_t draft);
void compose_keys(jd_app *app, struct jd_compose *c);
void compose_draw(jd_app *app, struct jd_compose *c, Rectangle r);
const char *compose_title(struct jd_compose *c);
/* a compose tab with changes saves them as a draft before it closes */
int compose_may_close(jd_app *app, struct jd_compose *c, int tab);
/* the open tab's message thrown away: no draft kept, the saved ones of it removed from Drafts */
void compose_discard(jd_app *app);
/* the open tab's message scheduled for a typed local time, "2026-11-19 08:00" */
void compose_schedule_typed(jd_app *app, const char *text);
void compose_free(jd_app *app, struct jd_compose *c);
void compose_sent(jd_app *app, int64_t serial, const char *error);
void compose_saved(jd_app *app, int64_t serial, const char *error);
void compose_attach_paths(jd_app *app, struct jd_compose *c, char **paths, int n);
void compose_set_to(struct jd_compose *c, const char *mailto);

/* calendar.c: a worker per [[calendar]] account started (its secret in the keystore) and stopped */
void calendar_start(jd_app *app);
void calendar_stop(jd_app *app);
/* every frame: the worker's events, reminders due */
void calendar_tick(jd_app *app);
void calendar_open(jd_app *app);
void calendar_keys(jd_app *app);
void calendar_draw(jd_app *app, Rectangle r);
/* the event editor has the keyboard */
int calendar_typing(void);
const char *calendar_title(void);
/* the worker's status and last failure, for the footer while the calendar shows */
const char *calendar_status(void);
const char *calendar_error(void);
void calendar_delete_confirmed(jd_app *app);
/* a sent mail's outcome: 1 when it was the calendar's answer to an organizer */
int calendar_sent(jd_app *app, int64_t serial, const char *error);
/* every calendar account's password or Google sign-in out of the keystore (logging out) */
void calendar_forget_all(jd_app *app);
/* a calendar account's worker runs and is signed in */
int calendar_connected(void);
/* Settings' Calendar section; returns the y below it */
float calendar_settings(jd_app *app, float x, float y, float w, int *focus);
int calendar_settings_keys(jd_app *app);
/* the shared settings' JD_SHARE_CHOICES: as TOML from a shared copy t, else this store's followed by keep's for the
   calendars not found here yet; and a copy's onto this store */
char *calendar_choices_text(jd_app *app, const jd_toml *t, const jd_toml *keep);
void calendar_choices_apply(jd_app *app, const jd_toml *t);
/* a shared copy's calendar accounts in place of these, the ones it lacks forgotten here */
void calendar_accounts_apply(jd_app *app, const jd_toml *t);

/* settings.c: the settings shared with the account's other jackdaws ([sync] settings). share_check merges the
   server's copy (after the store changed), share_changed after a change made here, share_decided answers the
   first look's question (1: the other jackdaw's settings, 0: this device's) */
void share_check(jd_app *app);
void share_changed(jd_app *app);
void share_decided(jd_app *app, int theirs);
/* a device out of the shared copy's list (it writes itself in again when it next shares) */
void share_forget_device(jd_app *app, const char *id);
/* a calendar worker filed calendars: the agreed choices onto the ones just found */
void share_calendars_found(jd_app *app);
/* an invitation, update, cancellation or answer in a message, with Accept / Maybe / Decline; the y below it */
float invite_card(jd_app *app, const char *ics, float x, float y, float w, Rectangle clip);

void tabs_draw(jd_app *app, Rectangle r);
int tabs_keys(jd_app *app);
jd_tab *tab_add(jd_app *app, tab_kind kind);
const char *tab_title(jd_app *app, int i);
void tab_close(jd_app *app, int i);
void save_prefs(jd_app *app);

void dialog_open(jd_app *app, dialog_kind kind, action act, const char *title, const char *value);
void dialog_draw(jd_app *app);
int64_t *selection_ids(jd_app *app, int *n);

void app_connect(jd_app *app);
void app_disconnect(jd_app *app);
/* forgets the account: its password, its settings section and its local mail (to the trash) */
void app_logout(jd_app *app);
void format_date(int64_t t, char *out, int len);
