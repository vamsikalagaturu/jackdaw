#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "ui/fx.h"
#include "ui/icons.h"

void dialog_open(jd_app *app, dialog_kind kind, action act, const char *title, const char *value)
{
    free(app->dialog.ids);
    jd_field_free(&app->dialog.field);
    app->dialog = (jd_dialog){.kind = kind, .act = act};
    snprintf(app->dialog.title, sizeof app->dialog.title, "%s", title);
    jd_field_set(&app->dialog.field, value);
    if (kind == DLG_PICK_FOLDER) app->dialog.field.placeholder = "Type to filter";
    GetCharPressed();
}

static void close_dialog(jd_app *app)
{
    free(app->dialog.ids);
    jd_field_free(&app->dialog.field);
    app->dialog = (jd_dialog){0};
}

/* The picker's rows: with nothing typed, the tree as the sidebar shows it (a closed folder's folders left out);
   typed, every folder whose path as shown (Inbox, not the server's INBOX. before each) has it */
static int matches(jd_app *app, int i, const char *filter)
{
    const jd_folder_row *f = &app->folders[i];
    if (!filter[0]) return !folder_hidden(app, i);
    return strcasestr(f->path, filter) || strcasestr(folder_label(f), filter);
}

static int pick_count(jd_app *app)
{
    int n = 0;
    for (int i = 0; i < app->nfolders; i++) n += matches(app, i, jd_field_text(&app->dialog.field));
    return n;
}

static const jd_folder_row *picked(jd_app *app)
{
    for (int i = 0, k = 0; i < app->nfolders; i++)
        if (matches(app, i, jd_field_text(&app->dialog.field)) && k++ == app->dialog.selected) return &app->folders[i];
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
        char *typed = jd_strdup(jd_field_text(&d->field));
        close_dialog(app);
        compose_schedule_typed(app, typed);
        free(typed);
        return;
    }
    if (d->act == ACT_DELETE_EVENT) {
        close_dialog(app);
        calendar_delete_confirmed(app);
        return;
    }
    if (d->act == ACT_USE_SHARED) {
        close_dialog(app);
        share_decided(app, 1);
        return;
    }
    if (d->act == ACT_SNOOZE_AT) {
        char *typed = jd_strdup(jd_field_text(&d->field));
        close_dialog(app);
        snooze_typed(app, typed);
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
    case ACT_SEND_LATER:
    case ACT_DELETE_EVENT:
    case ACT_USE_SHARED:
    case ACT_SNOOZE_AT: break;
    case ACT_CREATE_FOLDER:
        if (d->field.len) {
            char *name = server_name(app, NULL, jd_field_text(&d->field));
            jd_sync_create_folder(app->sync, name);
            free(name);
        }
        break;
    case ACT_RENAME_FOLDER:
        if (d->field.len && current && !jd_streq(jd_field_text(&d->field), current->path)) {
            char *name = server_name(app, current, jd_field_text(&d->field));
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
    case ACT_LEAVE_OUT: {
        const jd_folder_row *to = picked(app);
        if (to && !folder_left_out(app, to)) {
            char *name = jd_strdup(to->name);
            close_dialog(app);
            folder_toggle_new(app, name);
            free(name);
            return;
        }
        break;
    }
    case ACT_ADD_RULE:
    {
        char *match = search_full_query(app, 0);
        if (d->field.len && match && match[0]) {
            jd_prefs_add_rule(&app->prefs, match, jd_field_text(&d->field));
            save_prefs(app);
            notify_user(app, "Rule added: %s -> %s", match, jd_field_text(&d->field));
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

/* the folder picker's list: how far it is scrolled, and the selection it last scrolled to */
static float pick_scroll;
static int pick_followed = -1;

void dialog_draw(jd_app *app)
{
    jd_dialog *d = &app->dialog;
    static dialog_kind shown_kind;
    static double opened;
    if (!d->kind) {
        shown_kind = 0;
        return;
    }
    if (d->kind != shown_kind) shown_kind = d->kind, opened = GetTime(), pick_scroll = 0, pick_followed = -1;
    /* it rises into place as it opens: 10 px over 180 ms, easing out */
    float t = fx_enabled() ? fminf(1, (float)(GetTime() - opened) / 0.18f) : 1, rise = 1 - (1 - t) * (1 - t) * (1 - t);
    jd_ui *ui = &app->ui;
    if (IsKeyPressed(KEY_ESCAPE)) {
        close_dialog(app);
        return;
    }
    /* a phone has no Esc: a tap outside the box cancels, past the tap that opened it; the shared settings'
       question is not one to cancel */
    int tap_away = plat_touch() && GetTime() - opened > 0.25 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                   d->act != ACT_USE_SHARED;
    if (d->kind == DLG_RULES) {
        float sw = jd_screen_w(), sh = jd_screen_h();
        fx_backdrop((Color){0x2e, 0x34, 0x40, 0xb0});
        float rw = sw - 32 < 720 ? sw - 32 : 720, rh = sh - 32 < 420 ? sh - 32 : 420;
        Rectangle box = {(sw - rw) / 2.0f, (sh - rh) / 2 + 10 * (1 - rise), rw, rh};
        if (tap_away && !CheckCollisionPointRec(GetMousePosition(), box)) {
            close_dialog(app);
            return;
        }
        fx_shadow(box, 0.04f, rise, 1);
        DrawRectangleRounded(box, 0.04f, 8, JD_NORD1);
        DrawRectangleRoundedLinesEx(box, 0.04f, 8, 1, JD_NORD2);
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
    /* the tree (nothing typed): Right opens the selected folder, Left closes it */
    if (d->kind == DLG_PICK_FOLDER && !d->field.len && (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_LEFT))) {
        for (int i = 0, k = 0; i < app->nfolders; i++) {
            if (!matches(app, i, "")) continue;
            if (k++ != d->selected) continue;
            if (folder_has_children(app, i) && app->folders[i].collapsed == IsKeyPressed(KEY_RIGHT)) toggle_collapsed(app, i);
            break;
        }
    }
    if (d->kind == DLG_PICK_FOLDER) {
        int n = pick_count(app);
        if (IsKeyPressed(KEY_DOWN) || (IsKeyPressed(KEY_N) && IsKeyDown(KEY_LEFT_CONTROL))) d->selected++;
        if (IsKeyPressed(KEY_UP) || (IsKeyPressed(KEY_P) && IsKeyDown(KEY_LEFT_CONTROL))) d->selected--;
        if (d->selected >= n) d->selected = n - 1;
        if (d->selected < 0) d->selected = 0;
    }

    float sw = jd_screen_w(), sh = jd_screen_h();
    fx_backdrop((Color){0x2e, 0x34, 0x40, 0xb0});
    /* a phone's screen is narrower than the box; 22 px of room on every side of what it holds */
    float w = sw - 32 < 520 ? sw - 32 : 520;
    float h = d->kind == DLG_PICK_FOLDER ? fminf(640, sh - 64) : d->kind == DLG_PROMPT ? (plat_touch() ? 158 : 140) : 120;
    if (h > sh - 32) h = sh - 32;
    Rectangle box = {(sw - w) / 2, (sh - h) / 2 + 10 * (1 - rise), w, h};
    if (tap_away && !CheckCollisionPointRec(GetMousePosition(), box)) {
        close_dialog(app);
        return;
    }
    fx_shadow(box, 0.04f, rise, 1);
    DrawRectangleRounded(box, 0.04f, 8, JD_NORD1);
    DrawRectangleRoundedLinesEx(box, 0.04f, 8, 1, JD_NORD2);
    float x = box.x + 22, y = box.y + 22;
    jd_text_fit(ui, d->title, x, y, w - 44, 16, JD_NORD6, 1);
    y += 34;
    if (d->kind == DLG_CONFIRM && d->act == ACT_USE_SHARED) {
        if (jd_button(ui, "Use those", (Rectangle){x, y + 8, 130, 34}, 1)) confirm(app);
        else if (jd_button(ui, "Keep this device's", (Rectangle){x + 142, y + 8, 170, 34}, 0)) {
            close_dialog(app);
            share_decided(app, 0);
        }
        return;
    }
    if (d->kind == DLG_CONFIRM) {
        if (jd_button(ui, d->act == ACT_LOGOUT ? "Log out" : "Delete", (Rectangle){x, y + 8, 110, 34}, 1)) confirm(app);
        else if (jd_button(ui, "Cancel", (Rectangle){x + 122, y + 8, 110, 34}, 0)) close_dialog(app);
        return;
    }
    jd_field_draw(ui, &d->field, (Rectangle){x, y, w - 44, 34}, 1);
    y += 46;
    if (d->kind == DLG_PROMPT && plat_touch()) {
        if (jd_button(ui, "OK", (Rectangle){x, y, 110, 34}, 1)) confirm(app);
        else if (jd_button(ui, "Cancel", (Rectangle){x + 122, y, 110, 34}, 0)) close_dialog(app);
        return;
    }
    if (d->kind == DLG_PROMPT) {
        jd_text(ui, "Enter to confirm, Esc to cancel.", x, y, 13, JD_NORD4, 0);
        return;
    }
    /* As the sidebar names them: the folder's icon, the folders it is in (muted), then its own name. The list
       scrolls (wheel, scrollbar), and follows the selection when keys move it. */
    Rectangle list = {box.x + 8, y - 6, w - 16, box.y + box.height - 14 - (y - 6)};
    float content = (float)pick_count(app) * 28 + 8;
    if (d->selected != pick_followed) {
        float sy = (float)d->selected * 28;
        if (sy < pick_scroll) pick_scroll = sy;
        if (sy + 36 > pick_scroll + list.height) pick_scroll = sy + 36 - list.height;
        pick_followed = d->selected;
    }
    int on_list = CheckCollisionPointRec(GetMousePosition(), list);
    if (on_list) pick_scroll -= jd_wheel_y(28 * 3);
    if (pick_scroll > content - list.height) pick_scroll = content - list.height;
    if (pick_scroll < 0) pick_scroll = 0;
    float bar = content > list.height ? 10 : 0;
    /* nothing typed: the tree, each folder indented under the one it is in, an arrow opening or closing it */
    int tree = !d->field.len;
    jd_scissor(list.x, list.y, list.width, list.height);
    for (int i = 0, k = 0; i < app->nfolders; i++) {
        jd_folder_row *f = &app->folders[i];
        if (!matches(app, i, jd_field_text(&d->field))) continue;
        y = list.y + 10 + (float)k * 28 - pick_scroll;
        if (y + 24 < list.y || y - 4 > list.y + list.height) {
            k++;
            continue;
        }
        Rectangle row = {x - 6, y - 4, w - 32 - bar, 26};
        int sel = k == d->selected;
        if (sel) DrawRectangleRounded(row, 0.2f, 6, JD_NORD2);
        float ix = tree ? x + 18 + 16 * (float)folder_depth(f) : x;
        if (tree && folder_has_children(app, i)) {
            Rectangle arrow = {ix - 20, y - 4, 20, 26};
            int on_arrow = on_list && CheckCollisionPointRec(GetMousePosition(), arrow);
            jd_icon(f->collapsed ? "chevron-right" : "chevron-down", arrow.x + 3, y + 2, 14, on_arrow ? JD_NORD8 : JD_NORD4);
            if (on_arrow && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                toggle_collapsed(app, i);
                jd_scissor_end();
                return;
            }
        }
        if (on_list && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), row)) {
            d->selected = k;
            jd_scissor_end();
            confirm(app);
            return;
        }
        jd_icon(folder_icon(f), ix, y + 1, 16, sel ? JD_NORD8 : JD_NORD4);
        const char *leaf = folder_label(f);
        float tx = ix + 26, room = x + w - 56 - tx;
        /* typed: the folders it is in before its name (folder_label is the path's last part, except INBOX's) */
        if (!tree && f->delim && strcasecmp(f->name, "INBOX") && leaf != f->path) {
            char parents[256];
            int n = 0;
            for (const char *s = f->path; s < leaf - 1 && n < (int)sizeof parents - 4; s++) {
                if (*s == f->delim) parents[n++] = ' ', parents[n++] = '/', parents[n++] = ' ';
                else parents[n++] = *s;
            }
            snprintf(parents + n, sizeof parents - (size_t)n, " / ");
            float pw = fminf(jd_text_width(ui, parents, 15, 0), room * 0.6f);
            jd_text_fit(ui, parents, tx, y, pw, 15, JD_MUTED, 0);
            tx += pw, room -= pw;
        }
        jd_text_fit(ui, leaf, tx, y, room - bar, 15, JD_NORD6, sel);
        k++;
    }
    jd_scissor_end();
    jd_scrollbar(list, content, &pick_scroll);
}
