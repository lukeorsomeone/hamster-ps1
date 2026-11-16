/*
 * RODENT CAGE - PS1 slice of the rodent pet game.
 *
 * Everything is fixed-point integer maths (the PS1 has no FPU):
 *   angles  : 0..4095 = one full turn
 *   sin/cos : 4096 = 1.0
 *   world   : x right, y DOWN (so "up" is negative y), z away from the camera
 * Models are built from coloured boxes, flat shaded per face, and sorted
 * into the ordering table by depth - the classic PS1 recipe.
 */
#include <stdint.h>
#include "game.h"
#include "plat.h"
#include "sintab.h"
#include "font_psx.h"

/* ------------------------------------------------------------------ maths */

static int fsin(int a) {
	a &= 4095;
	int i = (a & 1023) >> 2;
	switch (a >> 10) {
	case 0:  return SIN_Q[i];
	case 1:  return SIN_Q[256 - i];
	case 2:  return -SIN_Q[i];
	default: return -SIN_Q[256 - i];
	}
}
static int fcos(int a) { return fsin(a + 1024); }

static int iabs(int v) { return v < 0 ? -v : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* angle whose (sin, cos) points along (x, z) */
static int fatan2(int x, int z) {
	if (x == 0 && z == 0) return 0;
	int ax = iabs(x), az = iabs(z), a, r;
	if (ax <= az) {
		r = (ax << 12) / az;
		a = ((512 * r) >> 12) + ((((178 * r) >> 12) * (4096 - r)) >> 12);
	} else {
		r = (az << 12) / ax;
		a = 1024 - (((512 * r) >> 12) + ((((178 * r) >> 12) * (4096 - r)) >> 12));
	}
	if (z < 0) a = 2048 - a;
	if (x < 0) a = -a;
	return a & 4095;
}

static int angle_diff(int to, int from) { return ((to - from + 2048) & 4095) - 2048; }

static uint32_t rng_state = 0x1234567;
static int rnd(int n) {
	rng_state = rng_state * 1103515245u + 12345u;
	return (int)((rng_state >> 16) % (uint32_t)n);
}

typedef struct { int m[9]; int t[3]; } Mat;

static void mat_ident(Mat *o) {
	for (int i = 0; i < 9; i++) o->m[i] = (i % 4 == 0) ? 4096 : 0;
	o->t[0] = o->t[1] = o->t[2] = 0;
}
static void mat_rotx(Mat *o, int a) {
	int c = fcos(a), s = fsin(a);
	mat_ident(o);
	o->m[4] = c; o->m[5] = -s;
	o->m[7] = s; o->m[8] = c;
}
static void mat_roty(Mat *o, int a) {
	int c = fcos(a), s = fsin(a);
	mat_ident(o);
	o->m[0] = c;  o->m[2] = s;
	o->m[6] = -s; o->m[8] = c;
}
static void mat_rotz(Mat *o, int a) {
	int c = fcos(a), s = fsin(a);
	mat_ident(o);
	o->m[0] = c; o->m[1] = -s;
	o->m[3] = s; o->m[4] = c;
}
/* o = a * b (apply b first) */
static void mat_mul(Mat *o, const Mat *a, const Mat *b) {
	Mat r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			r.m[i * 3 + j] = (a->m[i * 3] * b->m[j] + a->m[i * 3 + 1] * b->m[3 + j] +
			                  a->m[i * 3 + 2] * b->m[6 + j]) >> 12;
		r.t[i] = ((a->m[i * 3] * b->t[0] + a->m[i * 3 + 1] * b->t[1] +
		           a->m[i * 3 + 2] * b->t[2]) >> 12) + a->t[i];
	}
	*o = r;
}
static void xform(const Mat *m, int x, int y, int z, int *o) {
	o[0] = ((m->m[0] * x + m->m[1] * y + m->m[2] * z) >> 12) + m->t[0];
	o[1] = ((m->m[3] * x + m->m[4] * y + m->m[5] * z) >> 12) + m->t[1];
	o[2] = ((m->m[6] * x + m->m[7] * y + m->m[8] * z) >> 12) + m->t[2];
}

/* ------------------------------------------------------------- renderer */


/* fixed layers for the ground, so nothing sorts underneath it */
#define L_SORT     0
#define L_ROOM     (OT_LEN - 1)   /* floor + walls */
#define L_DECO     (OT_LEN - 2)   /* rug, window, shelf... */
#define L_TABLE    (OT_LEN - 3)
#define L_TRAY     (OT_LEN - 4)
#define L_BEDDING  (OT_LEN - 5)
#define L_SHADOW   (OT_LEN - 6)

static Mat view;
static int light_r = 256, light_g = 256, light_b = 256;   /* day/night tint */


/* ---- primitive cache
 * Most of the scene (room, cage, furniture) doesn't move. While the camera is
 * still, the finished screen-space quads are replayed from a list instead of
 * being transformed again - that's the big speed-up. Lighting is applied on
 * replay so day/night fades still work. */
typedef struct { int16_t otz, xy[8]; uint8_t r, g, b, semi; } CPrim;
typedef struct { CPrim *buf; int cap, count, valid, overflow; int key[16]; } Cache;

static CPrim cbuf_static[2000], cbuf_wheel[600], cbuf_small[2][160];
static Cache c_static = {cbuf_static, 2000, 0, 0, 0, {0}};
static Cache c_wheel  = {cbuf_wheel, 600, 0, 0, 0, {0}};
static Cache c_bowl   = {cbuf_small[0], 160, 0, 0, 0, {0}};
static Cache c_bottle = {cbuf_small[1], 160, 0, 0, 0, {0}};
static Cache *recording = 0;

static void lit_quad(int otz, const int16_t *xy, int r, int g, int b, int semi) {
	if (!semi) { r = (r * light_r) >> 8; g = (g * light_g) >> 8; b = (b * light_b) >> 8; }
	plat_quad(otz, xy, r, g, b, semi);
}

static void out_quad(int otz, const int16_t *xy, int r, int g, int b, int semi) {
	if (recording) {
		if (recording->count < recording->cap) {
			CPrim *c = &recording->buf[recording->count++];
			c->otz = (int16_t)otz;
			for (int i = 0; i < 8; i++) c->xy[i] = xy[i];
			c->r = (uint8_t)r; c->g = (uint8_t)g; c->b = (uint8_t)b; c->semi = (uint8_t)semi;
		} else recording->overflow = 1;
	}
	lit_quad(otz, xy, r, g, b, semi);
}

/* returns 1 if the caller must draw (and record), 0 if it was replayed */
static int cache_begin(Cache *c, const int *key, int nkey) {
	int same = c->valid;
	for (int i = 0; i < nkey && same; i++) same = (c->key[i] == key[i]);
	if (same) {
		for (int i = 0; i < c->count; i++) {
			const CPrim *p = &c->buf[i];
			lit_quad(p->otz, p->xy, p->r, p->g, p->b, p->semi);
		}
		return 0;
	}
	for (int i = 0; i < nkey; i++) c->key[i] = key[i];
	c->count = 0; c->overflow = 0; c->valid = 0;
	recording = c;
	return 1;
}
static void cache_end(void) {
	if (recording) recording->valid = !recording->overflow;
	recording = 0;
}

/* one projected vertex: ok = usable by the GPU; rx/ry = raw screen position */
typedef struct { int16_t x, y; int z, ok, front, rx, ry; } PV;

/* Project a point in the space of matrix m. The matrix is only re-sent to the
 * GTE when it actually changes. */
static Mat gte_cur;
static int gte_valid = 0;

static void use_matrix(const Mat *m) {
	if (gte_valid) {
		int same = 1;
		for (int i = 0; i < 9 && same; i++) same = (m->m[i] == gte_cur.m[i]);
		if (same && m->t[0] == gte_cur.t[0] && m->t[1] == gte_cur.t[1] && m->t[2] == gte_cur.t[2]) return;
	}
	gte_cur = *m;
	gte_valid = 1;
	plat_set_matrix(m->m, m->t);
}

static void project_pt(const Mat *m, int x, int y, int z, PV *o) {
	int sx, sy;
	use_matrix(m);
	o->z = plat_project(x, y, z, &sx, &sy);
	o->front = o->z >= NEAR_Z;
	o->rx = sx; o->ry = sy;
	o->ok = o->front && sx > -1000 && sx < 1000 && sy > -1000 && sy < 1000;
	o->x = (int16_t)sx; o->y = (int16_t)sy;
}

/* is a sphere (camera space centre c, radius rad) at least partly in view? */
static int sphere_visible(const int *c, int rad) {
	if (c[2] + rad < NEAR_Z) return 0;
	if ((iabs(c[0]) - rad) * FOCAL > (c[2] + rad) * (SCREEN_W / 2 + 32)) return 0;
	if ((iabs(c[1]) - rad) * FOCAL > (c[2] + rad) * (SCREEN_H / 2 + 32)) return 0;
	return 1;
}

/* the PS1 GPU skips polygons wider than 1023 or taller than 511 pixels */
static int oversize(const PV *a, const PV *b, const PV *c, const PV *d) {
	int mnx = a->x, mxx = a->x, mny = a->y, mxy = a->y;
	const PV *q[3] = {b, c, d};
	for (int i = 0; i < 3; i++) {
		if (q[i]->x < mnx) mnx = q[i]->x;
		if (q[i]->x > mxx) mxx = q[i]->x;
		if (q[i]->y < mny) mny = q[i]->y;
		if (q[i]->y > mxy) mxy = q[i]->y;
	}
	return (mxx - mnx) > 1000 || (mxy - mny) > 500;
}

/* emit a quad whose corners are already known to be usable */
static void emit_raw(const PV *a, const PV *b, const PV *c, const PV *d,
                     int r, int g, int bl, int layer, int semi) {
	int cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
	if (cross <= 0) return;                     /* back face */
	int otz = layer;
	if (layer == L_SORT)
		otz = clampi((a->z + b->z + c->z + d->z) >> 4, 2, OT_LEN - 7);   /* avg z / 4 */
	int16_t xy[8] = {a->x, a->y, b->x, b->y, c->x, c->y, d->x, d->y};
	out_quad(otz, xy, r, g, bl, semi);
}

static void emit_pv(const PV *a, const PV *b, const PV *c, const PV *d,
                    int r, int g, int bl, int layer, int semi) {
	if (!a->ok || !b->ok || !c->ok || !d->ok) return;
	if (oversize(a, b, c, d)) return;
	int cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
	if (cross <= 0) return;                     /* back face */
	int otz = layer;
	if (layer == L_SORT)
		otz = clampi((a->z + b->z + c->z + d->z) >> 4, 2, OT_LEN - 7);   /* avg z / 4 */
	int16_t xy[8] = {a->x, a->y, b->x, b->y, c->x, c->y, d->x, d->y};
	out_quad(otz, xy, r, g, bl, semi);
}

/* corner index bits: 1 = +x, 2 = +y, 4 = +z.  Faces as TL, TR, BL, BR seen
 * from outside the box. */
static const uint8_t FACES[6][4] = {
	{0, 1, 2, 3},  /* front  -z */
	{5, 4, 7, 6},  /* back   +z */
	{1, 5, 3, 7},  /* right  +x */
	{4, 0, 6, 2},  /* left   -x */
	{4, 5, 0, 1},  /* top    -y */
	{2, 3, 6, 7},  /* bottom +y */
};
static const int SHADE[6] = {210, 210, 178, 178, 256, 120};

/* quad in model space (TL, TR, BL, BR). Big or partly off-screen quads are split
 * into four, up to 3 times, so they still draw on real hardware. */
static void quad_rec(const Mat *m, const int q[4][3], const PV *pre, int r, int g, int b,
                     int layer, int depth) {
	PV p[4];
	if (pre) { p[0] = pre[0]; p[1] = pre[1]; p[2] = pre[2]; p[3] = pre[3]; }
	else for (int k = 0; k < 4; k++) project_pt(m, q[k][0], q[k][1], q[k][2], &p[k]);
	int allok = p[0].ok && p[1].ok && p[2].ok && p[3].ok;
	if (allok && !oversize(&p[0], &p[1], &p[2], &p[3])) {
		emit_pv(&p[0], &p[1], &p[2], &p[3], r, g, b, layer, 0);
		return;
	}
	if (depth >= 2) return;
	int nfront = p[0].front + p[1].front + p[2].front + p[3].front;
	if (nfront == 0) return;
	if (nfront == 4) {      /* entirely off one edge of the screen? */
		int l = 1, rr = 1, t = 1, bt = 1;
		for (int k = 0; k < 4; k++) {
			if (p[k].rx >= 0) l = 0;
			if (p[k].rx < SCREEN_W) rr = 0;
			if (p[k].ry >= 0) t = 0;
			if (p[k].ry < SCREEN_H) bt = 0;
		}
		if (l || rr || t || bt) return;
	}
	int e[9][3];   /* 3x3 grid of points */
	for (int a = 0; a < 3; a++) {
		e[0][a] = q[0][a]; e[2][a] = q[1][a]; e[6][a] = q[2][a]; e[8][a] = q[3][a];
		e[1][a] = (q[0][a] + q[1][a]) / 2; e[7][a] = (q[2][a] + q[3][a]) / 2;
		e[3][a] = (q[0][a] + q[2][a]) / 2; e[5][a] = (q[1][a] + q[3][a]) / 2;
		e[4][a] = (e[3][a] + e[5][a]) / 2;
	}
	static const uint8_t SUB[4][4] = {{0, 1, 3, 4}, {1, 2, 4, 5}, {3, 4, 6, 7}, {4, 5, 7, 8}};
	for (int s4 = 0; s4 < 4; s4++) {
		int sq[4][3];
		for (int k = 0; k < 4; k++)
			for (int a = 0; a < 3; a++) sq[k][a] = e[SUB[s4][k]][a];
		/* skip pieces that are out of view before projecting them */
		int c[3], ctr[3], rad = 0;
		for (int a = 0; a < 3; a++) ctr[a] = (sq[0][a] + sq[3][a]) / 2;
		for (int a = 0; a < 3; a++) rad += iabs(sq[3][a] - sq[0][a]) / 2 + iabs(sq[1][a] - sq[2][a]) / 2;
		xform(m, ctr[0], ctr[1], ctr[2], c);
		if (!sphere_visible(c, rad)) continue;
		quad_rec(m, (const int (*)[3])sq, 0, r, g, b, layer, depth + 1);
	}
}

/* any convex quad, whichever way round it faces */
static void emit_any(const PV *a, const PV *b, const PV *c, const PV *d, int r, int g, int bl, int layer) {
	int cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
	if (cross < 0) emit_pv(a, c, b, d, r, g, bl, layer, 0);
	else           emit_pv(a, b, c, d, r, g, bl, layer, 0);
}

/* ---- static mesh
 * The room, cage and props never move, so at startup they are captured once
 * into world-space vertex and face lists (the same drawing code runs with
 * capturing = 1). Each frame the GTE projects every vertex three at a time,
 * then the faces are emitted from those results. That is far cheaper than
 * transforming hundreds of separate boxes whenever the camera moves.
 * Groups keep the capture order (the same-OT-slot rule depends on it) and are
 * culled as a whole by a bounding sphere. */
typedef struct { int16_t x, y, z, pad; } V4;          /* SVECTOR layout for the GTE */
enum { F_QUAD, F_TRI, F_WIRE };
typedef struct { uint16_t v[4]; int16_t layer; uint8_t r, g, b, kind, depth; } MFace;
typedef struct { uint16_t v0, nv, f0, nf; int16_t c[3], rad; uint8_t cond; } MGroup;
enum { MAXV = 2400, MAXF = 1600, MAXG = 400 };
enum { C_ALWAYS, C_DAY, C_NIGHT };

static V4 mv[MAXV] __attribute__((aligned(8)));
static MFace mf[MAXF];
static MGroup mg[MAXG];
static PV mpv[MAXV];
static int nmv = 0, nmf = 0, nmg = 0;
static int capturing = 0, cap_cond = C_ALWAYS, cap_open = 0, cap_merge = 0;

static void cg_begin(void) {
	if (cap_open || nmg >= MAXG) return;
	MGroup *g = &mg[nmg];
	g->v0 = (uint16_t)nmv; g->f0 = (uint16_t)nmf; g->cond = (uint8_t)cap_cond;
	cap_open = 1;
}
/* close the open group (unless merging, or force) and work out its sphere */
static void cg_end(int force) {
	if (!cap_open || (cap_merge && !force)) return;
	cap_open = 0;
	MGroup *g = &mg[nmg];
	g->nv = (uint16_t)(nmv - g->v0); g->nf = (uint16_t)(nmf - g->f0);
	if (!g->nv || !g->nf) { nmv = g->v0; nmf = g->f0; return; }
	int lo[3] = {32767, 32767, 32767}, hi[3] = {-32768, -32768, -32768};
	for (int i = g->v0; i < nmv; i++) {
		const int16_t *p = &mv[i].x;
		for (int a = 0; a < 3; a++) { if (p[a] < lo[a]) lo[a] = p[a]; if (p[a] > hi[a]) hi[a] = p[a]; }
	}
	int rad = 0;
	for (int a = 0; a < 3; a++) { g->c[a] = (int16_t)((lo[a] + hi[a]) / 2); rad += (hi[a] - lo[a]) / 2 + 1; }
	g->rad = (int16_t)rad;
	nmg++;
}
static void cap_set_cond(int c) { cg_end(1); cap_cond = c; }

static int cap_v(int x, int y, int z) {
	if (nmv >= MAXV) return 0;
	V4 *v = &mv[nmv];
	v->x = (int16_t)x; v->y = (int16_t)y; v->z = (int16_t)z; v->pad = 0;
	return nmv++;
}
static void cap_f(int kind, int a, int b, int c, int d, int r, int g, int bl, int layer, int depth) {
	if (nmf >= MAXF) return;
	MFace *f = &mf[nmf++];
	f->v[0] = (uint16_t)a; f->v[1] = (uint16_t)b; f->v[2] = (uint16_t)c; f->v[3] = (uint16_t)d;
	f->r = (uint8_t)r; f->g = (uint8_t)g; f->b = (uint8_t)bl;
	f->kind = (uint8_t)kind; f->layer = (int16_t)layer; f->depth = (uint8_t)depth;
}
/* capture a point given in the space of m (during capture, view = identity) */
static int cap_pt(const Mat *m, int x, int y, int z) {
	int w[3];
	xform(m, x, y, z, w);
	return cap_v(w[0], w[1], w[2]);
}

static void quad_rec(const Mat *m, const int q[4][3], const PV *pre, int r, int g, int b,
                     int layer, int depth);

/* project vertices v0..v0+n-1 into mpv with the current GTE matrix */
static void project_batch(int v0, int n) {
	static int32_t sxy[48], sz[48];
	while (n > 0) {
		int k = n > 48 ? 48 : n;
		plat_project_n(&mv[v0].x, k, sxy, sz);
		for (int i = 0; i < k; i++) {
			PV *o = &mpv[v0 + i];
			int sx = (int16_t)(sxy[i] & 0xffff), sy = (int16_t)(sxy[i] >> 16);
			o->z = sz[i];
			o->front = sz[i] >= NEAR_Z;
			o->rx = sx; o->ry = sy;
			o->x = (int16_t)sx; o->y = (int16_t)sy;
			o->ok = o->front && sx > -1000 && sx < 1000 && sy > -1000 && sy < 1000;
		}
		v0 += k; n -= k;
	}
}

static void wire_pv(const PV *a, const PV *b, int thick, int r, int g, int bl);

static void draw_mesh(const Mat *view_m, int night) {
	use_matrix(view_m);
	for (int gi = 0; gi < nmg; gi++) {
		const MGroup *g = &mg[gi];
		if ((g->cond == C_DAY && night) || (g->cond == C_NIGHT && !night)) continue;
		int c[3];
		xform(view_m, g->c[0], g->c[1], g->c[2], c);
		if (!sphere_visible(c, g->rad)) continue;
		project_batch(g->v0, g->nv);
		use_matrix(view_m);
		for (int fi = g->f0; fi < g->f0 + g->nf; fi++) {
			const MFace *f = &mf[fi];
			const PV *a = &mpv[f->v[0]], *b = &mpv[f->v[1]], *cc = &mpv[f->v[2]], *d = &mpv[f->v[3]];
			if (f->kind == F_WIRE) { wire_pv(a, b, f->depth, f->r, f->g, f->b); continue; }
			if (f->kind == F_TRI) {
				int cross = (b->x - a->x) * (cc->y - a->y) - (b->y - a->y) * (cc->x - a->x);
				if (cross < 0) emit_pv(a, cc, b, b, f->r, f->g, f->b, f->layer, 0);
				else           emit_pv(a, b, cc, cc, f->r, f->g, f->b, f->layer, 0);
				continue;
			}
			if (a->ok && b->ok && cc->ok && d->ok && !oversize(a, b, cc, d)) {
				emit_raw(a, b, cc, d, f->r, f->g, f->b, f->layer, 0);
				continue;
			}
			/* near the camera or off-screen: split it up (rare) */
			int q[4][3];
			PV pre[4] = {*a, *b, *cc, *d};
			for (int k = 0; k < 4; k++) { q[k][0] = mv[f->v[k]].x; q[k][1] = mv[f->v[k]].y; q[k][2] = mv[f->v[k]].z; }
			quad_rec(view_m, (const int (*)[3])q, pre, f->r, f->g, f->b, f->layer, f->depth);
		}
	}
}

/* thin rod between two projected points, drawn as one flat strip facing the
 * camera: 2 projections instead of a box's 8 */
static void wire_pv(const PV *a, const PV *b, int thick, int r, int g, int bl) {
	if (!a->ok || !b->ok) return;
	int dx = b->x - a->x, dy = b->y - a->y;
	int adx = iabs(dx), ady = iabs(dy);
	int len = (adx > ady) ? adx + (ady >> 1) : ady + (adx >> 1);   /* ~length */
	int z = (a->z + b->z) >> 1;
	int hw = (thick * FOCAL) / (z > 1 ? z : 1);
	if (hw < 1) hw = 1;
	int ox, oy;
	if (len == 0) { ox = hw; oy = 0; }
	else { ox = (-dy * hw) / len; oy = (dx * hw) / len; }
	if (ox == 0 && oy == 0) { if (ady > adx) ox = 1; else oy = 1; }
	PV q[4] = {*a, *b, *a, *b};
	q[0].x += ox; q[0].y += oy; q[1].x += ox; q[1].y += oy;
	q[2].x -= ox; q[2].y -= oy; q[3].x -= ox; q[3].y -= oy;
	emit_any(&q[0], &q[1], &q[2], &q[3], r, g, bl, L_SORT);
}

static void wire(const Mat *m, int x0, int y0, int z0, int x1, int y1, int z1, int thick,
                 int r, int g, int b) {
	if (capturing) {
		cg_begin();
		int va = cap_pt(m, x0, y0, z0), vb = cap_pt(m, x1, y1, z1);
		cap_f(F_WIRE, va, vb, vb, vb, r, g, b, L_SORT, thick);
		cg_end(0);
		return;
	}
	PV a, c;
	project_pt(m, x0, y0, z0, &a);
	project_pt(m, x1, y1, z1, &c);
	wire_pv(&a, &c, thick, r, g, b);
}

/* box centred at (cx,cy,cz) in the space of matrix m (model->camera) */
static void box(const Mat *m, int cx, int cy, int cz, int hx, int hy, int hz,
                int r, int g, int b, int layer) {
	if (capturing) {
		cg_begin();
		int v0 = nmv;
		for (int i = 0; i < 8; i++)
			cap_pt(m, cx + ((i & 1) ? hx : -hx), cy + ((i & 2) ? hy : -hy), cz + ((i & 4) ? hz : -hz));
		for (int f = 0; f < 6; f++) {
			const uint8_t *q = FACES[f];
			int s = SHADE[f];
			cap_f(F_QUAD, v0 + q[0], v0 + q[1], v0 + q[2], v0 + q[3],
			      (r * s) >> 8, (g * s) >> 8, (b * s) >> 8, layer, 1);
		}
		cg_end(0);
		return;
	}
	/* quick reject: bounding sphere entirely behind or beside the camera */
	{
		int c[3];
		xform(m, cx, cy, cz, c);
		if (!sphere_visible(c, hx + hy + hz)) return;
	}
	PV v[8];
	int lc[8][3];
	for (int i = 0; i < 8; i++) {
		lc[i][0] = cx + ((i & 1) ? hx : -hx);
		lc[i][1] = cy + ((i & 2) ? hy : -hy);
		lc[i][2] = cz + ((i & 4) ? hz : -hz);
		project_pt(m, lc[i][0], lc[i][1], lc[i][2], &v[i]);
	}
	for (int f = 0; f < 6; f++) {
		const uint8_t *q = FACES[f];
		int s = SHADE[f];
		const PV *a = &v[q[0]], *b2 = &v[q[1]], *c2 = &v[q[2]], *d = &v[q[3]];
		if (a->ok && b2->ok && c2->ok && d->ok && !oversize(a, b2, c2, d)) {
			emit_pv(a, b2, c2, d, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8, layer, 0);
		} else {
			int fq[4][3];
			for (int k = 0; k < 4; k++)
				for (int ax = 0; ax < 3; ax++) fq[k][ax] = lc[q[k]][ax];
			quad_rec(m, (const int (*)[3])fq, 0, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8, layer, 1);
		}
	}
}

/* flat, upward-facing rectangle on the ground (world space) */
static void ground_rect(int x0, int z0, int x1, int z1, int y, int r, int g, int b,
                        int layer, int semi) {
	int w[4][3] = {{x0, y, z1}, {x1, y, z1}, {x0, y, z0}, {x1, y, z0}};
	PV v[4];
	for (int i = 0; i < 4; i++) project_pt(&view, w[i][0], w[i][1], w[i][2], &v[i]);
	emit_pv(&v[0], &v[1], &v[2], &v[3], r, g, b, layer, semi);
}

/* model matrix (yaw, roll, position) composed with the camera */
static void model_mat(Mat *o, int x, int y, int z, int yaw, int roll) {
	Mat ry, rz, mdl;
	mat_roty(&ry, yaw);
	mat_rotz(&rz, roll);
	mat_mul(&mdl, &ry, &rz);
	mdl.t[0] = x; mdl.t[1] = y; mdl.t[2] = z;
	mat_mul(o, &view, &mdl);
}

static int world_to_screen(int x, int y, int z, int *sx, int *sy) {
	PV p;
	project_pt(&view, x, y, z, &p);
	if (!p.ok) return 0;
	*sx = p.x; *sy = p.y;
	return 1;
}

/* ------------------------------------------------------------ the world */

/* cage interior: x -300..300, z -200..200, bedding surface at y = 0 */
/* same layout as the Godot cage (cage front faces the camera, -z) */
#define CAGE_X   300
#define CAGE_Z   180
#define TRAY_TOP (-53)
#define CAGE_TOP (-293)

#define BOWL_X  (-112)
#define BOWL_Z  (-75)
#define BOTTLE_X 15
#define HOUSE_X 200
#define HOUSE_Z 85
#define HOUSE_W 60
#define HOUSE_D 49
#define WHEEL_X (-190)
#define WHEEL_Z 92
#define WHEEL_Y (-86)
#define WHEEL_R 75
#define WHEEL_D 32

#define DAY_LEN   7200      /* frames per day or night (2 minutes) */
#define MAX_POOP  10

enum { S_IDLE, S_WALK, S_EAT, S_DRINK, S_SLEEP, S_WHEEL, S_PET, S_POPCORN, S_TREAT };

static const char *STATE_NAME[] = {
	"CHILLING", "WANDERING", "EATING", "DRINKING", "SLEEPING", "RUNNING!", "HAPPY!",
	"POPCORNING!", "SNACKING"
};

/* ---- species */
enum { SP_HAMSTER, SP_GUINEA, SP_CHINCHILLA, SP_GLIDER, SP_COUNT };

typedef struct {
	const char *kind, *name;
	const char *colour_name[3];
	uint8_t fur[3][3];      /* main coat */
	uint8_t alt[3][3];      /* belly / patch / stripe */
	int speed;
} Species;

static const Species SPECIES[SP_COUNT] = {
	{"HAMSTER", "HAMMY", {"GOLDEN", "GREY", "WHITE"},
	 {{225, 150, 70}, {150, 150, 162}, {238, 232, 220}},
	 {{248, 228, 196}, {225, 225, 230}, {252, 248, 240}}, 3},
	{"GUINEA PIG", "POPCORN", {"TAN & WHITE", "BLACK & WHITE", "GINGER"},
	 {{200, 140, 80}, {44, 40, 40}, {222, 112, 42}},
	 {{246, 242, 232}, {246, 242, 232}, {244, 206, 160}}, 2},
	{"CHINCHILLA", "MISTY", {"STANDARD GREY", "BEIGE", "BLACK VELVET"},
	 {{150, 150, 162}, {212, 192, 160}, {52, 52, 60}},
	 {{240, 240, 244}, {248, 238, 220}, {200, 200, 208}}, 3},
	{"SUGAR GLIDER", "KIWI", {"CLASSIC GREY", "LEUCISTIC", "MOSAIC"},
	 {{150, 148, 160}, {242, 242, 236}, {204, 198, 192}},
	 {{40, 36, 42}, {242, 242, 236}, {120, 110, 112}}, 4},
};

static struct {
	int x, z, yaw;
	int state, next, tx, tz;
	int timer, phase, hop, moving, spin;
} ham;

static int species = SP_HAMSTER, fur = 0;
static int hunger = 650, thirst = 600, happy = 550, energy = 750;  /* 0..1000 */
static int food = 0;          /* seed portions in the bowl, 0..3 */
static int water = 400;       /* bottle level 0..1000 */
static int treats = 2, treat_on = 0, treat_x, treat_z;
static int poop_n = 0, poop_x[MAX_POOP], poop_z[MAX_POOP], poop_timer = 1800;
static int wheel_angle = 0, wheel_speed = 0;
static int hearts = 0;
static int frame = 0, day_t = 1200;
static int upd_count = 0, last_steps = 1;   /* updates per drawn frame (speed readout) */
static int title = 1, menu = 0, menu_sel = 0;
static int zoom = 0;

static int cam_yaw = 0, cam_pitch = 330, cam_dist = 920;
static int cam_tx = 0, cam_ty = -170, cam_tz = 0;

static char msg_buf[48];
static int msg_timer = 0;
static uint16_t prev_held = 0;

static int text_w(const char *s) {
	int w = 0;
	for (; *s; s++) {
		int c = (unsigned char)*s;
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c >= 32 && c < 128) w += FONT_GLYPH[c - 32][6];
	}
	return w;
}

/* message = a + b + c (any may be NULL) */
static void say3(const char *a, const char *b, const char *c) {
	const char *parts[3] = {a, b, c};
	int n = 0;
	for (int p = 0; p < 3; p++)
		for (const char *s = parts[p]; s && *s && n < 40; s++) msg_buf[n++] = *s;
	msg_buf[n] = 0;
	msg_timer = 180;
}
static void say(const char *s) { say3(s, 0, 0); }
static const char *pet_name(void) { return SPECIES[species].name; }

static int is_night(void) { return day_t >= DAY_LEN; }

static void walk_to(int x, int z, int then) {
	ham.tx = x; ham.tz = z; ham.next = then;
	ham.state = S_WALK;
}

static int busy(void) {
	return ham.state == S_EAT || ham.state == S_DRINK || ham.state == S_SLEEP ||
	       ham.state == S_PET || ham.state == S_TREAT ||
	       (ham.state == S_WALK && ham.next != S_IDLE);
}

static void go_eat(void)   { walk_to(BOWL_X + 72, BOWL_Z, S_EAT); }
static void go_drink(void) { walk_to(BOTTLE_X, CAGE_Z - 70, S_DRINK); }
static void go_sleep(void) { walk_to(HOUSE_X, HOUSE_Z - HOUSE_D - 30, S_SLEEP); }
static void go_play(void) {
	if (species == SP_GUINEA) { ham.state = S_POPCORN; ham.timer = 150; }  /* no wheels for piggies */
	else walk_to(WHEEL_X, WHEEL_Z, S_WHEEL);
}

static int open_spot(int x, int z) {
	if (x > 110 && z > 0) return 0;           /* house */
	if (x < -100 && z > 20) return 0;         /* wheel */
	if (x < -40 && x > -190 && z < -10) return 0;   /* bowl */
	if (x > 10 && x < 140 && z < -60) return 0;     /* chew log */
	return 1;
}

static void pick_wander(void) {
	for (int tries = 0; tries < 10; tries++) {
		int x = rnd(480) - 240, z = rnd(300) - 150;
		if (!open_spot(x, z)) continue;
		walk_to(x, z, S_IDLE);
		return;
	}
	walk_to(0, 0, S_IDLE);
}

/* direction from the pet towards the camera */
static int yaw_to_camera(void) {
	return fatan2(fsin(cam_yaw), -fcos(cam_yaw));
}

static void think(void) {
	int night = is_night();
	if (treat_on) { walk_to(treat_x, treat_z - 40, S_TREAT); return; }
	if (hunger < 400 && food > 0) { go_eat(); return; }
	if (thirst < 400 && water > 0) { go_drink(); return; }
	/* nocturnal: sleepy in the day, lively at night */
	if (energy < (night ? 200 : 420)) { go_sleep(); return; }
	if (energy > 400 && rnd(night ? 2 : 5) == 0) { go_play(); return; }
	if (rnd(3) == 0) { ham.state = S_IDLE; ham.timer = 60 + rnd(120); return; }
	pick_wander();
}

static void update_needs(void) {
	if (frame % 20 == 0) hunger--;
	if (frame % 16 == 0) thirst--;
	if (frame % (is_night() ? 40 : 26) == 0 && ham.state != S_SLEEP) energy--;
	if (frame % 40 == 0) happy--;
	if (frame % 30 == 0 && (hunger < 200 || thirst < 200)) happy--;
	if (frame % 50 == 0 && poop_n >= 3) happy--;
	hunger = clampi(hunger, 0, 1000);
	thirst = clampi(thirst, 0, 1000);
	energy = clampi(energy, 0, 1000);
	happy  = clampi(happy, 0, 1000);

	/* droppings */
	if (ham.state != S_SLEEP && ham.state != S_WHEEL && --poop_timer <= 0) {
		poop_timer = 2000 + rnd(1600);
		if (poop_n < MAX_POOP) {
			poop_x[poop_n] = ham.x - (fsin(ham.yaw) * 55 >> 12);
			poop_z[poop_n] = ham.z - (fcos(ham.yaw) * 55 >> 12);
			poop_n++;
		}
	}
	/* treats come back over time */
	if (frame % 3600 == 0 && treats < 3) treats++;
	/* clock */
	day_t = (day_t + 1) % (DAY_LEN * 2);
	if (day_t == 0) say("GOOD MORNING!");
	if (day_t == DAY_LEN) say3("NIGHT TIME - ", pet_name(), " IS WIDE AWAKE");
}

static void update_hamster(void) {
	ham.moving = 0;
	switch (ham.state) {
	case S_IDLE:
		if (--ham.timer <= 0) think();
		break;

	case S_WALK: {
		int dx = ham.tx - ham.x, dz = ham.tz - ham.z;
		if (iabs(dx) < 5 && iabs(dz) < 5) {
			ham.state = ham.next;
			ham.timer = 0;
			if (ham.state == S_IDLE)  ham.timer = 40 + rnd(100);
			if (ham.state == S_EAT)   ham.timer = 160;
			if (ham.state == S_WHEEL) ham.timer = 300 + rnd(300);
			if (ham.state == S_TREAT) {
				if (!treat_on) { ham.state = S_IDLE; ham.timer = 30; }
				else ham.timer = 80;
			}
			break;
		}
		int want = fatan2(dx, dz);
		int d = angle_diff(want, ham.yaw);
		ham.yaw = (ham.yaw + clampi(d, -90, 90)) & 4095;
		if (iabs(d) < 500) {
			int sp = SPECIES[species].speed + (ham.next == S_TREAT ? 2 : 0);
			ham.x += (fsin(ham.yaw) * sp) >> 12;
			ham.z += (fcos(ham.yaw) * sp) >> 12;
			if (iabs(dx) < 8) ham.x += dx > 0 ? 1 : (dx < 0 ? -1 : 0);
			if (iabs(dz) < 8) ham.z += dz > 0 ? 1 : (dz < 0 ? -1 : 0);
			ham.moving = 1;
		}
		break;
	}

	case S_EAT:
		ham.yaw = (ham.yaw + clampi(angle_diff(3072, ham.yaw), -60, 60)) & 4095;
		if (food <= 0) { say("THE BOWL IS EMPTY"); ham.state = S_IDLE; ham.timer = 60; break; }
		if (--ham.timer % 50 == 0) { food--; hunger += 140; }
		if (hunger >= 1000 || ham.timer <= 0) { hunger = clampi(hunger, 0, 1000); ham.state = S_IDLE; ham.timer = 60; }
		break;

	case S_DRINK:
		ham.yaw = (ham.yaw + clampi(angle_diff(0, ham.yaw), -60, 60)) & 4095;
		if (water <= 0) { say("THE BOTTLE IS EMPTY"); ham.state = S_IDLE; ham.timer = 60; break; }
		thirst += 4; water -= 3;
		if (thirst >= 1000) { thirst = 1000; ham.state = S_IDLE; ham.timer = 60; }
		break;

	case S_TREAT:
		ham.yaw = (ham.yaw + clampi(angle_diff(0, ham.yaw), -60, 60)) & 4095;
		if (--ham.timer <= 0) {
			treat_on = 0;
			happy = clampi(happy + 160, 0, 1000);
			hunger = clampi(hunger + 60, 0, 1000);
			hearts = 70;
			say3(pet_name(), " LOVED THE TREAT!", 0);
			ham.state = S_IDLE; ham.timer = 40;
		}
		break;

	case S_SLEEP:
		energy += 2;
		if (frame % 6 == 0) happy++;
		if (energy >= 1000) {
			energy = 1000;
			ham.x = HOUSE_X; ham.z = HOUSE_Z - HOUSE_D - 30; ham.yaw = 2048;
			ham.state = S_IDLE; ham.timer = 30;
			say3(pet_name(), " WOKE UP", 0);
		}
		break;

	case S_WHEEL:
		ham.x = WHEEL_X; ham.z = WHEEL_Z;
		ham.yaw = 1024;                   /* run along +x, wheel turns under */
		wheel_speed = 70;
		ham.moving = 2;
		if (frame % 2 == 0) energy--;
		if (frame % 5 == 0) happy++;
		if (--ham.timer <= 0 || energy < 150) {
			ham.x = WHEEL_X + 110; ham.state = S_IDLE; ham.timer = 60;
		}
		break;

	case S_POPCORN: {
		/* guinea pig joy: little vertical hops with a twist */
		int t = ham.timer % 25;
		ham.hop = (t < 12) ? t * 4 : (25 - t) * 4;
		if (t == 0) ham.yaw = (ham.yaw + 300 + rnd(900)) & 4095;
		if (frame % 4 == 0) { happy++; energy--; }
		if (--ham.timer <= 0) { ham.hop = 0; ham.state = S_IDLE; ham.timer = 60; }
		break;
	}

	case S_PET: {
		int d = angle_diff(yaw_to_camera(), ham.yaw);
		ham.yaw = (ham.yaw + clampi(d, -120, 120)) & 4095;
		int t = 90 - ham.timer;
		ham.hop = (t % 30 < 15) ? (t % 30) * 2 : (30 - t % 30) * 2;
		if (--ham.timer <= 0) { ham.hop = 0; ham.state = S_IDLE; ham.timer = 40; }
		break;
	}
	}

	ham.x = clampi(ham.x, -CAGE_X + 40, CAGE_X - 40);
	ham.z = clampi(ham.z, -CAGE_Z + 40, CAGE_Z - 40);
	if (ham.moving) ham.phase += (ham.moving == 2) ? 520 : 300;
}

static int pressed(uint16_t held, uint16_t b) { return (held & b) && !(prev_held & b); }

static void draw_room(void);
static void draw_cage(void);
static void draw_props_static(void);

/* capture the room, cage and props into the static mesh, once */
static void build_mesh(void) {
	mat_ident(&view);                  /* model matrices become model->world */
	capturing = 1;
	draw_room();
	draw_cage();
	draw_props_static();
	cg_end(1);
	capturing = 0;
}

void game_init(void) {
	build_mesh();
	ham.x = -40; ham.z = 0; ham.yaw = 2048;
	ham.state = S_IDLE; ham.timer = 90;
}

static void stop_activity(void) {
	if (ham.state == S_WHEEL) ham.x = WHEEL_X + 110;
	ham.hop = 0;
}

static void update_menu(uint16_t held) {
	if (pressed(held, B_UP))   menu_sel = (menu_sel + 2) % 3;
	if (pressed(held, B_DOWN)) menu_sel = (menu_sel + 1) % 3;
	int dir = pressed(held, B_RIGHT) ? 1 : (pressed(held, B_LEFT) ? -1 : 0);
	if (pressed(held, B_CROSS) && menu_sel < 2) dir = 1;
	if (dir && menu_sel == 0) {
		species = (species + dir + SP_COUNT) % SP_COUNT;
		fur = 0;
		stop_activity();
		if (ham.state != S_SLEEP) { ham.state = S_IDLE; ham.timer = 30; }
	}
	if (dir && menu_sel == 1) fur = (fur + dir + 3) % 3;
	if (pressed(held, B_START) || pressed(held, B_TRIANGLE) ||
	    (pressed(held, B_CROSS) && menu_sel == 2)) {
		menu = 0;
		say3("SAY HI TO ", pet_name(), "!");
	}
}

/* camera glides towards its target; runs at the fixed 60 Hz update rate so
 * it feels the same however fast frames are drawn */
static void ease_camera(void) {
	int ttx = 0, tty = -170, ttz = 0, tdist = 920;
	if (zoom && ham.state != S_SLEEP) { ttx = ham.x; tty = -45; ttz = ham.z; tdist = 440; }
	cam_tx += (ttx - cam_tx) / 8;
	cam_ty += (tty - cam_ty) / 8;
	cam_tz += (ttz - cam_tz) / 8;
	cam_dist += (tdist - cam_dist) / 8;
}

void game_update(uint16_t held) {
	frame++;
	upd_count++;
	ease_camera();

	if (title) {
		/* camera holds still on the title so the scene stays cached */
		if (pressed(held, B_START) || pressed(held, B_CROSS)) {
			title = 0;
			say3("SAY HI TO ", pet_name(), "!");
		}
		update_hamster();
		prev_held = held;
		return;
	}

	if (menu) {

		update_menu(held);
		prev_held = held;
		return;
	}
	if (pressed(held, B_START)) { menu = 1; menu_sel = 0; prev_held = held; return; }

	/* camera */
	if (held & (B_L1 | B_LEFT))  cam_yaw = clampi(cam_yaw + 20, -720, 720);
	if (held & (B_R1 | B_RIGHT)) cam_yaw = clampi(cam_yaw - 20, -720, 720);
	if (held & B_UP)   cam_pitch = clampi(cam_pitch + 8, 120, 800);
	if (held & B_DOWN) cam_pitch = clampi(cam_pitch - 8, 120, 800);

	/* actions */
	if (pressed(held, B_CROSS)) {
		if (food >= 3) say("THE BOWL IS ALREADY FULL");
		else {
			food = 3;
			say("YOU FILLED THE FOOD BOWL");
			if (!busy() && hunger < 900) { stop_activity(); go_eat(); }
		}
	}
	if (pressed(held, B_CIRCLE)) {
		if (water >= 950) say("THE BOTTLE IS ALREADY FULL");
		else {
			water = 1000;
			say("YOU REFILLED THE WATER");
			if (!busy() && thirst < 900) { stop_activity(); go_drink(); }
		}
	}
	if (pressed(held, B_SQUARE)) {
		if (ham.state == S_SLEEP) say3("SHH... ", pet_name(), " IS SLEEPING");
		else {
			stop_activity();
			ham.state = S_PET; ham.timer = 90;
			happy = clampi(happy + 110, 0, 1000);
			hearts = 90;
			say3(pet_name(), " LOVES THAT!", 0);
		}
	}
	if (pressed(held, B_L2)) {
		if (treat_on) say("THERE'S ALREADY A TREAT OUT");
		else if (treats <= 0) say("NO TREATS LEFT - WAIT A BIT");
		else {
			treats--;
			treat_on = 1;
			do { treat_x = rnd(400) - 200; treat_z = rnd(240) - 100; } while (!open_spot(treat_x, treat_z - 40));
			if (ham.state == S_SLEEP) say3("A TREAT! ", pet_name(), " WILL FIND IT LATER");
			else {
				say("YOU DROPPED A YOGHURT DROP");
				stop_activity();
				walk_to(treat_x, treat_z - 40, S_TREAT);
			}
		}
	}
	if (pressed(held, B_R2)) {
		if (poop_n == 0) say("THE CAGE IS ALREADY CLEAN");
		else {
			poop_n = 0;
			happy = clampi(happy + 80, 0, 1000);
			say("SPOT CLEANED - SPARKLING!");
		}
	}
	if (pressed(held, B_TRIANGLE)) zoom ^= 1;
	if (pressed(held, B_SELECT)) {
		fur = (fur + 1) % 3;
		say(SPECIES[species].colour_name[fur]);
	}

	update_needs();
	update_hamster();

	if (ham.state != S_WHEEL && wheel_speed > 0) wheel_speed -= 1;
	wheel_angle = (wheel_angle + wheel_speed) & 4095;
	if (hearts > 0) hearts--;
	if (msg_timer > 0) msg_timer--;

	prev_held = held;
}

/* ------------------------------------------------------------ drawing */

static void update_light(void) {
	/* fade between day (full) and night (dim blue) over ~10 seconds */
	int t;
	if (day_t < DAY_LEN) t = (day_t < 600) ? 600 - day_t : 0;              /* dawn */
	else t = (day_t - DAY_LEN < 600) ? day_t - DAY_LEN : 600;              /* dusk */
	if (title || menu) t = 0;
	light_r = 256 - (t * 120) / 600;
	light_g = 256 - (t * 104) / 600;
	light_b = 256 - (t * 40) / 600;
	plat_set_bg((40 * light_r) >> 8, (30 * light_g) >> 8, (52 * light_b) >> 8);
}

static void setup_camera(void) {
	Mat rx, ry;
	mat_rotx(&rx, cam_pitch);
	mat_roty(&ry, cam_yaw);
	mat_mul(&view, &rx, &ry);
	int t[3];
	xform(&view, -cam_tx, -cam_ty, -cam_tz, t);   /* t already includes rotation */
	view.t[0] = t[0];
	view.t[1] = t[1];
	view.t[2] = t[2] + cam_dist;
}

/* ---- room (matches the Godot room: 4 m square, 750 units per metre) */

#define TABLE_TOP  22          /* table surface; bedding is at y = 0 */
#define ROOM_FLOOR 577         /* 0.74 m below the table top */
#define ROOM_TOP   (-1373)     /* 2.6 m walls */
#define BACK_WALL  412         /* wall behind the cage */
#define SIDE_WALL  1500
#define FRONT_WALL (BACK_WALL - 3000)
#define DADO_Y     (-173)      /* rail 1 m up the wall */

/* a flat grid of quads: o = top-left corner, u = across, v = down (as seen from the front) */
static void panel(const int *o, const int *u, const int *v, int nu, int nv,
                  int r0, int g0, int b0, int r1, int g1, int b1, int stripe, int layer) {
	/* captured only (static scenery): one group, grid corners shared */
	cg_end(1);
	cg_begin();
	int v0 = nmv;
	for (int jj = 0; jj <= nv; jj++)
		for (int ii = 0; ii <= nu; ii++)
			cap_v(o[0] + u[0] * ii / nu + v[0] * jj / nv,
			      o[1] + u[1] * ii / nu + v[1] * jj / nv,
			      o[2] + u[2] * ii / nu + v[2] * jj / nv);
	for (int j = 0; j < nv; j++)
		for (int i = 0; i < nu; i++) {
			int tl = v0 + j * (nu + 1) + i, bl = tl + nu + 1;
			int alt = stripe == 1 ? (i & 1) : (stripe == 2 ? ((i + j * 3) % 4 == 0) : 0);
			if (alt) cap_f(F_QUAD, tl, tl + 1, bl, bl + 1, r1, g1, b1, layer, 0);
			else     cap_f(F_QUAD, tl, tl + 1, bl, bl + 1, r0, g0, b0, layer, 0);
		}
	cg_end(1);
}

/* single triangle, visible from either side */
static void tri_world(const Mat *m, const int *a, const int *b, const int *c, int r, int g, int bl, int layer) {
	if (capturing) {
		cg_begin();
		int va = cap_pt(m, a[0], a[1], a[2]), vb = cap_pt(m, b[0], b[1], b[2]), vc = cap_pt(m, c[0], c[1], c[2]);
		cap_f(F_TRI, va, vb, vc, vc, r, g, bl, layer, 0);
		cg_end(0);
		return;
	}
	PV pa, pb, pc;
	project_pt(m, a[0], a[1], a[2], &pa);
	project_pt(m, b[0], b[1], b[2], &pb);
	project_pt(m, c[0], c[1], c[2], &pc);
	int cross = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
	if (cross < 0) emit_pv(&pa, &pc, &pb, &pb, r, g, bl, layer, 0);
	else           emit_pv(&pa, &pb, &pc, &pc, r, g, bl, layer, 0);
}

/* flat octagon on the floor (for the rug) */
static void floor_disc(int cx, int cz, int rad, int r, int g, int b) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);
	int ctr[3] = {cx, ROOM_FLOOR - 1, cz};
	cap_merge = 1;                       /* the whole disc is one group */
	for (int i = 0; i < 8; i++) {
		int a0 = i * 512 + 256, a1 = a0 + 512;
		int p0[3] = {cx + (fcos(a0) * rad >> 12), ROOM_FLOOR - 1, cz + (fsin(a0) * rad >> 12)};
		int p1[3] = {cx + (fcos(a1) * rad >> 12), ROOM_FLOOR - 1, cz + (fsin(a1) * rad >> 12)};
		tri_world(&m, ctr, p0, p1, r, g, b, L_DECO);
	}
	cap_merge = 0;
	cg_end(1);
}

/* the view out of the window: hill + sun by day, moon and stars by night */
static void draw_sky(const Mat *m, int wy, int wz, int night) {
	if (night) {
		box(m, 180, wy - 120, wz + 4, 30, 30, 2, 250, 245, 200, L_DECO);
		for (int i = 0; i < 7; i++)
			box(m, -320 + i * 100, wy - 200 + ((i * 67) % 160), wz + 5, 5, 5, 1, 255, 255, 230, L_DECO);
	} else {
		box(m, -190, wy - 120, wz + 4, 40, 40, 2, 255, 240, 160, L_DECO);
	}
	box(m, 0, wy + 180, wz + 6, 352, 60, 2, night ? 40 : 120, night ? 70 : 190, night ? 60 : 110, L_DECO);
	box(m, 0, wy, wz + 8, 352, 240, 2, night ? 30 : 190, night ? 40 : 225, night ? 100 : 255, L_DECO);
}

/* captured once into the static mesh (see build_mesh) */
static void draw_room(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);

	/* floor: wooden planks */
	{
		int o[3] = {-SIDE_WALL, ROOM_FLOOR, BACK_WALL}, u[3] = {SIDE_WALL * 2, 0, 0}, v[3] = {0, 0, -3000};
		panel(o, u, v, 8, 6, 176, 124, 82, 156, 106, 68, 2, L_ROOM);
	}
	/* walls: striped wallpaper above the dado rail, plain below */
	{
		int o[3] = {-SIDE_WALL, ROOM_TOP, BACK_WALL}, u[3] = {SIDE_WALL * 2, 0, 0};
		int v1[3] = {0, DADO_Y - ROOM_TOP, 0}, o2[3] = {-SIDE_WALL, DADO_Y, BACK_WALL}, v2[3] = {0, ROOM_FLOOR - DADO_Y, 0};
		/* wide, cheery stripes: 8 per wall (was 16) */
		panel(o, u, v1, 8, 1, 236, 214, 200, 222, 194, 184, 1, L_ROOM);
		panel(o2, u, v2, 4, 1, 170, 196, 186, 170, 196, 186, 0, L_ROOM);
		int lo[3] = {-SIDE_WALL, ROOM_TOP, FRONT_WALL}, lu[3] = {0, 0, 3000};
		int lo2[3] = {-SIDE_WALL, DADO_Y, FRONT_WALL};
		panel(lo, lu, v1, 8, 1, 236, 214, 200, 222, 194, 184, 1, L_ROOM);
		panel(lo2, lu, v2, 4, 1, 170, 196, 186, 170, 196, 186, 0, L_ROOM);
		int ro[3] = {SIDE_WALL, ROOM_TOP, BACK_WALL}, ru[3] = {0, 0, -3000};
		int ro2[3] = {SIDE_WALL, DADO_Y, BACK_WALL};
		panel(ro, ru, v1, 8, 1, 236, 214, 200, 222, 194, 184, 1, L_ROOM);
		panel(ro2, ru, v2, 4, 1, 170, 196, 186, 170, 196, 186, 0, L_ROOM);
	}
	/* skirting boards + dado rails (cream trim) */
	box(&m, 0, ROOM_FLOOR - 37, BACK_WALL - 10, SIDE_WALL, 37, 10, 242, 237, 225, L_DECO);
	box(&m, 0, DADO_Y, BACK_WALL - 8, SIDE_WALL, 15, 8, 242, 237, 225, L_DECO);
	box(&m, -SIDE_WALL + 10, ROOM_FLOOR - 37, BACK_WALL - 1500, 10, 37, 1500, 242, 237, 225, L_DECO);
	box(&m, -SIDE_WALL + 8, DADO_Y, BACK_WALL - 1500, 8, 15, 1500, 242, 237, 225, L_DECO);
	box(&m, SIDE_WALL - 10, ROOM_FLOOR - 37, BACK_WALL - 1500, 10, 37, 1500, 242, 237, 225, L_DECO);
	box(&m, SIDE_WALL - 8, DADO_Y, BACK_WALL - 1500, 8, 15, 1500, 242, 237, 225, L_DECO);

	/* round rug in the middle of the room: inner rings first (same slot = drawn later) */
	floor_disc(0, BACK_WALL - 1500, 262, 191, 128, 140);
	floor_disc(0, BACK_WALL - 1500, 525, 242, 217, 178);
	floor_disc(0, BACK_WALL - 1500, 675, 217, 140, 140);

	/* window above the cage, with curtains. Same-slot rule: add front pieces first. */
	{
		int wy = ROOM_FLOOR - 1215, wz = BACK_WALL - 14;
		box(&m, -330, wy - 30, wz - 30, 70, 300, 14, 226, 120, 140, L_DECO);    /* curtains */
		box(&m,  330, wy - 30, wz - 30, 70, 300, 14, 226, 120, 140, L_DECO);
		box(&m, 0, wy - 330, wz - 30, 440, 10, 10, 170, 120, 80, L_DECO);       /* rail */
		box(&m, 0, wy + 270, wz - 30, 395, 15, 45, 242, 237, 225, L_DECO);     /* sill */
		box(&m, 0, wy - 248, wz, 375, 19, 15, 242, 237, 225, L_DECO);          /* frame */
		box(&m, 0, wy + 248, wz, 375, 19, 15, 242, 237, 225, L_DECO);
		box(&m, -360, wy, wz, 19, 262, 15, 242, 237, 225, L_DECO);
		box(&m,  360, wy, wz, 19, 262, 15, 242, 237, 225, L_DECO);
		box(&m, 0, wy, wz, 11, 248, 11, 242, 237, 225, L_DECO);
		/* both skies are captured; only one is drawn at a time */
		cap_set_cond(C_DAY);   draw_sky(&m, wy, wz, 0);
		cap_set_cond(C_NIGHT); draw_sky(&m, wy, wz, 1);
		cap_set_cond(C_ALWAYS);
	}

	/* framed pictures on the side walls */
	for (int s = -1; s <= 1; s += 2) {
		int px = s * (SIDE_WALL - 14), py = ROOM_FLOOR - 1162, pz = -200;
		box(&m, px, py + 40, pz + 60 * s, 6, 30, 30, 255, 235, 170, L_DECO);    /* sun */
		box(&m, px, py + 90, pz, 8, 45, 187, s < 0 ? 90 : 120, s < 0 ? 140 : 110, s < 0 ? 90 : 180, L_DECO);
		box(&m, px, py, pz, 10, 131, 187, s < 0 ? 140 : 150, s < 0 ? 190 : 175, s < 0 ? 140 : 230, L_DECO);
		box(&m, px, py, pz, 12, 168, 225, 140, 96, 60, L_DECO);
	}

	/* bookshelf to the right of the table */
	{
		int bx = 1000, bz = BACK_WALL - 120, top = ROOM_FLOOR - 1100;
		box(&m, bx - 230, (top + ROOM_FLOOR) / 2, bz, 12, 550, 110, 150, 104, 66, L_DECO);   /* sides */
		box(&m, bx + 230, (top + ROOM_FLOOR) / 2, bz, 12, 550, 110, 150, 104, 66, L_DECO);
		for (int sh = 0; sh < 4; sh++)
			box(&m, bx, top + sh * 275 + 10, bz, 230, 10, 110, 170, 120, 80, L_DECO);
		static const uint8_t BOOK[5][3] = {{200, 70, 70}, {70, 110, 190}, {230, 190, 80}, {80, 160, 110}, {150, 90, 170}};
		for (int sh = 0; sh < 3; sh++)
			for (int k = 0; k < 5; k++) {      /* 5 chunky books per shelf */
				int h = 90 + ((k * 37 + sh * 11) % 60);
				const uint8_t *c = BOOK[(k + sh * 2) % 5];
				box(&m, bx - 176 + k * 88, top + (sh + 1) * 275 - h, bz + 20, 32, h, 80, c[0], c[1], c[2], L_DECO);
			}
		box(&m, bx, (top + ROOM_FLOOR) / 2, bz + 105, 230, 550, 6, 120, 80, 52, L_DECO);   /* back */
	}

	/* potted plant in the corner */
	{
		int px = -1150, pz = BACK_WALL - 200;
		for (int i = 0; i < 6; i++) {
			int a = i * 683;
			box(&m, px + (fsin(a) * 90 >> 12), ROOM_FLOOR - 330 - (i % 3) * 60, pz + (fcos(a) * 90 >> 12),
			    45, 70, 45, 60 + i * 8, 150 + (i % 2) * 30, 70, L_DECO);
		}
		box(&m, px, ROOM_FLOOR - 220, pz, 30, 90, 30, 90, 70, 40, L_DECO);
		box(&m, px, ROOM_FLOOR - 75, pz, 95, 75, 95, 200, 110, 80, L_DECO);
	}

	/* the table: white top, warm-grey legs and apron (legs added after = drawn first) */
	box(&m, 0, TABLE_TOP + 18, 0, 394, 18, 255, 236, 232, 224, L_TABLE);
	box(&m, 0, TABLE_TOP + 66, -224, 364, 30, 11, 216, 204, 192, L_TABLE);
	for (int i = 0; i < 4; i++)
		box(&m, (i & 1) ? 356 : -356, (TABLE_TOP + 36 + ROOM_FLOOR) / 2, (i & 2) ? 217 : -217,
		    19, (ROOM_FLOOR - TABLE_TOP - 36) / 2, 19, 204, 191, 178, L_TABLE);
}

/* ---- the cage: 80 x 50 cm, open front (diorama style) like the Godot game */

static void draw_cage(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);
	const int tr = 107, tg = 173, tb = 219;          /* tray blue */

	/* tray base + back and side walls (no front wall: clear panel) */
	box(&m, 0, TABLE_TOP - 4, 0, CAGE_X + 15, 4, CAGE_Z + 15, tr, tg, tb, L_TRAY);
	box(&m, 0, (TRAY_TOP + TABLE_TOP) / 2,  CAGE_Z + 10, CAGE_X + 15, (TABLE_TOP - TRAY_TOP) / 2, 5, tr, tg, tb, L_SORT);
	box(&m, -CAGE_X - 10, (TRAY_TOP + TABLE_TOP) / 2, 0, 5, (TABLE_TOP - TRAY_TOP) / 2, CAGE_Z + 10, tr, tg, tb, L_SORT);
	box(&m,  CAGE_X + 10, (TRAY_TOP + TABLE_TOP) / 2, 0, 5, (TABLE_TOP - TRAY_TOP) / 2, CAGE_Z + 10, tr, tg, tb, L_SORT);
	/* rim all round, including over the clear front panel */
	box(&m, 0, TRAY_TOP, -CAGE_Z - 10, CAGE_X + 18, 5, 8, 92, 152, 196, L_SORT);
	box(&m, 0, TRAY_TOP,  CAGE_Z + 10, CAGE_X + 18, 5, 8, 92, 152, 196, L_SORT);
	box(&m, -CAGE_X - 10, TRAY_TOP, 0, 8, 5, CAGE_Z + 10, 92, 152, 196, L_SORT);
	box(&m,  CAGE_X + 10, TRAY_TOP, 0, 8, 5, CAGE_Z + 10, 92, 152, 196, L_SORT);
	/* front panel edges (the clear plastic itself is invisible on the PS1) */
	box(&m, -CAGE_X - 10, (TRAY_TOP + TABLE_TOP) / 2, -CAGE_Z - 10, 4, (TABLE_TOP - TRAY_TOP) / 2, 4, 200, 225, 240, L_SORT);
	box(&m,  CAGE_X + 10, (TRAY_TOP + TABLE_TOP) / 2, -CAGE_Z - 10, 4, (TABLE_TOP - TRAY_TOP) / 2, 4, 200, 225, 240, L_SORT);

	/* aspen bedding: a lumpy height grid, heaped up against the walls.
	 * Front rows are added first so the back rows draw first. */
	{
		enum { NX = 10, NZ = 6 };
		static int hy[NZ + 1][NX + 1];
		static int built = 0;
		if (!built) {
			for (int iz = 0; iz <= NZ; iz++)
				for (int ix = 0; ix <= NX; ix++) {
					int edge = 0;
					if (ix == 0 || ix == NX || iz == NZ) edge = 14;
					hy[iz][ix] = -edge - (int)((ix * 7 + iz * 13) % 5);
				}
			built = 1;
		}
		cg_end(1);
		cg_begin();
		int v0 = nmv;
		for (int iz = 0; iz <= NZ; iz++)
			for (int ix = 0; ix <= NX; ix++)
				cap_v(-CAGE_X - 5 + ix * (CAGE_X * 2 + 10) / NX, hy[iz][ix],
				      -CAGE_Z - 5 + iz * (CAGE_Z * 2 + 10) / NZ);
		for (int iz = 0; iz < NZ; iz++)
			for (int ix = 0; ix < NX; ix++) {
				/* TL, TR, BL, BR seen from above: far-left, far-right, near-left, near-right */
				int nl = v0 + iz * (NX + 1) + ix, fl = nl + NX + 1;
				int v = ((ix * 7 + iz * 13) % 5) * 7;
				cap_f(F_QUAD, fl, fl + 1, nl, nl + 1, 226 + v / 3, 200 + v / 2, 150 + v / 2, L_BEDDING, 2);
			}
		cg_end(1);
	}

	/* white wire bars on the back and both sides */
	const int wr = 224, wg = 227, wb = 235;
	int hy = (TRAY_TOP - CAGE_TOP) / 2, cy = (TRAY_TOP + CAGE_TOP) / 2;
	(void)hy;
	int y0 = TRAY_TOP, y1 = CAGE_TOP;
	(void)cy;
	cap_merge = 1;                       /* all the bars are one group */
	for (int x = -CAGE_X - 10; x <= CAGE_X + 10; x += 32)
		wire(&m, x, y0, CAGE_Z + 10, x, y1, CAGE_Z + 10, 2, wr, wg, wb);
	for (int z = -CAGE_Z - 10; z <= CAGE_Z + 10; z += 32) {
		wire(&m, -CAGE_X - 10, y0, z, -CAGE_X - 10, y1, z, 2, wr, wg, wb);
		wire(&m,  CAGE_X + 10, y0, z,  CAGE_X + 10, y1, z, 2, wr, wg, wb);
	}
	/* rails: split in thirds so each strip sorts with what's near it */
	static const int RAILS[3] = {TRAY_TOP - 8, CAGE_TOP + 60, CAGE_TOP};
	for (int i = 0; i < 3; i++) {
		int y = RAILS[i];
		for (int k = 0; k < 3; k++) {
			int xa = -CAGE_X - 10 + k * (CAGE_X * 2 + 20) / 3, xb = -CAGE_X - 10 + (k + 1) * (CAGE_X * 2 + 20) / 3;
			int za = -CAGE_Z - 10 + k * (CAGE_Z * 2 + 20) / 3, zb = -CAGE_Z - 10 + (k + 1) * (CAGE_Z * 2 + 20) / 3;
			wire(&m, xa, y, CAGE_Z + 10, xb, y, CAGE_Z + 10, 3, wr, wg, wb);
			wire(&m, -CAGE_X - 10, y, za, -CAGE_X - 10, y, zb, 3, wr, wg, wb);
			wire(&m,  CAGE_X + 10, y, za,  CAGE_X + 10, y, zb, 3, wr, wg, wb);
		}
	}
	cap_merge = 0;
	cg_end(1);
}

static void draw_bowl_food(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);
	{
		static const uint8_t MIX[5][3] = {{158, 115, 64}, {217, 178, 77}, {51, 46, 41}, {140, 158, 77}, {230, 217, 191}};
		for (int i = 0; i < food * 8; i++) {
			int sx = BOWL_X - 20 + (i * 37) % 40, sz = BOWL_Z - 20 + (i * 23) % 40;
			const uint8_t *c = MIX[i % 5];
			box(&m, sx, -28 - (i % 3) * 2, sz, 4, 3, 5, c[0], c[1], c[2], L_SORT);
		}
	}

}

static void draw_bottle(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);
	/* glass water bottle on the back bars: blue cap and base, metal spout */
	{
		int bx = BOTTLE_X, bz = CAGE_Z - 2, lvl = (water * 96) / 1000;
		box(&m, bx, -202, bz, 20, 52, 20, 215, 240, 252, L_SORT);
		if (lvl > 0) box(&m, bx, -150 - lvl / 2, bz, 17, lvl / 2, 17, 90, 158, 240, L_SORT);
		box(&m, bx, -258, bz, 21, 5, 21, 77, 140, 230, L_SORT);
		box(&m, bx, -142, bz, 16, 11, 16, 77, 140, 230, L_SORT);
		box(&m, bx, -112, bz - 12, 3, 22, 3, 205, 209, 220, L_SORT);   /* spout */
		box(&m, bx, -225, bz + 18, 24, 2, 2, 216, 216, 224, L_SORT);   /* clip */
	}
}

static void draw_props_static(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);

	/* ceramic bowl: cream body, red rim */
	box(&m, BOWL_X, -10, BOWL_Z, 32, 10, 32, 245, 235, 214, L_SORT);
	box(&m, BOWL_X, -21, BOWL_Z, 35, 3, 35, 219, 115, 102, L_SORT);
	box(&m, BOWL_X, -24, BOWL_Z, 26, 1, 26, 189, 128, 107, L_SORT);

	/* wooden hut with a pitched roof and an open doorway */
	{
		const int wr = 214, wg = 172, wb = 122;
		int hx = HOUSE_X, hz = HOUSE_Z, H = 75;
		box(&m, hx, -H / 2, hz + HOUSE_D, HOUSE_W, H / 2, 5, wr, wg, wb, L_SORT);              /* back */
		box(&m, hx - HOUSE_W, -H / 2, hz, 5, H / 2, HOUSE_D, wr - 10, wg - 10, wb - 10, L_SORT);
		box(&m, hx + HOUSE_W, -H / 2, hz, 5, H / 2, HOUSE_D, wr - 10, wg - 10, wb - 10, L_SORT);
		box(&m, hx - HOUSE_W + 17, -H / 2, hz - HOUSE_D, 17, H / 2, 5, wr, wg, wb, L_SORT);    /* front */
		box(&m, hx + HOUSE_W - 17, -H / 2, hz - HOUSE_D, 17, H / 2, 5, wr, wg, wb, L_SORT);
		box(&m, hx, -H + 13, hz - HOUSE_D, HOUSE_W - 34, 13, 5, wr, wg, wb, L_SORT);
		box(&m, hx, -H / 2 + 6, hz, HOUSE_W - 6, H / 2 - 6, HOUSE_D - 6, 60, 40, 30, L_SORT);   /* dark inside */
		/* gables */
		int g0[3] = {hx - HOUSE_W, -H, hz - HOUSE_D}, g1[3] = {hx + HOUSE_W, -H, hz - HOUSE_D}, g2[3] = {hx, -H - 38, hz - HOUSE_D};
		tri_world(&m, g0, g1, g2, 230, 191, 140, L_SORT);
		int k0[3] = {hx - HOUSE_W, -H, hz + HOUSE_D}, k1[3] = {hx + HOUSE_W, -H, hz + HOUSE_D}, k2[3] = {hx, -H - 38, hz + HOUSE_D};
		tri_world(&m, k0, k1, k2, 230, 191, 140, L_SORT);
		/* roof slabs, tilted with the roll angle */
		Mat rl, rr;
		model_mat(&rl, hx - HOUSE_W / 2, -H - 20, hz, 0, 450);
		model_mat(&rr, hx + HOUSE_W / 2, -H - 20, hz, 0, -450);
		box(&rl, 0, 0, 0, HOUSE_W / 2 + 18, 5, HOUSE_D + 12, 209, 158, 115, L_SORT);
		box(&rr, 0, 0, 0, HOUSE_W / 2 + 18, 5, HOUSE_D + 12, 209, 158, 115, L_SORT);
	}

	/* chew log near the front */
	{
		Mat lm;
		model_mat(&lm, 70, -14, -100, 260, 0);
		box(&lm, 0, 0, 0, 34, 13, 13, 140, 107, 76, L_SORT);
		box(&lm, -35, 0, 0, 2, 11, 11, 219, 189, 140, L_SORT);
		box(&lm,  35, 0, 0, 2, 11, 11, 219, 189, 140, L_SORT);
	}

	/* pink wheel: stand at the back, two rims with rungs between, spokes */
	{
		Mat sl, sr;
		model_mat(&sl, WHEEL_X - 22, WHEEL_Y / 2, WHEEL_Z + WHEEL_D + 9, 0, -230);
		model_mat(&sr, WHEEL_X + 22, WHEEL_Y / 2, WHEEL_Z + WHEEL_D + 9, 0, 230);
		box(&sl, 0, 0, 0, 5, -WHEEL_Y / 2 + 8, 5, 217, 107, 148, L_SORT);
		box(&sr, 0, 0, 0, 5, -WHEEL_Y / 2 + 8, 5, 217, 107, 148, L_SORT);
		box(&m, WHEEL_X, -4, WHEEL_Z + WHEEL_D + 9, 52, 4, 18, 217, 107, 148, L_SORT);
		box(&m, WHEEL_X, WHEEL_Y, WHEEL_Z + 30, 6, 6, 8, 204, 204, 210, L_SORT);   /* axle */
	}
}

static void draw_wheel(void) {
	{
		const int pr = 242, pg = 140, pb = 178;
		Mat wm;
		model_mat(&wm, WHEEL_X, WHEEL_Y, WHEEL_Z, 0, wheel_angle);
		/* 14 points round each rim, shared by the rims, rungs and spokes */
		enum { N = 14 };
		PV front[N], back[N], hub;
		for (int i = 0; i < N; i++) {
			int a = i * 4096 / N;
			int x = (fsin(a) * WHEEL_R) >> 12, y = (fcos(a) * WHEEL_R) >> 12;
			project_pt(&wm, x, y, -WHEEL_D, &front[i]);
			project_pt(&wm, x, y,  WHEEL_D, &back[i]);
		}
		project_pt(&wm, 0, 0, WHEEL_D, &hub);
		for (int i = 0; i < N; i++) {
			int j = (i + 1) % N;
			wire_pv(&back[i], &back[j], 4, pr - 20, pg - 20, pb - 20);
			wire_pv(&back[i], &front[i], 2, 247, 191, 214);         /* rung */
			if (i % 3 == 0) wire_pv(&hub, &back[i], 3, pr, pg, pb); /* spoke */
			wire_pv(&front[i], &front[j], 4, pr, pg, pb);
		}
	}
}

static void draw_extras(void) {
	Mat m;
	model_mat(&m, 0, 0, 0, 0, 0);
	/* droppings: two little pellets each */
	for (int i = 0; i < poop_n; i++) {
		box(&m, poop_x[i], -3, poop_z[i], 4, 3, 6, 70, 46, 30, L_SORT);
		box(&m, poop_x[i] + 9, -3, poop_z[i] + 5, 4, 3, 6, 64, 42, 28, L_SORT);
	}
	/* yoghurt drop treat, bobbing so you can spot it */
	if (treat_on) {
		int b = (fsin(frame * 90) * 4) >> 12;
		box(&m, treat_x, -10 + b, treat_z, 9, 8, 9, 250, 200, 220, L_SORT);
		box(&m, treat_x, -20 + b, treat_z, 4, 4, 4, 255, 230, 240, L_SORT);
	}
}

/* shared leg helper: four feet, diagonal pairs swing together */
static void feet(const Mat *m, int sx, int fz, int bz, int step, int y) {
	box(m, -sx, y, fz + step, 7, 4, 8, 240, 160, 170, L_SORT);
	box(m,  sx, y, bz + step, 7, 4, 8, 240, 160, 170, L_SORT);
	box(m,  sx, y, fz - step, 7, 4, 8, 240, 160, 170, L_SORT);
	box(m, -sx, y, bz - step, 7, 4, 8, 240, 160, 170, L_SORT);
}

static void draw_pet(void) {
	if (ham.state == S_SLEEP) return;   /* tucked away in the house */

	int hx = ham.x, hz = ham.z, base = -ham.hop;
	if (ham.state == S_WHEEL) base = WHEEL_Y + WHEEL_R - 4;

	int bob = 0, step = 0;
	if (ham.moving) { bob = iabs(fsin(ham.phase)) >> 10; step = fsin(ham.phase) >> 9; }
	int nod = 0;
	if (ham.state == S_EAT || ham.state == S_DRINK || ham.state == S_TREAT)
		nod = (fsin(frame * 160) + 4096) >> 10;

	const Species *sp = &SPECIES[species];
	int fr = sp->fur[fur][0], fg = sp->fur[fur][1], fb = sp->fur[fur][2];
	int ar = sp->alt[fur][0], ag = sp->alt[fur][1], ab = sp->alt[fur][2];

	/* blob shadow */
	if (ham.state != S_WHEEL) {
		int sw = (species == SP_GUINEA) ? 44 : 34, sl = (species == SP_GUINEA) ? 62 : 46;
		ground_rect(hx - sw, hz - sl, hx + sw, hz + sl, -1, 0, 0, 0, L_SHADOW, 1);
	}

	Mat m;
	model_mat(&m, hx, base - bob, hz, ham.yaw, 0);

	switch (species) {
	case SP_HAMSTER:
		box(&m, 0, -30, -4, 31, 26, 40, fr, fg, fb, L_SORT);                 /* body  */
		box(&m, 0, -16, 2, 25, 14, 35, ar, ag, ab, L_SORT);                  /* belly */
		box(&m, 0, -38 + nod, 40, 24, 22, 20, fr, fg, fb, L_SORT);           /* head  */
		box(&m, -19, -27 + nod, 46, 9, 10, 11, ar, ag, ab, L_SORT);          /* cheeks */
		box(&m,  19, -27 + nod, 46, 9, 10, 11, ar, ag, ab, L_SORT);
		box(&m, 0, -31 + nod, 61, 10, 8, 3, ar, ag, ab, L_SORT);             /* snout */
		box(&m, 0, -34 + nod, 64, 4, 3, 2, 240, 120, 140, L_SORT);           /* nose  */
		box(&m, -12, -45 + nod, 60, 4, 4, 2, 20, 16, 24, L_SORT);            /* eyes  */
		box(&m,  12, -45 + nod, 60, 4, 4, 2, 20, 16, 24, L_SORT);
		box(&m, -16, -63 + nod, 34, 8, 7, 3, fr, fg, fb, L_SORT);            /* ears  */
		box(&m,  16, -63 + nod, 34, 8, 7, 3, fr, fg, fb, L_SORT);
		box(&m, -16, -63 + nod, 37, 5, 5, 1, 240, 150, 160, L_SORT);
		box(&m,  16, -63 + nod, 37, 5, 5, 1, 240, 150, 160, L_SORT);
		box(&m, 0, -26, -45, 5, 5, 4, fr, fg, fb, L_SORT);                   /* tail  */
		feet(&m, 18, 26, -24, step, -4);
		break;

	case SP_GUINEA:
		box(&m, 0, -34, -6, 38, 30, 54, fr, fg, fb, L_SORT);                 /* body  */
		box(&m, 0, -44, -30, 30, 18, 26, ar, ag, ab, L_SORT);                /* saddle patch */
		box(&m, 0, -40 + nod, 54, 30, 25, 20, fr, fg, fb, L_SORT);           /* head  */
		box(&m, 0, -42 + nod, 70, 12, 22, 6, ar, ag, ab, L_SORT);            /* blaze */
		box(&m, 0, -30 + nod, 76, 12, 8, 3, ar, ag, ab, L_SORT);             /* muzzle */
		box(&m, 0, -33 + nod, 79, 5, 3, 2, 210, 120, 130, L_SORT);           /* nose  */
		box(&m, -18, -48 + nod, 72, 5, 5, 2, 20, 16, 24, L_SORT);            /* eyes  */
		box(&m,  18, -48 + nod, 72, 5, 5, 2, 20, 16, 24, L_SORT);
		box(&m, -27, -60 + nod, 48, 10, 4, 9, 200, 130, 120, L_SORT);        /* droopy ears */
		box(&m,  27, -60 + nod, 48, 10, 4, 9, 200, 130, 120, L_SORT);
		feet(&m, 24, 36, -36, step, -3);
		break;

	case SP_CHINCHILLA:
		box(&m, 0, -40, -6, 30, 34, 36, fr, fg, fb, L_SORT);                 /* body  */
		box(&m, 0, -26, 6, 24, 22, 26, ar, ag, ab, L_SORT);                  /* belly */
		box(&m, 0, -56 + nod, 34, 24, 22, 20, fr, fg, fb, L_SORT);           /* head  */
		box(&m, 0, -48 + nod, 54, 9, 7, 3, ar, ag, ab, L_SORT);              /* muzzle */
		box(&m, 0, -51 + nod, 57, 4, 3, 2, 220, 140, 150, L_SORT);
		box(&m, -13, -62 + nod, 54, 4, 5, 2, 16, 12, 20, L_SORT);            /* eyes  */
		box(&m,  13, -62 + nod, 54, 4, 5, 2, 16, 12, 20, L_SORT);
		box(&m, -19, -90 + nod, 28, 14, 16, 3, fr, fg, fb, L_SORT);          /* big round ears */
		box(&m,  19, -90 + nod, 28, 14, 16, 3, fr, fg, fb, L_SORT);
		box(&m, -19, -90 + nod, 31, 9, 11, 1, 236, 170, 176, L_SORT);
		box(&m,  19, -90 + nod, 31, 9, 11, 1, 236, 170, 176, L_SORT);
		box(&m, 0, -22, -56, 10, 10, 24, fr, fg, fb, L_SORT);                /* bushy tail */
		box(&m, 0, -44, -80, 10, 18, 9, fr, fg, fb, L_SORT);
		feet(&m, 18, 22, -20, step, -4);
		break;

	case SP_GLIDER:
		box(&m, 0, -24, -4, 24, 18, 36, fr, fg, fb, L_SORT);                 /* body  */
		box(&m, -30, -16, -4, 8, 3, 30, fr, fg, fb, L_SORT);                 /* gliding membrane */
		box(&m,  30, -16, -4, 8, 3, 30, fr, fg, fb, L_SORT);
		box(&m, 0, -43, -4, 4, 2, 37, ar, ag, ab, L_SORT);                   /* back stripe */
		box(&m, 0, -30 + nod, 36, 20, 18, 16, fr, fg, fb, L_SORT);           /* head  */
		box(&m, 0, -49 + nod, 34, 3, 2, 16, ar, ag, ab, L_SORT);             /* head stripe */
		box(&m, 0, -24 + nod, 53, 6, 6, 3, fr, fg, fb, L_SORT);              /* snout */
		box(&m, 0, -26 + nod, 56, 3, 2, 2, 230, 140, 150, L_SORT);
		box(&m, -10, -36 + nod, 51, 7, 7, 2, 10, 8, 14, L_SORT);             /* huge eyes */
		box(&m,  10, -36 + nod, 51, 7, 7, 2, 10, 8, 14, L_SORT);
		box(&m, -15, -55 + nod, 30, 7, 10, 2, 220, 160, 170, L_SORT);        /* ears  */
		box(&m,  15, -55 + nod, 30, 7, 10, 2, 220, 160, 170, L_SORT);
		box(&m, 0, -22, -64, 6, 6, 28, ar, ag, ab, L_SORT);                  /* long tail */
		box(&m, 0, -22, -96, 7, 7, 5, 245, 245, 240, L_SORT);                /* white tip */
		feet(&m, 16, 22, -22, step, -4);
		break;
	}
}

static void bar(int y, const char *label, int value) {
	int r = 70, g = 210, b = 90;
	if (value < 500) { r = 230; g = 200; b = 60; }
	if (value < 250) { r = 230; g = 60; b = 60; if ((frame >> 4) & 1) { r = 255; g = 140; b = 140; } }
	plat_text(8, y, label);
	/* same OT slot draws last-added first, so the fill goes in before the frame */
	plat_tile(1, 76, y + 1, value / 10, 6, r, g, b);
	plat_tile(1, 75, y, 102, 8, 10, 10, 20);
}

static void text_c(int y, const char *s) { plat_text(160 - text_w(s) / 2, y, s); }
static void text_r(int y, const char *s) { plat_text(312 - text_w(s), y, s); }

/* The pause screen holds everything the in-game screen used to show. */
static void draw_menu(void) {
	static const char *ROW_LABEL[3] = {"SPECIES", "COLOUR", "RESUME"};
	const Species *sp = &SPECIES[species];

	text_c(10, "- PAUSED -");

	/* needs + status */
	bar(28, "FOOD",   hunger);
	bar(39, "WATER",  thirst);
	bar(50, "HAPPY",  happy);
	bar(61, "ENERGY", energy);
	text_r(28, sp->name);
	text_r(39, STATE_NAME[ham.state]);
	text_r(50, is_night() ? "NIGHT" : "DAY");
	{
		char t[12] = "TREATS 0";
		t[7] = (char)('0' + treats);
		text_r(61, t);
	}
	if (poop_n >= 3) text_c(76, "THE CAGE NEEDS A CLEAN");
	else if (msg_buf[0]) text_c(76, msg_buf);

	/* options */
	for (int i = 0; i < 3; i++) {
		int y = 94 + i * 14;
		if (i == menu_sel && ((frame >> 3) & 3)) plat_text(26, y, ">");
		plat_text(40, y, ROW_LABEL[i]);
		if (i == 0) { plat_text(132, y, "<"); plat_text(146, y, sp->kind); }
		if (i == 1) { plat_text(132, y, "<"); plat_text(146, y, sp->colour_name[fur]); }
	}
	text_c(140, "UP/DN PICK   L/R CHANGE");

	{
		char f[16] = "SPEED 00 FPS";
		int fps = 60 / last_steps;
		f[6] = (char)('0' + fps / 10); f[7] = (char)('0' + fps % 10);
		plat_text(12, 156, f);
	}

	/* backdrop last: in the same OT slot it draws first, under the bars */
	plat_tile(1, 6, 4, 308, 168, 18, 10, 36);
}

static void draw_hud(void) {
	int sx, sy;
	if (hearts > 0 && ham.state != S_SLEEP &&
	    world_to_screen(ham.x, -120 - (90 - hearts) / 2, ham.z, &sx, &sy))
		plat_text(sx - 8, sy, "<3");
	if (ham.state == S_SLEEP &&
	    world_to_screen(HOUSE_X, -150 - (frame % 60) / 2, HOUSE_Z, &sx, &sy))
		plat_text(sx + ((frame / 20) % 2) * 4, sy, (frame % 90 < 45) ? "Z" : "Z Z");

	if (title) {
		plat_tile(1, 60, 70, 200, 52, 20, 10, 40);
		text_c(82, "RODENT CAGE");
		text_c(98, "PS1 EDITION");
		if ((frame >> 5) & 1) text_c(180, "PRESS START");
		return;
	}
	if (menu) { draw_menu(); return; }
	/* in-game: just the pet's needs in the corner; the rest is on the pause screen */
	bar(8,  "FOOD",   hunger);
	bar(19, "WATER",  thirst);
	bar(30, "HAPPY",  happy);
	bar(41, "ENERGY", energy);
}

void game_render(void) {
	gte_valid = 0;
	last_steps = upd_count > 0 ? upd_count : 1;
	upd_count = 0;
	update_light();
	setup_camera();

	/* cache key: the camera, plus whatever changes that piece */
	int key[16];
	for (int i = 0; i < 9; i++) key[i] = view.m[i];
	key[9] = view.t[0]; key[10] = view.t[1]; key[11] = view.t[2];
	key[12] = is_night() && !title && !menu;     /* window sky changes */
	if (cache_begin(&c_static, key, 13)) { draw_mesh(&view, key[12]); cache_end(); }
	key[12] = wheel_angle;
	if (cache_begin(&c_wheel, key, 13)) { draw_wheel(); cache_end(); }
	key[12] = food;
	if (cache_begin(&c_bowl, key, 13)) { draw_bowl_food(); cache_end(); }
	key[12] = water / 10;
	if (cache_begin(&c_bottle, key, 13)) { draw_bottle(); cache_end(); }

	draw_extras();
	draw_pet();
	draw_hud();
}
