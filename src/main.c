/*
 * HAMSTER CAGE - PS1 platform layer (PSn00bSDK).
 * Display/double-buffer code is based on the PSn00bSDK template
 * (C) 2020-2023 Lameguy64, spicyjpeg - MPL licensed.
 */
#include <stddef.h>
#include <stdint.h>
#include <psxgpu.h>
#include <psxapi.h>
#include <psxpad.h>
#include <psxgte.h>
#include <inline_c.h>
#include "plat.h"
#include "game.h"
#define FONT_PSX_PIXELS
#include "font_psx.h"

#define BUFFER_LENGTH 65536

typedef struct {
	DISPENV  disp_env;
	DRAWENV  draw_env;
	uint32_t ot[OT_LEN];
	uint8_t  buffer[BUFFER_LENGTH];
} RenderBuffer;

typedef struct {
	RenderBuffer buffers[2];
	uint8_t      *next_packet;
	int          active_buffer;
} RenderContext;

static RenderContext ctx;          /* static: far too big for the stack */
static uint8_t pad_buff[2][34];

static void setup_context(int w, int h, int r, int g, int b) {
	SetDefDrawEnv(&ctx.buffers[0].draw_env, 0, 0, w, h);
	SetDefDispEnv(&ctx.buffers[0].disp_env, 0, 0, w, h);
	SetDefDrawEnv(&ctx.buffers[1].draw_env, 0, h, w, h);
	SetDefDispEnv(&ctx.buffers[1].disp_env, 0, h, w, h);

	for (int i = 0; i < 2; i++) {
		setRGB0(&ctx.buffers[i].draw_env, r, g, b);
		ctx.buffers[i].draw_env.isbg = 1;
		ctx.buffers[i].draw_env.dtd  = 1;   /* dithering: the PS1 look */
	}
	ctx.active_buffer = 0;
	ctx.next_packet   = ctx.buffers[0].buffer;
	ClearOTagR(ctx.buffers[0].ot, OT_LEN);
	SetDispMask(1);
}

/* Locked 30 fps: each frame is shown for at least two vblanks, so the frame
 * rate is steady instead of flickering between 60 and lower. */
#define FRAME_VBLANKS 2

static void flip_buffers(void) {
	static int last_flip = 0;
	DrawSync(0);
	do VSync(0); while (VSync(-1) - last_flip < FRAME_VBLANKS);
	last_flip = VSync(-1);

	RenderBuffer *draw_buffer = &ctx.buffers[ctx.active_buffer];
	RenderBuffer *disp_buffer = &ctx.buffers[ctx.active_buffer ^ 1];

	PutDispEnv(&disp_buffer->disp_env);
	DrawOTagEnv(&draw_buffer->ot[OT_LEN - 1], &draw_buffer->draw_env);

	ctx.active_buffer ^= 1;
	ctx.next_packet    = disp_buffer->buffer;
	ClearOTagR(disp_buffer->ot, OT_LEN);
}

/* returns NULL instead of crashing if the frame runs out of packet space */
static void *new_primitive(int z, size_t size) {
	RenderBuffer *buffer = &ctx.buffers[ctx.active_buffer];
	uint8_t      *prim   = ctx.next_packet;
	if (prim + size > &buffer->buffer[BUFFER_LENGTH - 2048]) return NULL;
	addPrim(&buffer->ot[z], prim);
	ctx.next_packet += size;
	return (void *)prim;
}

/* ---------------------------------------------------------- plat_* API */

void plat_quad(int otz, const int16_t *xy, int r, int g, int b, int semi) {
	POLY_F4 *p = (POLY_F4 *)new_primitive(otz, sizeof(POLY_F4));
	if (!p) return;
	setPolyF4(p);
	setXY4(p, xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], xy[6], xy[7]);
	setRGB0(p, r, g, b);
	if (semi) setSemiTrans(p, 1);
}

void plat_tile(int otz, int x, int y, int w, int h, int r, int g, int b) {
	if (w <= 0 || h <= 0) return;
	TILE *t = (TILE *)new_primitive(otz, sizeof(TILE));
	if (!t) return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
}

/* The Godot game's PS1 font, uploaded to VRAM as a 4-bit texture beside the
 * framebuffers, drawn one sprite per letter. */
#define FONT_VRAM_X 640
#define FONT_VRAM_Y 0
#define FONT_CLUT_X 640
#define FONT_CLUT_Y 480

static uint16_t font_tpage, font_clut;

static void font_init(void) {
	RECT tex  = {FONT_VRAM_X, FONT_VRAM_Y, FONT_TEX_W / 4, FONT_TEX_H};   /* 4 texels per VRAM pixel */
	RECT clut = {FONT_CLUT_X, FONT_CLUT_Y, 16, 1};
	LoadImage(&tex, (uint32_t *)FONT_TEX);
	DrawSync(0);
	LoadImage(&clut, (uint32_t *)FONT_CLUT);
	DrawSync(0);
	font_tpage = getTPage(0, 0, FONT_VRAM_X, FONT_VRAM_Y);   /* 0 = 4-bit */
	font_clut  = getClut(FONT_CLUT_X, FONT_CLUT_Y);
}

void plat_text(int x, int y, const char *s) {
	for (; *s; s++) {
		int c = (unsigned char)*s;
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c < 32 || c > 127) c = '?';
		const uint8_t *g = FONT_GLYPH[c - 32];
		if (c != ' ' && g[2]) {
			SPRT *sp = (SPRT *)new_primitive(0, sizeof(SPRT));
			if (!sp) return;
			setSprt(sp);
			setRGB0(sp, 128, 128, 128);      /* 128 = draw texture colours as-is */
			setXY0(sp, x + (int8_t)g[4], y + (int8_t)g[5]);
			setUV0(sp, g[0], g[1]);
			setWH(sp, g[2], g[3]);
			sp->clut = font_clut;
		}
		x += g[6];
	}
	/* same OT slot draws last-added first, so this runs before the letters */
	DR_TPAGE *tp = (DR_TPAGE *)new_primitive(0, sizeof(DR_TPAGE));
	if (tp) setDrawTPage(tp, 0, 1, font_tpage);
}

void plat_set_bg(int r, int g, int b) {
	for (int i = 0; i < 2; i++) setRGB0(&ctx.buffers[i].draw_env, r, g, b);
}

/* ------------------------------------------------------- GTE (3D maths) */

void plat_set_matrix(const int *m9, const int *t3) {
	static MATRIX mtx;
	for (int i = 0; i < 9; i++) mtx.m[i / 3][i % 3] = (int16_t)m9[i];
	mtx.t[0] = t3[0]; mtx.t[1] = t3[1]; mtx.t[2] = t3[2];
	gte_SetRotMatrix(&mtx);
	gte_SetTransMatrix(&mtx);
}

int plat_project(int x, int y, int z, int *sx, int *sy) {
	static SVECTOR v;
	static int32_t sxy, sz;
	v.vx = (int16_t)x; v.vy = (int16_t)y; v.vz = (int16_t)z;
	gte_ldv0(&v);
	gte_rtps();
	gte_stsxy(&sxy);
	gte_stsz(&sz);
	*sx = (int16_t)(sxy & 0xffff);
	*sy = (int16_t)(sxy >> 16);
	return sz;
}

/* three points per GTE RTPT: much cheaper than one RTPS call per point */
void plat_project_n(const int16_t *v, int n, int32_t *sxy, int32_t *sz) {
	int i = 0;
	for (; i + 3 <= n; i += 3, v += 12) {
		gte_ldv3(v, v + 4, v + 8);
		gte_rtpt();
		gte_stsxy3(&sxy[i], &sxy[i + 1], &sxy[i + 2]);
		gte_stsz3(&sz[i], &sz[i + 1], &sz[i + 2]);
	}
	for (; i < n; i++, v += 4) {
		gte_ldv0(v);
		gte_rtps();
		gte_stsxy(&sxy[i]);
		gte_stsz(&sz[i]);
	}
}

uint16_t plat_buttons(void) {
	PADTYPE *pad = (PADTYPE *)pad_buff[0];
	if (pad->stat != 0) return 0;   /* nothing plugged into port 1 */
	/* digital (0x4), analog stick (0x5) and DualShock (0x7) */
	if (pad->type != 0x4 && pad->type != 0x5 && pad->type != 0x7) return 0;

	uint16_t raw = ~pad->btn;       /* hardware is active-low */
	uint16_t b = 0;
	if (raw & PAD_UP)       b |= B_UP;
	if (raw & PAD_DOWN)     b |= B_DOWN;
	if (raw & PAD_LEFT)     b |= B_LEFT;
	if (raw & PAD_RIGHT)    b |= B_RIGHT;
	if (raw & PAD_CROSS)    b |= B_CROSS;
	if (raw & PAD_CIRCLE)   b |= B_CIRCLE;
	if (raw & PAD_SQUARE)   b |= B_SQUARE;
	if (raw & PAD_TRIANGLE) b |= B_TRIANGLE;
	if (raw & PAD_L1)       b |= B_L1;
	if (raw & PAD_R1)       b |= B_R1;
	if (raw & PAD_L2)       b |= B_L2;
	if (raw & PAD_R2)       b |= B_R2;
	if (raw & PAD_START)    b |= B_START;
	if (raw & PAD_SELECT)   b |= B_SELECT;
	return b;
}

/* ---------------------------------------------------------------- main */

int main(int argc, const char **argv) {
	(void)argc; (void)argv;

	ResetGraph(0);
	font_init();
	setup_context(SCREEN_W, SCREEN_H, 40, 30, 52);

	InitPAD(pad_buff[0], 34, pad_buff[1], 34);
	StartPAD();
	ChangeClearPAD(0);

	InitGeom();
	gte_SetGeomOffset(SCREEN_W / 2, SCREEN_H / 2);
	gte_SetGeomScreen(FOCAL);

	game_init();
	/* Game time follows the 60 Hz vblank counter: if a frame takes longer to
	 * draw, the game logic catches up instead of going into slow motion. */
	int last = VSync(-1);
	for (;;) {
		int now = VSync(-1), steps = now - last;
		last = now;
		if (steps < 1) steps = 1;
		if (steps > 4) steps = 4;
		uint16_t pad = plat_buttons();
		for (int i = 0; i < steps; i++) game_update(pad);
		game_render();
		flip_buffers();
	}
	return 0;
}
