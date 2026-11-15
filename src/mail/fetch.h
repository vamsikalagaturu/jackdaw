#pragma once
#include <stddef.h>

/* Remote images for HTML mail, fetched on a thread of their own: http and https only, certificates checked,
   no cookies or referrer, at most JD_FETCH_MAX bytes, redirects followed three times. Each answer is kept in
   cache_dir under the SHA-256 of its URL, so a picture is fetched once. */
typedef struct jd_fetcher jd_fetcher;

enum { JD_FETCH_MAX = 5 * 1024 * 1024 };

jd_fetcher *jd_fetcher_start(const char *cache_dir);
void jd_fetcher_stop(jd_fetcher *f);
/* the cache file for url (it may not exist yet); free() it */
char *jd_fetch_cache_path(jd_fetcher *f, const char *url);
/* queues url unless it is cached, queued or failed before */
void jd_fetch(jd_fetcher *f, const char *url);
/* a finished fetch: url, and ok 1 with the image in its cache file, 0 when it failed; 0 when none is ready */
int jd_fetch_poll(jd_fetcher *f, char **url, int *ok);
