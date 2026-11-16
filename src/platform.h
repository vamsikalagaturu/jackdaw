#pragma once
#include <stdint.h>

/* The program's life on the platform: plat_init before the window (Android: HOME, XDG_* and certificates),
   plat_window_ready after it (Android: touch and the soft keyboard), plat_frame at the start of every frame,
   typing when a text field has the keys (Android shows the soft keyboard then). */
void plat_init(void);
void plat_window_ready(void);
void plat_frame(int typing);
/* pixels per interface pixel at 100%: 1 on desktops (raylib scales HiDPI itself), the screen density on phones */
float plat_density(void);
/* a touch screen without a keyboard to rely on: no vim modes, typing always where text is edited */
int plat_touch(void);
/* a sideways swipe that ended this frame: -1 or 1 pages (finger went left or right), 2 springs back, 0 none */
int plat_swipe(void);
/* while a sideways swipe is in progress, how far the finger has gone (logical pixels, right positive); else 0 */
float plat_drag_x(void);
/* a finger is on the screen, scrolling vertically */
int plat_finger_scrolling(void);
/* this frame's finger (or fling) scroll in logical pixels, down and right positive; 0 leaves it to the wheel */
int plat_scroll(float *dx, float *dy);

/* The system keystore. Without one (no libsecret at build time) passwords last until jackdaw quits. */
int plat_secret_available(void);
char *plat_secret_load(const char *account);
int plat_secret_save(const char *account, const char *password);
/* forgets the account's password: 0 when it is gone (or was never stored) */
int plat_secret_delete(const char *account);

/* a file or folder to the desktop's trash (freedesktop.org Trash spec); deleted where there is no trash (Android) */
int plat_trash(const char *path);

/* Desktop notifications. The tag of a clicked notification comes back from plat_notify_poll, 0 when none. */
/* -1 when there is no notification service to send to */
int plat_notify(const char *summary, const char *body, int64_t tag);
int64_t plat_notify_poll(void);
/* Android: a foreground service keeps IMAP IDLE open while the app is closed; nothing on a desktop */
void plat_push(int on);
/* Android: hands a downloaded APK to the system's installer (the user confirms); -1 elsewhere */
int plat_install_package(const char *path);
void plat_notify_shutdown(void);

/* The system's file chooser, opened without blocking; the chosen paths arrive later through plat_picked_files
   (NULL until then, and when nothing was chosen). Returns -1 when there is no chooser. */
int plat_pick_files(const char *title);
/* the system's handler for a link (browser for http/https) */
int plat_open_uri(const char *uri);
/* the system's handler for a local file (by its type) */
int plat_open_file(const char *path);
/* the system's print dialog for a PDF file; -1 when there is none */
int plat_print(const char *path, const char *title);
char **plat_picked_files(int *n);
