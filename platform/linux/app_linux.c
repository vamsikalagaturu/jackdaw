/* platform.h's program hooks on Linux: GLFW and raylib already do what they are for elsewhere. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/util.h"
#include "platform.h"

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

void plat_init(void) {}
void plat_window_ready(void) {}
void plat_frame(int typing) { (void)typing; }
float plat_density(void) { return 1; }
int plat_touch(void) { return 0; }
int plat_swipe(void) { return 0; }
int plat_install_package(const char *path) { (void)path; return -1; }
void plat_push(int on) { (void)on; }
void plat_wake_at(int64_t t) { (void)t; }
float plat_drag_x(void) { return 0; }
int plat_finger_scrolling(void) { return 0; }
int plat_scroll(float *dx, float *dy) { (void)dx, (void)dy; return 0; }
int plat_finger(float *x, float *y) { (void)x, (void)y; return 0; }
void plat_finger_claim(void) {}
int plat_share_text(const char *text) { (void)text; return -1; }
char *plat_text_actions(void) { return NULL; }
int plat_text_action(int i, const char *text) { (void)i, (void)text; return -1; }
