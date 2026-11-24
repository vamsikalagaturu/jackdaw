#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "core/toml.h"
#include "core/util.h"
#include "mail/mime.h"
#include "mail/send.h"
#include "platform.h"
#include "ui/icons.h"

enum { S_ACCOUNT, S_APPEARANCE, S_FONTS, S_LAYOUT, S_READING, S_COMPOSE, S_NOTIFY, S_SYNC, S_CALENDAR, S_RULES, S_FILTERS, S_KEYS, S_ABOUT, S_COUNT };
enum { FOCUS_NONE = -1, FOCUS_SIG_NEW = 100, FOCUS_SIG_REPLY, FOCUS_RULE_MATCH, FOCUS_FONT, FOCUS_KEYS };

/* what the Keyboard section is narrowed to */
static jd_field keys_filter;

/* the new rule: its match (a query, as in search), what it does (actions[]) and the folder of a move or copy; or,
   rule_editing >= 0, that rule loaded to be changed in its place */
static jd_qfield rule_match;
static int rule_act, rule_editing = -1;
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
    {"Compose", "pencil"}, {"Notifications", "bell"}, {"Sync", "refresh-cw"}, {"Calendar", "calendar"}, {"Rules", "list"},
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

void settings_show_calendar(jd_app *app)
{
    settings_open(app);
    app->settings_section = S_CALENDAR;
    app->settings_focus = FOCUS_NONE;
    app->settings_scroll = 0;
    narrow_detail = 1;
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
    jd_prefs *p = &app->prefs;
    if (rule_editing >= 0 && rule_editing < p->nrules) {
        free(p->rules[rule_editing].match), free(p->rules[rule_editing].action);
        p->rules[rule_editing].match = match, p->rules[rule_editing].action = action;
        match = action = NULL;
    } else {
        jd_prefs_add_rule(p, match, action);
    }
    save_prefs(app);
    free(match), free(action);
    qfield_clear(&rule_match);
    rule_editing = -1;
}

static void rule_edit_end(void)
{
    qfield_clear(&rule_match);
    rule_editing = -1, rule_act = 0, rule_folder[0] = 0;
}

/* rule i into the form, its action and folder chosen there */
static void rule_edit(jd_app *app, int i)
{
    const jd_rule *r = &app->prefs.rules[i];
    qfield_set(&rule_match, r->match);
    rule_act = 0, rule_folder[0] = 0;
    for (int k = 0; k < (int)(sizeof actions / sizeof *actions); k++) {
        size_t n = strlen(actions[k].action);
        if (actions[k].action[n - 1] == ':' ? !strncmp(r->action, actions[k].action, n) : !strcmp(r->action, actions[k].action)) {
            rule_act = k;
            if (actions[k].action[n - 1] == ':') snprintf(rule_folder, sizeof rule_folder, "%s", r->action + n);
        }
    }
    rule_editing = i;
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
    if (calendar_settings_keys(app)) return;
    if (vacation_keys(app, &app->settings_focus)) return;
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
/* What the search bar finds in Settings: each entry by the name its section shows, with words it is looked for by.
   find_here, called where an entry is drawn, scrolls the one asked for to the top and tints it a moment. */
static const struct {
    int section;
    const char *name, *words;
} entries[] = {
    {S_ACCOUNT, "Accounts", "account add switch mail server imap smtp password"},
    {S_ACCOUNT, "Log out", "logout sign out forget account"},
    {S_APPEARANCE, "Theme", "dark light system colours colors nord"},
    {S_APPEARANCE, "Interface size", "scale zoom bigger smaller"},
    {S_APPEARANCE, "Message list text", "list font size"},
    {S_APPEARANCE, "Message text", "reader font size reading"},
    {S_APPEARANCE, "Message list", "density compact comfortable"},
    {S_APPEARANCE, "Preview text", "snippets"},
    {S_APPEARANCE, "Animations and effects", "effects animation motion paper plane burn blur"},
    {S_FONTS, "Interface", "font family"},
    {S_FONTS, "Monospace", "font fixed width"},
    {S_FONTS, "Plain-text mail in monospace", "font fixed width ascii"},
    {S_LAYOUT, "Reading pane", "preview right bottom off"},
    {S_LAYOUT, "Sidebar", "rail icons folders"},
    {S_LAYOUT, "Tabs", "tab bar"},
    {S_LAYOUT, "Group conversations", "threads conversation"},
    {S_LAYOUT, "Pane sizes", "reset width"},
    {S_READING, "Show mail as", "html plain text"},
    {S_READING, "Mark as read when opened", "read unread"},
    {S_READING, "Remote images", "images pictures tracking privacy"},
    {S_COMPOSE, "Format with Markdown", "markdown html bold"},
    {S_COMPOSE, "List attachments in the message", "attachments"},
    {S_COMPOSE, "Undo send", "undo delay send"},
    {S_COMPOSE, "Signatures", "signature"},
    {S_NOTIFY, "Desktop notifications", "notify new mail alert"},
    {S_NOTIFY, "Also while jackdaw is in front", "focused notifications"},
    {S_NOTIFY, "Instant (push)", "push"},
    {S_NOTIFY, "Check for updates automatically", "update release version"},
    {S_SYNC, "Keep recent mail offline", "offline prefetch download"},
    {S_SYNC, "Check other folders", "sync interval minutes"},
    {S_SYNC, "Share settings with my other jackdaws", "share sync devices"},
    {S_CALENDAR, "Calendar", "caldav google calendars account"},
    {S_CALENDAR, "Opens in", "calendar view week month day agenda"},
    {S_CALENDAR, "Check every", "calendar sync interval"},
    {S_CALENDAR, "Reminders", "calendar alarm notification"},
    {S_RULES, "Rules", "filter move mark rule"},
    {S_FILTERS, "Server filters", "sieve filters"},
    {S_FILTERS, "Vacation reply", "away out of office autoreply holiday"},
    {S_KEYS, "Keyboard", "keys shortcuts"},
    {S_ABOUT, "About", "version update"},
};
enum { NENTRIES = sizeof entries / sizeof *entries };
static const char *find_name;  /* the entry to scroll to */
static const char *flash_name; /* the entry tinted, since flash_at */
static double flash_at;
static float content_top, content_w;

int settings_find(const char *text, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < NENTRIES && n < max; i++)
        if (strcasestr(entries[i].name, text) || strcasestr(entries[i].words, text) ||
            strcasestr(sections[entries[i].section].label, text))
            out[n++] = i;
    return n;
}

const char *settings_entry(int i, const char **section)
{
    *section = sections[entries[i].section].label;
    return entries[i].name;
}

void settings_goto(jd_app *app, int i)
{
    settings_open(app);
    app->settings_section = entries[i].section;
    app->settings_focus = FOCUS_NONE;
    narrow_detail = 1;
    app->settings_scroll = 0;
    find_name = flash_name = entries[i].name;
    flash_at = GetTime();
}

static void find_here(jd_app *app, const char *name, float x, float y)
{
    if (find_name && !strcmp(find_name, name)) {
        app->settings_scroll += y - (content_top + 24);
        if (app->settings_scroll < 0) app->settings_scroll = 0;
        find_name = NULL;
    }
    float u = (float)(GetTime() - flash_at) / 1.6f;
    if (flash_name && !strcmp(flash_name, name) && u < 1)
        DrawRectangleRounded((Rectangle){x - 12, y - 8, content_w + 24, 60}, 0.15f, 6, ColorAlpha(JD_NORD10, 0.35f * (1 - u)));
}

static float label(jd_app *app, float x, float *y, const char *name, const char *hint)
{
    find_here(app, name, x, *y);
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
static void find_here(jd_app *app, const char *name, float x, float y);

static float heading(jd_app *app, float x, float y, const char *title, const char *sub)
{
    find_here(app, title, x, y);
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
    jd_ui *ui = &app->ui;
    float fw = w > 560 ? 560 : w;
    if (app->have_account) {
        /* every account: the open one marked, the others to open; then one more */
        y = heading(app, x, y, "Accounts", "Each syncs in the background; one is open at a time (also from the top of the sidebar).");
        int n = accounts_count(app);
        for (int i = 0; i < n; i++) {
            const jd_account *a = account_at(app, i);
            const jd_slot *s = app->nslots ? &app->slots[i] : NULL;
            int open = i == app->cur;
            Rectangle row = {x, y, fw, 44};
            DrawRectangleRounded(row, 0.2f, 6, open ? JD_NORD2 : JD_NORD1);
            jd_icon(open ? "check" : "mail", x + 12, y + 14, 16, open ? JD_NORD8 : JD_NORD4);
            char line[400];
            snprintf(line, sizeof line, "%s%s", a->address ? a->address : "?", s && s->refused ? "  (sign in)" : "");
            jd_text_fit(ui, line, x + 38, y + 14, fw - 38 - 110, 14, JD_NORD6, open);
            if (open) jd_text(ui, "Open", x + fw - 60, y + 15, 13, JD_MUTED, 0);
            else if (jd_button(ui, "Open", (Rectangle){x + fw - 86, y + 6, 78, 32}, 0) && !app->dialog.kind) {
                account_switch(app, i);
                return;
            }
            y += 50;
        }
        if (jd_icon_button(ui, "user", "Add an account", (Rectangle){x, y + 4, 180, 36}, 0) && !app->dialog.kind) {
            account_add_open(app);
            return;
        }
        y = divider(x, y + 56, w);
        reach(y);
    }
    char head[300];
    snprintf(head, sizeof head, "%s", app->have_account && app->account.address ? app->account.address : "Account");
    y = heading(app, x, y, head, plat_secret_available() ? "The open account's servers. The password lives in the system keystore; leave it empty to keep it."
                                                          : "No keystore: the password lasts until jackdaw quits.");
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
    /* System follows the desktop theme's colors.toml, which only a Linux desktop has */
    static const char *const themes[] = {"Dark", "Light", "System"};
    float cx = label(app, x, &y, "Theme", "Nord, or the desktop theme's colours; [theme] in settings.toml overrides");
    int t = jd_segmented(&app->ui, cx, y + 6, themes, plat_touch() ? 2 : 3, p->scheme);
    if (t != p->scheme) p->scheme = t, changed(app);
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
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Animations and effects", "Sent mail flies off as a paper plane, deleted mail burns, dialogs float over a frosted page");
    if (jd_switch(&app->ui, cx, y + 10, p->effects)) p->effects = !p->effects, changed(app);
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
    return !font_filter.len || strcasestr(family, jd_field_text(&font_filter));
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
    cx = label(app, x, &y, "Sidebar", plat_touch() ? "Icons only keeps folders one tap away" : "Icons only keeps folders one click away (Ctrl+B)");
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
    float cx = label(app, x, &y, "Show mail as",
                     plat_touch() ? "HTML when the sender wrote it, or always plain" : "HTML when the sender wrote it, or always plain (Shift+P switches)");
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
    cx = label(app, x, &y, "Undo send", "A sent message waits this long, with Undo, before it goes");
    static const char *const waits[] = {"Off", "5 s", "10 s", "20 s", "30 s"};
    static const int wait_s[] = {0, 5, 10, 20, 30};
    int cur = 2;
    for (int i = 0; i < 5; i++)
        if (wait_s[i] == p->undo_seconds) cur = i;
    int pick = jd_segmented(ui, cx, y + 6, waits, 5, cur);
    if (pick != cur) p->undo_seconds = wait_s[pick], changed(app);
    y = divider(x, y + row_h, w);
    find_here(app, "Signatures", x, y);
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
        if (jd_button(&app->ui, update_checking() ? "Checking..." : "Check for updates", (Rectangle){x, y, 200, 34}, 0) &&
            !update_checking())
            update_check(app, 1);
        y += 46;
        if (update_status()[0]) y = para(app, update_status(), x, y, w, 14, JD_NORD4) + 12;
        y += update_action(app, x, y, w);
    } else {
        y = para(app, "A development build: it does not update itself.", x, y, w, 14, JD_MUTED);
    }
    reach(y + 20);
}

static float devices_draw(jd_app *app, float x, float y, float w);

static void sync_section(jd_app *app, float x, float y, float w)
{
    jd_prefs *p = &app->prefs;
    y = heading(app, x, y, "Sync", "The Inbox updates at once (IMAP IDLE); other folders on a timer.");
    float cx = label(app, x, &y, "Keep recent mail offline", "Inbox messages this many days back are downloaded whole");
    if (stepper(app, cx, y + 6, &p->prefetch_days, 7, 0, 365, " days")) changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Check other folders", "How often every folder is synced");
    if (stepper(app, cx, y + 6, &p->check_minutes, 1, 1, 120, " min")) changed(app);
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Share settings with my other jackdaws",
               "Shared: theme, reading, notifications, compose, signatures, rules and calendars, through the folder "
               "\"jackdaw\" on the mail server.\n"
               "Per device: layout, sizes, fonts, push and sync timers; passwords and sign-ins never leave it.");
    if (jd_switch(&app->ui, cx, y + 10, p->share)) {
        p->share = !p->share;
        changed(app);
    }
    y = divider(x, y + row_h, w);
    if (p->share) y = devices_draw(app, x, y, w);
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
        DrawRectangleRounded(row, 0.2f, 6, i == rule_editing ? ColorAlpha(JD_NORD10, 0.3f) : JD_NORD1);
        query_pills_draw(app, p->rules[i].match, x + 10, y + 9, pw - 10);
        float ax = narrow ? x + 10 : x + w * 0.6f + 10, ay = narrow ? y + 38 : y + 13;
        jd_icon("chevron-right", ax, ay, 16, JD_MUTED);
        jd_text_fit(ui, does, ax + 22, ay, x + w - 72 - ax - 22, 14, JD_NORD8, 1);
        Rectangle edit = {x + w - 62, y + (rh - 24) / 2, 24, 24}, del = {x + w - 34, y + (rh - 24) / 2, 24, 24};
        int on_edit = CheckCollisionPointRec(GetMousePosition(), edit), hover = CheckCollisionPointRec(GetMousePosition(), del);
        jd_icon("pencil", edit.x + 4, edit.y + 4, 16, on_edit || i == rule_editing ? JD_NORD8 : JD_MUTED);
        jd_icon("trash-2", del.x + 4, del.y + 4, 16, hover ? JD_NORD11 : JD_MUTED);
        if (on_edit && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            rule_edit(app, i);
            return;
        }
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            free(p->rules[i].match), free(p->rules[i].action);
            memmove(p->rules + i, p->rules + i + 1, sizeof *p->rules * (size_t)(p->nrules - i - 1));
            p->nrules--;
            if (rule_editing == i) rule_edit_end();
            else if (rule_editing > i) rule_editing--;
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
    char form_title[32];
    snprintf(form_title, sizeof form_title, rule_editing >= 0 ? "Edit rule %d" : "New rule", rule_editing + 1);
    jd_text(ui, form_title, x, y, 15, JD_NORD6, 1);
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
    if (!blocked && jd_icon_button(ui, "check", rule_editing >= 0 ? "Save" : "Add rule", add, 1)) add_rule(app);
    else if (rule_editing >= 0 && !blocked && jd_icon_button(ui, "x", "Cancel", (Rectangle){x + 130, y + 4, 110, 36}, 0))
        rule_edit_end();
    qfield_hints_draw(app, &rule_match, focused);
}

static void calendar(jd_app *app, float x, float y, float w)
{
    y = heading(app, x, y, "Calendar",
                "A CalDAV server (Nextcloud, Fastmail, iCloud, mailbox.org, Radicale...) or Google Calendar. "
                "The calendar opens from the sidebar or with g c; invitations in mail are answered from the message.");
    int focus = app->settings_focus;
    y = calendar_settings(app, x, y, w, &focus);
    if (focus != app->settings_focus) app->settings_focus = focus;
    reach(y + 20);
    if (!app->prefs.ncals) return;
    y = divider(x, y, w);
    /* [view] calendar: the segments in the order shown, and the views they are (jd_prefs.cal_view) */
    static const char *const views[] = {"Day", "Week", "Month", "Agenda"};
    static const int view_of[] = {1, 2, 3, 0};
    int cv = app->prefs.cal_view >= 0 ? app->prefs.cal_view : plat_touch() ? 0 : 2, cur = 0;
    for (int i = 0; i < 4; i++)
        if (view_of[i] == cv) cur = i;
    float cx = label(app, x, &y, "Opens in", "The view the calendar shows when it opens");
    int v = jd_segmented(&app->ui, cx, y + 6, views, 4, cur);
    if (v != cur) {
        app->prefs.cal_view = view_of[v];
        save_prefs(app);
    }
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Check every", "How often the calendars are read again; changes made here go at once");
    /* one setting for every account */
    int minutes = app->prefs.cals[0].check_minutes;
    if (stepper(app, cx, y + 6, &minutes, 5, 5, 240, " min")) {
        for (int i = 0; i < app->prefs.ncals; i++) app->prefs.cals[i].check_minutes = minutes;
        save_prefs(app);
        calendar_start(app);
    }
    y = divider(x, y + row_h, w);
    cx = label(app, x, &y, "Reminders",
               plat_touch() ? "A notification at each event's reminder, also while jackdaw is closed"
                            : "A notification at each event's reminder, while jackdaw runs");
    if (jd_switch(&app->ui, cx, y + 10, app->prefs.cal_reminders)) {
        app->prefs.cal_reminders = !app->prefs.cal_reminders;
        save_prefs(app);
    }
}

static void vacation(jd_app *app, float x, float y, float w);

static void server_filters(jd_app *app, float x, float y, float w)
{
    y = heading(app, x, y, "Server filters", "Sieve scripts (RFC 5228) run by the server, even while jackdaw is closed.");
    char line[96];
    snprintf(line, sizeof line, "ManageSieve on %s, port %d.", app->account.imap.host ? app->account.imap.host : "the IMAP host",
             app->account.sieve_port ? app->account.sieve_port : 4190);
    y = para(app, line, x, y, w, 14, JD_NORD4);
    if (jd_button(&app->ui, "Edit server filters", (Rectangle){x, y + 14, 200, 36}, 1)) filters_open(app);
    y = divider(x, y + 64, w);
    vacation(app, x, y, w);
}

/* the vacation reply's form: on and off, the dates, the subject, the reply and how often */
static void vacation(jd_app *app, float x, float y, float w)
{
    jd_ui *ui = &app->ui;
    jd_vacation_form *v = vacation_form(app);
    int *focus = &app->settings_focus;
    float cx = label(app, x, &y, "Vacation reply", "Answers mail on the server while you are away, even while jackdaw is closed");
    if (v->state <= 1) {
        jd_text_fit(ui, v->state < 0 ? v->message : "Reading the server's filters...", cx, y + 18, w - (cx - x), 14,
                    v->state < 0 ? JD_NORD11 : JD_NORD4, 0);
        reach(y + row_h);
        return;
    }
    if (jd_switch(ui, cx, y + 10, v->on)) v->on = !v->on;
    y = divider(x, y + row_h, w);
    float fw = w - (cx - x) > 420 ? 420 : w - (cx - x);
    if (v->on) {
        cx = label(app, x, &y, "From and until", "Empty: from now, until it is turned off. Written 2026-12-24");
        float half = (fw - 10) / 2;
        v->f[VF_FROM].placeholder = "From now";
        v->f[VF_UNTIL].placeholder = "Until turned off";
        if (jd_field_draw(ui, &v->f[VF_FROM], (Rectangle){cx, y + 6, half, 34}, *focus == FOCUS_VAC + VF_FROM))
            *focus = FOCUS_VAC + VF_FROM;
        if (jd_field_draw(ui, &v->f[VF_UNTIL], (Rectangle){cx + half + 10, y + 6, half, 34}, *focus == FOCUS_VAC + VF_UNTIL))
            *focus = FOCUS_VAC + VF_UNTIL;
        y = divider(x, y + row_h, w);
        cx = label(app, x, &y, "Subject", "Empty: \"Auto:\" and the subject of their message");
        v->f[VF_SUBJECT].placeholder = "Away until 6 January";
        if (jd_field_draw(ui, &v->f[VF_SUBJECT], (Rectangle){cx, y + 6, fw, 34}, *focus == FOCUS_VAC + VF_SUBJECT))
            *focus = FOCUS_VAC + VF_SUBJECT;
        y = divider(x, y + row_h, w);
        cx = label(app, x, &y, "Reply", "What each sender gets back");
        if (jd_editor_draw(ui, &v->text, (Rectangle){cx, y + 6, fw, 120}, *focus == FOCUS_VAC + VF_COUNT))
            *focus = FOCUS_VAC + VF_COUNT;
        if (!v->text.len && *focus != FOCUS_VAC + VF_COUNT) jd_text(ui, "I am away until...", cx + 10, y + 15, 14, JD_MUTED, 0);
        reach(y + 136);
        y = divider(x, y + (row_h > 136 ? row_h : 136), w);
        cx = label(app, x, &y, "To each sender", "Once in this many days, however often they write");
        stepper(app, cx, y + 6, &v->days, 1, 1, 30, " days");
        y = divider(x, y + row_h, w);
    }
    int failed = v->message[0] && strncmp(v->message, "Saved", 5) != 0 && strncmp(v->message, "Checking", 8) != 0;
    if (jd_button(ui, v->saving ? "Saving..." : "Save vacation reply", (Rectangle){x, y, 190, 36}, 1)) vacation_save(app);
    jd_text_fit(ui, v->message, x + 204, y + 11, w - 204, 13, failed ? JD_NORD11 : JD_NORD4, 0);
    reach(y + 50);
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
    content_top = content.y, content_w = w;
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
    case S_SYNC: sync_section(app, x, top, w); break;
    case S_CALENDAR: calendar(app, x, top, w); break;
    case S_RULES: rules(app, x, top, w); break;
    case S_FILTERS: server_filters(app, x, top, w); break;
    case S_ABOUT: about(app, x, top, w); break;
    case S_KEYS:
        heading(app, x, top, "Keyboard", "Also on ? anywhere.");
        keys_filter.placeholder = "Type to find a key or an action";
        if (jd_field_draw(ui, &keys_filter, (Rectangle){x, top + 76, w, 34}, app->settings_focus == FOCUS_KEYS))
            app->settings_focus = FOCUS_KEYS;
        reach(help_content(app, x, top + 124, w, jd_field_text(&keys_filter)) + 20);
        break;
    }
    jd_scissor_end();
    last_height = bottom + app->settings_scroll - content.y + 20;
    jd_follow_end(&app->settings_scroll, content);
    jd_scrollbar(content, last_height, &app->settings_scroll);
}

/* ---- shared with the account's other jackdaws ([sync] settings) ----

   The server holds one copy (jd_sync_put_settings): each JD_SHARE_* group with the time it last changed. The
   account folder's shared-settings.toml holds the copy last agreed with, and the time of each change made here
   that no server copy has yet (pending), so an edit made offline survives a restart. A group changed on both sides
   goes to the newer change. The copy also lists the devices sharing it ([[device]]), each writing itself in when
   absent or a day old. */

enum { SEEN_EVERY = 24 * 3600 };

typedef struct {
    char *id, *name;
    int64_t seen; /* when it last wrote the copy */
} share_device;

static struct {
    char *dir;       /* the account's, whose base is loaded; NULL: none loaded */
    jd_store *view;  /* and the store it was loaded with: a new one (signed in again) loads it again */
    int have_base;   /* agreed with a server copy, or chose to begin from this device's */
    char *agreed[JD_SHARE_COUNT];
    int64_t stamp[JD_SHARE_COUNT];
    int64_t pending[JD_SHARE_COUNT];
    jd_toml *agreed_t;
    char *local[JD_SHARE_COUNT]; /* the groups here, as last looked at */
    uint32_t merged_uid;          /* the server copy last merged (0 none) */
    uint32_t pushed_for;          /* a copy is on its way, merged with this server copy (UINT32_MAX: none) */
    int64_t fetching;             /* the copy whose body was asked for */
    int asking, asked;            /* the first look, with a server copy and no base: which to begin from */
    char *me;                     /* this device's id, kept in the base */
    share_device *devices;        /* as the copy last merged lists them */
    int ndevices;
    int devices_dirty;            /* one was forgotten here: the copy is written again without it */
} sh = {.pushed_for = UINT32_MAX};

static void devices_free(void)
{
    for (int i = 0; i < sh.ndevices; i++) free(sh.devices[i].id), free(sh.devices[i].name);
    free(sh.devices);
    sh.devices = NULL, sh.ndevices = 0;
}

static void devices_read(const jd_toml *t)
{
    devices_free();
    int n = jd_toml_count(t, "device");
    sh.devices = jd_alloc(sizeof *sh.devices * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        char table[32];
        snprintf(table, sizeof table, "device.%d", i);
        const char *id = jd_toml_str(t, table, "id");
        long long seen = 0;
        jd_toml_int(t, table, "seen", &seen);
        if (id) sh.devices[sh.ndevices++] = (share_device){jd_strdup(id), jd_strdup(jd_toml_str(t, table, "name")), seen};
    }
}

static share_device *device_me(void)
{
    for (int i = 0; i < sh.ndevices; i++)
        if (jd_streq(sh.devices[i].id, sh.me)) return &sh.devices[i];
    return NULL;
}

/* this device missing from the copy, or written there more than a day ago */
static int me_stale(void)
{
    share_device *d = device_me();
    return !d || jd_now() - d->seen > SEEN_EVERY;
}

static char *device_name(void)
{
#ifdef __ANDROID__
    return jd_strdup("Android");
#else
    char host[256] = "";
    if (gethostname(host, sizeof host - 1) != 0 || !host[0]) return jd_strdup("Computer");
    return jd_strdup(host);
#endif
}

static char *new_device_id(void)
{
    unsigned char r[16] = {0};
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(r, 1, sizeof r, f) : 0;
    if (f) fclose(f);
    if (got != sizeof r) {
        int64_t seed[2] = {jd_now(), (int64_t)getpid()};
        memcpy(r, seed, sizeof r);
    }
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", r[i]);
    return jd_strdup(hex);
}

/* the first account's store, worker, folder and account, open or in the background: its folder carries the copy */
static struct {
    jd_store *view;
    jd_sync *sync;
    const char *dir;
    const jd_account *account;
} first;

static char *base_path(jd_app *app)
{
    (void)app;
    return jd_printf("%s/shared-settings.toml", first.dir);
}

static char *copy_text(jd_app *app, const jd_toml *t, int g)
{
    if (g == JD_SHARE_CHOICES) return calendar_choices_text(app, t, NULL);
    jd_prefs p = {0};
    jd_share_apply(&p, g, t);
    char *s = jd_share_text(&p, g);
    jd_prefs_free(&p);
    return s;
}

static char *local_text(jd_app *app, int g)
{
    if (g == JD_SHARE_CHOICES) return calendar_choices_text(app, NULL, sh.agreed_t);
    return jd_share_text(&app->prefs, g);
}

static int64_t stamp_of(const jd_toml *t, const char *table, int g)
{
    long long v;
    return t && jd_toml_int(t, table, jd_share_names[g], &v) == 0 ? (int64_t)v : 0;
}

/* The server's copy (base 0) or the base: stamps and the groups' texts (both left out of a base not agreed yet),
   the base's pending changes and this device's id, and the devices, this one written in as of now in a copy */
static char *copy_doc(int base, int64_t *stamps, char **texts)
{
    jd_buf b = {0};
    jd_buf_adds(&b, "# jackdaw: the settings shared by this account's jackdaws (Settings > Sync)\n");
    if (base) {
        jd_buf_adds(&b, "\n[this_device]\nid = ");
        jd_toml_quote(&b, sh.me);
        jd_buf_adds(&b, "\n");
    }
    if (stamps) {
        jd_buf_adds(&b, "\n[stamps]\n");
        for (int g = 0; g < JD_SHARE_COUNT; g++) jd_buf_addf(&b, "%s = %lld\n", jd_share_names[g], (long long)stamps[g]);
    }
    if (base && stamps) {
        jd_buf_adds(&b, "\n[pending]\n");
        for (int g = 0; g < JD_SHARE_COUNT; g++)
            if (sh.pending[g]) jd_buf_addf(&b, "%s = %lld\n", jd_share_names[g], (long long)sh.pending[g]);
    }
    for (int g = 0; stamps && g < JD_SHARE_COUNT; g++) jd_buf_adds(&b, texts[g] ? texts[g] : "");
    int me_listed = 0;
    for (int i = 0; i <= sh.ndevices; i++) {
        share_device me = {sh.me, NULL, 0};
        const share_device *d = i < sh.ndevices ? &sh.devices[i] : &me;
        if (i == sh.ndevices && me_listed) break;
        int mine = jd_streq(d->id, sh.me);
        me_listed |= mine;
        char *name = mine ? device_name() : jd_strdup(d->name ? d->name : "");
        jd_buf_adds(&b, "\n[[device]]\nid = ");
        jd_toml_quote(&b, d->id);
        jd_buf_adds(&b, "\nname = ");
        jd_toml_quote(&b, name);
        jd_buf_addf(&b, "\nseen = %lld\n", (long long)(mine && !base ? jd_now() : d->seen));
        free(name);
    }
    return b.s;
}

static void save_base(jd_app *app)
{
    char *doc = copy_doc(1, sh.have_base ? sh.stamp : NULL, sh.agreed);
    char *path = base_path(app);
    jd_write_file(path, doc, strlen(doc), 0600);
    free(path), free(doc);
}

static void set_text(char **slot, char *text)
{
    free(*slot);
    *slot = text;
}

static void forget_base(void)
{
    for (int g = 0; g < JD_SHARE_COUNT; g++) free(sh.agreed[g]), free(sh.local[g]);
    jd_toml_free(sh.agreed_t);
    free(sh.dir), free(sh.me);
    devices_free();
    sh = (typeof(sh)){.pushed_for = UINT32_MAX};
}

static void load_base(jd_app *app)
{
    if (sh.dir && jd_streq(sh.dir, first.dir) && sh.view == first.view) return;
    forget_base();
    sh.dir = jd_strdup(first.dir);
    sh.view = first.view;
    char *path = base_path(app);
    jd_toml *t = jd_toml_load(path, NULL, 0);
    free(path);
    long long v;
    if (t && jd_toml_int(t, "stamps", jd_share_names[0], &v) == 0) {
        sh.have_base = 1;
        for (int g = 0; g < JD_SHARE_COUNT; g++) {
            sh.stamp[g] = stamp_of(t, "stamps", g);
            sh.pending[g] = stamp_of(t, "pending", g);
            sh.agreed[g] = copy_text(app, t, g);
        }
    }
    sh.me = jd_strdup(t ? jd_toml_str(t, "this_device", "id") : NULL);
    if (t) devices_read(t);
    int fresh = !sh.me;
    if (fresh) sh.me = new_device_id();
    if (sh.have_base) sh.agreed_t = t;
    else jd_toml_free(t);
    if (fresh) save_base(app);
    /* changed while jackdaw was closed (by hand in settings.toml): as of the file's last write */
    struct stat st;
    int64_t written = stat(jd_settings_path(), &st) == 0 ? (int64_t)st.st_mtime : jd_now();
    for (int g = 0; g < JD_SHARE_COUNT; g++) {
        sh.local[g] = local_text(app, g);
        if (sh.have_base && !sh.pending[g] && strcmp(sh.local[g], sh.agreed[g]) != 0) sh.pending[g] = written;
    }
}

static int share_on(jd_app *app)
{
    if (!app->prefs.share || !app->have_account) return 0;
    account_first(app, &first.view, &first.sync, &first.dir, &first.account);
    return first.view && first.dir;
}

/* the server copy's text, NULL when not here yet (asked for) or there is none (*uid 0) */
static char *server_copy(jd_app *app, uint32_t *uid)
{
    *uid = 0;
    int64_t folder = jd_store_settings_folder(first.view, NULL);
    if (!folder) return NULL;
    int n;
    uint32_t *uids = jd_folder_uids(first.view, folder, &n);
    if (n) *uid = uids[n - 1];
    free(uids);
    if (!*uid) return NULL;
    jd_folder_state st;
    int64_t id = jd_message_id(first.view, folder, *uid);
    char *raw = NULL;
    size_t len = 0;
    if (id && jd_folder_state_get(first.view, folder, &st) == 0) {
        char *path = jd_body_path(first.dir, folder, st.uidvalidity, *uid);
        raw = jd_read_file(path, &len);
        free(path);
    }
    if (!raw) {
        if (id && sh.fetching != id && first.sync) jd_sync_fetch_body(first.sync, id), sh.fetching = id;
        return NULL;
    }
    (void)app;
    char *text = jd_mime_body_text(raw, len);
    free(raw);
    return text;
}

static void push(jd_app *app, uint32_t merged_with)
{
    if (!first.sync || sh.pushed_for == merged_with) return;
    (void)app;
    int64_t stamps[JD_SHARE_COUNT];
    for (int g = 0; g < JD_SHARE_COUNT; g++) stamps[g] = sh.pending[g] ? sh.pending[g] : sh.stamp[g];
    char *doc = copy_doc(0, stamps, sh.local);
    char err[200], **rcpts = NULL;
    int nrcpts = 0;
    size_t len;
    char *msg = jd_message_build(first.account,
                                 &(jd_outgoing){.to = first.account->address, .subject = "jackdaw settings", .body = doc},
                                 &len, &rcpts, &nrcpts, err, sizeof err);
    jd_rcpts_free(rcpts, nrcpts);
    if (msg) {
        jd_sync_put_settings(first.sync, msg, len, merged_with);
        sh.pushed_for = merged_with;
    }
    free(msg), free(doc);
}

void share_check(jd_app *app)
{
    if (!share_on(app)) return;
    load_base(app);
    uint32_t uid;
    char *text = server_copy(app, &uid);
    if (!text && uid) return; /* on its way */
    int pending = 0;
    for (int g = 0; g < JD_SHARE_COUNT; g++) pending |= sh.pending[g] != 0;
    if (uid == sh.merged_uid && sh.have_base && !pending && !sh.devices_dirty && (!me_stale() || sh.pushed_for == uid)) {
        free(text);
        return;
    }
    jd_toml *rt = text ? jd_toml_parse(text, NULL, 0) : NULL;
    free(text);
    if (!sh.have_base) {
        /* this device's first look: its own copy came back (it shared first), or another jackdaw's is there */
        int mine = rt != NULL;
        for (int g = 0; rt && g < JD_SHARE_COUNT && mine; g++) {
            char *theirs = copy_text(app, rt, g);
            mine = strcmp(theirs, sh.local[g]) == 0;
            free(theirs);
        }
        if (rt && !mine) {
            if (!sh.asked && !app->dialog.kind) {
                sh.asked = sh.asking = 1;
                dialog_open(app, DLG_CONFIRM, ACT_USE_SHARED,
                            "Use the settings your other jackdaw shares?", NULL);
            }
            jd_toml_free(rt);
            return;
        }
        if (!rt) {
            /* offered as the first copy; the server takes it only while it has none (else it is asked above) */
            for (int g = 0; g < JD_SHARE_COUNT; g++) sh.pending[g] = jd_now();
            push(app, 0);
            return;
        }
        sh.have_base = 1;
        for (int g = 0; g < JD_SHARE_COUNT; g++) set_text(&sh.agreed[g], jd_strdup(""));
    }
    int prefs_changed = 0;
    for (int g = 0; rt && g < JD_SHARE_COUNT; g++) {
        char *theirs = copy_text(app, rt, g);
        int64_t when = stamp_of(rt, "stamps", g);
        int take = 0;
        if (sh.pending[g]) {
            if (strcmp(theirs, sh.local[g]) == 0) sh.pending[g] = 0;          /* this device's change is in */
            else if (when > sh.pending[g]) take = 1, sh.pending[g] = 0;       /* a later change there */
        } else if (strcmp(theirs, sh.local[g]) != 0) {
            take = 1;
        }
        if (!sh.pending[g]) sh.stamp[g] = when, set_text(&sh.agreed[g], jd_strdup(theirs));
        if (take) {
            if (g == JD_SHARE_CHOICES) calendar_choices_apply(app, rt);
            else if (g == JD_SHARE_CALENDARS) calendar_accounts_apply(app, rt), prefs_changed = 1;
            else jd_share_apply(&app->prefs, g, rt), prefs_changed = 1;
            set_text(&sh.local[g], local_text(app, g));
        }
        free(theirs);
    }
    if (prefs_changed) {
        apply_prefs(app);
        settings_write(app);
    }
    /* a forgotten device stays out until it writes itself in again */
    if (rt && (uid != sh.merged_uid || !sh.devices_dirty)) devices_read(rt), sh.devices_dirty = 0;
    jd_toml_free(sh.agreed_t);
    sh.agreed_t = rt ? rt : NULL;
    sh.merged_uid = uid;
    save_base(app);
    pending = 0;
    for (int g = 0; g < JD_SHARE_COUNT; g++) pending |= sh.pending[g] != 0;
    if (pending || me_stale() || sh.devices_dirty) push(app, uid);
}

/* "now", "12 min ago", "5 h ago", "3 days ago" */
static void ago(int64_t when, char *out, size_t n)
{
    int64_t d = jd_now() - when;
    if (!when) snprintf(out, n, "not yet");
    else if (d < 120) snprintf(out, n, "now");
    else if (d < 3600) snprintf(out, n, "%lld min ago", (long long)(d / 60));
    else if (d < 2 * 86400) snprintf(out, n, "%lld h ago", (long long)(d / 3600));
    else snprintf(out, n, "%lld days ago", (long long)(d / 86400));
}

/* Settings > Sync: the devices the copy lists, each with when it last wrote it; the y below */
static float devices_draw(jd_app *app, float x, float y, float w)
{
    if (!share_on(app)) return y;
    load_base(app);
    jd_ui *ui = &app->ui;
    int others = 0;
    for (int i = 0; i < sh.ndevices; i++) others += !jd_streq(sh.devices[i].id, sh.me);
    char head[80];
    snprintf(head, sizeof head, others ? "In sync on %d devices" : "Only this device so far", others + 1);
    jd_text(ui, head, x, y + 4, 15, JD_NORD6, 1);
    jd_text(ui, "Each device writes itself in when it shares, at least once a day.", x, y + 26, 13, JD_MUTED, 0);
    y += 56;
    int listed_me = 0;
    for (int i = 0; i <= sh.ndevices; i++) {
        if (i == sh.ndevices && listed_me) break;
        share_device me = {sh.me, NULL, 0};
        const share_device *d = i < sh.ndevices ? &sh.devices[i] : &me;
        int mine = jd_streq(d->id, sh.me);
        listed_me |= mine;
        char *host = mine ? device_name() : NULL;
        char when[48], line[400];
        ago(d->seen, when, sizeof when);
        const char *name = mine ? host : d->name && d->name[0] ? d->name : "A device";
        snprintf(line, sizeof line, "%s%s", name, mine ? "  (this device)" : "");
        Rectangle row = {x, y, w, 40};
        DrawRectangleRounded(row, 0.2f, 6, JD_NORD1);
        jd_icon(jd_streq(name, "Android") ? "smartphone" : "monitor", x + 12, y + 12, 16, JD_NORD8);
        jd_text_fit(ui, line, x + 38, y + 11, w - 38 - 240, 14, JD_NORD6, mine);
        snprintf(line, sizeof line, "Last shared %s", when);
        jd_text_fit(ui, line, x + w - 230, y + 12, mine ? 220 : 130, 13, JD_MUTED, 0);
        free(host);
        if (!mine && jd_button(ui, "Forget", (Rectangle){x + w - 86, y + 5, 78, 30}, 0) && !app->dialog.kind) {
            char *id = jd_strdup(d->id);
            share_forget_device(app, id);
            free(id);
            break;
        }
        y += 46;
    }
    reach(y);
    return y + 8;
}

void share_forget_device(jd_app *app, const char *id)
{
    if (!share_on(app) || jd_streq(id, sh.me)) return;
    for (int i = 0; i < sh.ndevices; i++)
        if (jd_streq(sh.devices[i].id, id)) {
            free(sh.devices[i].id), free(sh.devices[i].name);
            memmove(sh.devices + i, sh.devices + i + 1, sizeof *sh.devices * (size_t)(sh.ndevices - i - 1));
            sh.ndevices--;
            break;
        }
    sh.devices_dirty = 1;
    sh.pushed_for = UINT32_MAX;
    save_base(app);
    share_check(app);
}

void share_changed(jd_app *app)
{
    if (!share_on(app)) return;
    load_base(app);
    int any = 0;
    for (int g = 0; g < JD_SHARE_COUNT; g++) {
        char *now = local_text(app, g);
        if (strcmp(now, sh.local[g]) != 0) {
            any = 1;
            sh.pending[g] = sh.have_base && strcmp(now, sh.agreed[g]) == 0 ? 0 : jd_now();
        }
        set_text(&sh.local[g], now);
    }
    if (!any) return;
    sh.pushed_for = UINT32_MAX;
    save_base(app);
    share_check(app);
}

void share_calendars_found(jd_app *app)
{
    if (!share_on(app)) return;
    load_base(app);
    if (!sh.agreed_t || sh.pending[JD_SHARE_CHOICES]) return;
    char *now = local_text(app, JD_SHARE_CHOICES);
    if (strcmp(now, sh.agreed[JD_SHARE_CHOICES]) != 0) {
        calendar_choices_apply(app, sh.agreed_t);
        free(now);
        now = local_text(app, JD_SHARE_CHOICES);
    }
    set_text(&sh.local[JD_SHARE_CHOICES], now);
}

/* the first look's answer: 1 the other jackdaw's settings, 0 this device's */
void share_decided(jd_app *app, int theirs)
{
    sh.asking = 0;
    if (!share_on(app)) return;
    load_base(app);
    sh.have_base = 1;
    for (int g = 0; g < JD_SHARE_COUNT; g++) {
        set_text(&sh.agreed[g], jd_strdup(""));
        sh.stamp[g] = 0;
        sh.pending[g] = theirs ? 0 : jd_now();
    }
    sh.merged_uid = 0;
    share_check(app);
}
