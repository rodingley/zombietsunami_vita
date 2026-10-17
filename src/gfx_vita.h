#ifndef GFX_VITA_H
#define GFX_VITA_H

int gfx_init(int w, int h);
void gfx_swap(void);
void gfx_log_errors(int frame);

typedef void (*touch_fn)(void *ctx, int id, int action, float x, float y);
void input_init(void);
/* returns bit 1 = Back requested (START) */
int input_poll(touch_fn touch, void *ctx);

void input_release_touches(void);       /* used when the pop-up opens */
void input_ignore_until_released(void); /* used when the pop-up closes */

void gfx_draw_cursor(void);
int input_menu_mode(void);
void input_get_accel(float *xyz);

#endif
