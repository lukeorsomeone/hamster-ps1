/* PC preview harness: software-renders the game the way the PS1 GPU would
 * (OT buckets, back to front, same-bucket = last added drawn first) and dumps
 * frames as PPM + a text overlay list. Not part of the PS1 build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plat.h"
#include "../src/game.h"
#define FONT_PSX_PIXELS
#include "../src/font_psx.h"

typedef struct { int kind, otz, seq; int16_t xy[8]; int r, g, b, semi; } Prim;
static Prim prims[20000];
static int nprim = 0;
static char texts[64][64]; static int tx[64], ty[64], ntext = 0;
static uint8_t fb[SCREEN_H][SCREEN_W][3];
static uint16_t cur_buttons = 0;

void plat_quad(int otz, const int16_t *xy, int r, int g, int b, int semi) {
	Prim *p = &prims[nprim]; p->kind = 0; p->otz = otz; p->seq = nprim;
	memcpy(p->xy, xy, 16); p->r = r; p->g = g; p->b = b; p->semi = semi; nprim++;
}
void plat_tile(int otz, int x, int y, int w, int h, int r, int g, int b) {
	Prim *p = &prims[nprim]; p->kind = 1; p->otz = otz; p->seq = nprim;
	p->xy[0] = x; p->xy[1] = y; p->xy[2] = w; p->xy[3] = h; p->r = r; p->g = g; p->b = b; p->semi = 0; nprim++;
}
void plat_text(int x, int y, const char *s) {
	if (ntext < 64) { strncpy(texts[ntext], s, 63); tx[ntext] = x; ty[ntext] = y; ntext++; }
}
uint16_t plat_buttons(void) { return cur_buttons; }
static int gm[9], gt[3];
void plat_set_matrix(const int *m9, const int *t3) { for (int i = 0; i < 9; i++) gm[i] = (int16_t)m9[i]; for (int i = 0; i < 3; i++) gt[i] = t3[i]; }
static long nproj = 0;
int plat_project(int x, int y, int z, int *sx, int *sy) {
	nproj++;
	int cx = ((gm[0] * x + gm[1] * y + gm[2] * z) >> 12) + gt[0];
	int cy = ((gm[3] * x + gm[4] * y + gm[5] * z) >> 12) + gt[1];
	int cz = ((gm[6] * x + gm[7] * y + gm[8] * z) >> 12) + gt[2];
	int sz = cz < 0 ? 0 : (cz > 65535 ? 65535 : cz);
	int h = FOCAL;
	long q = (sz * 2 > h) ? ((long)h << 16) / sz : 0x1ffff;   /* GTE: overflow when H >= 2*SZ */
	if (q > 0x1ffff) q = 0x1ffff;
	long px = SCREEN_W / 2 + ((cx * q) >> 16), py = SCREEN_H / 2 + ((cy * q) >> 16);
	*sx = px < -1024 ? -1024 : (px > 1023 ? 1023 : (int)px);
	*sy = py < -1024 ? -1024 : (py > 1023 ? 1023 : (int)py);
	return cz;
}
void plat_project_n(const int16_t *v, int n, int32_t *sxy, int32_t *sz) {
	for (int i = 0; i < n; i++, v += 4) {
		int sx, sy, z = plat_project(v[0], v[1], v[2], &sx, &sy);
		sxy[i] = (int32_t)(((uint32_t)(uint16_t)sy << 16) | (uint16_t)sx);
		sz[i] = z < 0 ? 0 : (z > 65535 ? 65535 : z);   /* GTE SZ is unsigned 16-bit */
	}
}
static int bgr = 40, bgg = 30, bgb = 52;
void plat_set_bg(int r, int g, int b) { bgr = r; bgg = g; bgb = b; }

static int cmp(const void *a, const void *b) {
	const Prim *p = a, *q = b;
	if (p->otz != q->otz) return q->otz - p->otz;   /* far first */
	return q->seq - p->seq;                          /* later added first */
}
static void put(int x, int y, const Prim *p) {
	if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
	if (p->semi) { for (int i = 0; i < 3; i++) fb[y][x][i] = (fb[y][x][i] + (i == 0 ? p->r : i == 1 ? p->g : p->b)) / 2; }
	else { fb[y][x][0] = p->r; fb[y][x][1] = p->g; fb[y][x][2] = p->b; }
}
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, const Prim *p) {
	int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	if (minx < 0) minx = 0; if (miny < 0) miny = 0;
	if (maxx >= SCREEN_W) maxx = SCREEN_W - 1; if (maxy >= SCREEN_H) maxy = SCREEN_H - 1;
	long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
	if (area == 0) return;
	for (int y = miny; y <= maxy; y++)
		for (int x = minx; x <= maxx; x++) {
			long w0 = (long)(x1 - x) * (y2 - y) - (long)(y1 - y) * (x2 - x);
			long w1 = (long)(x2 - x) * (y0 - y) - (long)(y2 - y) * (x0 - x);
			long w2 = (long)(x0 - x) * (y1 - y) - (long)(y0 - y) * (x1 - x);
			if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) put(x, y, p);
		}
}
static void flush(const char *name) {
	for (int y = 0; y < SCREEN_H; y++) for (int x = 0; x < SCREEN_W; x++) { fb[y][x][0] = bgr; fb[y][x][1] = bgg; fb[y][x][2] = bgb; }
	qsort(prims, nprim, sizeof(Prim), cmp);
	static int big = 0;
	for (int i = 0; i < nprim; i++) if (prims[i].kind == 0) {
		int mnx = 9999, mxx = -9999, mny = 9999, mxy = -9999;
		for (int k = 0; k < 4; k++) { int x = prims[i].xy[k*2], y = prims[i].xy[k*2+1];
			if (x < mnx) mnx = x; if (x > mxx) mxx = x; if (y < mny) mny = y; if (y > mxy) mxy = y; }
		if (mxx - mnx > 1023 || mxy - mny > 511) big++;
	}
	if (name) printf("  oversize quads so far: %d\n", big);
	for (int i = 0; i < nprim; i++) {
		Prim *p = &prims[i];
		if (p->kind == 1) { for (int y = p->xy[1]; y < p->xy[1] + p->xy[3]; y++) for (int x = p->xy[0]; x < p->xy[0] + p->xy[2]; x++) put(x, y, p); }
		else { int16_t *v = p->xy; tri(v[0], v[1], v[2], v[3], v[4], v[5], p); tri(v[2], v[3], v[6], v[7], v[4], v[5], p); }
	}
	for (int t = 0; t < ntext; t++) {
		int x = tx[t], y = ty[t];
		for (const char *c = texts[t]; *c; c++) {
			int ch = (unsigned char)*c; if (ch >= 'a' && ch <= 'z') ch -= 32;
			const uint8_t *g = FONT_GLYPH[ch - 32];
			if (ch != ' ')
				for (int j = 0; j < g[3]; j++) for (int i = 0; i < g[2]; i++) {
					int u = g[0] + i, v = g[1] + j;
					int idx = (FONT_TEX[(v * FONT_TEX_W + u) / 8] >> (4 * (u % 8))) & 15;
					uint16_t col = FONT_CLUT[idx];
					int px = x + (int8_t)g[4] + i, py = y + (int8_t)g[5] + j;
					if (!col || px < 0 || py < 0 || px >= SCREEN_W || py >= SCREEN_H) continue;
					fb[py][px][0] = (col & 31) << 3; fb[py][px][1] = ((col >> 5) & 31) << 3; fb[py][px][2] = ((col >> 10) & 31) << 3;
				}
			x += g[6];
		}
		if (x > SCREEN_W - 4) printf("  TEXT OVERFLOW: %s (ends at %d)\n", texts[t], x);
	}
	if (name) {
		char fn[128]; sprintf(fn, "%s.ppm", name);
		FILE *f = fopen(fn, "wb"); fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H); fwrite(fb, 1, sizeof fb, f); fclose(f);
		sprintf(fn, "%s.txt", name); f = fopen(fn, "w");
		for (int i = 0; i < ntext; i++) fprintf(f, "%d %d %s\n", tx[i], ty[i], texts[i]);
		fclose(f);
		printf("%s: %d prims, %ld projections this frame\n", name, nprim, nproj);
	}
	nprim = 0; ntext = 0; nproj = 0;
}

/* scripted input: frame -> buttons held that frame */
static uint16_t script(int f) {
	static const struct { int f; uint16_t b; } ev[] = {
		{100, B_START}, {200, B_L2},
		{400, B_START}, {420, B_RIGHT}, {470, B_START},          /* -> guinea pig */
		{800, B_START}, {820, B_RIGHT}, {840, B_DOWN}, {860, B_RIGHT}, {880, B_START}, /* chinchilla beige */
		{1200, B_START}, {1220, B_RIGHT}, {1240, B_START},       /* glider */
		{1300, B_TRIANGLE}, {3700, B_TRIANGLE}, {3800, B_R2},
	};
	for (unsigned i = 0; i < sizeof ev / sizeof *ev; i++)
		if (f >= ev[i].f && f < ev[i].f + 3) return ev[i].b;
	return 0;
}

int main(void) {
	game_init();
	int shots[] = {60, 250, 450, 3830};
	int ns = sizeof shots / sizeof *shots, si = 0;
	for (int f = 0; f <= 7600; f++) {
		cur_buttons = script(f);
		game_update(plat_buttons());
		game_render();
		char name[32] = {0};
		static int got_run = 0, got_sleep = 0;
		int is_run = 0, is_sleep = 0;
		for (int i = 0; i < ntext; i++) { if (!strcmp(texts[i], "RUNNING!")) is_run = 1; if (!strcmp(texts[i], "SLEEPING")) is_sleep = 1; }
		if (si < ns && f == shots[si]) { sprintf(name, "out/f%05d", f); si++; flush(name); }
		else if (0 && is_run && got_run++ == 40) { sprintf(name, "out/run%05d", f); flush(name); }
		else if (0 && is_sleep && got_sleep++ == 40) { sprintf(name, "out/sleep%05d", f); flush(name); }
		else flush(NULL);
	}
	return 0;
}
