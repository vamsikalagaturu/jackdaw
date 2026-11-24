#pragma once
#include "raylib.h"

/* Effects: a frosted backdrop under dialogs and paper planes for sent mail. Every call is made inside
   jd_scale_begin with no jd_scissor open, and returns that way. */

/* [view] effects: off, the backdrop is only its tint, nothing flies or burns, and fx_enabled() tells callers to skip
   their own motion; shadows stay */
void fx_enable(int on);
int fx_enabled(void);

/* what the screen shows so far, blurred, faded in and then tinted: the first call of a run of frames that each
   call it captures the screen, the others draw that capture again */
void fx_backdrop(Color tint);
/* src (a screen capture, upright unless flipped) blurred into *out, a quarter of its size; *out is allocated or
   resized as needed */
void fx_blur(Texture2D src, int flipped, RenderTexture2D *out);
void fx_blur_free(RenderTexture2D *out);
/* a texture of fx_blur over the whole screen */
void fx_draw_blurred(RenderTexture2D blurred, float alpha);
/* a soft shadow under a raised box (logical pixels, DrawRectangleRounded's roundness), deeper as lift goes to 1,
   darker as strength does (1 for a dialog) */
void fx_shadow(Rectangle box, float roundness, float lift, float strength);

/* A sent message folds into a paper plane that circles while Undo is offered: launch folds what page (logical
   pixels) shows on screen now; release flies it away; recall brings it back, unfolding, as Undo reopens the
   message. id is the message's own (the compose serial). */
void fx_plane_launch(int id, Rectangle page);
void fx_plane_release(int id);
void fx_plane_recall(int id);

/* A deleted row burns away: what box (logical pixels) shows on screen now, starting after delay seconds. Returns
   how long until it is gone, delay included. */
double fx_burn(Rectangle box, double delay);

/* a burst of rays and a ring at a point (logical pixels), as a flag pops on */
void fx_sparkle(Vector2 at, Color color);
/* a small paper plane of three flat facets in the current coordinates: nose along angle, roll -1 mirrors it */
void fx_mini_plane(Vector2 at, float angle, float size, float roll, float alpha);
/* the pulled plane let go at from (logical pixels): it shoots up and off the top */
void fx_fling(Vector2 from);
/* Send later: what page (logical pixels) shows becomes a card, an envelope with a clock closes over it, and it
   flies to target (has_target), else up and away */
void fx_envelope(Rectangle page, Vector2 target, int has_target);
/* an empty Inbox's sky in r (logical pixels): drifting clouds and a gliding plane; animates while drawn */
void fx_inbox_zero(Rectangle r, Color sky, Color cloud);

/* once a frame, after everything else is drawn: the burns, planes, envelopes and sparkles, over it all */
void fx_frame(void);
