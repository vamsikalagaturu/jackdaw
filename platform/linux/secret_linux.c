#include <stdlib.h>

#include "core/util.h"
#include "platform.h"

#ifdef JD_HAVE_LIBSECRET
#include <libsecret/secret.h>

static const SecretSchema schema = {
    .name = JD_APP_ID ".Password",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
};

int plat_secret_available(void) { return 1; }

char *plat_secret_load(const char *account)
{
    GError *err = NULL;
    gchar *pw = secret_password_lookup_sync(&schema, NULL, &err, "account", account, NULL);
    if (err) g_error_free(err);
    char *out = jd_strdup(pw);
    secret_password_free(pw);
    return out;
}

int plat_secret_save(const char *account, const char *password)
{
    GError *err = NULL;
    char *label = jd_printf(JD_APP_NAME ": %s", account);
    gboolean ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, label, password, NULL, &err,
                                             "account", account, NULL);
    free(label);
    if (err) g_error_free(err);
    return ok ? 0 : -1;
}

int plat_secret_delete(const char *account)
{
    GError *err = NULL;
    secret_password_clear_sync(&schema, NULL, &err, "account", account, NULL);
    int r = err ? -1 : 0;
    if (err) g_error_free(err);
    return r;
}

#else

int plat_secret_available(void) { return 0; }
char *plat_secret_load(const char *account) { return NULL; }
int plat_secret_save(const char *account, const char *password) { return -1; }
int plat_secret_delete(const char *account) { return 0; }

#endif
