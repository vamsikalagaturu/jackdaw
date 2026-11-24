/* The interface's keys, driven as a person types them: raylib automation events set a key down in one frame and
   up in the next, with pauses between the keys of a chord. The mailbox is seeded into a temporary directory, so
   no server is needed; a display is (a hidden window), and without one the test is skipped (77). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/app.h"
#include "core/util.h"
#include "platform.h"
#include "raylib.h"
#include "ui/fx.h"

/* raylib's AutomationEventType, which rcore.c keeps to itself */
enum { EV_KEY_UP = 1, EV_KEY_DOWN = 2 };

static RenderTexture2D target;
static int failures;
static char *tmp;

/* main.c's connection, without a server: the seeded store and no worker */
void app_connect(jd_app *app)
{
    char err[256];
    char *db = jd_printf("%s/mail.db", tmp);
    free(app->dir);
    app->dir = jd_strdup(tmp);
    app->view = jd_store_open(db, err, sizeof err);
    free(db);
    if (!app->view) {
        fprintf(stderr, "store: %s\n", err);
        exit(1);
    }
}

void app_disconnect(jd_app *app)
{
    jd_store_close(app->view);
    app->view = NULL;
}

static void seed(jd_store *s)
{
    static const struct {
        const char *name, *use;
    } folders[] = {{"INBOX", NULL}, {"Sent", "Sent"}, {"Drafts", "Drafts"}, {"Archive", "Archive"},
                   {"Trash", "Trash"}, {"Junk", "Junk"}};
    int64_t inbox = 0, trash = 0;
    jd_store_begin(s);
    for (size_t i = 0; i < sizeof folders / sizeof *folders; i++) {
        int64_t id = jd_folder_upsert(s, folders[i].name, '/', folders[i].use);
        if (i == 0) inbox = id;
        if (i == 4) trash = id;
    }
    /* five in the Inbox, one in the Trash */
    for (int i = 0; i < 6; i++) {
        int64_t folder = i < 5 ? inbox : trash;
        jd_addr from = {.name = "Ada Lovelace", .addr = "ada@example.org"};
        char subject[64], msgid[64];
        snprintf(subject, sizeof subject, "Message %d", i + 1);
        snprintf(msgid, sizeof msgid, "m%d@example.org", i + 1);
        jd_header h = {.uid = (uint32_t)(i + 1), .date = 1790000000 - i * 3600, .internaldate = 1790000000 - i * 3600,
                       .subject = subject, .msgid = msgid, .size = 100};
        h.addrs[JD_ROLE_FROM] = &from;
        h.naddrs[JD_ROLE_FROM] = 1;
        int is_new;
        jd_message_put(s, folder, &h, &is_new);
    }
    jd_threads_refresh(s, inbox);
    jd_threads_refresh(s, trash);
    jd_store_commit(s);
}

/* n messages from uid first in folder, an hour apart: older than the seeded ones (dir -1) or newer (1) */
static void add_mail(jd_store *s, int64_t folder, int first, int n, int dir)
{
    jd_store_begin(s);
    for (int i = 0; i < n; i++) {
        jd_addr from = {.name = "Grace Hopper", .addr = "grace@example.org"};
        char subject[64], msgid[64];
        snprintf(subject, sizeof subject, "Extra %d", first + i);
        snprintf(msgid, sizeof msgid, "x%d@example.org", first + i);
        int64_t date = dir < 0 ? 1790000000 - (10 + i) * 3600 : 1790000000 + (1 + i) * 600;
        jd_header h = {.uid = (uint32_t)(first + i), .date = date, .internaldate = date, .subject = subject,
                       .msgid = msgid, .size = 100};
        h.addrs[JD_ROLE_FROM] = &from;
        h.naddrs[JD_ROLE_FROM] = 1;
        int is_new;
        jd_message_put(s, folder, &h, &is_new);
    }
    jd_threads_refresh(s, folder);
    jd_store_commit(s);
}

static void frame(jd_app *app)
{
    plat_frame(0);
    BeginTextureMode(target);
    ClearBackground(JD_NORD0);
    jd_scale_begin();
    mail_frame(app);
    fx_frame();
    jd_scale_end();
    EndTextureMode();
}

static void key_event(int type, int key)
{
    AutomationEvent e = {0};
    e.type = (unsigned int)type;
    e.params[0] = key;
    PlayAutomationEvent(e);
}

/* one keypress over two frames, then a pause of a few frames as between a person's keys */
static void tap(jd_app *app, int key)
{
    PollInputEvents();
    key_event(EV_KEY_DOWN, key);
    frame(app);
    PollInputEvents();
    key_event(EV_KEY_UP, key);
    frame(app);
    for (int i = 0; i < 10; i++) {
        PollInputEvents();
        frame(app);
    }
}

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s  ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures += !ok;
}

static const char *folder_now(jd_app *app)
{
    return app->nfolders ? app->folders[app->folder].name : "";
}

static void go(jd_app *app, int pane, int letter, const char *want)
{
    app->pane = pane;
    tap(app, KEY_G);
    tap(app, letter);
    static const char *panes[] = {"folders", "list", "reader"};
    check(!strcmp(folder_now(app), want), "g %c from the %s pane opens %s (got %s)", 'a' + (letter - KEY_A),
          panes[pane], want, folder_now(app));
}

int main(void)
{
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        puts("no display: skipped");
        return 77;
    }
    char tmpl[] = "/tmp/jackdaw-keys-XXXXXX";
    tmp = mkdtemp(tmpl);
    if (!tmp) return 1;
    /* settings and data in the temporary directory, never the user's */
    setenv("XDG_CONFIG_HOME", tmp, 1);
    setenv("XDG_DATA_HOME", tmp, 1);

    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_ERROR);
    InitWindow(1200, 800, "jackdaw keys test");
    target = LoadRenderTexture(1200, 800);

    jd_app app = {.pane = PANE_LIST, .grouped = 1};
    jd_prefs_load(&app.prefs);
    tab_add(&app, TAB_MAIL);
    apply_prefs(&app);
    app.have_account = 1;
    app.account.address = jd_strdup("test@example.org");
    app_connect(&app);
    seed(app.view);
    mail_reload(&app);
    frame(&app);
    check(app.nfolders == 6 && app.nrows == 5, "seeded: %d folders, %d rows in %s", app.nfolders, app.nrows,
          folder_now(&app));

    go(&app, PANE_LIST, KEY_S, "Sent");
    go(&app, PANE_LIST, KEY_D, "Drafts");
    go(&app, PANE_FOLDERS, KEY_A, "Archive");
    go(&app, PANE_FOLDERS, KEY_J, "Junk");
    go(&app, PANE_FOLDERS, KEY_T, "Trash");
    go(&app, PANE_FOLDERS, KEY_I, "INBOX");

    app.pane = PANE_LIST;
    tap(&app, KEY_J);
    tap(&app, KEY_J);
    check(app.row == 2, "j j moves two rows down (row %d)", app.row);
    tap(&app, KEY_G);
    tap(&app, KEY_G);
    check(app.row == 0, "g g goes to the first row (row %d)", app.row);

    reader_open(&app, 0);
    frame(&app);
    go(&app, PANE_READER, KEY_S, "Sent");
    go(&app, PANE_LIST, KEY_I, "INBOX");

    app.pane = PANE_LIST;
    tap(&app, KEY_F);
    tap(&app, KEY_U);
    check(app.filter_unread, "f u turns the unread filter on");
    tap(&app, KEY_F);
    tap(&app, KEY_C);
    check(!app.filter_unread, "f c clears the filters");

    /* in the Trash, deleting asks first (and needs no server to): the question shows that dd reached it */
    go(&app, PANE_LIST, KEY_T, "Trash");
    app.pane = PANE_LIST;
    tap(&app, KEY_D);
    for (int i = 0; i < 30; i++) frame(&app);
    check(!app.dialog.kind, "one d deletes nothing");
    tap(&app, KEY_D);
    check(app.dialog.kind == DLG_CONFIRM && app.dialog.act == ACT_DESTROY, "d d deletes (asks: %s)", app.dialog.title);
    app.dialog.kind = 0;

    go(&app, PANE_LIST, KEY_I, "INBOX");
    int64_t inbox = app.folders[app.folder].id;
    add_mail(app.view, inbox, 100, 30, -1);
    mail_reload(&app);
    for (int i = 0; i < 3; i++) frame(&app);
    app.row = 12, app.followed_row = 12;
    app.list_scroll = 500;
    for (int i = 0; i < 3; i++) frame(&app);
    int64_t picked = app.rows[app.row].thread;
    float before = app.list_scroll;
    add_mail(app.view, inbox, 200, 3, 1);
    mail_reload(&app);
    for (int i = 0; i < 3; i++) frame(&app);
    check(app.rows[app.row].thread == picked && app.row == 15, "new mail keeps the selection (row %d)", app.row);
    check(app.list_scroll > before + 3 * 40 && app.list_scroll < before + 3 * 140,
          "new mail above keeps the view still (scroll %.0f -> %.0f)", before, app.list_scroll);

    int64_t wanted = app.rows[4].latest, wanted_thread = app.rows[4].thread;
    go(&app, PANE_LIST, KEY_S, "Sent");
    mail_open_message(&app, wanted);
    frame(&app);
    jd_conv *shown = app.prefs.reader_pos == 2 ? &app.tabs[app.tab].conv : preview(&app);
    check(!strcmp(folder_now(&app), "INBOX") && shown->thread == wanted_thread,
          "a notification opens its message (folder %s, thread %lld)", folder_now(&app), (long long)shown->thread);

    /* the keys help: ? (Shift+/) opens it, keys typed into its filter, then one Esc closes it */
    app.tab = 0;
    app.pane = PANE_LIST;
    PollInputEvents();
    key_event(EV_KEY_DOWN, KEY_LEFT_SHIFT);
    frame(&app);
    tap(&app, KEY_SLASH);
    PollInputEvents();
    key_event(EV_KEY_UP, KEY_LEFT_SHIFT);
    frame(&app);
    check(app.help, "? opens the keys help");
    tap(&app, KEY_D);
    tap(&app, KEY_E);
    tap(&app, KEY_MINUS);
    tap(&app, KEY_ESCAPE);
    check(!app.help, "Esc closes the keys help after typing (searching %d)", app.searching);

    /* a tap some keyboards send as press and release together: both read in one poll, it still counts */
    app.help = 1;
    for (int i = 0; i < 3; i++) frame(&app);
    PollInputEvents();
    key_event(EV_KEY_DOWN, KEY_ESCAPE);
    key_event(EV_KEY_UP, KEY_ESCAPE);
    frame(&app);
    check(!app.help, "an Esc pressed and released within one frame closes the keys help");

    /* the magnifier (Ctrl + wheel): enlarged twice around the pointer, the point under it stays under it */
    {
        AutomationEvent e = {0};
        e.type = 7; /* INPUT_MOUSE_POSITION */
        e.params[0] = 600, e.params[1] = 400;
        PlayAutomationEvent(e);
        frame(&app);
        Vector2 before = GetMousePosition();
        jd_magnify(2);
        for (int i = 0; i < 3; i++) frame(&app);
        Vector2 after = GetMousePosition();
        Rectangle on_screen = jd_to_pixels((Rectangle){after.x, after.y, 0, 0});
        check(fabsf(after.x - before.x) < 1 && fabsf(after.y - before.y) < 1 && fabsf(on_screen.x - 600) < 2 &&
                  fabsf(on_screen.y - 400) < 2,
              "magnified twice, the pointer stays on the same point (%.0f,%.0f -> %.0f,%.0f, drawn at %.0f,%.0f)",
              before.x, before.y, after.x, after.y, on_screen.x, on_screen.y);
        jd_magnify(1);
        frame(&app);
    }

    /* the folder tree: a special folder's own folder follows it, not the plain folders listed after the specials */
    jd_store_begin(app.view);
    jd_folder_upsert(app.view, "Archive/2019", '/', NULL);
    jd_folder_upsert(app.view, "00-todo", '/', NULL);
    jd_store_commit(app.view);
    mail_reload(&app);
    {
        int archive = -1, inside = -1;
        for (int i = 0; i < app.nfolders; i++) {
            if (!strcmp(app.folders[i].name, "Archive")) archive = i;
            if (!strcmp(app.folders[i].name, "Archive/2019")) inside = i;
        }
        check(archive >= 0 && inside == archive + 1, "a folder inside Archive comes right after it (%d, %d)", archive, inside);
    }

    /* the search bar: / from another page focuses it; what is typed finds a setting; Down and Enter open it */
    settings_open(&app);
    frame(&app);
    tap(&app, KEY_SLASH);
    check(app.searching, "/ focuses the search bar from Settings");
    jd_field_set(&app.search, "undo");
    /* the results come once typing pauses */
    for (double t = GetTime(); GetTime() - t < 0.25;) frame(&app);
    tap(&app, KEY_DOWN);
    tap(&app, KEY_ENTER);
    /* 5: Settings' Compose section, where Undo send is */
    check(!app.searching && app.tabs[app.tab].kind == TAB_SETTINGS && app.settings_section == 5,
          "the search bar finds Undo send and opens Settings at it (section %d)", app.settings_section);
    tab_close(&app, app.tab);
    app.tab = 0;

    /* the calendar: g c opens its tab (with no calendar set up), n asks for one first, Esc closes the tab */
    app.tab = 0;
    app.pane = PANE_LIST;
    tap(&app, KEY_G);
    tap(&app, KEY_C);
    check(app.tabs[app.tab].kind == TAB_CALENDAR, "g c opens the calendar");
    tap(&app, KEY_M);
    tap(&app, KEY_N);
    check(!calendar_typing() && strstr(app.notice, "Settings"), "n without a calendar points to Settings (%s)", app.notice);
    tap(&app, KEY_ESCAPE);
    check(app.tabs[app.tab].kind != TAB_CALENDAR, "Esc closes the calendar");

    UnloadRenderTexture(target);
    CloseWindow();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
