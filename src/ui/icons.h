#pragma once
#include "raylib.h"

typedef struct {
    const char *name;
    const unsigned char *data;
    const int *size;
} jd_icon_file;

extern const jd_icon_file jd_icon_files[];

/* Lucide icon by file name (assets/icons/<name>.svg), size px square, in color; unknown names draw nothing */
void jd_icon(const char *name, float x, float y, float size, Color color);
void jd_icons_unload(void);
