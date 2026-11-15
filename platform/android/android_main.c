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
#include <android_native_app_glue.h>
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
#include "platform.h"
#include "raylib.h"
#include "ui/theme.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "jackdaw", __VA_ARGS__)

struct android_app *GetAndroidApp(void); /* rcore_android.c; not in raylib.h */

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

void plat_init(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    const char *files = act->internalDataPath;
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
    /* OpenSSL (IMAP, SMTP, Sieve, images) reads the system's trusted roots from here: hashed PEM files */
    setenv("SSL_CERT_DIR", "/system/etc/security/cacerts", 1);
#ifdef JD_HAVE_TEST_CA
    /* a test build trusts the test server's CA too (tests/imap) */
    char *ca = jd_printf("%s/test-ca.crt", files);
    jd_write_file(ca, (const char *)jd_test_ca, (size_t)jd_test_ca_size, 0600);
    setenv("SSL_CERT_FILE", ca, 1);
    free(ca);
#endif
    if (chdir(files)) LOG("chdir %s failed", files);
    free(config), free(data), free(cache), free(run), free(downloads);
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

int __real_GetCharPressed(void);
int __wrap_GetCharPressed(void)
{
    return frame_char_at < nframe_chars ? frame_chars[frame_char_at++] : 0;
}

float __real_GetMouseWheelMove(void);
/* as raylib's: the larger of the two axes (a sideways swipe scrolls the tab bar) */
float __wrap_GetMouseWheelMove(void)
{
    return fabsf(frame_wheel_x) > fabsf(frame_wheel) ? frame_wheel_x : frame_wheel;
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
#define FLING_MIN_DP 100.0f  /* per second, at release */
#define FLING_MAX_DP 3500.0f /* per second */
#define FLING_DECAY 2.6f     /* per second */
#define FLING_STOP_DP 40.0f  /* per second */

static struct {
    int pending, scrolling, held;
    int sideways; /* the swipe went more across than down when it started: it scrolls sideways */
    double t;
    float x, y, cx, cy;
} touch;
static float fling_v;

typedef struct {
    double t;
    float y;
} sample;
static sample samples[32];
static int nsamples;

static float dp(void) { return plat_density(); }

static void add_samples(const AInputEvent *e)
{
    int hist = (int)AMotionEvent_getHistorySize(e);
    for (int h = 0; h <= hist; h++) {
        float y = h < hist ? AMotionEvent_getHistoricalY(e, 0, (size_t)h) : AMotionEvent_getY(e, 0);
        double t = (h < hist ? AMotionEvent_getHistoricalEventTime(e, (size_t)h) : AMotionEvent_getEventTime(e)) / 1e9;
        if (nsamples == 32) memmove(samples, samples + 1, sizeof samples[0] * 31), nsamples--;
        samples[nsamples++] = (sample){t, y};
    }
}

/* px/s over the last 100 ms; none if the finger rested before lifting */
static int release_velocity(double up_t, float *v)
{
    if (nsamples < 2 || up_t - samples[nsamples - 1].t > 0.05) return 0;
    int first = nsamples - 1;
    while (first > 0 && samples[nsamples - 1].t - samples[first - 1].t <= 0.1) first--;
    double dt = samples[nsamples - 1].t - samples[first].t;
    if (dt < 0.005) return 0;
    *v = (float)((samples[nsamples - 1].y - samples[first].y) / dt);
    return 1;
}

/* the pointer sits where the finger is while it scrolls, so the wheel goes to what is under it */
static void point_at(float x, float y)
{
    automation(EV_MOUSE_POSITION, (int)x, (int)y, 0);
}

/* A tap clicks when the finger lifts, a moving finger scrolls (and flings when let go), a finger held still
   right-clicks at the long press time (plat_frame). */
static void on_touch(const AInputEvent *e, int action)
{
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        touch.pending = 1, touch.scrolling = touch.held = 0;
        touch.t = now(), touch.x = touch.cx = x, touch.y = touch.cy = y, touch.sideways = 0;
        nsamples = 0;
        add_samples(e);
        /* a finger on a moving list stops it, and that touch is no tap */
        if (fling_v) touch.pending = 0, touch.scrolling = 1;
        fling_v = 0;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        if (touch.held) break;
        add_samples(e);
        if (touch.pending && hypotf(x - touch.x, y - touch.y) > SLOP_DP * dp()) {
            touch.pending = 0, touch.scrolling = 1;
            touch.sideways = fabsf(x - touch.x) > fabsf(y - touch.y);
        }
        if (touch.scrolling) {
            point_at(touch.x, touch.y);
            if (touch.sideways) wheel_acc_x += (x - touch.cx) / (NOTCH_DP * dp());
            else wheel_acc += (y - touch.cy) / (NOTCH_DP * dp());
            touch.cx = x, touch.cy = y;
        }
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (action == AMOTION_EVENT_ACTION_UP && !touch.held) {
            add_samples(e);
            if (touch.pending) tap(MOUSE_BUTTON_LEFT, touch.x, touch.y);
            float v;
            if (touch.scrolling && !touch.sideways && release_velocity(AMotionEvent_getEventTime(e) / 1e9, &v) &&
                fabsf(v) > FLING_MIN_DP * dp())
                fling_v = fmaxf(-FLING_MAX_DP * dp(), fminf(FLING_MAX_DP * dp(), v));
        }
        touch.pending = touch.scrolling = touch.held = 0;
        break;
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
    SetTargetFPS(60);
    struct android_app *a = GetAndroidApp();
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
    static double last;
    double t = now(), dt = fmin(t - last, 0.05);
    last = t;
    /* a fling slows down until it stops */
    if (fling_v) {
        wheel_acc += fling_v * (float)dt / (NOTCH_DP * dp());
        fling_v *= expf(-FLING_DECAY * (float)dt);
        if (fabsf(fling_v) < FLING_STOP_DP * dp()) fling_v = 0, push_step((step){.kind = 3});
    }
    frame_wheel = wheel_acc, frame_wheel_x = wheel_acc_x;
    wheel_acc = wheel_acc_x = 0;
    if (touch.pending && t - touch.t >= LONG_PRESS_S) {
        touch.pending = 0, touch.held = 1;
        tap(MOUSE_BUTTON_RIGHT, touch.x, touch.y);
    }
    apply_step();
    take_text();

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
        if (in && sscanf(in, "%f %f %f %f", &l, &top, &r, &bottom) == 4) jd_set_insets(top, bottom);
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

/* notifications come later on Android */
int plat_notify(const char *summary, const char *body, int64_t tag) { return -1; }
int64_t plat_notify_poll(void) { return 0; }
void plat_notify_shutdown(void) {}

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

int plat_open_file(const char *path)
{
    char *r = java("openFile", 1, path);
    int ok = r != NULL;
    free(r);
    return ok ? 0 : -1;
}

/* printing comes later on Android */
int plat_print(const char *path, const char *title) { return -1; }
