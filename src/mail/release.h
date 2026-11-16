#pragma once
#include <stddef.h>

/* jackdaw's releases on GitHub: the latest one, and its program for this platform (the APK on Android,
   jackdaw-linux-<arch> on Linux) */
#define JD_RELEASE_REPO "vamsikalagaturu/jackdaw"

typedef struct {
    char *tag;    /* "v0.3.0" */
    char *page;   /* the release's page */
    char *asset;  /* this platform's download URL, NULL when the release has none */
    char *sha256; /* its digest as GitHub publishes it (hex), NULL when absent */
} jd_release;

/* blocking (a worker thread's call); 0, or -1 with err */
int jd_release_latest(jd_release *out, char *err, int errlen);
int jd_release_parse(const char *json, jd_release *out);
void jd_release_free(jd_release *r);

/* "v1.2.3" or "1.2.3-dev+abc" against another, by major, minor and patch, a pre-release before its release:
   <0, 0, >0 */
int jd_version_cmp(const char *a, const char *b);

/* url to path, refused unless its SHA-256 is sha256 (when given); *progress goes 0 to 1 while it runs. Blocking;
   0, or -1 with err */
int jd_release_download(const char *url, const char *sha256, const char *path, volatile float *progress, char *err,
                        int errlen);
