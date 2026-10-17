#ifndef POPUP_H
#define POPUP_H

/* The "report bugs" pop-up that the main menu's top-right button opens, laid
 * out like the game's own pop-ups and made from the game's own art (popup.c),
 * drawn over the game with the game's hazy blue behind it.  Only tapping its X
 * closes it; the Vita's buttons do nothing while it is open. */

void popup_preload(void);   /* call once after the graphics are set up, before the game starts:
                               gets the pictures (and the menu button's warning sign) ready */
void popup_request(void);   /* ask for it to open (safe to call from inside the game's code) */
int popup_frame_start(void); /* call once per frame before the game: opens it if asked, handles
                                its input; returns 1 while it is showing (skip the game's input) */
void popup_draw(void);       /* call after the game has drawn the frame, before swapping */

/* The credit line ("PS Vita port by rodi - v1.0", app0:credit.png) along the bottom
 * of the start-up loading screen; it fades out when the game has finished loading. */
void credit_preload(void);  /* once, after the graphics are set up */
void credit_draw(void);     /* every frame, after the game has drawn it, before swapping */

/* PNG reader used by the pop-up: 8-bit greyscale / RGB / RGBA / palette, not interlaced.
 * Returns RGBA pixels (free() them) or NULL. */
unsigned char *png_load_rgba(const char *path, int *w, int *h);

#endif
