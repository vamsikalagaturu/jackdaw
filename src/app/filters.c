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

/* ---- the vacation reply (RFC 5230): a block of its own in the active script, the rest of it left as it is ---- */

#define VAC_BEGIN "# jackdaw vacation: begin (Settings > Server filters; edited there)"
#define VAC_END "# jackdaw vacation: end"

static jd_vacation_form vac = {.days = 7};

/* the quoted string at *p (after spaces), its escapes undone; p moves past it; NULL when there is none */
static char *sieve_string(const char **p)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '"') return NULL;
    jd_buf b = {0};
    for (s++; *s && *s != '"'; s++) {
        if (*s == '\\' && s[1]) s++;
        jd_buf_add(&b, s, 1);
    }
    *p = *s ? s + 1 : s;
    return b.s ? b.s : jd_strdup("");
}

static void sieve_quote(jd_buf *b, const char *s)
{
    jd_buf_adds(b, "\"");
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') jd_buf_adds(b, "\\");
        if (*s != '\r') jd_buf_add(b, s, 1);
    }
    jd_buf_adds(b, "\"");
}

/* the date of `currentdate :value "<op>" "date" "<date>"` in block, into f */
static void block_date(const char *block, const char *op, jd_field *f)
{
    char key[32];
    snprintf(key, sizeof key, "\"%s\" \"date\"", op);
    const char *at = strstr(block, key);
    char *d = NULL;
    if (at) at += strlen(key), d = sieve_string(&at);
    jd_field_set(f, d);
    free(d);
}

/* the form from the script's block, off when it has none */
static void vac_read(const char *script)
{
    const char *b = script ? strstr(script, VAC_BEGIN) : NULL;
    const char *e = b ? strstr(b, VAC_END) : NULL;
    vac.on = b && e;
    if (!vac.on) return;
    char *block = jd_strndup(b, (size_t)(e - b));
    block_date(block, "ge", &vac.f[VF_FROM]);
    block_date(block, "le", &vac.f[VF_UNTIL]);
    const char *v = strstr(block, "vacation :days ");
    if (v) {
        vac.days = atoi(v + 15);
        const char *p = strstr(v, ":subject");
        char *subject = p ? (p += 8, sieve_string(&p)) : NULL;
        if (!p) p = v + 15;
        while (*p && *p != '"') p++;
        char *text = sieve_string(&p);
        jd_field_set(&vac.f[VF_SUBJECT], subject);
        jd_editor_set(&vac.text, text ? text : "");
        free(subject), free(text);
    }
    free(block);
}

/* script without its block, and with the form's block when on: after the requires it starts with, as RFC 5228
   wants every require before any other command */
static char *vac_script(const char *script)
{
    jd_buf out = {0};
    const char *s = script ? script : "";
    const char *b = strstr(s, VAC_BEGIN), *e = b ? strstr(b, VAC_END) : NULL;
    jd_buf rest = {0};
    if (b && e) {
        e += strlen(VAC_END);
        if (*e == '\n') e++;
        jd_buf_add(&rest, s, (size_t)(b - s));
        jd_buf_adds(&rest, e);
    } else {
        jd_buf_adds(&rest, s);
    }
    const char *r = rest.s ? rest.s : "";
    /* past the leading comments, blank lines and require commands */
    const char *at = r;
    for (const char *line = r; *line;) {
        const char *nl = strchr(line, '\n');
        const char *next = nl ? nl + 1 : line + strlen(line);
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '#' || *t == '\n' || *t == '\r' || !*t) {
            line = next;
            continue;
        }
        if (strncmp(t, "require", 7) != 0) break;
        const char *semi = strchr(t, ';');
        if (!semi) break;
        line = strchr(semi, '\n') ? strchr(semi, '\n') + 1 : semi + 1;
        at = line;
    }
    jd_buf_add(&out, r, (size_t)(at - r));
    if (vac.on) {
        if (at > r && at[-1] != '\n') jd_buf_adds(&out, "\n");
        jd_buf_adds(&out, VAC_BEGIN "\nrequire [\"vacation\", \"date\", \"relational\"];\n");
        const char *from = vac.f[VF_FROM].text, *until = vac.f[VF_UNTIL].text;
        if (from[0] && until[0]) {
            jd_buf_adds(&out, "if allof (currentdate :value \"ge\" \"date\" ");
            sieve_quote(&out, from);
            jd_buf_adds(&out, ", currentdate :value \"le\" \"date\" ");
            sieve_quote(&out, until);
            jd_buf_adds(&out, ") {\n");
        } else if (from[0] || until[0]) {
            jd_buf_addf(&out, "if currentdate :value \"%s\" \"date\" ", from[0] ? "ge" : "le");
            sieve_quote(&out, from[0] ? from : until);
            jd_buf_adds(&out, " {\n");
        } else {
            jd_buf_adds(&out, "if true {\n");
        }
        jd_buf_addf(&out, "    vacation :days %d", vac.days);
        if (vac.f[VF_SUBJECT].len) {
            jd_buf_adds(&out, " :subject ");
            sieve_quote(&out, vac.f[VF_SUBJECT].text);
        }
        jd_buf_adds(&out, " ");
        sieve_quote(&out, vac.text.text ? vac.text.text : "");
        jd_buf_adds(&out, ";\n}\n" VAC_END "\n");
    }
    jd_buf_adds(&out, at);
    jd_buf_free(&rest);
    return out.s ? out.s : jd_strdup("");
}

static int is_date(const char *s)
{
    int y, m, d;
    char tail;
    return sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &tail) == 3 && strlen(s) == 10 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static void vac_save(jd_app *app)
{
    if (vac.state != 2 || vac.saving || !app->sync) return;
    const char *from = vac.f[VF_FROM].text, *until = vac.f[VF_UNTIL].text;
    if (vac.on && ((from[0] && !is_date(from)) || (until[0] && !is_date(until)))) {
        snprintf(vac.message, sizeof vac.message, "Dates are written 2026-12-24, or left empty.");
        return;
    }
    if (vac.on && from[0] && until[0] && strcmp(from, until) > 0) {
        snprintf(vac.message, sizeof vac.message, "The reply ends before it starts.");
        return;
    }
    if (vac.on && !vac.text.len) {
        snprintf(vac.message, sizeof vac.message, "Write the reply first.");
        return;
    }
    char *script = vac_script(vac.script);
    vac.saving = 1;
    snprintf(vac.message, sizeof vac.message, "Checking and saving...");
    jd_sync_sieve_put(app->sync, vac.name ? vac.name : "jackdaw", script);
    free(vac.sent);
    vac.sent = script;
}

jd_vacation_form *vacation_form(jd_app *app)
{
    if (!jd_streq(vac.of, app->account.address) && !vac.saving) {
        free(vac.script), free(vac.name), free(vac.sent), free(vac.of);
        jd_editor_free(&vac.text);
        vac = (jd_vacation_form){.days = 7, .of = jd_strdup(app->account.address)};
    }
    if (vac.state == 0 && app->sync) {
        vac.state = 1;
        jd_sync_sieve_get(app->sync);
    }
    return &vac;
}

void vacation_save(jd_app *app)
{
    vac_save(app);
}

int vacation_keys(jd_app *app, int *focus)
{
    int f = *focus - FOCUS_VAC;
    if (f < 0 || f > VF_COUNT) return 0;
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_S)) vac_save(app);
    else if (IsKeyPressed(KEY_TAB)) *focus = FOCUS_VAC + (f + 1) % (VF_COUNT + 1);
    else if (f == VF_COUNT) jd_editor_input(&app->ui, &vac.text);
    else if (IsKeyPressed(KEY_ENTER)) vac_save(app);
    else jd_field_input(&vac.f[f]);
    return 1;
}

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
        vac.state = -1;
        snprintf(vac.message, sizeof vac.message, "%s", f->message);
        return;
    }
    if (!vac.saving) {
        free(vac.script), free(vac.name);
        vac.script = jd_strdup(ev->text ? ev->text : "");
        vac.name = jd_strdup(ev->text2);
        vac.state = 2;
        vac_read(vac.script);
    }
    f->state = 1;
    jd_editor_set(&f->script, ev->text && ev->text[0] ? ev->text : starter);
    jd_field_set(&f->name, ev->text2 ? ev->text2 : "jackdaw");
    snprintf(f->message, sizeof f->message, "%s", ev->text2 ? "" : "The server has no filters yet.");
}

void filters_saved(jd_app *app, const char *error)
{
    jd_filters *f = &app->filters;
    if (vac.saving) {
        /* the vacation reply's save; the script editor, when open, shows the script as it now is */
        vac.saving = 0;
        snprintf(vac.message, sizeof vac.message, "%s", error ? error : vac.on ? "Saved: replies are on." : "Saved: replies are off.");
        if (!error) {
            free(vac.script);
            vac.script = vac.sent;
            vac.sent = NULL;
            if (!vac.name) vac.name = jd_strdup("jackdaw");
            if (f->state == 1) {
                jd_editor_set(&f->script, vac.script);
                f->script.changed = 0;
            }
        }
        free(vac.sent);
        vac.sent = NULL;
        return;
    }
    f->saving = 0;
    snprintf(f->message, sizeof f->message, "%s", error ? error : "Saved and active on the server.");
    if (!error) {
        f->script.changed = 0;
        /* the vacation form reads the script it saved */
        free(vac.script);
        vac.script = jd_strdup(f->script.text ? f->script.text : "");
        if (f->name.len) free(vac.name), vac.name = jd_strdup(f->name.text);
        vac.state = 2;
        vac_read(vac.script);
    }
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
