/* Updates from the latest GitHub release (mail/release.h): checked once a day at start when [updates] check is on,
 * and from Settings -> About. A newer version shows a bar in the list; Update downloads it (verified against the
 * SHA-256 GitHub publishes), then on Linux replaces the installed program and offers a restart, on Android hands
 * the APK to Android's installer. Only the "jackdaw" app updates itself, never a jackdaw-dev build. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "app.h"
#include "core/util.h"
#include "mail/release.h"
#include "platform.h"
#include "ui/icons.h"

#define CHECK_EVERY_S (24 * 3600)

enum { IDLE, CHECKING, DOWNLOADING };

static struct {
    int state, manual, started;
    atomic_int done;
    pthread_t thread;
    jd_release found; /* the worker's answer */
    int rc;
    char err[200];
    jd_release rel; /* a release newer than this build; tag NULL when none */
    int dismissed, installed;
    volatile float progress;
    char *path; /* where the download goes */
    char status[280];
} U;

static int updatable(void) { return !strcmp(JD_APP_NAME, "jackdaw"); }

static char *stamp_path(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    return xdg && xdg[0] == '/' ? jd_printf("%s/" JD_APP_NAME "/update", xdg)
                                : jd_printf("%s/.cache/" JD_APP_NAME "/update", home && *home ? home : "/tmp");
}

static int checked_lately(void)
{
    char *dir = stamp_path();
    struct stat st;
    int recent = !stat(dir, &st) && time(NULL) - st.st_mtime < CHECK_EVERY_S;
    free(dir);
    return recent;
}

static void mark_checked(void)
{
    char *dir = stamp_path();
    jd_mkdirs(dir);
    utime(dir, NULL);
    free(dir);
}

/* ~/.local/bin/jackdaw (platform/linux/install.sh) when that is what runs; a build run from elsewhere only gets the
   release's page */
static char *installed_program(void)
{
#ifdef __ANDROID__
    return NULL;
#else
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    const char *home = getenv("HOME");
    if (n <= 0 || !home) return NULL;
    self[n] = 0;
    char *want = jd_printf("%s/.local/bin/jackdaw", home);
    if (strcmp(self, want)) free(want), want = NULL;
    return want;
#endif
}

static void *work(void *arg)
{
    if (U.state == CHECKING) U.rc = jd_release_latest(&U.found, U.err, sizeof U.err);
    else U.rc = jd_release_download(U.rel.asset, U.rel.sha256, U.path, &U.progress, U.err, sizeof U.err);
    atomic_store(&U.done, 1);
    return NULL;
}

static void start(int state)
{
    U.state = state;
    atomic_store(&U.done, 0);
    if (pthread_create(&U.thread, NULL, work, NULL) != 0) U.state = IDLE;
}

void update_check(jd_app *app, int manual)
{
    if (U.state != IDLE) return;
    U.manual = manual;
    snprintf(U.status, sizeof U.status, "Checking...");
    start(CHECKING);
}

static void finish_check(jd_app *app)
{
    if (U.rc) {
        snprintf(U.status, sizeof U.status, "Could not check: %s", U.err);
        if (U.manual) notify_user(app, "Could not check for updates: %s", U.err);
        return;
    }
    mark_checked();
    if (jd_version_cmp(U.found.tag, jd_version) > 0 && U.found.asset) {
        jd_release_free(&U.rel);
        U.rel = U.found;
        U.found = (jd_release){0};
        U.dismissed = 0;
        snprintf(U.status, sizeof U.status, "jackdaw %s is available.", U.rel.tag + (U.rel.tag[0] == 'v'));
    } else {
        snprintf(U.status, sizeof U.status, "jackdaw %s is the latest.", jd_version);
        if (U.manual) notify_user(app, "%s", U.status);
        jd_release_free(&U.found);
    }
}

static void finish_download(jd_app *app)
{
    if (U.rc) {
        notify_user(app, "Could not update: %s", U.err);
        snprintf(U.status, sizeof U.status, "Could not update: %s", U.err);
    } else {
#ifdef __ANDROID__
        if (plat_install_package(U.path) != 0) notify_user(app, "Android did not open its installer.");
        U.dismissed = 1;
#else
        char *prog = installed_program();
        /* renamed over the program: the running one keeps its inode until it exits */
        if (!prog || chmod(U.path, 0755) || rename(U.path, prog)) {
            notify_user(app, "Could not install the update: %s", strerror(errno));
            remove(U.path);
        } else {
            U.installed = 1;
            snprintf(U.status, sizeof U.status, "jackdaw %s is installed; restart to use it.", U.rel.tag + (U.rel.tag[0] == 'v'));
        }
        free(prog);
#endif
    }
    free(U.path);
    U.path = NULL;
}

void update_tick(jd_app *app)
{
    if (!U.started) {
        U.started = 1;
        if (updatable() && app->prefs.update_check && !checked_lately()) update_check(app, 0);
    }
    if (U.state == IDLE || !atomic_load(&U.done)) return;
    pthread_join(U.thread, NULL);
    int was = U.state;
    U.state = IDLE;
    if (was == CHECKING) finish_check(app);
    else finish_download(app);
}

static void update_now(jd_app *app)
{
    char *prog = installed_program();
    if (!plat_touch() && !prog) {
        if (U.rel.page && plat_open_uri(U.rel.page) != 0) notify_user(app, "Could not open %s", U.rel.page);
        return;
    }
    if (prog) {
        *strrchr(prog, '/') = 0;
        U.path = jd_printf("%s/.jackdaw-%s.new", prog, U.rel.tag);
    } else {
        char *dir = stamp_path();
        jd_mkdirs(dir);
        U.path = jd_printf("%s/jackdaw-%s.apk", dir, U.rel.tag);
        free(dir);
    }
    free(prog);
    start(DOWNLOADING);
}

static void restart(jd_app *app)
{
#ifndef __ANDROID__
    char *prog = installed_program();
    if (!prog) return;
    save_prefs(app);
    pending_tick(app, 1);
    app_disconnect(app);
    execv(prog, (char *[]){prog, NULL});
    notify_user(app, "Could not restart: %s", strerror(errno));
    free(prog);
#endif
}

const char *update_status(void) { return U.status; }
int update_busy(void) { return U.state != IDLE; }

float update_bar(jd_app *app, float x, float y, float w)
{
    if (!U.rel.tag || (U.dismissed && U.state != DOWNLOADING)) return 0;
    jd_ui *ui = &app->ui;
    const float h = 40;
    const char *v = U.rel.tag + (U.rel.tag[0] == 'v');
    Rectangle bar = {x, y, w, h};
    DrawRectangleRounded(bar, 0.25f, 6, JD_NORD2);
    jd_icon("download", x + 12, y + 12, 16, JD_NORD8);
    char line[96];
    if (U.state == DOWNLOADING) {
        snprintf(line, sizeof line, "Downloading jackdaw %s... %d%%", v, (int)(U.progress * 100));
        jd_text_fit(ui, line, x + 38, y + 12, w - 50, 14, JD_NORD6, 0);
        DrawRectangleRounded((Rectangle){x + 38, y + h - 9, w - 50, 4}, 1, 4, JD_NORD1);
        DrawRectangleRounded((Rectangle){x + 38, y + h - 9, (w - 50) * U.progress, 4}, 1, 4, JD_NORD8);
        return h + 6;
    }
    snprintf(line, sizeof line, U.installed ? "jackdaw %s is installed" : "jackdaw %s is available", v);
    char *prog = installed_program();
    const char *go = U.installed ? "Restart" : (plat_touch() || prog) ? "Update" : "Open";
    free(prog);
    float gw = jd_text_width(ui, go, 14, 1) + 24, lw = jd_text_width(ui, "Later", 14, 1) + 24;
    jd_text_fit(ui, line, x + 38, y + 12, w - 60 - gw - lw, 14, JD_NORD6, 1);
    if (jd_button(ui, go, (Rectangle){x + w - 4 - gw, y + 4, gw, h - 8}, 1)) U.installed ? restart(app) : update_now(app);
    if (!U.installed && jd_button(ui, "Later", (Rectangle){x + w - 8 - gw - lw, y + 4, lw, h - 8}, 0)) U.dismissed = 1;
    return h + 6;
}
