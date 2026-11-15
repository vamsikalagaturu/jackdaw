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

/* The system keystore. Without one (no libsecret at build time) passwords last until jackdaw quits. */
int plat_secret_available(void);
char *plat_secret_load(const char *account);
int plat_secret_save(const char *account, const char *password);

/* Desktop notifications. The tag of a clicked notification comes back from plat_notify_poll, 0 when none. */
/* -1 when there is no notification service to send to */
int plat_notify(const char *summary, const char *body, int64_t tag);
int64_t plat_notify_poll(void);
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
