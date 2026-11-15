#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/util.h"
#include "mail/fetch.h"
#include "platform.h"
#include "raylib.h"
#include "ui/icons.h"

void app_connect(jd_app *app)
{
    free(app->dir);
    app->dir = jd_account_dir(&app->account);
    char *db = jd_printf("%s/mail.db", app->dir);
    char err[512];
    jd_store_close(app->view);
    /* opened (and the schema created) here first, so the worker and the UI agree on it */
    app->view = jd_store_open(db, err, sizeof err);
    if (!app->view) {
        free(app->error);
        app->error = jd_strdup(err);
    } else {
        app->sync = jd_sync_start(&app->account, app->password, app->dir, db);
        char *images = jd_printf("%s/images", app->dir);
        app->fetcher = jd_fetcher_start(images);
        free(images);
        mail_reload(app);
    }
    free(db);
}

/* open compose tabs leave drafts behind; the worker sends what is queued before it stops */
void app_disconnect(jd_app *app)
{
    while (app->ntabs > 1) tab_close(app, app->ntabs - 1);
    jd_sync_stop(app->sync);
    app->sync = NULL;
    jd_fetcher_stop(app->fetcher);
    app->fetcher = NULL;
    jd_store_close(app->view);
    app->view = NULL;
    reader_close(app);
    jd_folder_rows_free(app->folders, app->nfolders);
    jd_thread_rows_free(app->rows, app->nrows);
    app->folders = NULL;
    app->rows = NULL;
    app->nfolders = app->folder = app->nrows = app->row = 0;
    free(app->status), free(app->error), free(app->query), free(app->server_hits), free(app->filter_from);
    app->status = app->error = app->query = app->filter_from = NULL;
    app->server_hits = NULL;
    app->nserver_hits = 0;
    free(app->dialog.ids);
    app->dialog = (jd_dialog){0};
}

static void handle_events(jd_app *app)
{
    jd_event ev;
    int changed = 0;
    while (app->sync && jd_sync_poll(app->sync, &ev)) {
        switch (ev.kind) {
        case JD_EV_STATUS:
            free(app->status);
            app->status = jd_strdup(ev.text);
            if (ev.text) {
                free(app->error);
                app->error = NULL;
            }
            break;
        case JD_EV_ERROR:
            free(app->error);
            app->error = jd_strdup(ev.text);
            break;
        case JD_EV_AUTH: {
            char *why = jd_strdup(ev.text);
            jd_event_free(&ev);
            app_disconnect(app);
            form_open(app, why);
            free(why);
            return;
        }
        case JD_EV_CHANGED:
            changed = 1;
            pending_settled(app);
            break;
        case JD_EV_BODY:
            changed = 1;
            mail_body_ready(app, ev.id);
            break;
        case JD_EV_SEARCH:
            mail_server_results(app, ev.id, ev.ids, ev.nids);
            break;
        case JD_EV_SENT:
            compose_sent(app, ev.id, ev.text);
            break;
        case JD_EV_SAVED:
            compose_saved(app, ev.id, ev.text);
            break;
        case JD_EV_SIEVE:
            filters_loaded(app, &ev);
            break;
        case JD_EV_SIEVE_SAVED:
            filters_saved(app, ev.text);
            break;
        case JD_EV_NEW_MAIL:
            changed = 1;
            if (app->prefs.notify && (app->prefs.notify_focused || !IsWindowFocused())) {
                if (ev.count == 1) {
                    plat_notify(ev.text, ev.text2, ev.id);
                } else {
                    char *summary = jd_printf("%d new messages", ev.count);
                    char *body = jd_printf("%s: %s", ev.text, ev.text2);
                    plat_notify(summary, body, ev.id);
                    free(summary), free(body);
                }
            }
            break;
        }
        jd_event_free(&ev);
    }
    if (changed) mail_reload(app);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("jackdaw " JD_VERSION);
        return 0;
    }
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1100, 720, "jackdaw");
    SetExitKey(KEY_NULL);
    jd_app app = {.grouped = 1, .pane = PANE_LIST};

    char err[512];
    jd_prefs_load(&app.prefs);
    tab_add(&app, TAB_MAIL);
    apply_prefs(&app);
    app.have_account = jd_account_load(&app.account, err, sizeof err) == 0;
    if (app.have_account) app.password = plat_secret_load(app.account.address);
    if (app.have_account && app.password) app_connect(&app);
    else form_open(&app, NULL);

    while (!WindowShouldClose()) {
        handle_events(&app);
        char *url;
        int ok;
        while (app.fetcher && jd_fetch_poll(app.fetcher, &url, &ok)) {
            images_arrived(&app, url, ok);
            free(url);
        }
        if (plat_notify_poll()) {
            mail_select_inbox(&app);
            SetWindowFocused();
        }
        if (!app.show_form && IsKeyPressed(KEY_COMMA) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)))
            settings_open(&app);
        BeginDrawing();
        ClearBackground(JD_NORD0);
        jd_scale_begin();
        if (app.show_form) form_frame(&app);
        else mail_frame(&app);
        jd_scale_end();
        EndDrawing();
        if (app.print_tab) {
            if (app.print_tab <= app.ntabs) print_conv(&app, &app.tabs[app.print_tab - 1].conv);
            app.print_tab = 0;
        }
    }
    pending_tick(&app, 1);
    app_disconnect(&app);
    plat_notify_shutdown();
    jd_account_free(&app.account);
    jd_prefs_free(&app.prefs);
    if (app.password) memset(app.password, 0, strlen(app.password));
    free(app.password), free(app.dir);
    conv_close(&app.tabs[0].conv);
    free(app.tabs);
    jd_editor_free(&app.filters.script);
    jd_icons_unload();
    fonts_release(&app);
    CloseWindow();
    return 0;
}
