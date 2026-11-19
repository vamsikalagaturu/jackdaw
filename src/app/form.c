#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "core/util.h"
#include "mail/fetch.h"
#include "platform.h"

/* ---- the accounts ---- */

int accounts_count(jd_app *app)
{
    return app->nslots ? app->nslots : app->have_account;
}

const jd_account *account_at(jd_app *app, int i)
{
    return !app->nslots || i == app->cur ? &app->account : &app->slots[i].account;
}

void settings_write(jd_app *app)
{
    int n = accounts_count(app);
    const jd_account **all = jd_alloc(sizeof *all * (size_t)(n + 1));
    for (int i = 0; i < n; i++) all[i] = account_at(app, i);
    jd_settings_save(all, app->have_account ? n : 0, &app->prefs);
    free(all);
}

void account_first(jd_app *app, jd_store **view, jd_sync **sync, const char **dir, const jd_account **account)
{
    int open = !app->nslots || app->cur == 0;
    jd_slot *s = app->nslots ? &app->slots[0] : NULL;
    *view = open ? app->view : s->view;
    *sync = open ? app->sync : s->sync;
    *dir = open ? app->dir : s->dir;
    *account = open ? &app->account : &s->account;
}

/* the Inbox's unread messages of a store */
static int inbox_unread(jd_store *view)
{
    int n, unread = 0;
    jd_folder_row *f = view ? jd_store_folders(view, &n) : NULL;
    for (int i = 0; f && i < n; i++)
        if (strcasecmp(f[i].name, "INBOX") == 0) unread = f[i].unread;
    if (f) jd_folder_rows_free(f, n);
    return unread;
}

static void wipe(char **password)
{
    if (*password) memset(*password, 0, strlen(*password));
    free(*password);
    *password = NULL;
}

/* account i's worker started in the background */
static void slot_start(jd_app *app, int i)
{
    jd_slot *s = &app->slots[i];
    free(s->refused);
    s->refused = NULL;
    if (!s->password) s->password = plat_secret_load(s->account.address);
    if (!s->password) {
        s->refused = jd_strdup("No password in the keystore: open the account to sign in.");
        return;
    }
    if (!s->dir) s->dir = jd_account_dir(&s->account);
    char *db = jd_printf("%s/mail.db", s->dir);
    char err[512];
    if (!s->view) s->view = jd_store_open(db, err, sizeof err);
    if (s->view && !s->sync) s->sync = jd_sync_start(&s->account, s->password, s->dir, db);
    if (!s->view) s->refused = jd_strdup(err);
    s->unread = inbox_unread(s->view);
    free(db);
}

static void slot_stop(jd_slot *s)
{
    jd_sync_stop(s->sync);
    s->sync = NULL;
    jd_store_close(s->view);
    s->view = NULL;
    wipe(&s->password);
    free(s->dir), free(s->refused);
    s->dir = s->refused = NULL;
}

/* the open account's account and handles into its slot, the app's fields left empty */
static void stash(jd_app *app)
{
    jd_slot *s = &app->slots[app->cur];
    s->account = app->account;
    s->password = app->password, s->dir = app->dir;
    s->view = app->view, s->sync = app->sync;
    app->account = (jd_account){0};
    app->password = app->dir = NULL;
    app->view = NULL, app->sync = NULL;
}

static void unstash(jd_app *app, int i)
{
    jd_slot *s = &app->slots[i];
    app->cur = i;
    app->account = s->account;
    app->password = s->password, app->dir = s->dir;
    app->view = s->view, app->sync = s->sync;
    s->account = (jd_account){0};
    s->password = s->dir = NULL;
    s->view = NULL, s->sync = NULL;
}

void accounts_start(jd_app *app)
{
    jd_account *all;
    int n;
    char err[512];
    app->have_account = jd_accounts_load(&all, &n, err, sizeof err) == 0;
    if (!app->have_account) {
        form_open(app, NULL);
        return;
    }
    app->slots = jd_alloc(sizeof *app->slots * (size_t)n);
    for (int i = 0; i < n; i++) app->slots[i] = (jd_slot){.account = all[i]};
    free(all);
    app->nslots = n;
    unstash(app, 0);
    app->password = plat_secret_load(app->account.address);
    if (app->password) app_connect(app);
    else form_open(app, NULL);
    for (int i = 1; i < n; i++) slot_start(app, i);
}

void accounts_stop(jd_app *app)
{
    for (int i = 0; i < app->nslots; i++) {
        if (i == app->cur) continue;
        slot_stop(&app->slots[i]);
        jd_account_free(&app->slots[i].account);
    }
    free(app->slots);
    app->slots = NULL;
    app->nslots = app->cur = 0;
}

/* the open account out of view, its worker going on in the background; 0 when a tab would not close */
static int park(jd_app *app)
{
    /* an archive or delete still waiting for Undo is this account's: it goes now */
    pending_tick(app, 1);
    /* its own tabs close: conversations, messages being written, its server filters; Settings and the Calendar stay */
    for (int t = app->ntabs - 1; t > 0; t--) {
        tab_kind k = app->tabs[t].kind;
        if (k != TAB_CONV && k != TAB_COMPOSE && k != TAB_FILTERS) continue;
        int before = app->ntabs;
        tab_close(app, t);
        if (app->ntabs == before) return 0;
    }
    /* sent and waiting for Undo: it goes now, by this account */
    compose_held_flush(app);
    app->slots[app->cur].unread = inbox_unread(app->view);
    mail_view_reset(app);
    jd_fetcher_stop(app->fetcher);
    app->fetcher = NULL;
    stash(app);
    return 1;
}

/* account i in view: its worker running, its folders read */
static void open_slot(jd_app *app, int i)
{
    unstash(app, i);
    jd_slot *s = &app->slots[i];
    if (s->refused || !app->sync) {
        char *why = s->refused ? jd_strdup(s->refused) : NULL;
        free(s->refused);
        s->refused = NULL;
        if (app->sync) jd_sync_stop(app->sync), app->sync = NULL;
        jd_store_close(app->view);
        app->view = NULL;
        form_open(app, why);
        free(why);
        return;
    }
    char *images = jd_printf("%s/images", app->dir);
    app->fetcher = jd_fetcher_start(images);
    free(images);
    /* Settings > Account shows this one's servers now */
    form_fill(app);
    mail_reload(app);
}

void account_switch(jd_app *app, int i)
{
    if (i == app->cur || i < 0 || i >= app->nslots || app->show_form) return;
    if (!park(app)) return;
    open_slot(app, i);
    if (app->sync) notify_user(app, "%s", app->account.address);
}

void account_add_open(jd_app *app)
{
    app->adding = 1;
    jd_account none = {0};
    form_fill_from(app, &none, 0);
    app->focus = F_ADDRESS;
    app->show_form = 1;
}

void accounts_tick(jd_app *app)
{
    for (int i = 0; i < app->nslots; i++) {
        jd_slot *s = &app->slots[i];
        if (i == app->cur || !s->sync) continue;
        jd_event ev;
        int changed = 0;
        while (s->sync && jd_sync_poll(s->sync, &ev)) {
            switch (ev.kind) {
            case JD_EV_NEW_MAIL:
                changed = 1;
                if (app->prefs.notify) {
                    char *body = jd_printf("%s\n%s", ev.text2 ? ev.text2 : "", s->account.address);
                    if (ev.count == 1) {
                        plat_notify(ev.text, body, notify_tag(i, ev.id));
                    } else {
                        char *summary = jd_printf("%d new messages", ev.count);
                        plat_notify(summary, body, notify_tag(i, ev.id));
                        free(summary);
                    }
                    free(body);
                }
                break;
            case JD_EV_CHANGED: changed = 1; break;
            case JD_EV_AUTH:
                free(s->refused);
                s->refused = jd_strdup(ev.text ? ev.text : "The server refused the password.");
                break;
            case JD_EV_SENT:
                if (!calendar_sent(app, ev.id, ev.text)) compose_sent(app, ev.id, ev.text);
                break;
            case JD_EV_SCHEDULED:
                if (ev.text) notify_user(app, "%s: scheduled message not sent: %s; it is in Drafts.", s->account.address, ev.text);
                else notify_user(app, "Sent from %s: %s", s->account.address, ev.text2 && ev.text2[0] ? ev.text2 : "(no subject)");
                break;
            default: break;
            }
            jd_event_free(&ev);
        }
        if (s->refused && s->sync) {
            jd_sync_stop(s->sync);
            s->sync = NULL;
        }
        if (changed) {
            s->unread = inbox_unread(s->view);
            /* the first account's folder carries the shared settings */
            if (i == 0) share_check(app);
        }
    }
}

void app_logout(jd_app *app)
{
    char *address = jd_strdup(app->account.address), *dir = jd_strdup(app->dir);
    app_disconnect(app);
    if (address) plat_secret_delete(address);
    wipe(&app->password);
    free(app->dir);
    app->dir = NULL;
    jd_account_free(&app->account);
    app->account = (jd_account){0};
    /* its slot goes; another account opens */
    if (app->nslots) {
        memmove(app->slots + app->cur, app->slots + app->cur + 1, sizeof *app->slots * (size_t)(app->nslots - app->cur - 1));
        app->nslots--;
    }
    app->have_account = app->nslots > 0;
    settings_write(app);
    char *why = NULL;
    if (dir && access(dir, F_OK) == 0 && plat_trash(dir) != 0)
        why = jd_printf("Logged out, but the local mail in %s could not be removed.", dir);
    free(address), free(dir);
    if (app->nslots) {
        open_slot(app, 0);
        if (why) notify_user(app, "%s", why);
    } else {
        free(app->slots);
        app->slots = NULL;
        app->cur = 0;
        form_open(app, why);
    }
    free(why);
}

static const char *labels[F_COUNT] = {"Name", "Address", "Username", "IMAP server", "Port", "SMTP server", "Port", "Password"};

void form_fill_from(jd_app *app, const jd_account *a, int existing)
{
    jd_field *f = app->fields;
    jd_field_set(&f[F_NAME], a->name);
    jd_field_set(&f[F_ADDRESS], a->address);
    jd_field_set(&f[F_USERNAME], a->username && !jd_streq(a->username, a->address) ? a->username : NULL);
    jd_field_set(&f[F_IMAP_HOST], a->imap.host);
    jd_field_set(&f[F_SMTP_HOST], a->smtp.host);
    app->imap_sec = existing ? a->imap.security : JD_SEC_TLS;
    app->smtp_sec = existing ? a->smtp.security : JD_SEC_TLS;
    char port[16];
    snprintf(port, sizeof port, "%d", existing ? a->imap.port : jd_default_port(1, app->imap_sec));
    jd_field_set(&f[F_IMAP_PORT], port);
    snprintf(port, sizeof port, "%d", existing ? a->smtp.port : jd_default_port(0, app->smtp_sec));
    jd_field_set(&f[F_SMTP_PORT], port);
    jd_field_set(&f[F_PASSWORD], NULL);
    f[F_PASSWORD].secret = 1;
    f[F_NAME].placeholder = "Shown in the sidebar (optional)";
    f[F_ADDRESS].placeholder = "you@example.org";
    f[F_USERNAME].placeholder = "Same as the address";
    f[F_PASSWORD].placeholder = existing && app->password ? "Unchanged" : "App password";
    app->form_error[0] = 0;
}

void form_fill(jd_app *app)
{
    form_fill_from(app, &app->account, app->have_account);
}

void form_open(jd_app *app, const char *error)
{
    app->adding = 0;
    form_fill(app);
    app->fields[F_PASSWORD].placeholder = "App password";
    app->focus = app->have_account ? F_PASSWORD : F_ADDRESS;
    snprintf(app->form_error, sizeof app->form_error, "%s", error ? error : "");
    app->show_form = 1;
}

static void toggle_security(jd_app *app, int imap)
{
    jd_security *sec = imap ? &app->imap_sec : &app->smtp_sec;
    jd_field *port = &app->fields[imap ? F_IMAP_PORT : F_SMTP_PORT];
    int was_default = atoi(port->text) == jd_default_port(imap, *sec);
    *sec = *sec == JD_SEC_TLS ? JD_SEC_STARTTLS : JD_SEC_TLS;
    if (was_default || !port->len) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", jd_default_port(imap, *sec));
        jd_field_set(port, buf);
    }
}

static int port_of(const jd_field *f)
{
    char *end;
    long p = strtol(f->text, &end, 10);
    return *end || p <= 0 || p > 65535 ? 0 : (int)p;
}

/* an empty password field keeps the password in use (from Settings); returns 0 when connected anew */
int form_submit(jd_app *app)
{
    jd_field *f = app->fields;
    const char *why = NULL;
    if (!strchr(f[F_ADDRESS].text, '@')) why = "The address needs an @.";
    else if (!f[F_IMAP_HOST].len || !f[F_SMTP_HOST].len) why = "Both servers are needed.";
    else if (!port_of(&f[F_IMAP_PORT]) || !port_of(&f[F_SMTP_PORT])) why = "Ports are numbers from 1 to 65535.";
    else if (!f[F_PASSWORD].len && (app->adding || !app->password)) why = "The password is needed.";
    for (int i = 0; !why && app->adding && i < accounts_count(app); i++)
        if (account_at(app, i)->address && strcasecmp(account_at(app, i)->address, f[F_ADDRESS].text) == 0)
            why = "That account is here already.";
    if (why) {
        snprintf(app->form_error, sizeof app->form_error, "%s", why);
        return -1;
    }
    char *password = jd_strdup(f[F_PASSWORD].len ? f[F_PASSWORD].text : app->password);
    int sieve_port = !app->adding && app->account.sieve_port ? app->account.sieve_port : 4190;
    if (app->adding && app->have_account) {
        /* the open account goes on in the background; the new one gets a slot of its own and opens */
        if (!app->nslots) {
            app->slots = jd_alloc(sizeof *app->slots);
            app->nslots = 1, app->cur = 0;
        }
        if (!park(app)) {
            free(password);
            return -1;
        }
        app->slots = jd_realloc(app->slots, sizeof *app->slots * (size_t)(app->nslots + 1));
        app->slots[app->nslots] = (jd_slot){0};
        app->cur = app->nslots++;
    } else {
        app_disconnect(app);
        jd_account_free(&app->account);
        if (!app->nslots) {
            app->slots = jd_alloc(sizeof *app->slots);
            app->slots[0] = (jd_slot){0};
            app->nslots = 1, app->cur = 0;
        }
    }
    app->adding = 0;
    jd_account *a = &app->account;
    a->address = jd_strdup(f[F_ADDRESS].text);
    a->name = jd_strdup(f[F_NAME].len ? f[F_NAME].text : f[F_ADDRESS].text);
    a->username = jd_strdup(f[F_USERNAME].len ? f[F_USERNAME].text : f[F_ADDRESS].text);
    a->imap = (jd_server){jd_strdup(f[F_IMAP_HOST].text), port_of(&f[F_IMAP_PORT]), app->imap_sec};
    a->smtp = (jd_server){jd_strdup(f[F_SMTP_HOST].text), port_of(&f[F_SMTP_PORT]), app->smtp_sec};
    a->sieve_port = sieve_port;
    app->have_account = 1;
    settings_write(app);
    free(app->password);
    app->password = password;
    if (f[F_PASSWORD].len && plat_secret_available() && plat_secret_save(a->address, app->password) != 0)
        fprintf(stderr, "jackdaw: the keystore did not take the password; it lasts until jackdaw quits\n");
    jd_field_set(&f[F_PASSWORD], NULL);
    app->show_form = 0;
    app_connect(app);
    return 0;
}

/* the account fields from y down, w wide; returns the y below them */
float form_fields_draw(jd_app *app, float x, float y, float w, int *focus)
{
    jd_ui *ui = &app->ui;
    for (int i = 0; i < F_COUNT; i++) {
        if (i == F_IMAP_PORT || i == F_SMTP_PORT) continue;
        jd_text(ui, labels[i], x, y, 13, JD_NORD4, 1);
        y += 20;
        int server = i == F_IMAP_HOST || i == F_SMTP_HOST;
        float fw = server ? w - 90 - 130 - 16 : w;
        if (jd_field_draw(ui, &app->fields[i], (Rectangle){x, y, fw, 36}, *focus == i)) *focus = i;
        if (server) {
            int port = i + 1, imap = i == F_IMAP_HOST;
            if (jd_field_draw(ui, &app->fields[port], (Rectangle){x + fw + 8, y, 90, 36}, *focus == port)) *focus = port;
            jd_security sec = imap ? app->imap_sec : app->smtp_sec;
            if (jd_button(ui, sec == JD_SEC_TLS ? "TLS" : "STARTTLS", (Rectangle){x + fw + 106, y, 130, 36}, 0))
                toggle_security(app, imap);
        }
        y += 48;
    }
    if (app->form_error[0]) {
        jd_text_fit(ui, app->form_error, x, y, w, 14, JD_NORD11, 0);
        y += 26;
    }
    return y;
}

void form_frame(jd_app *app)
{
    jd_ui *ui = &app->ui;
    if (IsKeyPressed(KEY_TAB)) {
        int back = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        app->focus = (app->focus + (back ? F_COUNT - 1 : 1)) % F_COUNT;
    }
    /* the soft keyboard's Enter goes to the next field, as on a phone's forms; the last one submits */
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (plat_touch() && app->focus != F_PASSWORD) app->focus = (app->focus + 1) % F_COUNT;
        else form_submit(app);
    }
    if (!app->show_form) return;
    if (IsKeyPressed(KEY_ESCAPE) && app->have_account && app->sync) app->show_form = 0, app->adding = 0;
    jd_field_input(&app->fields[app->focus]);

    /* narrower on a phone; scrolled (finger or wheel) when the keyboard leaves too little room */
    static float scroll, content;
    float view = jd_screen_h();
    scroll -= jd_wheel_y(40);
    if (scroll > content - view + 24) scroll = content - view + 24;
    if (scroll < 0) scroll = 0;
    float w = jd_screen_w() - 32 < 460 ? jd_screen_w() - 32 : 460, x = (jd_screen_w() - w) / 2, y = 48 - scroll;
    jd_follow_begin();
    jd_text(ui, app->adding ? "Add a mail account" : app->have_account ? "Account" : "Add your mail account", x, y, 24,
            JD_NORD6, 1);
    y += 36;
    jd_text(ui, plat_secret_available() ? "The password goes to the system keystore."
                                        : "No keystore: the password is kept until jackdaw quits.",
            x, y, 14, JD_NORD4, 0);
    y += 34;
    y = form_fields_draw(app, x, y, w, &app->focus);
    if (jd_button(ui, "Save and connect", (Rectangle){x, y, 180, 38}, 1)) form_submit(app);
    if (app->have_account && app->sync && jd_button(ui, "Cancel", (Rectangle){x + 192, y, 110, 38}, 0))
        app->show_form = 0, app->adding = 0;
    content = y + 38 + scroll;
    jd_follow_end(&scroll, (Rectangle){0, 0, jd_screen_w(), view});
}
