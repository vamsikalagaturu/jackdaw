#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"

void dialog_open(jd_app *app, dialog_kind kind, action act, const char *title, const char *value)
{
    free(app->dialog.ids);
    app->dialog = (jd_dialog){.kind = kind, .act = act};
    snprintf(app->dialog.title, sizeof app->dialog.title, "%s", title);
    jd_field_set(&app->dialog.field, value);
    if (kind == DLG_PICK_FOLDER) app->dialog.field.placeholder = "Type to filter";
    GetCharPressed();
}

static void close_dialog(jd_app *app)
{
    free(app->dialog.ids);
    app->dialog = (jd_dialog){0};
}

static int matches(const jd_folder_row *f, const char *filter)
{
    return !filter[0] || strcasestr(f->name, filter) || strcasestr(folder_label(f), filter);
}

static int pick_count(jd_app *app)
{
    int n = 0;
    for (int i = 0; i < app->nfolders; i++) n += matches(&app->folders[i], app->dialog.field.text);
    return n;
}

static const jd_folder_row *picked(jd_app *app)
{
    for (int i = 0, k = 0; i < app->nfolders; i++)
        if (matches(&app->folders[i], app->dialog.field.text) && k++ == app->dialog.selected) return &app->folders[i];
    return NULL;
}

/* a typed folder path as the server names it: inside the personal namespace unless renaming a folder outside it */
static char *server_name(jd_app *app, const jd_folder_row *renamed, const char *typed)
{
    if (renamed && !strcmp(renamed->path, renamed->name)) return jd_strdup(typed);
    char *prefix = app->view ? jd_store_namespace(app->view) : jd_strdup("");
    char *out = jd_printf("%s%s", prefix, typed);
    free(prefix);
    return out;
}

static void confirm(jd_app *app)
{
    jd_dialog *d = &app->dialog;
    const jd_folder_row *current = app->nfolders ? &app->folders[app->folder] : NULL;
    if (d->act == ACT_LOGOUT) {
        close_dialog(app);
        app_logout(app);
        return;
    }
    if (d->act == ACT_DISCARD) {
        close_dialog(app);
        compose_discard(app);
        return;
    }
    if (d->act == ACT_SEND_LATER) {
        char *typed = jd_strdup(d->field.text);
        close_dialog(app);
        compose_schedule_typed(app, typed);
        free(typed);
        return;
    }
    if (!app->sync) {
        close_dialog(app);
        return;
    }
    switch (d->act) {
    case ACT_LOGOUT:
    case ACT_DISCARD:
    case ACT_SEND_LATER: break;
    case ACT_CREATE_FOLDER:
        if (d->field.len) {
            char *name = server_name(app, NULL, d->field.text);
            jd_sync_create_folder(app->sync, name);
            free(name);
        }
        break;
    case ACT_RENAME_FOLDER:
        if (d->field.len && current && !jd_streq(d->field.text, current->path)) {
            char *name = server_name(app, current, d->field.text);
            jd_sync_rename_folder(app->sync, current->name, name);
            free(name);
        }
        break;
    case ACT_DELETE_FOLDER:
        if (current) jd_sync_delete_folder(app->sync, current->name);
        break;
    case ACT_DESTROY:
        jd_sync_destroy(app->sync, d->ids, d->nids);
        break;
    case ACT_MOVE:
    case ACT_COPY: {
        const jd_folder_row *to = picked(app);
        if (to) {
            char *name = jd_strdup(to->name);
            int copy = d->act == ACT_COPY;
            /* ids: one message of a conversation (a reader card), else the list's selection */
            int64_t *ids = d->ids;
            int nids = d->nids;
            d->ids = NULL;
            close_dialog(app);
            if (ids) act_move_ids(app, ids, nids, name, copy, 1);
            else act_move_to(app, name, copy);
            free(name);
            return;
        }
        break;
    }
    case ACT_ADD_RULE:
    {
        char *match = search_full_query(app, 0);
        if (d->field.len && match && match[0]) {
            jd_prefs_add_rule(&app->prefs, match, d->field.text);
            save_prefs(app);
            notify_user(app, "Rule added: %s -> %s", match, d->field.text);
        }
        free(match);
        break;
    }
    case ACT_NONE: break;
    }
    close_dialog(app);
}

static void rules_draw(jd_app *app, Rectangle box)
{
    jd_dialog *d = &app->dialog;
    jd_prefs *p = &app->prefs;
    if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) d->selected++;
    if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) d->selected--;
    if (d->selected >= p->nrules) d->selected = p->nrules - 1;
    if (d->selected < 0) d->selected = 0;
    if (IsKeyPressed(KEY_D) && p->nrules) {
        free(p->rules[d->selected].match), free(p->rules[d->selected].action);
        memmove(p->rules + d->selected, p->rules + d->selected + 1, sizeof *p->rules * (size_t)(p->nrules - d->selected - 1));
        p->nrules--;
        save_prefs(app);
    }
    float x = box.x + 20, y = box.y + 52, w = box.width - 40;
    if (!p->nrules) {
        jd_text(&app->ui, "No rules yet. Search (/), then Shift+M makes the search a rule.", x, y, 14, JD_NORD4, 0);
        return;
    }
    for (int i = 0; i < p->nrules && y < box.y + box.height - 60; i++, y += 30) {
        if (i == d->selected) DrawRectangleRounded((Rectangle){x - 6, y - 4, w + 12, 26}, 0.2f, 6, JD_NORD2);
        char line[600];
        snprintf(line, sizeof line, "%d.  %s   ->   %s", i + 1, p->rules[i].match, p->rules[i].action);
        jd_text_fit(&app->ui, line, x, y, w, 14, JD_NORD6, 0);
    }
    jd_text(&app->ui, "j/k select, d deletes, Esc closes. Rules run on new Inbox mail, top to bottom.", x,
            box.y + box.height - 30, 13, JD_NORD4, 0);
}

void dialog_draw(jd_app *app)
{
    jd_dialog *d = &app->dialog;
    if (!d->kind) return;
    jd_ui *ui = &app->ui;
    if (IsKeyPressed(KEY_ESCAPE)) {
        close_dialog(app);
        return;
    }
    if (d->kind == DLG_RULES) {
        float sw = jd_screen_w(), sh = jd_screen_h();
        DrawRectangleV((Vector2){0, 0}, (Vector2){sw, sh}, (Color){0x2e, 0x34, 0x40, 0xb0});
        Rectangle box = {(sw - 720) / 2.0f, sh * 0.15f, 720, 420};
        DrawRectangleRounded(box, 0.04f, 8, JD_NORD1);
        jd_text(ui, "Rules", box.x + 20, box.y + 18, 16, JD_NORD6, 1);
        rules_draw(app, box);
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || (d->kind == DLG_CONFIRM && IsKeyPressed(KEY_Y))) {
        confirm(app);
        return;
    }
    if (d->kind != DLG_CONFIRM) {
        int before = d->field.len;
        jd_field_input(&d->field);
        if (d->field.len != before) d->selected = 0;
    }
    if (d->kind == DLG_PICK_FOLDER) {
        int n = pick_count(app);
        if (IsKeyPressed(KEY_DOWN) || (IsKeyPressed(KEY_N) && IsKeyDown(KEY_LEFT_CONTROL))) d->selected++;
        if (IsKeyPressed(KEY_UP) || (IsKeyPressed(KEY_P) && IsKeyDown(KEY_LEFT_CONTROL))) d->selected--;
        if (d->selected >= n) d->selected = n - 1;
        if (d->selected < 0) d->selected = 0;
    }

    float sw = jd_screen_w(), sh = jd_screen_h();
    DrawRectangleV((Vector2){0, 0}, (Vector2){sw, sh}, (Color){0x2e, 0x34, 0x40, 0xb0});
    float w = 520, h = d->kind == DLG_PICK_FOLDER ? 420 : d->kind == DLG_PROMPT ? 150 : 130;
    Rectangle box = {(sw - w) / 2, sh * 0.2f, w, h};
    DrawRectangleRounded(box, 0.04f, 8, JD_NORD1);
    float x = box.x + 20, y = box.y + 18;
    jd_text_fit(ui, d->title, x, y, w - 40, 16, JD_NORD6, 1);
    y += 34;
    if (d->kind == DLG_CONFIRM) {
        if (jd_button(ui, d->act == ACT_LOGOUT ? "Log out" : "Delete", (Rectangle){x, y + 10, 110, 34}, 1)) confirm(app);
        else if (jd_button(ui, "Cancel", (Rectangle){x + 122, y + 10, 110, 34}, 0)) close_dialog(app);
        return;
    }
    jd_field_draw(ui, &d->field, (Rectangle){x, y, w - 40, 34}, 1);
    y += 46;
    if (d->kind == DLG_PROMPT) {
        jd_text(ui, "Enter to confirm, Esc to cancel.", x, y, 13, JD_NORD4, 0);
        return;
    }
    for (int i = 0, k = 0; i < app->nfolders && y < box.y + box.height - 30; i++) {
        const jd_folder_row *f = &app->folders[i];
        if (!matches(f, d->field.text)) continue;
        Rectangle row = {x - 6, y - 4, w - 28, 26};
        if (k == d->selected) DrawRectangleRounded(row, 0.2f, 6, JD_NORD2);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), row)) {
            d->selected = k;
            confirm(app);
            return;
        }
        jd_text_fit(ui, f->name, x, y, w - 50, 15, JD_NORD6, 0);
        y += 28;
        k++;
    }
}
