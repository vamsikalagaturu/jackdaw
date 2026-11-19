#pragma once
#include <stdint.h>

/* Google sign-in for its calendars: OAuth 2.0 for native apps (RFC 8252) with PKCE (RFC 7636). The browser shows
   Google's consent page, which hands the code to a one-shot listener on 127.0.0.1 (RFC 8252 7.3); the code becomes
   a refresh token, kept in the keystore, and that becomes short-lived access tokens for CalDAV. */
typedef struct jd_oauth jd_oauth;

#define JD_GOOGLE_CALDAV "https://apidata.googleusercontent.com/caldav/v2/"

enum { JD_OAUTH_WAITING = 0, JD_OAUTH_DONE = 1, JD_OAUTH_FAILED = -1, JD_OAUTH_REVOKED = -2 };

/* listens and gives the URL to open in the browser; NULL (with err) when no listener could start */
jd_oauth *jd_oauth_start(const char *login_hint, char **url, char *err, int errlen);
/* JD_OAUTH_WAITING, JD_OAUTH_DONE with *refresh_token, or JD_OAUTH_FAILED with err */
int jd_oauth_poll(jd_oauth *o, char **refresh_token, char *err, int errlen);
/* stops listening and frees o */
void jd_oauth_stop(jd_oauth *o);
/* an access token for refresh_token and when it expires (Unix seconds); JD_OAUTH_REVOKED when Google refuses the
   refresh token (signed out, password changed, access removed), JD_OAUTH_FAILED with err when it could not ask */
int jd_oauth_refresh(const char *refresh_token, char **access, int64_t *expires, char *err, int errlen);
