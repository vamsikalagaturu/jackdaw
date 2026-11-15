#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/util.h"

/* what an empty script starts as: RFC 5228 with fileinto, all commented out */
static const char *starter =
    "require [\"fileinto\"];\n"
    "\n"
    "# Runs on the server for every incoming message, even while jackdaw is closed.\n"
    "# if header :contains \"list-id\" \"announce.example.org\" {\n"
    "#     fileinto \"Archive\";\n"
    "#     stop;\n"
    "# }\n";

void filters_open(jd_app *app)
{
    for (int i = 0; i < app->ntabs; i++)
        if (app->tabs[i].kind == TAB_FILTERS) {
            app->tab = i;
            return;
        }
    tab_add(app, TAB_FILTERS);
    jd_filters *f = &app->filters;
    f->state = 0;
    f->message[0] = 0;
    jd_editor_set(&f->script, "");
    if (app->sync) jd_sync_sieve_get(app->sync);
}

void filters_loaded(jd_app *app, const jd_event *ev)
{
    jd_filters *f = &app->filters;
    if (ev->count < 0) {
        f->state = -1;
        snprintf(f->message, sizeof f->message, "%s", ev->text ? ev->text : "The server's filters could not be read.");
        return;
    }
    f->state = 1;
    jd_editor_set(&f->script, ev->text && ev->text[0] ? ev->text : starter);
    jd_field_set(&f->name, ev->text2 ? ev->text2 : "jackdaw");
    snprintf(f->message, sizeof f->message, "%s", ev->text2 ? "" : "The server has no filters yet.");
}

void filters_saved(jd_app *app, const char *error)
{
    jd_filters *f = &app->filters;
    f->saving = 0;
    snprintf(f->message, sizeof f->message, "%s", error ? error : "Saved and active on the server.");
    if (!error) f->script.changed = 0;
}

static void save(jd_app *app)
{
    jd_filters *f = &app->filters;
    if (f->state != 1 || f->saving || !app->sync) return;
    f->saving = 1;
    snprintf(f->message, sizeof f->message, "Checking and saving...");
    jd_sync_sieve_put(app->sync, f->name.len ? f->name.text : "jackdaw", f->script.text ? f->script.text : "");
}

void filters_keys(jd_app *app)
{
    jd_filters *f = &app->filters;
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_S)) {
        save(app);
        return;
    }
    if (f->state != 1) return;
    if (f->editing_name) {
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_TAB)) f->editing_name = 0;
        else jd_field_input(&f->name);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        jd_editor_insert(&f->script, "    ", 4);
        return;
    }
    jd_editor_input(&app->ui, &f->script);
}

void filters_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    jd_filters *f = &app->filters;
    float x = r.x + 28, w = r.width - 56, y = r.y + 16;
    jd_text(ui, "Server filters (Sieve)", x, y, 20, JD_NORD6, 1);
    y += 34;
    if (f->state == 0) {
        jd_text(ui, "Reading the server's filters...", x, y, 14, JD_NORD4, 0);
        return;
    }
    if (f->state < 0) {
        jd_text_fit(ui, f->message, x, y, w, 14, JD_NORD11, 0);
        return;
    }
    jd_text(ui, "Script", x, y + 9, 13, JD_NORD4, 1);
    if (jd_field_draw(ui, &f->name, (Rectangle){x + 60, y, 240, 32}, f->editing_name)) f->editing_name = 1;
    y += 42;
    float bottom = r.y + r.height - 60;
    if (jd_editor_draw(ui, &f->script, (Rectangle){x, y, w, bottom - y}, !f->editing_name)) f->editing_name = 0;
    y = bottom + 12;
    if (jd_button(ui, f->saving ? "Saving..." : "Save", (Rectangle){x, y, 110, 36}, 1)) save(app);
    int failed = f->message[0] && strncmp(f->message, "Saved", 5) != 0 && strncmp(f->message, "Checking", 8) != 0 &&
                 strncmp(f->message, "The server has no", 17) != 0;
    jd_text_fit(ui, f->message[0] ? f->message : "Ctrl+S checks the script on the server, then stores it and makes it active.",
                x + 124, y + 11, w - 124, 13, failed ? JD_NORD11 : JD_NORD4, 0);
}
