#pragma once
#include "store/store.h"

/* Search text to a list query. Operators: from: to: subject: in: is:unread is:read is:flagged has:attachment
   after:DD-MM-YYYY before:DD-MM-YYYY (local days; YYYY-MM-DD also works); "quoted values" may hold spaces; other words go to the
   full-text index. The strings in q and *folder_name are owned by the caller (jd_query_free). */
typedef struct {
    jd_list_query q;
    char *folder_name; /* from in:, resolved by the caller */
} jd_search;

void jd_search_parse(const char *text, jd_search *out);
void jd_search_free(jd_search *s);
