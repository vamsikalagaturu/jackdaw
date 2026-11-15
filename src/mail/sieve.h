#pragma once
#include "mail/account.h"

/* ManageSieve (RFC 5804) on the IMAP host: STARTTLS (never plain text), AUTHENTICATE PLAIN. */

/* the active script (or the first one) and its name; *script "" and *name NULL when the server has none */
int jd_sieve_get(const jd_account *a, const char *password, char **name, char **script, char *err, int errlen);
/* CHECKSCRIPT, then PUTSCRIPT name and SETACTIVE name; err holds the server's complaint about the script */
int jd_sieve_put(const jd_account *a, const char *password, const char *name, const char *script, char *err,
                 int errlen);
