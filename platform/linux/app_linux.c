/* platform.h's program hooks on Linux: GLFW and raylib already do what they are for elsewhere. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/util.h"
#include "platform.h"
#include "raylib.h"

/* https://specifications.freedesktop.org/trash-spec/latest/ (home trash). The .trashinfo is created first with
   O_EXCL, which reserves the name; rename fails across file systems rather than copying. */
int plat_trash(const char *path)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    char *trash = xdg && xdg[0] == '/' ? jd_printf("%s/Trash", xdg) : jd_printf("%s/.local/share/Trash", getenv("HOME"));
    char *files = jd_printf("%s/files", trash), *info = jd_printf("%s/info", trash);
    jd_mkdirs(files), jd_mkdirs(info);
    char *abs = realpath(path, NULL);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int r = -1;
    for (int n = 1; abs && n < 1000 && r < 0; n++) {
        char *name = n == 1 ? jd_strdup(base) : jd_printf("%s.%d", base, n);
        char *info_path = jd_printf("%s/%s.trashinfo", info, name), *dest = jd_printf("%s/%s", files, name);
        int fd = open(info_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char when[32];
            time_t t = time(NULL);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&t));
            /* Path is a percent-encoded URL path; printable ASCII (an address's '@') is kept */
            jd_buf b = {0};
            jd_buf_addf(&b, "[Trash Info]\nPath=");
            for (const unsigned char *c = (const unsigned char *)abs; *c; c++) {
                if (*c > 0x20 && *c < 0x7f && *c != '%') jd_buf_add(&b, (const char *)c, 1);
                else jd_buf_addf(&b, "%%%02X", *c);
            }
            jd_buf_addf(&b, "\nDeletionDate=%s\n", when);
            int ok = write(fd, b.s, b.len) == (ssize_t)b.len;
            close(fd);
            jd_buf_free(&b);
            if (ok && rename(path, dest) == 0) r = 0;
            else {
                unlink(info_path);
                n = 1000;
            }
        } else if (errno != EEXIST) {
            n = 1000;
        }
        free(name), free(info_path), free(dest);
    }
    free(abs), free(trash), free(files), free(info);
    return r;
}

/* Scrolling. raylib's GLFW callback keeps only a frame's last scroll event (it assigns), dropping the rest of a
   touchpad's; ours adds them up. A wheel's whole notches (Wayland's value120 in steps of 120) glide instead of
   jumping, eased with a 50 ms time constant: 95% in 150 ms, inside the 50-200 ms Firefox animates a notch over
   (general.smoothScroll.mouseWheel.duration*, Mozilla bug 1660933). A touchpad's fractions apply as they come.
   The values reach the app through GetMouseWheelMove, wrapped at link time (CMakeLists.txt) as on Android. */
typedef struct GLFWwindow GLFWwindow;
typedef void (*GLFWscrollfun)(GLFWwindow *, double, double);
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *window, GLFWscrollfun callback);
GLFWwindow *glfwGetCurrentContext(void);

static double direct_x, direct_y, glide_x, glide_y; /* in notches */
static float frame_x, frame_y;

static void add_scroll(double v, double *direct, double *glide)
{
    if (v != 0 && v == floor(v)) *glide += v;
    else *direct += v;
}

static void on_scroll(GLFWwindow *w, double x, double y)
{
    (void)w;
    add_scroll(x, &direct_x, &glide_x);
    add_scroll(y, &direct_y, &glide_y);
}

static float take(double *direct, double *glide, double k)
{
    double step = fabs(*glide) < 0.01 ? *glide : *glide * k;
    *glide -= step;
    float v = (float)(*direct + step);
    *direct = 0;
    return v;
}

Vector2 __real_GetMouseWheelMoveV(void);
float __wrap_GetMouseWheelMove(void) { return frame_y; }
Vector2 __wrap_GetMouseWheelMoveV(void) { return (Vector2){frame_x, frame_y}; }

/* Keys. raylib counts a press only when the key is down at one frame's poll and was up at the poll before, so a
   press and its release read in the same poll are lost to IsKeyPressed, though they reach GetKeyPressed's queue.
   Some keyboards send a tap that way: the record (JD_DEBUG_KEYS) showed every Esc tap arriving released, the key
   in the queue, and Esc needing four or five taps. The frame's queue is taken here; a key in it counts as pressed.
   IsKeyPressed is wrapped at link time (CMakeLists.txt); nothing else reads GetKeyPressed. */
static unsigned char tapped[512];

bool __real_IsKeyPressed(int key);
bool __wrap_IsKeyPressed(int key) { return __real_IsKeyPressed(key) || (key > 0 && key < 512 && tapped[key]); }

static void take_taps(void)
{
    memset(tapped, 0, sizeof tapped);
    for (int k = GetKeyPressed(); k; k = GetKeyPressed())
        if (k > 0 && k < 512) tapped[k] = 1;
}

void plat_init(void) {}
/* GetWindowHandle is the native window (a wl_surface on Wayland), not GLFW's: raylib's window is the one whose
   context it made current */
void plat_window_ready(void)
{
    GLFWwindow *w = glfwGetCurrentContext();
    if (w) glfwSetScrollCallback(w, on_scroll);
}

void plat_frame(int typing)
{
    (void)typing;
    take_taps();
    double k = 1 - exp(-GetFrameTime() / 0.05);
    /* raylib's own value: what its automation events (the tests) set */
    Vector2 raylib = __real_GetMouseWheelMoveV();
    frame_x = take(&direct_x, &glide_x, k) + raylib.x;
    frame_y = take(&direct_y, &glide_y, k) + raylib.y;
}
float plat_density(void) { return 1; }
int plat_touch(void) { return 0; }
int plat_swipe(void) { return 0; }
int plat_install_package(const char *path) { (void)path; return -1; }
void plat_push(int on) { (void)on; }
void plat_wake_at(int64_t t) { (void)t; }
int plat_reminders(int n, const int64_t *at, const char *const *title, const char *const *text)
{
    (void)n, (void)at, (void)title, (void)text;
    return 0;
}
float plat_drag_x(void) { return 0; }
int plat_finger_scrolling(void) { return 0; }
int plat_scroll(float *dx, float *dy) { (void)dx, (void)dy; return 0; }
int plat_finger(float *x, float *y) { (void)x, (void)y; return 0; }
void plat_finger_claim(void) {}
int plat_share_text(const char *text) { (void)text; return -1; }
char *plat_text_actions(void) { return NULL; }
int plat_text_action(int i, const char *text) { (void)i, (void)text; return -1; }
