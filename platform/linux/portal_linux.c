/* org.freedesktop.portal.FileChooser over sd-bus:
   https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "core/util.h"
#include "platform.h"

static sd_bus *bus;
static char *request_path;
static char **picked;
static int npicked, have_answer;

static char *uri_to_path(const char *uri)
{
    if (strncmp(uri, "file://", 7) != 0) return NULL;
    jd_buf b = {0};
    for (const char *p = uri + 7; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            char c = (char)strtol(hex, NULL, 16);
            jd_buf_add(&b, &c, 1);
            p += 2;
        } else {
            jd_buf_add(&b, p, 1);
        }
    }
    return b.s;
}

static int on_response(sd_bus_message *m, void *data, sd_bus_error *err)
{
    if (!request_path || strcmp(sd_bus_message_get_path(m), request_path) != 0) return 0;
    uint32_t response;
    have_answer = 1;
    if (sd_bus_message_read(m, "u", &response) < 0 || response != 0) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key;
        sd_bus_message_read(m, "s", &key);
        if (strcmp(key, "uris") == 0 && sd_bus_message_enter_container(m, 'v', "as") > 0) {
            sd_bus_message_enter_container(m, 'a', "s");
            const char *uri;
            while (sd_bus_message_read(m, "s", &uri) > 0) {
                char *path = uri_to_path(uri);
                if (!path) continue;
                picked = jd_realloc(picked, sizeof *picked * (size_t)(npicked + 1));
                picked[npicked++] = path;
            }
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    return 0;
}

int plat_pick_files(const char *title)
{
    static int listening;
    if (!bus && sd_bus_open_user(&bus) < 0) return -1;
    if (!listening) {
        sd_bus_match_signal(bus, NULL, "org.freedesktop.portal.Desktop", NULL, "org.freedesktop.portal.Request",
                            "Response", on_response, NULL);
        listening = 1;
    }
    /* the reply's object path is known in advance from our name and token, so no Response is missed */
    const char *unique;
    if (sd_bus_get_unique_name(bus, &unique) < 0) return -1;
    char token[32];
    snprintf(token, sizeof token, "jackdaw%d_%ld", getpid(), (long)jd_now());
    char *sender = jd_strdup(unique + 1);
    for (char *c = sender; *c; c++)
        if (*c == '.') *c = '_';
    free(request_path);
    request_path = jd_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    free(sender);
    have_answer = 0;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                               "org.freedesktop.portal.FileChooser", "OpenFile", &err, &reply, "ssa{sv}", "", title, 2,
                               "multiple", "b", 1, "handle_token", "s", token);
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r < 0 ? -1 : 0;
}

/* org.freedesktop.portal.OpenURI */
int plat_open_uri(const char *uri)
{
    if (!bus && sd_bus_open_user(&bus) < 0) return -1;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                               "org.freedesktop.portal.OpenURI", "OpenURI", &err, &reply, "ssa{sv}", "", uri, 0);
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r < 0 ? -1 : 0;
}

/* OpenURI.OpenFile takes the file as a descriptor, which also works from inside a sandbox:
   https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.OpenURI.html */
int plat_open_file(const char *path)
{
    if (!bus && sd_bus_open_user(&bus) < 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                               "org.freedesktop.portal.OpenURI", "OpenFile", &err, &reply, "sha{sv}", "", fd, 0);
    close(fd);
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r < 0 ? -1 : 0;
}

/* Print without a PreparePrint token: the portal shows its print dialog itself.
   https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Print.html */
int plat_print(const char *path, const char *title)
{
    if (!bus && sd_bus_open_user(&bus) < 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                               "org.freedesktop.portal.Print", "Print", &err, &reply, "ssha{sv}", "", title ? title : "",
                               fd, 0);
    close(fd);
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r < 0 ? -1 : 0;
}

char **plat_picked_files(int *n)
{
    *n = 0;
    if (!bus) return NULL;
    while (sd_bus_process(bus, NULL) > 0) {}
    if (!have_answer) return NULL;
    have_answer = 0;
    char **out = picked;
    *n = npicked;
    picked = NULL;
    npicked = 0;
    return out;
}
