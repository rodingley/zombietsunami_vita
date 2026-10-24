/* game_art.h -- see game_art.c */
#ifndef GAME_ART_H
#define GAME_ART_H

#include <stdint.h>

/* a piece cut out of one of the game's sprite sheets: premultiplied RGBA, with
 * `pad` pixels of the sheet's own surroundings kept round it (the sheets repeat
 * every sprite's edge pixels there, so smooth scaling has something to blend with) */
typedef struct {
  int w, h, pad;   /* w x h is the sprite itself; the pixels are (w+2pad) x (h+2pad) */
  uint8_t *px;
} ga_img;

/* a picture being put together, premultiplied RGBA */
typedef struct {
  int w, h;
  uint8_t *px;
} ga_canvas;

/* cut sprites (module numbers from the sheet's .spr) out of a sheet, e.g.
 * ga_cut("menu_16bpp", "menu", ...).  0 = ok. */
int ga_cut(const char *sheet, const char *spr, const int *mods, int n, int pad, ga_img *out);
void ga_free(ga_img *im);

/* draw `im` into the rectangle (x0,y0)-(x1,y1) of `c` (any size, smooth scaling);
 * flip_x / flip_y mirror it; bright > 0 lights it up like a pressed button */
void ga_blit(ga_canvas *c, const ga_img *im, float x0, float y0, float x1, float y1, int flip_x, int flip_y,
             int bright);

/* the game's in-run warning sign (triangle + "!") centred on (cx, cy); s = 1 is its
 * size in the game's sheet; outline > 0 draws a black outline that many pixels
 * wide round it.  Returns 0 when the pieces could not be read. */
int ga_draw_warning(ga_canvas *c, float cx, float cy, float s, float outline);

/* the warning sign for the main menu's report-bugs button, as pixels for the
 * menu sheet (RGBA4444 premultiplied, like the sheet), for the big sheet
 * (small = 0, menu_16bpp) or the small one (small = 1, menu_320_16bpp).
 * 0 = ok; *px is malloc'ed. */
int ga_menu_icon(int small, unsigned *sheet_w, unsigned *sheet_h, unsigned *x, unsigned *y, unsigned *w,
                 unsigned *h, uint16_t **px);

#endif
