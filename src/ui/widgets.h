#pragma once
#include "theme.h"

/* Fonts by role: the interface, the message list, the message text, and monospace (regular in both slots).
   jd_text and the rest draw with active, the interface fonts when it is NULL. */
typedef struct {
    jd_fonts fonts;
    jd_fonts list, reader, mono;
    const jd_fonts *active;
    int touch; /* a touch screen: menus leave out keyboard shortcuts */
} jd_ui;

void jd_ui_use(jd_ui *ui, const jd_fonts *role);

void jd_text(jd_ui *ui, const char *s, float x, float y, float size, Color c, int bold);
float jd_text_width(jd_ui *ui, const char *s, float size, int bold);
/* s cut with "..." to fit width */
void jd_text_fit(jd_ui *ui, const char *s, float x, float y, float width, float size, Color c, int bold);

/* One-line text input. The text is UTF-8; the cursor is a byte offset on a character boundary. */
typedef struct {
    char text[512];
    int len, cursor;
    int selecting, anchor; /* a selection from anchor to cursor, while selecting */
    int secret;
    int bare; /* no box of its own: it sits inside one the caller draws */
    const char *placeholder;
} jd_field;

/* For a left press: 1, or 2 and 3 for a second and third press within 0.4 s and a few pixels (a word, a line) */
int jd_click_count(void);

void jd_field_set(jd_field *f, const char *s);
/* typing, editing keys and paste, for the focused field */
void jd_field_input(jd_field *f);
/* returns 1 when clicked */
int jd_field_draw(jd_ui *ui, jd_field *f, Rectangle r, int focused);
/* the fields' right-click menu; call last in the frame, so it lands over dialogs too */
void jd_field_menu_draw(jd_ui *ui);

int jd_button(jd_ui *ui, const char *label, Rectangle r, int primary);

/* 1 when a key other than a modifier went down this frame: the second key of a chord (gg, dd, gi) is waited for
   until then, not dropped on the next frame */
int jd_key_went_down(void);

/* A tooltip at (x, y), drawn by jd_tooltip_draw once the frame's content is drawn, so nothing covers it */
void jd_tooltip(const char *text, float x, float y);
void jd_tooltip_draw(jd_ui *ui);

/* A right-click menu. An item with no label is a separator. */
typedef struct {
    const char *label, *icon, *keys;
    int disabled;
} jd_popup_item;
typedef struct {
    int open;
    Vector2 at;
    Rectangle box; /* where it was drawn, for callers keeping clicks off what lies under it */
} jd_popup;
void jd_popup_open(jd_popup *p);
/* draws an open menu; returns the item clicked, else -1. Esc or a click elsewhere closes it. */
int jd_popup_draw(jd_ui *ui, jd_popup *p, const jd_popup_item *items, int n);
/* a button with an icon before its label (icon NULL: the label alone); label NULL: the icon alone */
int jd_icon_button(jd_ui *ui, const char *icon, const char *label, Rectangle r, int primary);
/* A vertical scrollbar along the right edge of view (shown when content is taller than view): the thumb drags,
   a click on the track pages. *scroll is kept within 0..content - view. */
void jd_scrollbar(Rectangle view, float content, float *scroll);
/* a touch screen: scrollbars show no hover (the pointer is where the last finger was) */
void jd_set_touch(int touch);
/* this frame's scroll in logical pixels, to subtract from the offset: a finger's exact movement, else wheel
   notches times notch */
float jd_wheel_y(float notch);
float jd_wheel_x(float notch);
/* the view between jd_follow_begin and jd_follow_end scrolls the focused field into sight when it just took the
   focus or the view got shorter (a phone's keyboard opening) */
void jd_follow_field(const void *field, Rectangle r);
void jd_follow_begin(void);
void jd_follow_end(float *scroll, Rectangle view);

/* an on/off switch at (x, y); returns 1 when clicked */
int jd_switch(jd_ui *ui, float x, float y, int on);
/* one of n labels; returns the index clicked, else selected */
int jd_segmented(jd_ui *ui, float x, float y, const char *const *labels, int n, int selected);

/* Breaks text into lines no wider than width: at newlines, else at the last space, else mid-word.
   Returns the byte offset each line starts at; line i ends where line i + 1 starts (or at the text's end). */
int *jd_wrap(jd_ui *ui, const char *text, float width, float size, int *nlines);
/* Multi-line text editing, wrapped to the box width. text is UTF-8 and always NUL-terminated. */
typedef struct {
    char *text;
    int len, cap, cursor;
    float scroll;
    int *starts, nlines;
    float wrapped_for;
    int changed;
    int selecting, anchor; /* a selection runs from anchor to cursor while selecting is set */
    int dragging;
    int inert; /* set by the caller for a frame: the mouse belongs to something over the editor */
} jd_editor;

void jd_editor_set(jd_editor *e, const char *s);
/* inserts at the cursor, in place of the selection when there is one */
void jd_editor_insert(jd_editor *e, const char *s, int n);
/* typing and editing keys; Shift with the arrows, Home and End selects, Ctrl+A all, Ctrl+C / Ctrl+X / Ctrl+V */
void jd_editor_input(jd_ui *ui, jd_editor *e);
/* the selection as byte offsets; 0 when nothing is selected */
int jd_editor_selection(const jd_editor *e, int *from, int *to);
void jd_editor_select_from(jd_editor *e, int anchor);
void jd_editor_deselect(jd_editor *e);
void jd_editor_delete_range(jd_editor *e, int from, int to);
/* vim motions for normal and visual mode: h j k l w b 0 $ and G (end) / g (start); select extends the selection
   (visual mode), else it ends. 0 when key is not a motion */
int jd_editor_motion(jd_ui *ui, jd_editor *e, int key, int shift, int select);
/* the start and end (before its newline) of the text line around pos */
int jd_editor_line_start(const jd_editor *e, int pos);
int jd_editor_line_stop(const jd_editor *e, int pos);
/* returns 1 when clicked */
int jd_editor_draw(jd_ui *ui, jd_editor *e, Rectangle r, int focused);
void jd_editor_free(jd_editor *e);

/* draws bytes [from, to) of text, without a trailing newline or space */
void jd_text_range(jd_ui *ui, const char *text, int from, int to, float x, float y, float size, Color c, int bold);
