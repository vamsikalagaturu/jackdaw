/* Desktop Notifications Specification 1.2 over sd-bus:
   https://specifications.freedesktop.org/notification-spec/latest/ */
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include "core/util.h"
#include "platform.h"

#define DEST "org.freedesktop.Notifications"
#define PATH "/org/freedesktop/Notifications"

typedef struct {
    uint32_t id;
    int64_t tag;
} shown;

static sd_bus *bus;
static int bus_failed;
static shown recent[16];
static int recent_next;
static int64_t clicked;

static int on_action(sd_bus_message *m, void *data, sd_bus_error *err)
{
    uint32_t id;
    const char *key;
    if (sd_bus_message_read(m, "us", &id, &key) < 0 || strcmp(key, "default") != 0) return 0;
    for (int i = 0; i < 16; i++)
        if (recent[i].id == id && recent[i].tag) clicked = recent[i].tag;
    return 0;
}

static int on_reply(sd_bus_message *m, void *data, sd_bus_error *err)
{
    uint32_t id;
    if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "u", &id) < 0) return 0;
    recent[recent_next] = (shown){id, (int64_t)(intptr_t)data};
    recent_next = (recent_next + 1) % 16;
    return 0;
}

static int connect_bus(void)
{
    if (bus) return 0;
    if (bus_failed || sd_bus_open_user(&bus) < 0) {
        bus_failed = 1;
        bus = NULL;
        return -1;
    }
    sd_bus_match_signal(bus, NULL, DEST, PATH, DEST, "ActionInvoked", on_action, NULL);
    return 0;
}

/* servers that support body markup read the body as XML-ish text */
static char *escape(const char *s)
{
    jd_buf b = {0};
    for (; s && *s; s++) {
        if (*s == '<') jd_buf_adds(&b, "&lt;");
        else if (*s == '>') jd_buf_adds(&b, "&gt;");
        else if (*s == '&') jd_buf_adds(&b, "&amp;");
        else jd_buf_add(&b, s, 1);
    }
    return b.s ? b.s : jd_strdup("");
}

int plat_notify(const char *summary, const char *body, int64_t tag)
{
    if (connect_bus() < 0) return -1;
    char *text = escape(body);
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(bus, &m, DEST, PATH, DEST, "Notify");
    if (r >= 0)
        r = sd_bus_message_append(m, "susssasa{sv}i", JD_APP_NAME, 0u, "mail-unread", summary, text, 2, "default", "Open",
                                  1, "category", "s", "email.arrived", -1);
    if (r >= 0) r = sd_bus_call_async(bus, NULL, m, on_reply, (void *)(intptr_t)tag, 0);
    sd_bus_message_unref(m);
    free(text);
    return r < 0 ? -1 : 0;
}

int64_t plat_notify_poll(void)
{
    if (!bus) return 0;
    while (sd_bus_process(bus, NULL) > 0) {}
    int64_t tag = clicked;
    clicked = 0;
    return tag;
}

void plat_notify_shutdown(void)
{
    if (bus) sd_bus_flush_close_unref(bus);
    bus = NULL;
}
