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
/* where the finger is (logical pixels), 0 when none is down */
int plat_finger(float *x, float *y);
/* the finger now down is the caller's until it lifts: no scroll, swipe, tap or long press comes of it */
void plat_finger_claim(void);

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
/* new mail's tag: the message id with its account's place in [[account]] above bit 48 (the first's is the id) */
static inline int64_t notify_tag(int account, int64_t id)
{
    return ((int64_t)account << 48) | (id & ((1LL << 48) - 1));
}
static inline void notify_untag(int64_t tag, int *account, int64_t *id)
{
    *account = (int)(tag >> 48);
    *id = tag & ((1LL << 48) - 1);
}
/* Android: a foreground service keeps IMAP IDLE open while the app is closed; nothing on a desktop */
void plat_push(int on);
/* Android: wakes a check at t (epoch seconds) to send a scheduled draft, 0 for none; nothing on a desktop, where
   the running worker keeps time itself */
void plat_wake_at(int64_t t);
/* Calendar reminders: the next ones, at[i] (epoch seconds) with title[i] and text[i]. Android sets them as alarms
   that notify with the app closed and returns 1; a desktop returns 0, and the running app notifies itself. */
int plat_reminders(int n, const int64_t *at, const char *const *title, const char *const *text);
/* Android: hands a downloaded APK to the system's installer (the user confirms); -1 elsewhere */
int plat_install_package(const char *path);
void plat_notify_shutdown(void);

/* The system's file chooser, opened without blocking; the chosen paths arrive later through plat_picked_files
   (NULL until then, and when nothing was chosen). Returns -1 when there is no chooser. */
int plat_pick_files(const char *title);
/* the system's handler for a link (browser for http/https) */
int plat_open_uri(const char *uri);
/* the system's share sheet for text; -1 when there is none */
int plat_share_text(const char *text);
/* the system's actions on selected text (Android: apps taking PROCESS_TEXT, such as Translate): their labels one
   per line, NULL when there are none; free() it. plat_text_action runs the i-th line's on text. */
char *plat_text_actions(void);
int plat_text_action(int i, const char *text);
/* the system's handler for a local file (by its type) */
int plat_open_file(const char *path);
/* the system's print dialog for a PDF file; -1 when there is none */
int plat_print(const char *path, const char *title);
char **plat_picked_files(int *n);
