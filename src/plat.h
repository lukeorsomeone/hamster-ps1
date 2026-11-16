/* Platform layer: the game only talks to the hardware through these.
 * src/main.c implements them for the PS1 (PSn00bSDK). */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>

#define SCREEN_W   320
#define SCREEN_H   240
#define OT_LEN     1024   /* depth buckets: 0 = front, OT_LEN-1 = back */
#define FOCAL      280    /* projection distance (GTE H register) */
#define NEAR_Z     150    /* GTE divide is only valid for z > FOCAL/2 */

/* Buttons (1 = held) */
enum {
	B_UP = 1 << 0, B_DOWN = 1 << 1, B_LEFT = 1 << 2, B_RIGHT = 1 << 3,
	B_CROSS = 1 << 4, B_CIRCLE = 1 << 5, B_SQUARE = 1 << 6, B_TRIANGLE = 1 << 7,
	B_L1 = 1 << 8, B_R1 = 1 << 9, B_L2 = 1 << 10, B_R2 = 1 << 11,
	B_START = 1 << 12, B_SELECT = 1 << 13
};

/* xy = x0,y0, x1,y1, x2,y2, x3,y3 in PS1 quad order (TL, TR, BL, BR) */
void     plat_quad(int otz, const int16_t *xy, int r, int g, int b, int semi);
void     plat_tile(int otz, int x, int y, int w, int h, int r, int g, int b);
void     plat_text(int x, int y, const char *s);   /* always on top */
uint16_t plat_buttons(void);
void     plat_set_bg(int r, int g, int b);           /* clear colour */

/* 3D: load a model->camera matrix (rotation 4.12 fixed, translation),
 * then project points through it. Returns camera-space z; sx/sy are
 * saturated to -1024..1023 like the GTE does. On the PS1 this is the GTE. */
void     plat_set_matrix(const int *m9, const int *t3);
int      plat_project(int x, int y, int z, int *sx, int *sy);

#endif
