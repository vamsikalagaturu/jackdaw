#pragma once
#include "util.h"

/* The TOML subset jackdaw writes: [table] and [[array]] headers, bare keys, basic strings, one-line arrays of them,
   integers, booleans, comments. Entry i of [[name]] reads as table "name.i". */
typedef struct jd_toml jd_toml;

jd_toml *jd_toml_parse(const char *text, char *err, size_t errlen);
jd_toml *jd_toml_load(const char *path, char *err, size_t errlen);
void jd_toml_free(jd_toml *t);

const char *jd_toml_str(const jd_toml *t, const char *table, const char *key);
int jd_toml_int(const jd_toml *t, const char *table, const char *key, long long *out);
int jd_toml_bool(const jd_toml *t, const char *table, const char *key, int fallback);
int jd_toml_count(const jd_toml *t, const char *array);
/* an array of strings: how many, each malloc'd in *out (malloc'd, NULL when none) */
int jd_toml_strs(const jd_toml *t, const char *table, const char *key, char ***out);

void jd_toml_quote(jd_buf *b, const char *s);
