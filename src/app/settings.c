#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/util.h"
#include "platform.h"
#include "ui/icons.h"

enum { S_ACCOUNT, S_APPEARANCE, S_FONTS, S_LAYOUT, S_READING, S_COMPOSE, S_NOTIFY, S_SYNC, S_RULES, S_FILTERS, S_KEYS, S_ABOUT, S_COUNT };
enum { FOCUS_NONE = -1, FOCUS_SIG_NEW = 100, FOCUS_SIG_REPLY, FOCUS_RULE_MATCH, FOCUS_FONT, FOCUS_KEYS };

/* what the Keyboard section is narrowed to */
static jd_field keys_filter;

/* the new rule: its match (a query, as in search), what it does (actions[]) and the folder of a move or copy */
static jd_qfield rule_match;
static int rule_act;
static char rule_folder[256];
static const struct {
    const char *label, *icon, *action;
} actions[] = {{"Mark read", "mail-open", "read"}, {"Flag", "star", "flag"}, {"Delete", "trash-2", "delete"},
               {"Move to", "folder-input", "move:"}, {"Copy to", "copy", "copy:"}};

/* A phone's width (narrow): the list of sections, then the section chosen from it (narrow_detail; Back returns
   to the list), each control under its explanation (label()) */
static int narrow, narrow_detail;

static const struct {
    const char *label, *icon;
} sections[S_COUNT] = {
    {"Account", "user"}, {"Appearance", "paintbrush"}, {"Fonts", "type"}, {"Layout", "panel-right"}, {"Reading", "mail-open"},
    {"Compose", "pencil"}, {"Notifications", "bell"}, {"Sync", "refresh-cw"}, {"Rules", "list"},
    {"Server filters", "flag"}, {"Keyboard", "keyboard"}, {"About", "circle-help"},
};

/* the font picker: which role's list is open (-1 none) and what is typed to narrow it */
static int font_open = -1;
static jd_field font_filter;
static void font_pick_first(jd_app *app);

void settings_open(jd_app *app)
{
    for (int i = 0; i < app->ntabs; i++)
        if (app->tabs[i].kind == TAB_SETTINGS) {
            app->tab = i;
            return;
        }
    tab_add(app, TAB_SETTINGS);
    form_fill(app);
    jd_editor_set(&app->sig_new, app->prefs.signature_new);
    jd_editor_set(&app->sig_reply, app->prefs.signature_reply);
    qfield_clear(&rule_match);
    rule_act = 3, rule_folder[0] = 0;
    app->settings_focus = FOCUS_NONE;
    app->settings_scroll = 0;
    narrow_detail = 0;
}

static void changed(jd_app *app)
{
    apply_prefs(app);
    save_prefs(app);
    list_reload(app);
}

static void save_signatures(jd_app *app)
{
    free(app->prefs.signature_new), free(app->prefs.signature_reply);
    app->prefs.signature_new = jd_strdup(app->sig_new.text ? app->sig_new.text : "");
    app->prefs.signature_reply = jd_strdup(app->sig_reply.text ? app->sig_reply.text : "");
    save_prefs(app);
    app->sig_new.changed = app->sig_reply.changed = 0;
    notify_user(app, "Signatures saved.");
}

static void add_rule(jd_app *app)
{
    char *match = qfield_query(&rule_match);
    int folder = rule_act >= 3;
    if (!match || (folder && !rule_folder[0])) {
        notify_user(app, !match ? "Type what the rule matches first." : "Choose the folder to %s to.",
                    rule_act == 3 ? "move" : "copy");
        free(match);
        return;
    }
    char *action = jd_printf("%s%s", actions[rule_act].action, folder ? rule_folder : "");
    jd_prefs_add_rule(&app->prefs, match, action);
    save_prefs(app);
    free(match), free(action);
    qfield_clear(&rule_match);
}

void settings_keys(jd_app *app)
{
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int f = app->settings_focus;
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (f == FOCUS_FONT) font_open = -1;
        if (f != FOCUS_NONE) app->settings_focus = FOCUS_NONE;
        else if (narrow && narrow_detail) narrow_detail = 0;
        else tab_close(app, app->tab);
        return;
    }
    if (f == FOCUS_FONT) {
        if (IsKeyPressed(KEY_ENTER)) font_pick_first(app);
        else jd_field_input(&font_filter);
        return;
    }
    if (f == FOCUS_KEYS) {
        jd_field_input(&keys_filter);
        return;
    }
    if (f == FOCUS_NONE) {
        if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_DOWN)) app->settings_section = (app->settings_section + 1) % S_COUNT;
        if (IsKeyPressed(KEY_K) || IsKeyPressed(KEY_UP)) app->settings_section = (app->settings_section + S_COUNT - 1) % S_COUNT;
        return;
    }
    if (f >= 0 && f < F_COUNT) {
        if (IsKeyPressed(KEY_TAB)) app->settings_focus = (f + 1) % F_COUNT;
        else if (IsKeyPressed(KEY_ENTER)) form_submit(app);
        else jd_field_input(&app->fields[f]);
    } else if (f == FOCUS_SIG_NEW || f == FOCUS_SIG_REPLY) {
        if (ctrl && IsKeyPressed(KEY_S)) save_signatures(app);
        else jd_editor_input(&app->ui, f == FOCUS_SIG_NEW ? &app->sig_new : &app->sig_reply);
    } else if (f == FOCUS_RULE_MATCH) {
        if (qfield_keys(app, &rule_match)) add_rule(app);
    }
}

/* the lowest point a section drew, for the scrollbar */
static float bottom;

static void reach(float y)
{
    if (y > bottom) bottom = y;
}

/* the height of the row label() drew last: taller when its explanation wraps */
static float row_h = 56;
static float narrow_w;

/* A setting's name and explanation in a column on the left of a row, the explanation wrapped to the column;
   returns the x where its control goes. When narrow the text takes the width and *y moves below it, so the
   control lands under it. */
static float label(jd_app *app, float x, float *y, const char *name, const char *hint)
{
    const float column = narrow ? narrow_w : 340;
    jd_text_fit(&app->ui, name, x, *y + 4, column, 15, JD_NORD6, 1);
    row_h = 56;
    int n = 0;
    if (hint) {
        int *starts = jd_wrap(&app->ui, hint, column, 13, &n);
        for (int i = 0; i < n; i++)
            jd_text_range(&app->ui, hint, starts[i], i + 1 < n ? starts[i + 1] : (int)strlen(hint), x, *y + 26 + (float)i * 18,
                          13, JD_MUTED, 0);
        free(starts);
        if (n > 1) row_h = 26 + (float)n * 18 + 12;
    }
    if (narrow) {
        *y += 26 + (float)n * 18 + 4;
        row_h = 50;
        reach(*y + row_h);
        return x;
    }
    reach(*y + row_h);
    return x + column + 20;
}

/* text wrapped to w from y; returns the y below it */
static float para(jd_app *app, const char *text, float x, float y, float w, float size, Color c)
{
    int n;
    int *starts = jd_wrap(&app->ui, text, w, size, &n);
    for (int i = 0; i < n; i++)
        jd_text_range(&app->ui, text, starts[i], i + 1 < n ? starts[i + 1] : (int)strlen(text), x, y + (float)i * (size + 6),
                      size, c, 0);
    free(starts);
    return y + (float)n * (size + 6);
}

/* a section's title, and its explanation wrapped to the section's width */
static float heading(jd_app *app, float x, float y, const char *title, const char *sub)
{
    jd_text(&app->ui, title, x, y, 22, JD_NORD6, 1);
    if (!sub) {
        reach(y + 50);
        return y + 50;
    }
    int n;
    int *starts = jd_wrap(&app->ui, sub, narrow_w, 14, &n);
    for (int i = 0; i < n; i++)
        jd_text_range(&app->ui, sub, starts[i], i + 1 < n ? starts[i + 1] : (int)strlen(sub), x, y + 34 + (float)i * 20, 14,
                      JD_MUTED, 0);
    free(starts);
    float end = y + 34 + (float)n * 20 + 22;
    reach(end);
    return end;
}

static float divider(float x, float y, float w)
{
    DrawRectangle((int)x, (int)y, (int)w, 1, JD_NORD1);
    reach(y + 18);
    return y + 18;
}

static int stepper(jd_app *app, float x, float y, int *value, int step, int min, int max, const char *unit)
{
    jd_ui *ui = &app->ui;
    int was = *value;
    if (jd_button(ui, "-", (Rectangle){x, y, 34, 32}, 0) && *value - step >= min) *value -= step;
    char text[32];
    snprintf(text, sizeof text, "%d%s", *value, unit);
    float tw = jd_text_width(ui, text, 15, 1);
    jd_text(ui, text, x + 34 + (90 - tw) / 2, y + 8, 15, JD_NORD6, 1);
    if (jd_button(ui, "+", (Rectangle){x + 124, y, 34, 32}, 0) && *value + step <= max) *value += step;
    return *value != was;
}

static void account(jd_app *app, float x, float y, float w)
{
    y = heading(app, x, y, "Account", plat_secret_available() ? "The password lives in the system keystore. Leave it empty to keep it."
                                                               : "No keystore: the password lasts until jackdaw quits.");
    float fw = w > 560 ? 560 : w;
    int focus = app->settings_focus;
    y = form_fields_draw(app, x, y, fw, &focus);
    reach(y + 50);
    if (focus != app->settings_focus) app->settings_focus = focus;
    if (jd_button(&app->ui, "Save and reconnect", (Rectangle){x, y, 190, 38}, 1)) {
        if (form_submit(app) == 0) {
            notify_user(app, "Account saved; reconnecting.");
        }
    }
    if (!app->have_account) return;
    y += 38 + 28;
    y = divider(x, y, w);
    y = heading(app, x, y, "Log out", plat_touch() ? "Forgets the password and the server settings, and deletes the mail kept on this device."
                                                   : "Forgets the password and the server settings, and moves the mail kept on this computer to the trash.");
    reach(y + 50);
    if (jd_button(&app->ui, "Log out", (Rectangle){x, y, 120, 38}, 0)) {
        char *title = jd_printf("Log out of %s?", app->account.address);
        dialog_open(app, DLG_CONFIRM, ACT_LOGOUT, title, NULL);
        free(title);
    }
}

static void appearance(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Appearance", NULL);
    static const char *const themes[] = {"Dark", "Light"};
    float cx = label(app, x, &y, "Theme", "Nord Polar Night or Snow Storm");
    int t = jd_segmented(&app->ui, cx, y + 6, themes, 2, p->light);
    if (t != p->light) p->light = t, changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Interface size", "Text and controls, 50 to 200 percent");
    if (stepper(app, cx, y + 6, &p->scale, 10, 50, 200, "%")) changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Message list text", "Sender and subject in the list, on top of the interface size");
    if (stepper(app, cx, y + 6, &p->list_text, 1, 8, 40, " px")) changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Message text", "The mail being read; HTML mail that sets its own sizes keeps them");
    if (stepper(app, cx, y + 6, &p->reader_text, 1, 8, 40, " px")) changed(app);
    y = divider(x, y + row_h, w);
    static const char *const densities[] = {"Comfortable", "Compact"};
    cx = label(app, x, &y, "Message list", "Compact fits one line per message");
    int d = jd_segmented(&app->ui, cx, y + 6, densities, 2, p->compact);
    if (d != p->compact) p->compact = d, changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Preview text", "The first words of each message under its subject");
    if (jd_switch(&app->ui, cx, y + 10, p->snippets)) p->snippets = !p->snippets, changed(app);
}

static char **families[2];
static int nfamilies[2];

static char **font_pref(jd_prefs *p, int role)
{
    char **slots[] = {&p->font_ui, &p->font_list, &p->font_reader, &p->font_mono};
    return slots[role];
}

static int font_matches(const char *family)
{
    return !font_filter.len || strcasestr(family, font_filter.text);
}

static void font_pick(jd_app *app, int role, const char *family)
{
    char **slot = font_pref(&app->prefs, role);
    free(*slot);
    *slot = family ? jd_strdup(family) : NULL;
    font_open = -1;
    app->settings_focus = FOCUS_NONE;
    changed(app);
}

/* Enter takes the first family the filter leaves */
static void font_pick_first(jd_app *app)
{
    int mono = font_open == 3;
    for (int i = 0; i < nfamilies[mono]; i++)
        if (font_matches(families[mono][i])) {
            font_pick(app, font_open, families[mono][i]);
            return;
        }
}

static float font_row(jd_app *app, float x, float y, float w, int role, const char *name, const char *hint)
{
    jd_ui *ui = &app->ui;
    static const char *defaults[] = {"Inter (built in)", "Inter (built in)", "Inter (built in)", "DejaVu Sans Mono"};
    const char *now = *font_pref(&app->prefs, role);
    float cx = label(app, x, &y, name, hint);
    Rectangle b = {cx, y + 4, w - (cx - x) > 360 ? 360 : w - (cx - x), 34};
    int hover = CheckCollisionPointRec(GetMousePosition(), b) && !app->dialog.kind;
    DrawRectangleRounded(b, 0.2f, 6, hover || font_open == role ? JD_NORD2 : JD_NORD1);
    const char *shown = now && now[0] ? now : defaults[role];
    jd_text_fit(ui, shown, b.x + 12, b.y + 9, b.width - 44, 15, JD_NORD6, 0);
    jd_icon(font_open == role ? "chevron-down" : "chevron-right", b.x + b.width - 26, b.y + 9, 16, JD_NORD4);
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        font_open = font_open == role ? -1 : role;
        jd_field_set(&font_filter, NULL);
        font_filter.placeholder = "Type to narrow the list";
        app->settings_focus = font_open >= 0 ? FOCUS_FONT : FOCUS_NONE;
        int mono = role == 3;
        if (font_open >= 0 && !families[mono]) families[mono] = jd_font_families(mono, &nfamilies[mono]);
    }
    if (font_open != role) return y + row_h;
    y += row_h - 10;
    if (jd_field_draw(ui, &font_filter, (Rectangle){b.x, y, b.width, 32}, app->settings_focus == FOCUS_FONT))
        app->settings_focus = FOCUS_FONT;
    y += 38;
    int mono = role == 3, shown_n = 0;
    Rectangle def = {b.x, y, b.width, 28};
    if (CheckCollisionPointRec(GetMousePosition(), def)) DrawRectangleRounded(def, 0.2f, 6, JD_NORD2);
    jd_text(ui, defaults[role], def.x + 12, y + 6, 14, JD_NORD8, 1);
    if (CheckCollisionPointRec(GetMousePosition(), def) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        font_pick(app, role, NULL);
        return y + 30;
    }
    y += 30;
    for (int i = 0; i < nfamilies[mono] && shown_n < 12; i++) {
        if (!font_matches(families[mono][i])) continue;
        Rectangle row = {b.x, y, b.width, 28};
        int on = CheckCollisionPointRec(GetMousePosition(), row);
        if (on) DrawRectangleRounded(row, 0.2f, 6, JD_NORD2);
        jd_text_fit(ui, families[mono][i], row.x + 12, y + 6, row.width - 24, 14,
                    now && !strcmp(now, families[mono][i]) ? JD_NORD8 : JD_NORD6, 0);
        if (on && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            font_pick(app, role, families[mono][i]);
            return y + 30;
        }
        y += 30;
        shown_n++;
    }
    if (!shown_n) {
        jd_text(ui, nfamilies[mono] ? "No installed family matches." : "fontconfig lists no families.", b.x + 12, y + 6, 13,
                JD_MUTED, 0);
        y += 30;
    }
    reach(y + 20);
    return y + 12;
}

static void fonts(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Fonts", "Installed TrueType and OpenType families, found through fontconfig.");
    y = divider(x, font_row(app, x, y, w, 0, "Interface", "Sidebar, toolbars, settings and dialogs"), w);
    y = divider(x, font_row(app, x, y, w, 1, "Message list", "Senders, subjects and previews"), w);
    y = divider(x, font_row(app, x, y, w, 2, "Message text", "Plain-text mail, and HTML mail that names no font"), w);
    y = divider(x, font_row(app, x, y, w, 3, "Monospace", "Plain-text mail when the switch below is on"), w);
    float cx = label(app, x, &y, "Plain-text mail in monospace", "Keeps tables and ASCII art drawn in fixed width aligned");
    if (jd_switch(&app->ui, cx, y + 10, p->mono_plain)) p->mono_plain = !p->mono_plain, changed(app);
}

static void layout(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Layout", NULL);
    static const char *const panes[] = {"Right", "Bottom", "Off"};
    float cx;
    /* on a phone messages always open on their own */
    if (!plat_touch()) {
        cx = label(app, x, &y, "Reading pane", "Where the open message shows (p); off opens messages in tabs");
        int r = jd_segmented(&app->ui, cx, y + 6, panes, 3, p->reader_pos);
        if (r != p->reader_pos) {
            p->reader_pos = r;
            if (r == 2) reader_close(app);
            changed(app);
        }
        y = divider(x, y + row_h, w);
    }
    static const char *const sides[] = {"Full", "Icons only"};
    cx = label(app, x, &y, "Sidebar", "Icons only keeps folders one click away (Ctrl+B)");
    int sr = jd_segmented(&app->ui, cx, y + 6, sides, 2, p->sidebar_rail);
    if (sr != p->sidebar_rail) p->sidebar_rail = sr, changed(app);
    y = divider(x, y + row_h, w);
    /* a phone keeps its tabs on top */
    if (!plat_touch()) {
        static const char *const tabs[] = {"Top", "Sidebar"};
        cx = label(app, x, &y, "Tabs", "Above the content (shown when more than one is open), or listed in the sidebar");
        int tr = jd_segmented(&app->ui, cx, y + 6, tabs, 2, p->tabs_side);
        if (tr != p->tabs_side) p->tabs_side = tr, changed(app);
        y = divider(x, y + row_h, w);
    }
    cx = label(app, x, &y, "Group conversations", "Replies join the message they answer, across folders (t)");
    if (jd_switch(&app->ui, cx, y + 10, p->conversations)) {
        p->conversations = !p->conversations;
        changed(app);
        reader_close(app);
    }
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Pane sizes", "Drag the edges between panes; this resets them");
    if (jd_button(&app->ui, "Reset", (Rectangle){cx, y + 4, 100, 32}, 0)) {
        p->sidebar_width = p->list_width = p->list_height = 0;
        changed(app);
    }
}

static void reading(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Reading", NULL);
    static const char *const formats[] = {"HTML", "Plain text"};
    float cx = label(app, x, &y, "Show mail as", "HTML when the sender wrote it, or always plain (Shift+P switches)");
    int f = jd_segmented(&app->ui, cx, y + 6, formats, 2, p->prefer_plain);
    if (f != p->prefer_plain) p->prefer_plain = f, changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Mark as read when opened", "Otherwise only u marks it read");
    if (jd_switch(&app->ui, cx, y + 10, p->mark_read)) p->mark_read = !p->mark_read, changed(app);
    y = divider(x, y + row_h, w);
    static const char *const remote[] = {"Ask", "Always load"};
    cx = label(app, x, &y, "Remote images", "Loading them tells the sender when you read, and your IP address");
    int ri = jd_segmented(&app->ui, cx, y + 6, remote, 2, p->remote_images);
    if (ri != p->remote_images) p->remote_images = ri, changed(app);
    y = divider(x, y + row_h, w);
    jd_text(&app->ui, "Senders whose images always load", x, y + 4, 15, JD_NORD6, 1);
    y += 32;
    int any = 0;
    char *copy = jd_strdup(p->image_senders ? p->image_senders : ""), *save = NULL;
    for (char *a = strtok_r(copy, ", ", &save); a; a = strtok_r(NULL, ", ", &save), y += 36) {
        any = 1;
        Rectangle row = {x, y, w > 560 ? 560 : w, 32};
        DrawRectangleRounded(row, 0.2f, 6, JD_NORD1);
        jd_text_fit(&app->ui, a, x + 12, y + 9, row.width - 60, 14, JD_NORD6, 0);
        Rectangle del = {x + row.width - 30, y + 6, 22, 22};
        int hover = CheckCollisionPointRec(GetMousePosition(), del);
        jd_icon("x", del.x + 3, del.y + 3, 16, hover ? JD_NORD11 : JD_MUTED);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            char *gone = jd_strdup(a);
            jd_prefs_trust_images(p, gone, 0);
            free(gone);
            save_prefs(app);
            break;
        }
    }
    free(copy);
    if (!any) y = para(app, "None yet: \"Always from this sender\" on a message adds one.", x, y, w, 13, JD_MUTED);
    reach(y + 40);
}

static void compose(jd_app *app, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Compose", NULL);
    float cx = label(app, x, &y, "Format with Markdown", "**bold**, *italic*, lists, links and > quotes also go out as HTML");
    if (jd_switch(ui, cx, y + 10, p->markdown)) p->markdown = !p->markdown, changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "List attachments in the message", "A numbered \"[1] name\" list below what you write");
    if (jd_switch(ui, cx, y + 10, p->attachment_list)) p->attachment_list = !p->attachment_list, changed(app);
    y = divider(x, y + row_h, w);
    y = para(app, "Signatures go below what you write, after a \"-- \" line (RFC 3676).", x, y, w, 14, JD_MUTED) + 14;
    float fw = w > 640 ? 640 : w;
    jd_text(ui, "New messages", x, y, 15, JD_NORD6, 1);
    if (jd_editor_draw(ui, &app->sig_new, (Rectangle){x, y + 24, fw, 110}, app->settings_focus == FOCUS_SIG_NEW))
        app->settings_focus = FOCUS_SIG_NEW;
    y += 150;
    jd_text(ui, "Replies and forwards", x, y, 15, JD_NORD6, 1);
    if (jd_editor_draw(ui, &app->sig_reply, (Rectangle){x, y + 24, fw, 110}, app->settings_focus == FOCUS_SIG_REPLY))
        app->settings_focus = FOCUS_SIG_REPLY;
    y += 150;
    reach(y + 50);
    int dirty = app->sig_new.changed || app->sig_reply.changed;
    if (jd_button(ui, dirty ? "Save signatures" : "Saved", (Rectangle){x, y, 170, 36}, dirty)) save_signatures(app);
}

static void notifications(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Notifications", NULL);
    float cx = label(app, x, &y, "Desktop notifications", "For new unread mail in the Inbox, after rules ran");
    if (jd_switch(&app->ui, cx, y + 10, p->notify)) p->notify = !p->notify, changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Also while jackdaw is in front", "Off: only when another window has the focus");
    if (jd_switch(&app->ui, cx, y + 10, p->notify_focused)) p->notify_focused = !p->notify_focused, changed(app);
    y = divider(x, y + row_h, w);
    if (plat_touch()) {
        cx = label(app, x, &y, "Instant (push)",
                    "Keeps listening while jackdaw is closed, as Thunderbird does. Android shows a \"Listening for new "
                    "mail\" notification; turn its \"Push\" channel off in Android's settings to hide it. Off: a check "
                    "about every 15 minutes");
        if (jd_switch(&app->ui, cx, y + 10, p->push)) p->push = !p->push, changed(app);
        y = divider(x, y + row_h, w);
    }
    if (jd_button(&app->ui, "Send a test notification", (Rectangle){x, y, 230, 34}, 0)) {
        if (plat_notify(JD_APP_NAME, "Notifications work.", 0) == 0) notify_user(app, "Sent to the notification service.");
        else notify_user(app, "No notification service on the session bus.");
    }
}

static void about(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    char built[96];
    snprintf(built, sizeof built, "%s %s, built %s", JD_APP_NAME, jd_version, jd_build_date);
    y = heading(app, x, y, "About", built);
    if (!strcmp(JD_APP_NAME, "jackdaw")) {
        float cx = label(app, x, &y, "Check for updates automatically", "Once a day, from the latest GitHub release");
        if (jd_switch(&app->ui, cx, y + 10, p->update_check)) p->update_check = !p->update_check, changed(app);
        y = divider(x, y + row_h, w);
        if (jd_button(&app->ui, update_busy() ? "Checking..." : "Check for updates", (Rectangle){x, y, 200, 34}, 0) &&
            !update_busy())
            update_check(app, 1);
        y += 46;
        if (update_status()[0]) y = para(app, update_status(), x, y, w, 14, JD_NORD4);
    } else {
        y = para(app, "A development build: it does not update itself.", x, y, w, 14, JD_MUTED);
    }
    reach(y + 20);
}

static void sync(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Sync", "The Inbox updates at once (IMAP IDLE); other folders on a timer.");
    float cx = label(app, x, &y, "Keep recent mail offline", "Inbox messages this many days back are downloaded whole");
    if (stepper(app, cx, y + 6, &p->prefetch_days, 7, 0, 365, " days")) changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Check other folders", "How often every folder is synced");
    if (stepper(app, cx, y + 6, &p->check_minutes, 1, 1, 120, " min")) changed(app);
    y = divider(x, y + row_h, w);
    if (jd_button(&app->ui, "Sync now", (Rectangle){x, y, 130, 34}, 1) && app->sync) jd_sync_now(app->sync);
}

static void rules(jd_app *app, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Rules", "Run by jackdaw on new Inbox mail, top to bottom. They match as the search box does.");
    /* each rule: its match as pills, then what it does ("Move to Receipts") */
    for (int i = 0; i < p->nrules; i++) {
        const char *a = p->rules[i].action;
        char does[300];
        if (!strncmp(a, "move:", 5)) snprintf(does, sizeof does, "Move to %s", a + 5);
        else if (!strncmp(a, "copy:", 5)) snprintf(does, sizeof does, "Copy to %s", a + 5);
        else if (!strcmp(a, "read")) snprintf(does, sizeof does, "Mark read");
        else if (!strcmp(a, "flag")) snprintf(does, sizeof does, "Flag");
        else if (!strcmp(a, "delete")) snprintf(does, sizeof does, "Delete");
        else snprintf(does, sizeof does, "%s", a);
        float rh = narrow ? 66 : 42, pw = narrow ? w - 50 : w * 0.6f;
        Rectangle row = {x, y, w, rh};
        DrawRectangleRounded(row, 0.2f, 6, JD_NORD1);
        query_pills_draw(app, p->rules[i].match, x + 10, y + 9, pw - 10);
        float ax = narrow ? x + 10 : x + w * 0.6f + 10, ay = narrow ? y + 38 : y + 13;
        jd_icon("chevron-right", ax, ay, 16, JD_MUTED);
        jd_text_fit(ui, does, ax + 22, ay, x + w - 44 - ax - 22, 14, JD_NORD8, 1);
        Rectangle del = {x + w - 34, y + (rh - 24) / 2, 24, 24};
        int hover = CheckCollisionPointRec(GetMousePosition(), del);
        jd_icon("trash-2", del.x + 4, del.y + 4, 16, hover ? JD_NORD11 : JD_MUTED);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            free(p->rules[i].match), free(p->rules[i].action);
            memmove(p->rules + i, p->rules + i + 1, sizeof *p->rules * (size_t)(p->nrules - i - 1));
            p->nrules--;
            save_prefs(app);
            return;
        }
        y += rh + 6;
    }
    if (!p->nrules) {
        jd_text(ui, "No rules yet.", x, y, 14, JD_MUTED, 0);
        y += 30;
    }

    /* a new rule: what it matches (a pill field with the search box's hints), what it does, and where to */
    y += 10;
    jd_text(ui, "New rule", x, y, 15, JD_NORD6, 1);
    y += 28;
    int focused = app->settings_focus == FOCUS_RULE_MATCH;
    Vector2 m = GetMousePosition();
    int blocked = focused && CheckCollisionPointRec(m, rule_match.hints_box);
    jd_text(ui, "Matches", x, y, 13, JD_NORD4, 1);
    y += 20;
    Rectangle field = {x, y, w, 36};
    /* a press anywhere else puts the field (and its hints) away */
    if (focused && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(m, field) && !blocked) {
        app->settings_focus = FOCUS_NONE;
        focused = 0;
    }
    if (qfield_draw(app, &rule_match, field, focused) && !blocked) app->settings_focus = FOCUS_RULE_MATCH;
    y += 46;
    jd_text(ui, "Then", x, y, 13, JD_NORD4, 1);
    y += 20;
    /* chips in rows: the actions, then (move, copy) the folders */
    float cx = x;
    for (int k = 0; k < (int)(sizeof actions / sizeof *actions); k++) {
        float cw = jd_text_width(ui, actions[k].label, 13, 1) + 42;
        if (cx > x && cx + cw > x + w) cx = x, y += 34;
        Rectangle c = {cx, y, cw, 28};
        int on = rule_act == k, hover = CheckCollisionPointRec(m, c) && !blocked && !app->dialog.kind;
        DrawRectangleRounded(c, 0.5f, 6, on ? JD_NORD10 : hover ? JD_NORD2 : JD_NORD1);
        if (!on) DrawRectangleRoundedLinesEx(c, 0.5f, 6, 1, JD_NORD2);
        jd_icon(actions[k].icon, cx + 11, y + 7, 14, on ? JD_ON_ACCENT : JD_NORD4);
        jd_text(ui, actions[k].label, cx + 31, y + 7, 13, on ? JD_ON_ACCENT : JD_NORD6, 1);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) rule_act = k;
        cx += cw + 6;
    }
    y += 40;
    if (rule_act >= 3) {
        jd_text(ui, "Folder", x, y, 13, JD_NORD4, 1);
        y += 20;
        cx = x;
        for (int i = 0; i < app->nfolders; i++) {
            const char *name = app->folders[i].name;
            if (!strcasecmp(name, "INBOX")) continue;
            const char *shown = folder_label(&app->folders[i]);
            float cw = jd_text_width(ui, shown, 13, 0) + 40;
            if (cx > x && cx + cw > x + w) cx = x, y += 34;
            Rectangle c = {cx, y, cw, 28};
            int on = !strcmp(rule_folder, name), hover = CheckCollisionPointRec(m, c) && !blocked && !app->dialog.kind;
            DrawRectangleRounded(c, 0.5f, 6, on ? JD_NORD10 : hover ? JD_NORD2 : JD_NORD1);
            if (!on) DrawRectangleRoundedLinesEx(c, 0.5f, 6, 1, JD_NORD2);
            jd_icon("folder", cx + 10, y + 7, 14, on ? JD_ON_ACCENT : JD_NORD8);
            jd_text(ui, shown, cx + 30, y + 7, 13, on ? JD_ON_ACCENT : JD_NORD6, 0);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) snprintf(rule_folder, sizeof rule_folder, "%s", name);
            cx += cw + 6;
        }
        y += 40;
    }
    Rectangle add = {x, y + 4, 120, 36};
    reach(add.y + 60);
    if (!blocked && jd_icon_button(ui, "check", "Add rule", add, 1)) add_rule(app);
    qfield_hints_draw(app, &rule_match, focused);
}

static void server_filters(jd_app *app, float x, float y, float w)
{
    y = heading(app, x, y, "Server filters", "Sieve scripts (RFC 5228) run by the server, even while jackdaw is closed.");
    char line[96];
    snprintf(line, sizeof line, "ManageSieve on %s, port %d.", app->account.imap.host ? app->account.imap.host : "the IMAP host",
             app->account.sieve_port ? app->account.sieve_port : 4190);
    y = para(app, line, x, y, w, 14, JD_NORD4);
    reach(y + 60);
    if (jd_button(&app->ui, "Edit server filters", (Rectangle){x, y + 14, 200, 36}, 1)) filters_open(app);
}

void settings_draw(jd_app *app, Rectangle r)
{
    jd_ui *ui = &app->ui;
    /* too narrow for the list beside the section (a phone): one, then the other */
    narrow = r.width < 640;
    float nav_w = narrow ? r.width : 230;
    if (!narrow || !narrow_detail) {
        DrawRectangleRec((Rectangle){r.x, r.y, nav_w, r.height}, JD_NORD1);
        /* on a phone the title is the way back out of Settings */
        Rectangle head = {r.x, r.y, nav_w, 56};
        int back = plat_touch() && CheckCollisionPointRec(GetMousePosition(), head) && !app->dialog.kind;
        if (back) DrawRectangleRec(head, JD_NORD2);
        jd_icon(plat_touch() ? "chevron-left" : "settings", r.x + 20, r.y + 22, 20, JD_NORD8);
        jd_text(ui, "Settings", r.x + 50, r.y + 22, 18, JD_NORD6, 1);
        if (back && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            tab_close(app, app->tab);
            return;
        }
        float y = r.y + 64, rh = narrow ? 48 : 36;
        for (int i = 0; i < S_COUNT; i++) {
            /* no keyboard on a phone to list keys for */
            if (i == S_KEYS && plat_touch()) continue;
            Rectangle row = {r.x + 8, y, nav_w - 16, rh - 4};
            int hover = CheckCollisionPointRec(GetMousePosition(), row) && !app->dialog.kind;
            int current = i == app->settings_section && !narrow;
            if (current) DrawRectangleRounded(row, 0.25f, 6, JD_NORD2);
            else if (hover) DrawRectangleRounded(row, 0.25f, 6, ColorAlpha(JD_NORD2, 0.5f));
            float ty = y + (rh - 4 - 16) / 2;
            jd_icon(sections[i].icon, row.x + 12, ty, 16, current ? JD_NORD8 : JD_NORD4);
            jd_text(ui, sections[i].label, row.x + 40, ty, narrow ? 15 : 14, JD_NORD6, current);
            if (narrow) jd_icon("chevron-right", row.x + row.width - 28, ty, 16, JD_MUTED);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                app->settings_section = i;
                app->settings_focus = FOCUS_NONE;
                app->settings_scroll = 0;
                narrow_detail = 1;
            }
            y += rh;
        }
        if (narrow) return;
    }

    Rectangle content = {r.x + nav_w, r.y, r.width - nav_w, r.height};
    if (narrow) {
        /* the way back to the list */
        Rectangle back = {r.x, r.y, r.width, 44};
        int hover = CheckCollisionPointRec(GetMousePosition(), back) && !app->dialog.kind;
        DrawRectangleRec(back, hover ? JD_NORD2 : JD_NORD1);
        jd_icon("chevron-left", r.x + 14, r.y + 14, 16, JD_NORD8);
        jd_text(ui, "Settings", r.x + 38, r.y + 14, 15, JD_NORD8, 1);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            narrow_detail = 0;
            app->settings_focus = FOCUS_NONE;
            return;
        }
        content = (Rectangle){r.x, r.y + 44, r.width, r.height - 44};
    }
    /* clamped before drawing, by last frame's height, so no frame is drawn past the end */
    static float last_height;
    if (CheckCollisionPointRec(GetMousePosition(), content)) app->settings_scroll -= jd_wheel_y(40);
    if (last_height > 0 && app->settings_scroll > last_height - content.height) app->settings_scroll = last_height - content.height;
    if (app->settings_scroll < 0) app->settings_scroll = 0;
    float pad = narrow ? 16 : 48;
    float w = content.width - 2 * pad;
    if (w > 820) w = 820;
    narrow_w = w;
    float x = content.x + pad, top = content.y + (narrow ? 16 : 32) - app->settings_scroll;
    bottom = 0;
    jd_scissor(content.x, content.y, content.width, content.height);
    jd_follow_begin();
    switch (app->settings_section) {
    case S_ACCOUNT: account(app, x, top, w); break;
    case S_APPEARANCE: appearance(app, x, top, w); break;
    case S_FONTS: fonts(app, x, top, w); break;
    case S_LAYOUT: layout(app, x, top, w); break;
    case S_READING: reading(app, x, top, w); break;
    case S_COMPOSE: compose(app, x, top, w); break;
    case S_NOTIFY: notifications(app, x, top, w); break;
    case S_SYNC: sync(app, x, top, w); break;
    case S_RULES: rules(app, x, top, w); break;
    case S_FILTERS: server_filters(app, x, top, w); break;
    case S_ABOUT: about(app, x, top, w); break;
    case S_KEYS:
        heading(app, x, top, "Keyboard", "Also on ? anywhere.");
        keys_filter.placeholder = "Type to find a key or an action";
        if (jd_field_draw(ui, &keys_filter, (Rectangle){x, top + 76, w, 34}, app->settings_focus == FOCUS_KEYS))
            app->settings_focus = FOCUS_KEYS;
        reach(help_content(app, x, top + 124, w, keys_filter.text) + 20);
        break;
    }
    jd_scissor_end();
    last_height = bottom + app->settings_scroll - content.y + 20;
    jd_follow_end(&app->settings_scroll, content);
    jd_scrollbar(content, last_height, &app->settings_scroll);
}
