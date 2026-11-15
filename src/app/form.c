#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"

static const char *labels[F_COUNT] = {"Name", "Address", "Username", "IMAP server", "Port", "SMTP server", "Port", "Password"};

void form_fill(jd_app *app)
{
    jd_account *a = &app->account;
    jd_field *f = app->fields;
    jd_field_set(&f[F_NAME], a->name);
    jd_field_set(&f[F_ADDRESS], a->address);
    jd_field_set(&f[F_USERNAME], a->username && !jd_streq(a->username, a->address) ? a->username : NULL);
    jd_field_set(&f[F_IMAP_HOST], a->imap.host);
    jd_field_set(&f[F_SMTP_HOST], a->smtp.host);
    app->imap_sec = app->have_account ? a->imap.security : JD_SEC_TLS;
    app->smtp_sec = app->have_account ? a->smtp.security : JD_SEC_TLS;
    char port[16];
    snprintf(port, sizeof port, "%d", app->have_account ? a->imap.port : jd_default_port(1, app->imap_sec));
    jd_field_set(&f[F_IMAP_PORT], port);
    snprintf(port, sizeof port, "%d", app->have_account ? a->smtp.port : jd_default_port(0, app->smtp_sec));
    jd_field_set(&f[F_SMTP_PORT], port);
    jd_field_set(&f[F_PASSWORD], NULL);
    f[F_PASSWORD].secret = 1;
    f[F_NAME].placeholder = "Shown in the sidebar (optional)";
    f[F_ADDRESS].placeholder = "you@example.org";
    f[F_USERNAME].placeholder = "Same as the address";
    f[F_PASSWORD].placeholder = app->password ? "Unchanged" : "App password";
    app->form_error[0] = 0;
}

void form_open(jd_app *app, const char *error)
{
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
    else if (!f[F_PASSWORD].len && !app->password) why = "The password is needed.";
    if (why) {
        snprintf(app->form_error, sizeof app->form_error, "%s", why);
        return -1;
    }
    char *password = jd_strdup(f[F_PASSWORD].len ? f[F_PASSWORD].text : app->password);
    app_disconnect(app);
    int sieve_port = app->account.sieve_port ? app->account.sieve_port : 4190;
    jd_account_free(&app->account);
    jd_account *a = &app->account;
    a->address = jd_strdup(f[F_ADDRESS].text);
    a->name = jd_strdup(f[F_NAME].len ? f[F_NAME].text : f[F_ADDRESS].text);
    a->username = jd_strdup(f[F_USERNAME].len ? f[F_USERNAME].text : f[F_ADDRESS].text);
    a->imap = (jd_server){jd_strdup(f[F_IMAP_HOST].text), port_of(&f[F_IMAP_PORT]), app->imap_sec};
    a->smtp = (jd_server){jd_strdup(f[F_SMTP_HOST].text), port_of(&f[F_SMTP_PORT]), app->smtp_sec};
    a->sieve_port = sieve_port;
    if (jd_settings_save(a, &app->prefs) != 0) {
        snprintf(app->form_error, sizeof app->form_error, "Cannot write %s.", jd_settings_path());
        free(password);
        return -1;
    }
    app->have_account = 1;
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
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) form_submit(app);
    if (!app->show_form) return;
    if (IsKeyPressed(KEY_ESCAPE) && app->have_account && app->sync) app->show_form = 0;
    jd_field_input(&app->fields[app->focus]);

    float w = 460, x = (jd_screen_w() - w) / 2, y = 48;
    jd_text(ui, app->have_account ? "Account" : "Add your mail account", x, y, 24, JD_NORD6, 1);
    y += 36;
    jd_text(ui, plat_secret_available() ? "The password goes to the system keystore."
                                        : "No keystore: the password is kept until jackdaw quits.",
            x, y, 14, JD_NORD4, 0);
    y += 34;
    y = form_fields_draw(app, x, y, w, &app->focus);
    if (jd_button(ui, "Save and connect", (Rectangle){x, y, 180, 38}, 1)) form_submit(app);
    if (app->have_account && app->sync && jd_button(ui, "Cancel", (Rectangle){x + 192, y, 110, 38}, 0))
        app->show_form = 0;
}
