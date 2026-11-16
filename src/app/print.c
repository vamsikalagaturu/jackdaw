#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "core/pdf.h"
#include "core/util.h"
#include "platform.h"
#include "ui/html.h"

/* A4 at 150 dpi; the page is drawn K times its logical size so 15 px text prints at about 11 pt */
enum { PX_W = 1240, PX_H = 1754 };
#define K 1.6f
#define MARGIN 50.0f
#define LW (PX_W / K)
#define LH (PX_H / K)
#define CW (LW - 2 * MARGIN)
#define CH (LH - 2 * MARGIN)

static const Color INK = {0x1f, 0x23, 0x28, 0xff}, MUTED = {0x57, 0x60, 0x6a, 0xff}, RULE = {0xd0, 0xd7, 0xde, 0xff};

static float header(jd_ui *ui, const char *label, const char *value, float y, int draw)
{
    if (!value || !value[0]) return y;
    if (draw) {
        jd_text(ui, label, MARGIN, y, 12, MUTED, 1);
        jd_text_fit(ui, value, MARGIN + 52, y, CW - 52, 12, INK, 0);
    }
    return y + 18;
}

/* the conversation laid out from y; draws when draw is set, and returns where it ends */
static float doc(jd_app *app, jd_conv *c, float y, int draw)
{
    jd_ui *ui = &app->ui;
    const char *subject = c->reading[c->n - 1].row.subject;
    int nlines;
    int *starts = jd_wrap(ui, subject && subject[0] ? subject : "(no subject)", CW, 20, &nlines);
    for (int i = 0; i < nlines; i++, y += 26)
        if (draw) jd_text_range(ui, subject, starts[i], i + 1 < nlines ? starts[i + 1] : (int)strlen(subject), MARGIN, y, 20, INK, 1);
    free(starts);
    y += 8;
    for (int i = 0; i < c->n; i++) {
        jd_reading *m = &c->reading[i];
        if (draw) DrawRectangleV((Vector2){MARGIN, y}, (Vector2){CW, 1}, RULE);
        y += 12;
        char date[64];
        format_date(m->row.date, date, sizeof date);
        y = header(ui, "From", m->row.from, y, draw);
        y = header(ui, "To", m->row.to, y, draw);
        y = header(ui, "Cc", m->row.cc, y, draw);
        y = header(ui, "Date", date, y, draw);
        y += 10;
        if (m->html && !c->plain) {
            if (m->html_w != CW) m->html_h = jd_html_layout(m->html, CW), m->html_w = CW;
            if (draw) jd_html_set_dark(m->html, 0), jd_html_draw(m->html, MARGIN, y, y - LH, y + m->html_h + LH);
            y += m->html_h;
        } else if (m->text) {
            jd_ui_use(ui, app->prefs.mono_plain ? &ui->mono : &ui->reader);
            int *ls = jd_wrap(ui, m->text, CW, 14, &nlines);
            for (int l = 0; l < nlines; l++, y += 20) {
                int from = ls[l], to = l + 1 < nlines ? ls[l + 1] : (int)strlen(m->text);
                if (draw) jd_text_range(ui, m->text, from, to, MARGIN, y, 14, m->text[from] == '>' ? MUTED : INK, 0);
            }
            free(ls);
            jd_ui_use(ui, NULL);
        } else {
            if (draw) jd_text(ui, "(not downloaded)", MARGIN, y, 13, MUTED, 0);
            y += 20;
        }
        for (int f = 0; f < m->nfiles; f++) {
            if (!f) y += 8;
            if (draw) jd_text_fit(ui, m->files[f].name, MARGIN + 18, y, CW - 18, 12, MUTED, 0);
            if (draw) jd_text(ui, "+", MARGIN + 4, y, 12, MUTED, 1);
            y += 18;
        }
        y += 18;
    }
    return y;
}

/* the last row at or above `from` (counting up at most `span` rows) that is one colour across the content,
   where a page can end without cutting a line of text */
static int quiet_row(Image *im, int from, int span)
{
    const unsigned char *px = im->data;
    int x0 = (int)(MARGIN * K), x1 = (int)((MARGIN + CW) * K);
    for (int r = from; r > from - span; r--) {
        const unsigned char *row = px + (size_t)r * PX_W * 4;
        int same = 1;
        for (int x = x0; x < x1 && same; x++)
            same = !memcmp(row + x * 4, row + x0 * 4, 3);
        if (same) return r;
    }
    return from;
}

void print_conv(jd_app *app, jd_conv *c)
{
    if (!c->n) return;
    float total = doc(app, c, 0, 0);
    RenderTexture2D rt = LoadRenderTexture(PX_W, PX_H);
    unsigned char **pages = NULL;
    int n = 0;
    Camera2D cam = {.zoom = K};
    for (float at = 0; at < total && n < 200;) {
        BeginTextureMode(rt);
        ClearBackground(WHITE);
        BeginMode2D(cam);
        BeginScissorMode((int)(MARGIN * K), (int)(MARGIN * K), (int)(CW * K), (int)(CH * K));
        doc(app, c, MARGIN - at, 1);
        EndScissorMode();
        EndMode2D();
        EndTextureMode();
        Image im = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&im);
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        int bottom = (int)((MARGIN + CH) * K) - 1;
        int cut = at + CH < total ? quiet_row(&im, bottom, (int)(CH * K * 0.2f)) : bottom;
        ImageDrawRectangle(&im, 0, cut + 1, PX_W, PX_H - cut - 1, WHITE);
        char num[32];
        snprintf(num, sizeof num, "%d", n + 1);
        ImageDrawText(&im, num, PX_W / 2 - 10, PX_H - (int)(MARGIN * K * 0.6f), 20, MUTED);
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        pages = jd_realloc(pages, sizeof *pages * (size_t)(n + 1));
        pages[n++] = im.data;
        at += (float)(cut + 1) / K - MARGIN;
    }
    UnloadRenderTexture(rt);
    /* the reader lays its HTML out again for its own width */
    for (int i = 0; i < c->n; i++) c->reading[i].html_w = 0;

    const char *run = getenv("XDG_RUNTIME_DIR");
    char *dir = run && run[0] ? jd_printf("%s/" JD_APP_NAME "/print", run) : jd_printf("%s/print", app->dir);
    const char *subject = c->reading[c->n - 1].row.subject;
    char name[200];
    snprintf(name, sizeof name, "%.180s.pdf", subject && subject[0] ? subject : "message");
    for (char *p = name; *p; p++)
        if (*p == '/') *p = '-';
    jd_mkdirs(dir);
    char *path = jd_unique_path(dir, name);
    free(app->error);
    app->error = NULL;
    if (jd_pdf_write(path, pages, n, PX_W, PX_H, subject) != 0) app->error = jd_printf("Cannot write %s", path);
    else if (plat_print(path, subject) != 0) app->error = jd_printf("No print service; the PDF is at %s", path);
    else notify_user(app, "Sent %d page%s to the print dialog.", n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) MemFree(pages[i]);
    free(pages), free(path), free(dir);
}
