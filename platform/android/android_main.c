/* platform.h on Android, over raylib's PLATFORM_ANDROID: the environment the shared sources expect (HOME, XDG_*,
 * the system's certificates), touch turned into the mouse the interface reads, the soft keyboard's text, and what
 * only Java reaches (JackdawActivity.java): the keystore, opening links and files, picking files, the clipboard.
 *
 * raylib on Android leaves three things the interface needs undone, so they are fed in here:
 *   - taps: touch goes to raylib as a touch per mouse button (index 0 left, 1 right) through its automation
 *     events, one state change per frame, so IsMouseButtonPressed sees each press once;
 *   - scrolling: a dragging finger becomes GetMouseWheelMove() (wrapped, -Wl,--wrap), with a fling after;
 *   - text: raylib never fills GetCharPressed() there, so it is wrapped too and fed from hardware keys and the
 *     soft keyboard. Backspace and Enter from the soft keyboard become key presses.
 * Everything runs on the android_main thread but native_text() and native_picked(), which Java calls on the UI
 * thread. */
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <ftw.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/util.h"
#include "mail/account.h"
#include "mail/sync.h"
#include "platform.h"
#include "raylib.h"
#include "store/store.h"
#include "ui/theme.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "jackdaw", __VA_ARGS__)

struct android_app *GetAndroidApp(void); /* rcore_android.c; not in raylib.h */

/* rcore_android.c's __wrap_fopen calls it; without --wrap=fopen nothing calls that wrapper (CMakeLists.txt) */
FILE *__real_fopen(const char *name, const char *mode);
FILE *__real_fopen(const char *name, const char *mode) { return fopen(name, mode); }

/* raylib's AutomationEventType, which rcore.c keeps to itself */
enum { EV_KEY_UP = 1, EV_KEY_DOWN = 2, EV_MOUSE_POSITION = 7, EV_TOUCH_UP = 14, EV_TOUCH_DOWN = 15 };

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---- JNI ---- */

static JNIEnv *jni(void)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    /* only the android_main thread calls Java here, and raylib may have detached it */
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    return env;
}

static int jfailed(JNIEnv *e)
{
    if (!(*e)->ExceptionCheck(e)) return 0;
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
    return 1;
}

static char *jstr(JNIEnv *e, jobject s)
{
    if (!s) return NULL;
    const char *u = (*e)->GetStringUTFChars(e, (jstring)s, NULL);
    char *r = u ? jd_strdup(u) : NULL;
    if (u) (*e)->ReleaseStringUTFChars(e, (jstring)s, u);
    return r;
}

/* JackdawActivity.<method>(String x nargs) returning String, at most 3 arguments; malloc'd, NULL for Java's null
   or an exception */
static char *java(const char *method, int nargs, ...)
{
    JNIEnv *e = jni();
    if (!e) return NULL;
    (*e)->PushLocalFrame(e, 16);
    char sig[96] = "(";
    jvalue args[3] = {0};
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 3; i++) {
        const char *s = va_arg(ap, const char *);
        args[i].l = s ? (*e)->NewStringUTF(e, s) : NULL;
        strcat(sig, "Ljava/lang/String;");
    }
    va_end(ap);
    strcat(sig, ")Ljava/lang/String;");
    jobject act = GetAndroidApp()->activity->clazz;
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = (*e)->GetMethodID(e, c, method, sig);
    char *r = NULL;
    if (!jfailed(e) && m) {
        jobject s = (*e)->CallObjectMethodA(e, act, m, args);
        if (!jfailed(e)) r = jstr(e, s);
    }
    (*e)->PopLocalFrame(e, NULL);
    return r;
}

/* ---- environment ---- */

#ifdef JD_HAVE_TEST_CA
extern const unsigned char jd_test_ca[];
extern const int jd_test_ca_size;
#endif

/* Android names its root certificates by OpenSSL's old (MD5) subject hash, which OpenSSL 3 never looks up, so
   they are joined into one PEM file. Android 14+ updates its roots in the Conscrypt APEX; /system keeps the
   shipped ones. */
static void write_roots(const char *path)
{
    static const char *dirs[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"};
    FILE *out = fopen(path, "w");
    if (!out) {
        LOG("cannot write %s", path);
        return;
    }
    int n = 0;
    for (size_t d = 0; d < sizeof dirs / sizeof *dirs && n == 0; d++) {
        DIR *dir = opendir(dirs[d]);
        for (struct dirent *e; dir && (e = readdir(dir));) {
            if (e->d_name[0] == '.') continue;
            char *file = jd_printf("%s/%s", dirs[d], e->d_name);
            size_t len;
            char *pem = jd_read_file(file, &len);
            if (pem) fwrite(pem, 1, len, out), fputc('\n', out), n++;
            free(pem), free(file);
        }
        if (dir) closedir(dir);
    }
#ifdef JD_HAVE_TEST_CA
    /* a test build trusts the test server's CA too (tests/imap) */
    fwrite(jd_test_ca, 1, (size_t)jd_test_ca_size, out), fputc('\n', out);
#endif
    fclose(out);
    LOG("%d trusted roots", n);
}

static void setup_env(const char *files)
{
    char *config = jd_printf("%s/.config", files), *data = jd_printf("%s/.local/share", files);
    char *cache = jd_printf("%s/cache", files), *run = jd_printf("%s/run", files);
    char *downloads = jd_printf("%s/Downloads", files);
    jd_mkdirs(config), jd_mkdirs(data), jd_mkdirs(cache), jd_mkdirs(run), jd_mkdirs(downloads);
    chmod(run, 0700);
    setenv("HOME", files, 1);
    setenv("XDG_CONFIG_HOME", config, 1);
    setenv("XDG_DATA_HOME", data, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    setenv("XDG_RUNTIME_DIR", run, 1);
    setenv("XDG_DOWNLOAD_DIR", downloads, 1);
    setenv("TMPDIR", cache, 1);
    /* OpenSSL (IMAP, SMTP, Sieve) and libcurl (images) trust what this file holds */
    char *roots = jd_printf("%s/roots.pem", cache);
    write_roots(roots);
    setenv("SSL_CERT_FILE", roots, 1);
    free(roots);
    if (chdir(files)) LOG("chdir %s failed", files);
    free(config), free(data), free(cache), free(run), free(downloads);
}

/* Runs on the sync thread: Android stops the frame loop in the background. Attached to the JVM for the call
   only, as a native thread that exits attached aborts the process. */
/* The account of settings.toml with this address, and its place in [[account]] (notification tags carry it,
   notify_tag); the first when address is NULL. 0 when found. */
static int account_named(const char *address, jd_account *out, int *index)
{
    jd_account *all;
    int n;
    char err[256];
    *out = (jd_account){0};
    if (jd_accounts_load(&all, &n, err, sizeof err) != 0) return -1;
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (!address || (all[i].address && strcasecmp(all[i].address, address) == 0)) found = i;
    if (found >= 0) {
        *out = all[found];
        all[found] = (jd_account){0};
        if (index) *index = found;
    }
    jd_accounts_free(all, n);
    return found >= 0 ? 0 : -1;
}

static void on_new_mail(const char *account, const char *from, const char *subject, int64_t id, int count)
{
    jd_account a;
    int index = 0;
    if (account_named(account, &a, &index) == 0) jd_account_free(&a);
    id = notify_tag(index, id);
    jd_prefs p;
    jd_prefs_load(&p);
    int notify = p.notify, focused = p.notify_focused;
    jd_prefs_free(&p);
    if (!notify) return;
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *e = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return;
        attached = 1;
    }
    char *title = count == 1 ? jd_strdup(from ? from : "") : jd_printf("%d new messages", count);
    char *text = count == 1 ? jd_strdup(subject ? subject : "") : jd_printf("%s: %s", from ? from : "", subject ? subject : "");
    char ids[32];
    snprintf(ids, sizeof ids, "%lld", (long long)id);
    (*e)->PushLocalFrame(e, 8);
    jobject act = GetAndroidApp()->activity->clazz;
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = (*e)->GetMethodID(e, c, "postNewMail",
                                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (m)
        (*e)->CallObjectMethod(e, act, m, (*e)->NewStringUTF(e, title), (*e)->NewStringUTF(e, text),
                               (*e)->NewStringUTF(e, ids), (*e)->NewStringUTF(e, focused ? "1" : "0"));
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    (*e)->PopLocalFrame(e, NULL);
    free(title), free(text);
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static void scheduled_note(const char *subject, const char *err, char **title, char **text)
{
    const char *what = subject && subject[0] ? subject : "(no subject)";
    *title = jd_strdup(err ? "Scheduled message not sent" : "Scheduled message sent");
    *text = err ? jd_printf("%s: %s. It is back in Drafts.", what, err) : jd_strdup(what);
}

/* as on_new_mail; shown when the app is not on screen, which says it in its own notice */
static void on_scheduled(const char *subject, const char *err)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *e = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return;
        attached = 1;
    }
    char *title, *text;
    scheduled_note(subject, err, &title, &text);
    (*e)->PushLocalFrame(e, 8);
    jobject act = GetAndroidApp()->activity->clazz;
    jclass c = (*e)->GetObjectClass(e, act);
    jmethodID m = (*e)->GetMethodID(e, c, "postNewMail",
                                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (m)
        (*e)->CallObjectMethod(e, act, m, (*e)->NewStringUTF(e, title), (*e)->NewStringUTF(e, text),
                               (*e)->NewStringUTF(e, "0"), (*e)->NewStringUTF(e, "0"));
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    (*e)->PopLocalFrame(e, NULL);
    free(title), free(text);
    if (attached) (*vm)->DetachCurrentThread(vm);
}

void plat_init(void)
{
    setup_env(GetAndroidApp()->activity->internalDataPath);
    jd_sync_new_mail_hook = on_new_mail;
    jd_sync_scheduled_hook = on_scheduled;
}

/* SendAlarm, at the soonest scheduled draft's time; 0 cancels it */
void plat_wake_at(int64_t t)
{
    static int64_t was = -1;
    if (t == was) return;
    was = t;
    char at[32];
    snprintf(at, sizeof at, "%lld", (long long)t);
    free(java("wakeAt", 1, at));
}

/* ReminderAlarm: the reminders as lines of "at\ttitle\ttext", tabs and newlines in the text made spaces */
int plat_reminders(int n, const int64_t *at, const char *const *title, const char *const *text)
{
    jd_buf spec = {0};
    for (int i = 0; i < n; i++) {
        jd_buf_addf(&spec, "%lld\t", (long long)at[i]);
        for (int k = 0; k < 2; k++) {
            for (const char *c = k ? text[i] : title[i]; c && *c; c++) jd_buf_add(&spec, *c == '\t' || *c == '\n' ? " " : c, 1);
            jd_buf_adds(&spec, k ? "\n" : "\t");
        }
    }
    static char *was;
    const char *now = spec.s ? spec.s : "";
    if (!was || strcmp(was, now) != 0) {
        free(java("setReminders", 1, now));
        free(was);
        was = jd_strdup(now);
    }
    jd_buf_free(&spec);
    return 1;
}

/* The soonest scheduled draft's time, 0 when none. One already due, or one waiting in the outbox, was not sent by
   the check just made: tried again after sync.c's OUTBOX_RETRY. */
static int64_t next_send(jd_store *view, const char *account_dir)
{
    int n = 0;
    int64_t drafts = jd_store_special(view, "Drafts", NULL), at = 0, retry = jd_now() + 5 * 60;
    jd_scheduled *due = drafts ? jd_store_scheduled(view, drafts, &n) : NULL;
    if (n) at = due[0].send_at <= jd_now() ? retry : due[0].send_at;
    free(due);
    char *outbox = jd_printf("%s/outbox", account_dir);
    DIR *d = opendir(outbox);
    for (struct dirent *e; d && (e = readdir(d));)
        if (e->d_name[0] != '.' && (!at || retry < at)) at = retry;
    if (d) closedir(d);
    free(outbox);
    return at;
}

/* ---- the background check (MailJobService) ---- */

/* every account's address, one a line; NULL when there is none */
JNIEXPORT jstring JNICALL Java_io_github_vamsikalagaturu_jackdaw_MailJobService_nativeAccounts(JNIEnv *e, jclass cls,
                                                                                                jstring files)
{
    char *dir = jstr(e, files);
    if (!dir) return NULL;
    setup_env(dir);
    free(dir);
    jd_account *all;
    int n;
    char err[256];
    if (jd_accounts_load(&all, &n, err, sizeof err) != 0) return NULL;
    jd_buf b = {0};
    for (int i = 0; i < n; i++)
        if (all[i].address) jd_buf_addf(&b, "%s%s", b.len ? "\n" : "", all[i].address);
    jstring out = b.len ? (*e)->NewStringUTF(e, b.s) : NULL;
    jd_buf_free(&b);
    jd_accounts_free(all, n);
    return out;
}

/* ---- push (PushService) ---- */

static volatile int push_quit;

/* Stopped while JackdawActivity.running: one sync at a time, or the first to store a message would leave the
   other nothing new to notify. */
JNIEXPORT void JNICALL Java_io_github_vamsikalagaturu_jackdaw_PushService_nativePush(JNIEnv *e, jclass cls,
                                                                                     jstring files, jstring addr,
                                                                                     jstring pw)
{
    char *dir_files = jstr(e, files), *password = jstr(e, pw), *address = jstr(e, addr);
    jd_account a = {0};
    int index = 0;
    char err[256];
    push_quit = 0;
    jclass activity = (*e)->FindClass(e, "io/github/vamsikalagaturu/jackdaw/JackdawActivity");
    jfieldID app_running = activity ? (*e)->GetStaticFieldID(e, activity, "running", "Z") : NULL;
    jmethodID posted = (*e)->GetStaticMethodID(e, cls, "posted", "(Ljava/lang/String;Ljava/lang/String;J)V");
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    if (!dir_files || !password || !address || !app_running || !posted) goto done;
    setup_env(dir_files);
    if (account_named(address, &a, &index) != 0) goto done;
    char *dir = jd_account_dir(&a), *db = jd_printf("%s/mail.db", dir);
    jd_store *view = jd_store_open(db, err, sizeof err);
    jd_sync *s = NULL;
    int refused = 0;
    while (view && !push_quit && !refused) {
        int app = (*e)->GetStaticBooleanField(e, activity, app_running);
        if (app && s) jd_sync_stop(s), s = NULL;
        if (!app && !s) {
            /* the hooks call into the activity, which is gone */
            jd_sync_new_mail_hook = NULL;
            jd_sync_scheduled_hook = NULL;
            s = jd_sync_start(&a, password, dir, db);
        }
        jd_event ev;
        while (s && jd_sync_poll(s, &ev)) {
            if (ev.kind == JD_EV_AUTH) refused = 1;
            if (ev.kind == JD_EV_NEW_MAIL && ev.count > 0) {
                char *title = ev.count == 1 ? jd_strdup(ev.text ? ev.text : "")
                                            : jd_printf("%d new messages", ev.count);
                char *text = ev.count == 1 ? jd_strdup(ev.text2 ? ev.text2 : "")
                                           : jd_printf("%s: %s", ev.text ? ev.text : "", ev.text2 ? ev.text2 : "");
                (*e)->PushLocalFrame(e, 4);
                (*e)->CallStaticVoidMethod(e, cls, posted, (*e)->NewStringUTF(e, title), (*e)->NewStringUTF(e, text),
                                           (jlong)notify_tag(index, ev.id));
                if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
                (*e)->PopLocalFrame(e, NULL);
                free(title), free(text);
            }
            if (ev.kind == JD_EV_SCHEDULED) {
                char *title, *text;
                scheduled_note(ev.text2, ev.text, &title, &text);
                (*e)->PushLocalFrame(e, 4);
                (*e)->CallStaticVoidMethod(e, cls, posted, (*e)->NewStringUTF(e, title), (*e)->NewStringUTF(e, text),
                                           (jlong)0);
                if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
                (*e)->PopLocalFrame(e, NULL);
                free(title), free(text);
            }
            jd_event_free(&ev);
        }
        usleep(300 * 1000);
    }
    if (s) jd_sync_stop(s);
    jd_store_close(view);
    free(dir), free(db);
done:
    if (password) jd_wipe(password, strlen(password));
    free(password), free(dir_files), free(address);
    jd_account_free(&a);
}

JNIEXPORT void JNICALL Java_io_github_vamsikalagaturu_jackdaw_PushService_nativePushStop(JNIEnv *e, jclass cls)
{
    push_quit = 1;
}

/* A line per new-mail event, "id\ttitle\ttext", or NULL. Android allows a job about ten minutes. */
#define CHECK_S 120.0

JNIEXPORT jstring JNICALL Java_io_github_vamsikalagaturu_jackdaw_MailJobService_nativeCheck(JNIEnv *e, jclass cls,
                                                                                             jstring files, jstring addr,
                                                                                             jstring pw)
{
    char *dir_files = jstr(e, files), *password = jstr(e, pw), *address = jstr(e, addr);
    jstring out = NULL;
    jd_prefs p;
    jd_account a = {0};
    int index = 0;
    char err[256];
    if (!dir_files || !password || !address) goto done;
    setup_env(dir_files);
    jd_prefs_load(&p);
    int notify = p.notify;
    jd_prefs_free(&p);
    if (account_named(address, &a, &index) != 0) goto done;
    char *dir = jd_account_dir(&a), *db = jd_printf("%s/mail.db", dir);
    /* opened here first, as the app does, so the schema exists before the worker writes */
    jd_store *view = jd_store_open(db, err, sizeof err);
    /* with notifications off it runs only to send scheduled drafts */
    if (view && !notify && !next_send(view, dir)) {
        jd_store_close(view);
    } else if (view) {
        jd_sync *s = jd_sync_start(&a, password, dir, db);
        jd_buf lines = {0};
        int busy = 0, finished = 0;
        double start = now();
        while (!finished && now() - start < CHECK_S) {
            jd_event ev;
            while (!finished && jd_sync_poll(s, &ev)) {
                if (ev.kind == JD_EV_STATUS && ev.text) busy = 1;
                else if (ev.kind == JD_EV_STATUS && busy) finished = 1;
                else if (ev.kind == JD_EV_AUTH || (ev.kind == JD_EV_ERROR && busy)) finished = 1;
                else if (ev.kind == JD_EV_NEW_MAIL && ev.count > 0 && notify) {
                    /* a tab or line break in a sender or subject would break the lines apart */
                    char *from = jd_strdup(ev.text ? ev.text : ""), *subject = jd_strdup(ev.text2 ? ev.text2 : "");
                    for (char *c = from; *c; c++)
                        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
                    for (char *c = subject; *c; c++)
                        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
                    long long tag = (long long)notify_tag(index, ev.id);
                    if (ev.count == 1) jd_buf_addf(&lines, "%lld\t%s\t%s\n", tag, from, subject);
                    else jd_buf_addf(&lines, "%lld\t%d new messages\t%s: %s\n", tag, ev.count, from, subject);
                    free(from), free(subject);
                } else if (ev.kind == JD_EV_SCHEDULED) {
                    char *title, *text;
                    scheduled_note(ev.text2, ev.text, &title, &text);
                    for (char *c = text; *c; c++)
                        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
                    jd_buf_addf(&lines, "0\t%s\t%s\n", title, text);
                    free(title), free(text);
                }
                jd_event_free(&ev);
            }
            if (!finished) usleep(200 * 1000);
        }
        jd_sync_stop(s);
        jd_store_close(view);
        if (lines.len) out = (*e)->NewStringUTF(e, lines.s);
        jd_buf_free(&lines);
    } else {
        LOG("background check: %s", err);
    }
    free(dir), free(db);
done:
    if (password) jd_wipe(password, strlen(password));
    free(password), free(dir_files), free(address);
    jd_account_free(&a);
    return out;
}

/* for SendAlarm after a background check: next_send in epoch seconds, 0 when none */
JNIEXPORT jlong JNICALL Java_io_github_vamsikalagaturu_jackdaw_MailJobService_nativeNextSend(JNIEnv *e, jclass cls,
                                                                                             jstring files)
{
    char *dir_files = jstr(e, files);
    jd_account *all = NULL;
    int n = 0;
    char err[256];
    int64_t at = 0;
    if (dir_files) setup_env(dir_files);
    /* the soonest of every account's */
    if (dir_files && jd_accounts_load(&all, &n, err, sizeof err) == 0) {
        for (int i = 0; i < n; i++) {
            char *dir = jd_account_dir(&all[i]), *db = jd_printf("%s/mail.db", dir);
            jd_store *view = jd_store_open(db, err, sizeof err);
            int64_t next = view ? next_send(view, dir) : 0;
            if (next && (!at || next < at)) at = next;
            jd_store_close(view);
            free(dir), free(db);
        }
        jd_accounts_free(all, n);
    }
    free(dir_files);
    return (jlong)at;
}

float plat_density(void) { return GetWindowScaleDPI().x; }
int plat_touch(void) { return 1; }

/* ---- what the interface reads: one state change per frame ---- */

/* steps applied one per frame, at its start: a tap is "down at x,y", then "up", then "away" */
typedef struct {
    int kind; /* 1 press (button at x, y), 2 release (button), 3 pointer away, 4 key down, 5 key up */
    int button, x, y, key;
} step;

static step steps[256];
static int nsteps;

static void push_step(step s)
{
    if (nsteps < (int)(sizeof steps / sizeof *steps)) steps[nsteps++] = s;
}

static void automation(int type, int a, int b, int c)
{
    AutomationEvent e = {0};
    e.type = (unsigned int)type;
    e.params[0] = a, e.params[1] = b, e.params[2] = c;
    PlayAutomationEvent(e);
}

static void apply_step(void)
{
    if (!nsteps) return;
    step s = steps[0];
    memmove(steps, steps + 1, sizeof *steps * (size_t)--nsteps);
    switch (s.kind) {
    case 1:
        automation(EV_MOUSE_POSITION, s.x, s.y, 0);
        automation(EV_TOUCH_DOWN, s.button, 0, 0);
        break;
    case 2: automation(EV_TOUCH_UP, s.button, 0, 0); break;
    case 3: automation(EV_MOUSE_POSITION, -10000, -10000, 0); break;
    case 4: automation(EV_KEY_DOWN, s.key, 0, 0); break;
    case 5: automation(EV_KEY_UP, s.key, 0, 0); break;
    }
}

static void tap(int button, float x, float y)
{
    push_step((step){.kind = 1, .button = button, .x = (int)x, .y = (int)y});
    push_step((step){.kind = 2, .button = button});
    push_step((step){.kind = 3});
}

static void key_tap(int key)
{
    push_step((step){.kind = 4, .key = key});
    push_step((step){.kind = 5, .key = key});
}

/* the text of this frame (GetCharPressed), and what waits for later frames */
static int frame_chars[256], nframe_chars, frame_char_at;
static float frame_wheel, wheel_acc, frame_wheel_x, wheel_acc_x;
/* the same finger movement in screen pixels (plat_scroll) */
static float frame_px, px_acc, frame_px_x, px_acc_x;

int __real_GetCharPressed(void);
int __wrap_GetCharPressed(void)
{
    return frame_char_at < nframe_chars ? frame_chars[frame_char_at++] : 0;
}

/* the vertical axis only: raylib's returns the larger one, which turned a sideways swipe into a scroll */
float __real_GetMouseWheelMove(void);
Vector2 __real_GetMouseWheelMoveV(void);
float __wrap_GetMouseWheelMove(void)
{
    return frame_wheel + __real_GetMouseWheelMoveV().y;
}

Vector2 __wrap_GetMouseWheelMoveV(void)
{
    Vector2 m = __real_GetMouseWheelMoveV();
    return (Vector2){frame_wheel_x + m.x, frame_wheel + m.y};
}

/* raylib's Android clipboard does nothing; Java's does */
void __real_SetClipboardText(const char *text);
void __wrap_SetClipboardText(const char *text)
{
    free(java("setClipboard", 1, text ? text : ""));
}

const char *__real_GetClipboardText(void);
const char *__wrap_GetClipboardText(void)
{
    static char *last;
    free(last);
    last = java("clipboard", 0);
    return last;
}

/* ---- touch ---- */

#define LONG_PRESS_S 0.4     /* ViewConfiguration.getLongPressTimeout() */
#define SLOP_DP 8.0f         /* ViewConfiguration's touch slop */
#define NOTCH_DP 40.0f       /* finger travel per wheel notch */
#define FLING_MIN_DP 50.0f   /* per second, at release: ViewConfiguration's minimum fling velocity */
#define FLING_MAX_DP 8000.0f /* per second: its maximum */

static struct {
    int pending, scrolling, held;
    int stopped_fling; /* the touch stopped a moving list: no tap, axis still open */
    int moved;         /* past the slop: no tap or long press, even with the axis undecided */
    int sideways; /* the swipe went more across than down when it started: it scrolls sideways */
    int down;
    double t;
    float x, y, cx, cy;
    float fx, fy; /* where the finger is now */
} touch;
static float inset_top_px;
static float fling_v; /* release velocity, px/s; 0 when nothing flings */

/* Distance and duration as android.widget.OverScroller (getSplineFlingDistance/Duration) with ViewConfiguration's
   friction; the content follows x(t) = d (1 - (1 - t/T)^p), p = v T / d so it starts at the finger's speed. */
static struct {
    double t0, T;
    float d, p, done;
} fling;

static void fling_start(float v)
{
    const double DECELERATION_RATE = log(0.78) / log(0.9), INFLEXION = 0.35, FRICTION = 0.015;
    /* physical coefficient: g (m/s^2) * inches per meter * pixels per inch * 0.84, as OverScroller computes it */
    double ppi = 160.0 * plat_density(), phys = 9.80665 * 39.37 * ppi * 0.84;
    double l = log(INFLEXION * fabs(v) / (FRICTION * phys));
    double dist = FRICTION * phys * exp(DECELERATION_RATE / (DECELERATION_RATE - 1) * l);
    double dur = exp(l / (DECELERATION_RATE - 1));
    if (!(dist > 1) || !(dur > 0.01)) return;
    fling = (typeof(fling)){.t0 = now(), .T = dur, .d = (float)(v < 0 ? -dist : dist), .p = (float)(fabs(v) * dur / dist)};
    fling_v = v;
}
/* frames after a tap until the keyboard is re-shown: Back closes it while the field keeps the focus, and the
   tap's click may move the focus first */
static int ime_recheck;
/* plat_swipe: -1 or 1 pages that way, 2 springs back. Pages past a quarter of the width, or past SWIPE_DP when
   flung, as ViewPager decides. */
#define SWIPE_DP 40.0f
#define PAGE_FLING_DP 400.0f /* ViewPager's MIN_FLING_VELOCITY, dp per second */
static int swipe_pending, frame_swipe;
int plat_swipe(void) { return frame_swipe; }
/* screen pixels */
static float drag_dx, release_dx, frame_release_dx;
float plat_drag_x(void) { return (frame_swipe ? frame_release_dx : drag_dx) / jd_scale; }

typedef struct {
    double t;
    float x, y;
} sample;
static sample samples[32];
static int nsamples;

static float dp(void) { return plat_density(); }

static void add_samples(const AInputEvent *e)
{
    int hist = (int)AMotionEvent_getHistorySize(e);
    for (int h = 0; h <= hist; h++) {
        float x = h < hist ? AMotionEvent_getHistoricalX(e, 0, (size_t)h) : AMotionEvent_getX(e, 0);
        float y = h < hist ? AMotionEvent_getHistoricalY(e, 0, (size_t)h) : AMotionEvent_getY(e, 0);
        double t = (h < hist ? AMotionEvent_getHistoricalEventTime(e, (size_t)h) : AMotionEvent_getEventTime(e)) / 1e9;
        if (nsamples == 32) memmove(samples, samples + 1, sizeof samples[0] * 31), nsamples--;
        samples[nsamples++] = (sample){t, x, y};
    }
}

/* px/s over the last 100 ms along x (sideways) or y; none if the finger rested before lifting */
static int release_velocity(double up_t, int sideways, float *v)
{
    if (nsamples < 2 || up_t - samples[nsamples - 1].t > 0.05) return 0;
    int first = nsamples - 1;
    while (first > 0 && samples[nsamples - 1].t - samples[first - 1].t <= 0.1) first--;
    double dt = samples[nsamples - 1].t - samples[first].t;
    if (dt < 0.005) return 0;
    const sample *a = &samples[first], *b = &samples[nsamples - 1];
    *v = (float)((sideways ? b->x - a->x : b->y - a->y) / dt);
    return 1;
}

/* the pointer sits where the finger is while it scrolls, so the wheel goes to what is under it */
static void point_at(float x, float y)
{
    automation(EV_MOUSE_POSITION, (int)x, (int)y, 0);
}

/* A tap clicks when the finger lifts, a moving finger scrolls (and flings when let go), a finger held still
   right-clicks at the long press time (plat_frame). */
int plat_finger_scrolling(void) { return touch.scrolling && !touch.sideways; }

int plat_finger(float *x, float *y)
{
    if (!touch.down) return 0;
    *x = touch.fx / jd_scale, *y = (touch.fy - inset_top_px) / jd_scale;
    return 1;
}

void plat_finger_claim(void)
{
    if (!touch.down) return;
    touch.held = 1, touch.pending = touch.scrolling = touch.sideways = 0;
    drag_dx = 0;
    px_acc = px_acc_x = wheel_acc = wheel_acc_x = 0;
    frame_px = frame_px_x = 0;
    frame_wheel = frame_wheel_x = 0;
}

int plat_scroll(float *dx, float *dy)
{
    if (!frame_px && !frame_px_x) return 0;
    *dx = frame_px_x / jd_scale, *dy = frame_px / jd_scale;
    return 1;
}

static void on_touch(const AInputEvent *e, int action)
{
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    touch.fx = x, touch.fy = y;
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        touch.down = 1;
        touch.pending = 1, touch.scrolling = touch.held = 0;
        touch.t = now(), touch.x = touch.cx = x, touch.y = touch.cy = y, touch.sideways = touch.moved = 0;
        nsamples = 0;
        add_samples(e);
        /* a finger on a moving list stops it, and that touch is no tap; its axis is still decided as for any other */
        touch.stopped_fling = fling_v != 0;
        fling_v = 0;
        break;
    case AMOTION_EVENT_ACTION_MOVE: {
        if (touch.held) break;
        add_samples(e);
        /* the axis is kept for the whole gesture; across must lead 1.5 times, as scrolling is the common case */
        float ax = fabsf(x - touch.x), ay = fabsf(y - touch.y), slop = SLOP_DP * dp();
        if (ax > slop || ay > slop) touch.moved = 1;
        if (touch.pending && (ax > slop || ay > slop)) {
            int across = ax > slop && ax > 1.5f * ay, down = ay > slop && ay >= ax;
            if (!across && !down && hypotf(ax, ay) > 4 * slop) across = ax > ay, down = !across;
            if (across || down) {
                touch.pending = 0, touch.scrolling = 1, touch.sideways = across;
                touch.cx = touch.x, touch.cy = touch.y;
            }
        }
        if (touch.scrolling && touch.sideways) drag_dx = x - touch.x;
        if (touch.scrolling) {
            point_at(touch.x, touch.y);
            if (touch.sideways) wheel_acc_x += (x - touch.cx) / (NOTCH_DP * dp()), px_acc_x += x - touch.cx;
            else wheel_acc += (y - touch.cy) / (NOTCH_DP * dp()), px_acc += y - touch.cy;
            touch.cx = x, touch.cy = y;
        }
        break;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL: {
        int sideways = touch.scrolling && touch.sideways;
        if (sideways) release_dx = x - touch.x;
        if (action == AMOTION_EVENT_ACTION_UP && !touch.held) {
            add_samples(e);
            if (touch.pending && !touch.moved && !touch.stopped_fling) tap(MOUSE_BUTTON_LEFT, touch.x, touch.y), ime_recheck = 4;
            float dx = x - touch.x, v = 0;
            int fast = release_velocity(AMotionEvent_getEventTime(e) / 1e9, sideways, &v) && fabsf(v) > FLING_MIN_DP * dp();
            if (sideways) {
                int far = fabsf(dx) > 0.25f * (float)ANativeWindow_getWidth(GetAndroidApp()->window);
                int flung = fabsf(v) > PAGE_FLING_DP * dp() && fabsf(dx) > SWIPE_DP * dp() && (v < 0) == (dx < 0);
                swipe_pending = far || flung ? (dx < 0 ? -1 : 1) : 2;
            } else if (touch.scrolling && fast) {
                fling_start(fmaxf(-FLING_MAX_DP * dp(), fminf(FLING_MAX_DP * dp(), v)));
            }
        } else if (sideways) {
            swipe_pending = 2;
        }
        touch.pending = touch.scrolling = touch.held = touch.stopped_fling = touch.moved = touch.down = 0;
        drag_dx = 0;
        break;
    }
    }
}

/* ---- keys and the soft keyboard ---- */

/* JackdawActivity's soft keyboard text: code points, and these */
#define IME_BACKSPACE -1
#define IME_ENTER -2
#define IME_DELETE -3
static pthread_mutex_t ime_lock = PTHREAD_MUTEX_INITIALIZER;
static int ime_q[1024], n_ime;

static void ime_push(int c)
{
    pthread_mutex_lock(&ime_lock);
    if (n_ime < (int)(sizeof ime_q / sizeof *ime_q)) ime_q[n_ime++] = c;
    pthread_mutex_unlock(&ime_lock);
}

static void JNICALL native_text(JNIEnv *e, jclass cls, jintArray codes)
{
    jsize n = (*e)->GetArrayLength(e, codes);
    jint *v = (*e)->GetIntArrayElements(e, codes, NULL);
    if (!v) return;
    for (jsize i = 0; i < n; i++) ime_push(v[i]);
    (*e)->ReleaseIntArrayElements(e, codes, v, JNI_ABORT);
    ALooper_wake(GetAndroidApp()->looper);
}

/* this frame's text: the queue's characters up to a Backspace, Enter or Delete, which become a key press (two
   frames) before the text after them, so the order typed is kept */
static void take_text(void)
{
    nframe_chars = frame_char_at = 0;
    if (nsteps) return;
    pthread_mutex_lock(&ime_lock);
    int i = 0;
    for (; i < n_ime && nframe_chars < (int)(sizeof frame_chars / sizeof *frame_chars); i++) {
        int c = ime_q[i];
        if (c >= 0) {
            frame_chars[nframe_chars++] = c;
            continue;
        }
        if (nframe_chars) break; /* the key after this frame's text */
        key_tap(c == IME_ENTER ? KEY_ENTER : c == IME_DELETE ? KEY_DELETE : KEY_BACKSPACE);
        i++;
        break;
    }
    memmove(ime_q, ime_q + i, sizeof *ime_q * (size_t)(n_ime - i));
    n_ime -= i;
    pthread_mutex_unlock(&ime_lock);
}

/* the character a hardware key types; the NDK has no getUnicodeChar(), android.view.KeyEvent does */
static int key_char(int code, int meta)
{
    static jclass cls;
    static jmethodID ctor, get;
    JNIEnv *e = jni();
    if (!cls) {
        jclass c = (*e)->FindClass(e, "android/view/KeyEvent");
        if (jfailed(e) || !c) return 0;
        cls = (*e)->NewGlobalRef(e, c);
        (*e)->DeleteLocalRef(e, c);
        ctor = (*e)->GetMethodID(e, cls, "<init>", "(JJIIII)V");
        get = (*e)->GetMethodID(e, cls, "getUnicodeChar", "(I)I");
    }
    jobject k = (*e)->NewObject(e, cls, ctor, (jlong)0, (jlong)0, (jint)AKEY_EVENT_ACTION_DOWN, (jint)code, (jint)0,
                                (jint)meta);
    if (jfailed(e) || !k) return 0;
    int ch = (*e)->CallIntMethod(e, k, get, (jint)meta);
    (*e)->DeleteLocalRef(e, k);
    return jfailed(e) ? 0 : ch;
}

/* ---- picked files ---- */

static pthread_mutex_t picked_lock = PTHREAD_MUTEX_INITIALIZER;
static char *picked_paths; /* newline-separated, from Java */
static int picked_ready;

static void JNICALL native_picked(JNIEnv *e, jclass cls, jstring paths)
{
    char *s = jstr(e, paths);
    pthread_mutex_lock(&picked_lock);
    free(picked_paths);
    picked_paths = s;
    picked_ready = 1;
    pthread_mutex_unlock(&picked_lock);
    ALooper_wake(GetAndroidApp()->looper);
}

/* ---- input and activity events ---- */

static int32_t (*raylib_input)(struct android_app *, AInputEvent *);

static int32_t on_input(struct android_app *a, AInputEvent *e)
{
    int type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e), meta = AKeyEvent_getMetaState(e);
        /* Back is Esc: it closes what is open, as Esc does */
        if (code == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(e) == 0) key_tap(KEY_ESCAPE);
            return 1;
        }
        int32_t handled = raylib_input(a, e);
        if (action == AKEY_EVENT_ACTION_DOWN && !(meta & (AMETA_CTRL_ON | AMETA_ALT_ON | AMETA_META_ON))) {
            int ch = key_char(code, meta);
            if (ch >= 32 && ch != 127) ime_push(ch);
        }
        return handled;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, source = AInputEvent_getSource(e);
        /* a mouse (a desktop session, the emulator's pointer with a mouse) is raylib's; a finger is ours */
        if (source & AINPUT_SOURCE_TOUCHSCREEN && (source & AINPUT_SOURCE_MOUSE) != AINPUT_SOURCE_MOUSE) {
            on_touch(e, action);
            return 1;
        }
    }
    return raylib_input(a, e);
}

void plat_window_ready(void)
{
    /* the display's rate (often 90 or 120 Hz): 60 there makes scrolling look heavy */
    char *hz = java("refreshRate", 0);
    int fps = hz ? (int)(atof(hz) + 0.5) : 60;
    free(hz);
    SetTargetFPS(fps >= 30 && fps <= 240 ? fps : 60);
    struct android_app *a = GetAndroidApp();
    /* raylib sets AWINDOW_FLAG_FULLSCREEN at start, which hides the status bar */
    ANativeActivity_setWindowFlags(a->activity, 0, AWINDOW_FLAG_FULLSCREEN);
    raylib_input = a->onInputEvent;
    a->onInputEvent = on_input;
    /* NativeActivity loads this library itself, so the class does not find its natives by name */
    JNIEnv *e = jni();
    jclass c = (*e)->GetObjectClass(e, a->activity->clazz);
    static const JNINativeMethod methods[] = {{"nativeText", "([I)V", (void *)native_text},
                                              {"nativePicked", "(Ljava/lang/String;)V", (void *)native_picked}};
    if ((*e)->RegisterNatives(e, c, methods, 2)) jfailed(e);
    (*e)->DeleteLocalRef(e, c);
}

static int keyboard_shown = -1;

void plat_frame(int typing)
{
    double t = now();
    /* a fling slows down until it stops */
    if (fling_v) {
        double k = fmin(1, (t - fling.t0) / fling.T);
        float at = fling.d * (1 - (float)pow(1 - k, fling.p)), delta = at - fling.done;
        fling.done = at;
        wheel_acc += delta / (NOTCH_DP * dp());
        px_acc += delta;
        if (k >= 1) fling_v = 0, push_step((step){.kind = 3});
    }
    frame_wheel = wheel_acc, frame_wheel_x = wheel_acc_x;
    frame_px = px_acc, frame_px_x = px_acc_x;
    px_acc = px_acc_x = 0;
    frame_swipe = swipe_pending, swipe_pending = 0;
    frame_release_dx = release_dx;
    wheel_acc = wheel_acc_x = 0;
    if (touch.pending && !touch.moved && !touch.stopped_fling && t - touch.t >= LONG_PRESS_S) {
        touch.pending = 0, touch.held = 1;
        tap(MOUSE_BUTTON_RIGHT, touch.x, touch.y);
    }
    apply_step();
    take_text();

    if (ime_recheck && --ime_recheck == 0 && typing) keyboard_shown = -1;
    if (typing != keyboard_shown) {
        free(java(typing ? "showKeyboard" : "hideKeyboard", 0));
        keyboard_shown = typing;
    }
    /* what the system covers: status bar and cutout at the top, navigation and the keyboard at the bottom */
    static double insets_at = -1;
    if (insets_at < 0 || t - insets_at > 0.1) {
        insets_at = t;
        char *in = java("systemInsets", 0);
        float l, top, r, bottom;
        if (in && sscanf(in, "%f %f %f %f", &l, &top, &r, &bottom) == 4) jd_set_insets(top, bottom), inset_top_px = top;
        free(in);
    }
}

/* ---- platform.h ---- */

/* the account's password, sealed under an Android Keystore key (JackdawActivity) */
int plat_secret_available(void) { return 1; }

char *plat_secret_load(const char *account)
{
    return java("secretGet", 1, account);
}

int plat_secret_save(const char *account, const char *password)
{
    char *r = java("secretPut", 2, account, password);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

int plat_secret_delete(const char *account)
{
    char *r = java("secretDelete", 1, account);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

static int remove_one(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    return remove(path);
}

/* an app's private files have no trash to go to */
int plat_trash(const char *path)
{
    return nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

int plat_notify(const char *summary, const char *body, int64_t tag)
{
    /* new mail (tag = message id) was notified from the sync thread already (on_new_mail) */
    if (tag) return 0;
    char id[32];
    snprintf(id, sizeof id, "%lld", (long long)(tag ? tag : 1));
    char *r = java("postNotification", 3, summary ? summary : "", body ? body : "", id);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

/* a tapped notification's message; asked twice a second, not every frame (it goes through Java) */
int64_t plat_notify_poll(void)
{
    static double asked;
    if (now() - asked < 0.5) return 0;
    asked = now();
    char *r = java("takeOpened", 0);
    int64_t id = r ? strtoll(r, NULL, 10) : 0;
    free(r);
    return id;
}

void plat_notify_shutdown(void) {}

int plat_install_package(const char *path)
{
    char *r = java("installPackage", 1, path);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

void plat_push(int on)
{
    static int was = -1;
    if (on == was) return;
    was = on;
    free(java("setPush", 1, on ? "1" : "0"));
}

int plat_pick_files(const char *title)
{
    char *r = java("pickFiles", 0);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

char **plat_picked_files(int *n)
{
    *n = 0;
    pthread_mutex_lock(&picked_lock);
    if (!picked_ready) {
        pthread_mutex_unlock(&picked_lock);
        return NULL;
    }
    picked_ready = 0;
    char *s = picked_paths;
    picked_paths = NULL;
    pthread_mutex_unlock(&picked_lock);
    if (!s || !s[0]) {
        free(s);
        return NULL;
    }
    char **out = NULL, *save = NULL;
    for (char *p = strtok_r(s, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
        out = realloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = jd_strdup(p);
    }
    free(s);
    return out;
}

int plat_open_uri(const char *uri)
{
    char *r = java("openUri", 1, uri);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

int plat_share_text(const char *text)
{
    char *r = java("shareText", 1, text);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

char *plat_text_actions(void)
{
    return java("textActions", 0);
}

int plat_text_action(int i, const char *text)
{
    char idx[16];
    snprintf(idx, sizeof idx, "%d", i);
    char *r = java("textAction", 2, idx, text);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

int plat_open_file(const char *path)
{
    char *r = java("openFile", 1, path);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

int plat_print(const char *path, const char *title)
{
    char *r = java("print", 2, path, title ? title : "");
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}
