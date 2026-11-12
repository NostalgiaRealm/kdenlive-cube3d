/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube3d - software renderer for a textured box.
 *
 * How it works
 *   1. The box is posed and its six faces are projected with a pinhole camera.
 *      The screen plane sits at z = 0, so an unrotated front face is shown at
 *      exactly its nominal size whatever the field of view.
 *   2. A box is convex: faces seen from outside never overlap each other on
 *      screen, and neither do faces seen from inside. So no depth buffer is
 *      needed - there is an "inside" layer and an "outside" layer, each the
 *      plain sum of its faces, and the result is outside OVER inside OVER
 *      background.
 *   3. Edges are antialiased with 32 coverage samples per pixel. Two faces
 *      that share an edge evaluate the very same line equation, so every
 *      sample belongs to exactly one of them and no seam can show.
 *   4. Each face maps screen -> texture with a projective transform
 *      (perspective correct), sampled trilinearly from a mip chain with a few
 *      extra taps along the direction a face is foreshortened in.
 */
#include "c3d_render.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "cube3d packs pixels assuming a little-endian target"
#endif

#define MAX_LEVELS 16
#define NSAMP 32              /* coverage samples per pixel = bits in a uint32_t   */
#define MAX_VERTS 8           /* a quad clipped by one plane has at most 5 corners */
#define ACC_ONE (255 * NSAMP) /* accumulator value of an opaque, fully covered channel */
#define EDGE_R 0.7072         /* a little over half a pixel diagonal */
#define ROW_CHUNK 8
#define TEX_GUARD 8.0 /* how far outside a texture coordinates may stray, in texels */
#define TEX_BIAS 32   /* keeps coordinates positive so (int) truncation equals floor */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

struct c3d_texture
{
    int nlevels;
    int w[MAX_LEVELS], h[MAX_LEVELS];
    uint32_t *px[MAX_LEVELS]; /* premultiplied, bytes R,G,B,A */
    double aspect;
};

static inline uint32_t div255(uint32_t v)
{
    v += 128;
    return (v + (v >> 8)) >> 8;
}

c3d_texture *c3d_texture_create(const uint8_t *rgba, int stride, int width, int height,
                                int rx, int ry, int rw, int rh, double display_aspect)
{
    if (!rgba || width <= 0 || height <= 0)
        return NULL;
    if (rx < 0) {
        rw += rx;
        rx = 0;
    }
    if (ry < 0) {
        rh += ry;
        ry = 0;
    }
    if (rw > width - rx)
        rw = width - rx;
    if (rh > height - ry)
        rh = height - ry;
    if (rw <= 0 || rh <= 0)
        return NULL;

    c3d_texture *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    uint32_t *dst = malloc((size_t) rw * (size_t) rh * sizeof(uint32_t));
    if (!dst) {
        free(t);
        return NULL;
    }
    for (int y = 0; y < rh; y++) {
        const uint8_t *s = rgba + (size_t) (ry + y) * (size_t) stride + (size_t) rx * 4;
        uint32_t *d = dst + (size_t) y * rw;
        for (int x = 0; x < rw; x++, s += 4) {
            uint32_t a = s[3];
            if (a == 255) {
                d[x] = (uint32_t) s[0] | (uint32_t) s[1] << 8 | (uint32_t) s[2] << 16 | 0xff000000u;
            } else {
                d[x] = div255(s[0] * a) | div255(s[1] * a) << 8 | div255(s[2] * a) << 16 | a << 24;
            }
        }
    }
    t->px[0] = dst;
    t->w[0] = rw;
    t->h[0] = rh;
    t->nlevels = 1;
    t->aspect = display_aspect > 0.0 && isfinite(display_aspect) ? display_aspect
                                                                 : (double) rw / (double) rh;
    return t;
}

void c3d_texture_destroy(c3d_texture *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->nlevels; i++)
        free(t->px[i]);
    free(t);
}

/* Adds one more (half size, box filtered) level. Returns 0 when there is
 * nothing left to add. Level n texel i covers level n-1 texels 2i and 2i+1,
 * so level n coordinates are level 0 coordinates divided by 2^n. */
static int tex_add_level(c3d_texture *t)
{
    int l = t->nlevels;
    if (l >= MAX_LEVELS)
        return 0;
    int sw = t->w[l - 1], sh = t->h[l - 1];
    if (sw <= 1 && sh <= 1)
        return 0;
    int dw = sw > 1 ? sw >> 1 : 1, dh = sh > 1 ? sh >> 1 : 1;
    int xs = sw > 1, ys = sh > 1;
    uint32_t *d = malloc((size_t) dw * (size_t) dh * sizeof(uint32_t));
    if (!d)
        return 0;
    const uint32_t *s = t->px[l - 1];
    for (int y = 0; y < dh; y++) {
        const uint32_t *r0 = s + (size_t) (ys ? y * 2 : 0) * sw;
        const uint32_t *r1 = r0 + (size_t) ys * sw;
        uint32_t *o = d + (size_t) y * dw;
        for (int x = 0; x < dw; x++) {
            int x0 = xs ? x * 2 : 0;
            uint32_t a = r0[x0], b = r0[x0 + xs], c = r1[x0], e = r1[x0 + xs];
            uint32_t rb = (((a & 0x00ff00ffu) + (b & 0x00ff00ffu) + (c & 0x00ff00ffu)
                            + (e & 0x00ff00ffu) + 0x00020002u)
                           >> 2)
                          & 0x00ff00ffu;
            uint32_t ag = ((((a >> 8) & 0x00ff00ffu) + ((b >> 8) & 0x00ff00ffu)
                            + ((c >> 8) & 0x00ff00ffu) + ((e >> 8) & 0x00ff00ffu) + 0x00020002u)
                           >> 2)
                          & 0x00ff00ffu;
            o[x] = rb | ag << 8;
        }
    }
    t->px[l] = d;
    t->w[l] = dw;
    t->h[l] = dh;
    t->nlevels = l + 1;
    return 1;
}

/* a*(256-f) + b*f, per channel, f in 0..256 */
static inline uint32_t px_lerp(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t g = 256 - f;
    uint32_t rb = (((a & 0x00ff00ffu) * g + (b & 0x00ff00ffu) * f + 0x00800080u) >> 8) & 0x00ff00ffu;
    uint32_t ag = (((a >> 8) & 0x00ff00ffu) * g + ((b >> 8) & 0x00ff00ffu) * f + 0x00800080u)
                  & 0xff00ff00u;
    return rb | ag;
}

static inline uint32_t tex_fetch(const uint32_t *px, int w, int h, int x, int y, int border)
{
    if ((unsigned) x >= (unsigned) w || (unsigned) y >= (unsigned) h) {
        if (border)
            return 0;
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    }
    return px[(size_t) y * w + x];
}

/* s,t in texels of this level, texel centres at +0.5; both > 0.5 - TEX_BIAS */
static inline uint32_t tex_bilinear(const uint32_t *px, int w, int h, float s, float t, int border)
{
    float sb = s + ((float) TEX_BIAS - 0.5f), tb = t + ((float) TEX_BIAS - 0.5f);
    int xi = (int) sb, yi = (int) tb;
    uint32_t fx = (uint32_t) ((sb - (float) xi) * 256.0f);
    uint32_t fy = (uint32_t) ((tb - (float) yi) * 256.0f);
    int x0 = xi - TEX_BIAS, y0 = yi - TEX_BIAS;
    uint32_t p00, p10, p01, p11;
    if (x0 >= 0 && y0 >= 0 && x0 < w - 1 && y0 < h - 1) {
        const uint32_t *p = px + (size_t) y0 * w + x0;
        p00 = p[0];
        p10 = p[1];
        p01 = p[w];
        p11 = p[w + 1];
    } else {
        p00 = tex_fetch(px, w, h, x0, y0, border);
        p10 = tex_fetch(px, w, h, x0 + 1, y0, border);
        p01 = tex_fetch(px, w, h, x0, y0 + 1, border);
        p11 = tex_fetch(px, w, h, x0 + 1, y0 + 1, border);
    }
    return px_lerp(px_lerp(p00, p10, fx), px_lerp(p01, p11, fx), fy);
}

/* s,t in level 0 texels */
static inline uint32_t tex_sample(const c3d_texture *t, float s, float tt, float lod, int border,
                                  int trilinear)
{
    int last = t->nlevels - 1;
    if (!(lod > 0.0f) || last == 0)
        return tex_bilinear(t->px[0], t->w[0], t->h[0], s, tt, border);
    int l;
    uint32_t f;
    if (trilinear) {
        l = (int) lod;
        f = (uint32_t) ((lod - (float) l) * 256.0f);
    } else {
        l = (int) (lod + 0.5f);
        f = 0;
    }
    if (l >= last) {
        l = last;
        f = 0;
    }
    float sc = 1.0f / (float) (1 << l);
    uint32_t a = tex_bilinear(t->px[l], t->w[l], t->h[l], s * sc, tt * sc, border);
    if (!f)
        return a;
    sc *= 0.5f;
    uint32_t b = tex_bilinear(t->px[l + 1], t->w[l + 1], t->h[l + 1], s * sc, tt * sc, border);
    return px_lerp(a, b, f);
}

/* log2 for x > 0, good to about 0.01 - plenty for picking a mip level */
static inline float fast_log2(float x)
{
    union
    {
        float f;
        uint32_t i;
    } u = {x};
    float e = (float) ((int) ((u.i >> 23) & 255) - 127);
    u.i = (u.i & 0x007fffffu) | 0x3f800000u;
    float m = u.f;
    return e + ((-0.33333333f * m + 2.0f) * m - 1.66666667f);
}

/* ------------------------------------------------------------------ */
/* polygons                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    double x, y, z;
} vec3;

static inline vec3 v3(double x, double y, double z)
{
    vec3 r = {x, y, z};
    return r;
}
static inline vec3 v3_sub(vec3 a, vec3 b)
{
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline double v3_dot(vec3 a, vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline vec3 v3_cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

typedef struct
{
    double a, b, c;        /* canonical line with unit normal: e = a*x + b*y + c */
    double off[NSAMP];     /* a*ox + b*oy of every coverage sample, ascending    */
    uint32_t ge[NSAMP + 1]; /* ge[k] = samples whose rank in off[] is >= k       */
    int sign;              /* +1: inside is e >= 0, -1: inside is e < 0          */
} edge_t;

typedef struct
{
    int n;                              /* corners */
    double vx[MAX_VERTS], vy[MAX_VERTS]; /* in pixels */
    int y0, y1; /* first / last pixel row touched */
    int layer;  /* 0 = seen from inside, 1 = seen from outside */
    c3d_texture *tex;
    int border;                  /* 1: outside the picture is empty, 0: repeat the edge */
    double fs[3], ft[3], fq[3];  /* s = fs(x,y)/fq(x,y), t = ft(x,y)/fq(x,y), in texels */
    double qmin;
    double smax, tmax;
    uint32_t color;      /* premultiplied, used when tex == NULL */
    uint32_t k_rgb, k_a; /* shading * opacity and opacity, 0..256 */
    int ne;              /* edges (degenerate ones are dropped) */
    edge_t e[MAX_VERTS]; /* keep last: build_polys() clears everything before it */
} poly_t;

static void sample_pattern(double ox[NSAMP], double oy[NSAMP])
{
    /* Hammersley points: every sample has its own column and its own row,
     * so near-horizontal and near-vertical edges get all 32 levels too. */
    for (int k = 0; k < NSAMP; k++) {
        unsigned r = 0;
        for (int b = 0; b < 5; b++)
            if (k & (1 << b))
                r |= 1u << (4 - b);
        ox[k] = (k + 0.5) / NSAMP - 0.5;
        oy[k] = (r + 0.5) / NSAMP - 0.5;
    }
}

/*
 * Line through p and q. orient is the sign of the polygon's area; with p,q in
 * polygon order the inside is where orient * (dx*(y-py) - dy*(x-px)) > 0.
 * The line is always built from the lexicographically smaller end point, so
 * the two faces sharing an edge get bit-identical coefficients and only
 * differ in sign.
 */
static int edge_setup(edge_t *e, double px, double py, double qx, double qy, int orient,
                      const double *ox, const double *oy)
{
    int sign = orient;
    if (px > qx || (px == qx && py > qy)) {
        double t = px;
        px = qx;
        qx = t;
        t = py;
        py = qy;
        qy = t;
        sign = -sign;
    }
    double dx = qx - px, dy = qy - py;
    double len = sqrt(dx * dx + dy * dy);
    if (!(len > 1e-9))
        return 0;
    e->a = -dy / len;
    e->b = dx / len;
    e->c = -(e->a * px + e->b * py);
    e->sign = sign;

    int idx[NSAMP];
    for (int k = 0; k < NSAMP; k++) {
        double v = e->a * ox[k] + e->b * oy[k];
        int j = k;
        while (j > 0 && e->off[j - 1] > v) {
            e->off[j] = e->off[j - 1];
            idx[j] = idx[j - 1];
            j--;
        }
        e->off[j] = v;
        idx[j] = k;
    }
    uint32_t m = 0;
    e->ge[NSAMP] = 0;
    for (int k = NSAMP - 1; k >= 0; k--) {
        m |= 1u << idx[k];
        e->ge[k] = m;
    }
    return 1;
}

/* Samples of one pixel that lie on the inside of an edge. ev is the canonical
 * line value at the pixel centre. */
static inline uint32_t edge_mask(const edge_t *e, double ev)
{
    /* canonical inside: ev + off >= 0  <=>  off >= -ev */
    double key = -ev;
    int lo = 0, hi = NSAMP;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (e->off[mid] >= key)
            hi = mid;
        else
            lo = mid + 1;
    }
    uint32_t m = e->ge[lo];
    return e->sign > 0 ? m : ~m;
}

/* Horizontal extent of the polygon inside the pixel row y. */
static int row_extent(const poly_t *p, int y, double *xl, double *xr)
{
    double top = (double) y, bot = (double) y + 1.0;
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < p->n; i++) {
        int j = i + 1 == p->n ? 0 : i + 1;
        double x0 = p->vx[i], y0 = p->vy[i], x1 = p->vx[j], y1 = p->vy[j];
        if (y0 > y1) {
            double t = x0;
            x0 = x1;
            x1 = t;
            t = y0;
            y0 = y1;
            y1 = t;
        }
        if (y1 < top || y0 > bot)
            continue;
        double xa = x0, xb = x1;
        if (y1 > y0) {
            double slope = (x1 - x0) / (y1 - y0);
            if (y0 < top)
                xa = x0 + (top - y0) * slope;
            if (y1 > bot)
                xb = x0 + (bot - y0) * slope;
        }
        if (xa > xb) {
            double t = xa;
            xa = xb;
            xb = t;
        }
        if (xa < lo)
            lo = xa;
        if (xb > hi)
            hi = xb;
    }
    if (lo > hi)
        return 0;
    *xl = lo;
    *xr = hi;
    return 1;
}

/* Squared singular values of d(s,t)/d(x,y) at a pixel, i.e. the squared
 * lengths of the axes of the pixel's footprint in the texture. */
static inline void footprint(float dsdx, float dtdx, float dsdy, float dtdy, float *l1, float *l2,
                             float *A, float *B, float *C)
{
    *A = dsdx * dsdx + dsdy * dsdy;
    *C = dtdx * dtdx + dtdy * dtdy;
    *B = dsdx * dtdx + dsdy * dtdy;
    float half = 0.5f * (*A + *C), diff = 0.5f * (*A - *C);
    float root = sqrtf(diff * diff + *B * *B);
    *l1 = half + root;
    *l2 = half - root;
}

static inline uint32_t shade_pixel(const poly_t *p, double xc, double srow, double trow,
                                   double qrow, int quality)
{
    const c3d_texture *tex = p->tex;
    double q = p->fq[0] * xc + qrow;
    if (!(q > p->qmin))
        q = p->qmin;
    double inv = 1.0 / q;
    double sd = (p->fs[0] * xc + srow) * inv;
    double td = (p->ft[0] * xc + trow) * inv;
    float dsdx = (float) ((p->fs[0] - sd * p->fq[0]) * inv);
    float dtdx = (float) ((p->ft[0] - td * p->fq[0]) * inv);
    float dsdy = (float) ((p->fs[1] - sd * p->fq[1]) * inv);
    float dtdy = (float) ((p->ft[1] - td * p->fq[1]) * inv);
    if (!(sd > -TEX_GUARD))
        sd = -TEX_GUARD;
    else if (sd > p->smax + TEX_GUARD)
        sd = p->smax + TEX_GUARD;
    if (!(td > -TEX_GUARD))
        td = -TEX_GUARD;
    else if (td > p->tmax + TEX_GUARD)
        td = p->tmax + TEX_GUARD;
    float s = (float) sd, t = (float) td;

    float l1, l2, A, B, C;
    footprint(dsdx, dtdx, dsdy, dtdy, &l1, &l2, &A, &B, &C);
    if (!(l1 > 1.0f)) /* magnified in every direction (also catches NaN) */
        return tex_bilinear(tex->px[0], tex->w[0], tex->h[0], s, t, p->border);
    if (!(l1 < 1e12f))
        l1 = 1e12f;

    if (quality == C3D_QUALITY_DRAFT)
        return tex_sample(tex, s, t, 0.5f * fast_log2(l1) - 0.5f, p->border, 0);

    /* Anisotropic: n taps along the long axis of the footprint, each from the
     * mip level that matches the footprint's width divided by n. */
    int maxn = quality == C3D_QUALITY_BEST ? 16 : 4;
    float sig1 = sqrtf(l1);
    int n = 1;
    if (l2 < l1 * 0.6944f) { /* ratio > 1.2 */
        float ratio = l2 > l1 * 1e-4f ? sqrtf(l1 / l2) : 100.0f;
        float want = ratio < sig1 ? ratio : sig1; /* no point in taps closer than a texel */
        n = (int) want;
        if ((float) n < want)
            n++;
        if (n > maxn)
            n = maxn;
        if (n < 1)
            n = 1;
    }
    float lod = fast_log2(sig1 / (float) n);
    if (n == 1)
        return tex_sample(tex, s, t, lod, p->border, 1);

    /* direction of the long axis in texture space: eigenvector of [[A,B],[B,C]] for l1 */
    float ex, ey;
    if (fabsf(l1 - C) > fabsf(l1 - A)) {
        ex = l1 - C;
        ey = B;
    } else {
        ex = B;
        ey = l1 - A;
    }
    float el = sqrtf(ex * ex + ey * ey);
    if (!(el > 1e-20f))
        return tex_sample(tex, s, t, lod, p->border, 1);
    float step = sig1 / ((float) n * el);
    ex *= step;
    ey *= step;
    float s0 = s - ex * 0.5f * (float) (n - 1), t0 = t - ey * 0.5f * (float) (n - 1);
    float gmax_s = (float) (p->smax + TEX_GUARD), gmax_t = (float) (p->tmax + TEX_GUARD);
    uint32_t r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < n; i++) {
        float ss = s0 + ex * (float) i, ts = t0 + ey * (float) i;
        if (ss < (float) -TEX_GUARD)
            ss = (float) -TEX_GUARD;
        else if (ss > gmax_s)
            ss = gmax_s;
        if (ts < (float) -TEX_GUARD)
            ts = (float) -TEX_GUARD;
        else if (ts > gmax_t)
            ts = gmax_t;
        uint32_t c = tex_sample(tex, ss, ts, lod, p->border, 1);
        r += c & 255;
        g += (c >> 8) & 255;
        b += (c >> 16) & 255;
        a += c >> 24;
    }
    uint32_t h = (uint32_t) n >> 1, un = (uint32_t) n;
    return ((r + h) / un) | ((g + h) / un) << 8 | ((b + h) / un) << 16 | ((a + h) / un) << 24;
}

/* Per-row scratch. Colours are premultiplied and scaled so that ACC_ONE is an
 * opaque, fully covered pixel. */
typedef struct
{
    uint16_t *front; /* faces seen from outside                                   */
    uint16_t *back;  /* faces seen from inside, already attenuated by the front   */
    uint32_t *fmask; /* per pixel: coverage samples inside any outside-seen face  */
    int fmin, fmax;  /* pixel ranges written in this row (min > max: none)        */
    int bmin, bmax;
} rowbuf_t;

/*
 * Adds this polygon's share of pixel row y to its layer.
 *
 * Outside-seen faces must be rastered before inside-seen ones: an inside face
 * only counts on the coverage samples no outside face claimed, and - scaled by
 * that face's transparency - on the ones it did. That keeps the silhouette of
 * a closed box exactly as wide as it should be instead of counting the same
 * sliver of a pixel twice.
 */
static void raster_row(const poly_t *p, int y, int W, rowbuf_t *rb, int quality)
{
    double xl, xr;
    if (!row_extent(p, y, &xl, &xr))
        return;
    if (xr < 0.0 || xl > (double) W)
        return;
    int x0 = xl <= 0.0 ? 0 : (int) xl;
    int x1 = xr >= (double) (W - 1) ? W - 1 : (int) xr;
    if (x0 > x1)
        return;

    double yc = (double) y + 0.5;
    double er[MAX_VERTS];
    /* pixel centres in [lo, hi] are at least EDGE_R inside every edge */
    double lo = -1e9, hi = 1e9;
    for (int i = 0; i < p->ne; i++) {
        const edge_t *e = &p->e[i];
        er[i] = e->b * yc + e->c;
        double a = e->sign > 0 ? e->a : -e->a;
        double r = e->sign > 0 ? er[i] : -er[i];
        if (a > 1e-12) {
            double v = (EDGE_R - r) / a;
            if (v > lo)
                lo = v;
        } else if (a < -1e-12) {
            double v = (EDGE_R - r) / a;
            if (v < hi)
                hi = v;
        } else if (r < EDGE_R) {
            lo = 1e9;
        }
    }
    /* one pixel of slack on both sides: those go through the exact test */
    int xi0 = lo >= 1e8 ? W : (lo <= -1e8 ? -1 : (int) ceil(lo - 0.5) + 1);
    int xi1 = hi <= -1e8 ? -1 : (hi >= 1e8 ? W : (int) floor(hi - 0.5) - 1);

    double srow = 0.0, trow = 0.0, qrow = 0.0;
    if (p->tex) {
        srow = p->fs[1] * yc + p->fs[2];
        trow = p->ft[1] * yc + p->ft[2];
        qrow = p->fq[1] * yc + p->fq[2];
    }

    const int outside = p->layer;
    uint16_t *acc = outside ? rb->front : rb->back;
    int first = -1, last = -1;
    for (int x = x0; x <= x1; x++) {
        double xc = (double) x + 0.5;
        uint32_t mask = 0xffffffffu;
        if (x < xi0 || x > xi1) {
            for (int i = 0; i < p->ne; i++) {
                const edge_t *e = &p->e[i];
                double ev = e->a * xc + er[i];
                double d = e->sign > 0 ? ev : -ev;
                if (d >= EDGE_R)
                    continue;
                if (d <= -EDGE_R) {
                    mask = 0;
                    break;
                }
                mask &= edge_mask(e, ev);
                if (!mask)
                    break;
            }
            if (!mask)
                continue;
        }

        uint32_t wq; /* effective coverage in 1/8 samples, 0..256 */
        if (outside) {
            rb->fmask[x] |= mask;
            if (first < 0)
                first = x;
            last = x;
            wq = (uint32_t) __builtin_popcount(mask) * 8u;
        } else {
            uint32_t fm = rb->fmask[x];
            uint32_t cov = (uint32_t) __builtin_popcount(mask);
            if (!fm) {
                wq = cov * 8u;
            } else {
                uint32_t behind = (uint32_t) __builtin_popcount(mask & fm);
                uint32_t fa = rb->front[(size_t) x * 4 + 3];
                uint32_t full = 255u * (uint32_t) __builtin_popcount(fm);
                float w = (float) (cov - behind);
                if (fa < full)
                    w += (float) behind * (1.0f - (float) fa / (float) full);
                wq = (uint32_t) (w * 8.0f + 0.5f);
            }
            if (!wq)
                continue; /* hidden: do not even sample the picture */
        }

        uint32_t c = p->tex ? shade_pixel(p, xc, srow, trow, qrow, quality) : p->color;
        if (!(c >> 24))
            continue;
        uint32_t w_rgb = p->k_rgb * wq, w_a = p->k_a * wq;
        uint16_t *o = acc + (size_t) x * 4;
        o[0] = (uint16_t) (o[0] + (((c & 255) * w_rgb + 1024) >> 11));
        o[1] = (uint16_t) (o[1] + ((((c >> 8) & 255) * w_rgb + 1024) >> 11));
        o[2] = (uint16_t) (o[2] + ((((c >> 16) & 255) * w_rgb + 1024) >> 11));
        o[3] = (uint16_t) (o[3] + (((c >> 24) * w_a + 1024) >> 11));
        if (!outside) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    if (first >= 0) {
        int *mn = outside ? &rb->fmin : &rb->bmin, *mx = outside ? &rb->fmax : &rb->bmax;
        if (first < *mn)
            *mn = first;
        if (last > *mx)
            *mx = last;
    }
}

/* ------------------------------------------------------------------ */
/* compositing a finished row                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    const poly_t *polys;
    int npolys;
    uint8_t *out;
    int stride, W, H;
    int quality;
    int bg_mode;
    uint8_t bg_color[4];
    const uint8_t *bg_image;
    int bg_stride;
    int next_row; /* shared between workers, advanced atomically */
} job_t;

static inline uint8_t to_u8(float v)
{
    return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (uint8_t) (v + 0.5f));
}

static void bg_fill(const job_t *j, int y, int x0, int x1) /* [x0, x1) */
{
    if (x1 <= x0)
        return;
    uint8_t *o = j->out + (size_t) y * j->stride + (size_t) x0 * 4;
    size_t n = (size_t) (x1 - x0);
    switch (j->bg_mode) {
    case C3D_BG_IMAGE: {
        const uint8_t *s = j->bg_image + (size_t) y * j->bg_stride + (size_t) x0 * 4;
        if (s != o)
            memcpy(o, s, n * 4);
        break;
    }
    case C3D_BG_COLOR:
        if (j->bg_color[3] == 0) {
            memset(o, 0, n * 4);
        } else {
            for (size_t i = 0; i < n; i++)
                memcpy(o + i * 4, j->bg_color, 4);
        }
        break;
    default:
        memset(o, 0, n * 4);
    }
}

static void composite(const job_t *j, int y, int x0, int x1, const rowbuf_t *rb) /* [x0, x1] */
{
    uint8_t *o = j->out + (size_t) y * j->stride + (size_t) x0 * 4;
    const uint8_t *bgrow = j->bg_mode == C3D_BG_IMAGE
                               ? j->bg_image + (size_t) y * j->bg_stride + (size_t) x0 * 4
                               : NULL;
    const float k = 1.0f / (float) ACC_ONE;
    for (int x = x0; x <= x1; x++, o += 4) {
        const uint16_t *F = rb->front + (size_t) x * 4, *B = rb->back + (size_t) x * 4;
        const uint8_t *bg = bgrow ? bgrow + (size_t) (x - x0) * 4
                                  : (j->bg_mode == C3D_BG_COLOR ? j->bg_color : NULL);
        if (F[3] >= ACC_ONE) {
            unsigned r = (F[0] + NSAMP / 2u) / NSAMP, g = (F[1] + NSAMP / 2u) / NSAMP,
                     b = (F[2] + NSAMP / 2u) / NSAMP;
            o[0] = (uint8_t) (r > 255 ? 255 : r);
            o[1] = (uint8_t) (g > 255 ? 255 : g);
            o[2] = (uint8_t) (b > 255 ? 255 : b);
            o[3] = 255;
            continue;
        }
        /* the inside layer was already weighted by what the outside layer lets through */
        float r = (float) (F[0] + B[0]) * k, g = (float) (F[1] + B[1]) * k,
              b = (float) (F[2] + B[2]) * k, a = (float) (F[3] + B[3]) * k;
        if (a > 1.0f)
            a = 1.0f;
        if (bg && bg[3] && a < 1.0f) {
            float t = (1.0f - a) * (float) bg[3] * (1.0f / 255.0f);
            r += t * (float) bg[0] * (1.0f / 255.0f);
            g += t * (float) bg[1] * (1.0f / 255.0f);
            b += t * (float) bg[2] * (1.0f / 255.0f);
            a += t;
        }
        if (!(a > 0.5f / 255.0f)) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        float inv = 255.0f / a;
        o[0] = to_u8(r * inv);
        o[1] = to_u8(g * inv);
        o[2] = to_u8(b * inv);
        o[3] = to_u8(a * 255.0f);
    }
}

static void render_row(const job_t *j, int y, rowbuf_t *rb)
{
    int W = j->W;
    rb->fmin = rb->bmin = W;
    rb->fmax = rb->bmax = -1;
    for (int layer = 1; layer >= 0; layer--) /* outside-seen faces first */
        for (int i = 0; i < j->npolys; i++) {
            const poly_t *p = &j->polys[i];
            if (p->layer != layer || y < p->y0 || y > p->y1)
                continue;
            raster_row(p, y, W, rb, j->quality);
        }
    int lo = rb->fmin < rb->bmin ? rb->fmin : rb->bmin;
    int hi = rb->fmax > rb->bmax ? rb->fmax : rb->bmax;
    if (hi < lo) {
        bg_fill(j, y, 0, W);
        return;
    }
    bg_fill(j, y, 0, lo);
    composite(j, y, lo, hi, rb);
    bg_fill(j, y, hi + 1, W);
    if (rb->fmax >= rb->fmin) {
        size_t n = (size_t) (rb->fmax - rb->fmin + 1);
        memset(rb->front + (size_t) rb->fmin * 4, 0, n * 4 * sizeof(uint16_t));
        memset(rb->fmask + rb->fmin, 0, n * sizeof(uint32_t));
    }
    if (rb->bmax >= rb->bmin)
        memset(rb->back + (size_t) rb->bmin * 4, 0,
               (size_t) (rb->bmax - rb->bmin + 1) * 4 * sizeof(uint16_t));
}

typedef struct
{
    job_t *job;
    rowbuf_t rb;
} worker_t;

static int rowbuf_init(rowbuf_t *rb, int W)
{
    rb->front = calloc((size_t) W * 4, sizeof(uint16_t));
    rb->back = calloc((size_t) W * 4, sizeof(uint16_t));
    rb->fmask = calloc((size_t) W, sizeof(uint32_t));
    return rb->front && rb->back && rb->fmask;
}

static void rowbuf_free(rowbuf_t *rb)
{
    free(rb->front);
    free(rb->back);
    free(rb->fmask);
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    job_t *j = w->job;
    for (;;) {
        int y0 = __atomic_fetch_add(&j->next_row, ROW_CHUNK, __ATOMIC_RELAXED);
        if (y0 >= j->H)
            break;
        int y1 = y0 + ROW_CHUNK < j->H ? y0 + ROW_CHUNK : j->H;
        for (int y = y0; y < y1; y++)
            render_row(j, y, &w->rb);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* scene set-up                                                        */
/* ------------------------------------------------------------------ */

void c3d_scene_defaults(c3d_scene *s)
{
    memset(s, 0, sizeof(*s));
    s->size = 0.5;
    s->depth = 1.0;
    s->fov = 40.0;
    s->shade = 0.35;
    s->opacity = 1.0;
    s->shape = C3D_SHAPE_CUBE;
    s->fit = C3D_FIT_FILL;
    s->order = C3D_ORDER_YXZ;
    s->quality = C3D_QUALITY_GOOD;
    s->inside = 1;
    s->bg_mode = C3D_BG_TRANSPARENT;
    s->bg_color[3] = 255;
}

static double finite_or(double v, double fallback)
{
    return isfinite(v) ? v : fallback;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* corner index: bit0 = +x, bit1 = +y, bit2 = +z.
 * Each face lists its corners as seen from outside: top-left, top-right,
 * bottom-right, bottom-left, so pictures are upright when the face is turned
 * to the front by the shortest rotation about X or Y. */
static const int FACE_CORNERS[C3D_NUM_FACES][4] = {
    {6, 7, 5, 4}, /* front  (+z) */
    {7, 3, 1, 5}, /* right  (+x) */
    {3, 2, 0, 1}, /* back   (-z) */
    {2, 6, 4, 0}, /* left   (-x) */
    {2, 3, 7, 6}, /* top    (+y) */
    {4, 5, 1, 0}, /* bottom (-y) */
};

static void mat3_mul(double r[9], const double a[9], const double b[9])
{
    double t[9];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            t[i * 3 + k] = a[i * 3] * b[k] + a[i * 3 + 1] * b[3 + k] + a[i * 3 + 2] * b[6 + k];
    memcpy(r, t, sizeof(t));
}

static void rotation(double m[9], const c3d_scene *s)
{
    double ax = finite_or(s->rot_x, 0.0) * M_PI / 180.0;
    double ay = finite_or(s->rot_y, 0.0) * M_PI / 180.0;
    double az = -finite_or(s->rot_z, 0.0) * M_PI / 180.0; /* clockwise on screen */
    double cx = cos(ax), sx = sin(ax), cy = cos(ay), sy = sin(ay), cz = cos(az), sz = sin(az);
    const double rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    const double ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    const double rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    double t[9];
    if (s->order == C3D_ORDER_XYZ)
        mat3_mul(t, ry, rx);
    else
        mat3_mul(t, rx, ry);
    mat3_mul(m, rz, t);
}

/* coefficients of the affine function (x,y) -> K . ray(x,y), where ray is the
 * eye-space direction through pixel position (x,y) */
static void ray_form(double f[3], vec3 K, double g, double d, int W, int H)
{
    f[0] = K.x * g;
    f[1] = -K.y * g;
    f[2] = -K.x * g * (double) W * 0.5 + K.y * g * (double) H * 0.5 - K.z * d;
}

static inline double form_at(const double f[3], double x, double y)
{
    return f[0] * x + f[1] * y + f[2];
}

static int build_polys(const c3d_scene *s, int W, int H, poly_t *polys)
{
    double aspect = (double) W / (double) H;
    double size = finite_or(s->size, 0.0);
    double depth = finite_or(s->depth, 1.0);
    if (!(size > 1e-6))
        return 0;
    if (depth < 0.0)
        depth = 0.0;

    double bw = 1.0, bh = 1.0, bd = 1.0;
    if (s->shape == C3D_SHAPE_FRAME_H) {
        bw = aspect;
        bd = aspect;
    } else if (s->shape == C3D_SHAPE_FRAME_V) {
        bw = aspect;
    }
    double hx = 0.5 * bw * size, hy = 0.5 * bh * size, hz = 0.5 * bd * size * depth;

    double fov = clampd(finite_or(s->fov, 40.0), 1.0, 160.0);
    double d = 0.5 / tan(fov * M_PI / 360.0); /* eye distance from the screen plane */
    double wn = d * 0.02;                     /* near plane, measured from the eye   */

    double rot[9];
    rotation(rot, s);

    /* The unrotated front face lies in the screen plane; the box turns about
     * its own centre. World: x right, y up, z towards the viewer, unit = frame
     * height. Eye space is the same with the eye at the origin. */
    double cx = finite_or(s->pos_x, 0.0) * aspect;
    double cy = -finite_or(s->pos_y, 0.0);
    double cz = -hz - finite_or(s->pos_z, 0.0);

    vec3 eye[8];
    for (int i = 0; i < 8; i++) {
        double lx = i & 1 ? hx : -hx, ly = i & 2 ? hy : -hy, lz = i & 4 ? hz : -hz;
        eye[i] = v3(cx + rot[0] * lx + rot[1] * ly + rot[2] * lz,
                    cy + rot[3] * lx + rot[4] * ly + rot[5] * lz,
                    cz + rot[6] * lx + rot[7] * ly + rot[8] * lz - d);
    }

    /* optional shrink so that the whole box stays inside the frame */
    double zf = 1.0;
    double keep = clampd(finite_or(s->keep_in_frame, 0.0), 0.0, 1.0);
    if (keep > 0.0) {
        double m = 0.0;
        int ok = 1;
        for (int i = 0; i < 8 && ok; i++) {
            double w = -eye[i].z;
            if (w < wn) {
                ok = 0;
                break;
            }
            double ex = fabs(eye[i].x * d / w) / (0.5 * aspect);
            double ey = fabs(eye[i].y * d / w) / 0.5;
            if (ex > m)
                m = ex;
            if (ey > m)
                m = ey;
        }
        if (ok && m > 1.0)
            zf = 1.0 - keep * (1.0 - 1.0 / m);
    }
    double proj = zf * (double) H * d;
    double g = 1.0 / (zf * (double) H);

    double shade = clampd(finite_or(s->shade, 0.0), 0.0, 1.0);
    double opacity = clampd(finite_or(s->opacity, 1.0), 0.0, 1.0);
    if (opacity <= 0.0)
        return 0;

    double ox[NSAMP], oy[NSAMP];
    sample_pattern(ox, oy);

    int np = 0;
    for (int f = 0; f < C3D_NUM_FACES; f++) {
        const c3d_face *face = &s->face[f];
        if (!face->tex && !(face->solid && face->color[3]))
            continue;
        const int *ci = FACE_CORNERS[f];
        vec3 O = eye[ci[0]];
        vec3 U = v3_sub(eye[ci[1]], O), V = v3_sub(eye[ci[3]], O);
        double fw = f == C3D_FACE_RIGHT || f == C3D_FACE_LEFT ? 2.0 * hz : 2.0 * hx;
        double fh = f == C3D_FACE_TOP || f == C3D_FACE_BOTTOM ? 2.0 * hz : 2.0 * hy;
        if (!(fw > 1e-9) || !(fh > 1e-9))
            continue;

        vec3 N = v3_cross(U, V); /* points into the box */
        vec3 ctr = v3(O.x + 0.5 * (U.x + V.x), O.y + 0.5 * (U.y + V.y), O.z + 0.5 * (U.z + V.z));
        double facing = v3_dot(N, ctr); /* > 0: we look at the outside */
        double nl = sqrt(v3_dot(N, N)), cl = sqrt(v3_dot(ctr, ctr));
        if (!(nl > 0.0) || !(cl > 0.0))
            continue;
        double cosv = fabs(facing) / (nl * cl);
        if (cosv < 1e-7)
            continue; /* exactly edge-on */
        int outside = facing > 0.0;
        if (!outside && !s->inside)
            continue;

        /* clip against the near plane */
        vec3 cp[MAX_VERTS];
        int n = 0;
        for (int i = 0; i < 4; i++) {
            int k = (i + 1) & 3;
            vec3 pi = eye[ci[i]], pk = eye[ci[k]];
            int in_i = -pi.z >= wn, in_k = -pk.z >= wn;
            if (in_i)
                cp[n++] = pi;
            if (in_i != in_k) {
                /* always interpolate from the lower corner index so that both
                 * faces sharing this box edge get the same point */
                vec3 a = pi, b = pk;
                if (ci[i] > ci[k]) {
                    a = pk;
                    b = pi;
                }
                double t = (wn - (-a.z)) / ((-b.z) - (-a.z));
                cp[n++] = v3(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), -wn);
            }
        }
        if (n < 3)
            continue;

        poly_t *p = &polys[np];
        memset(p, 0, offsetof(poly_t, e));
        p->n = n;
        double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300, area = 0.0;
        for (int i = 0; i < n; i++) {
            double w = -cp[i].z;
            p->vx[i] = (double) W * 0.5 + proj * cp[i].x / w;
            p->vy[i] = (double) H * 0.5 - proj * cp[i].y / w;
            if (p->vx[i] < minx)
                minx = p->vx[i];
            if (p->vx[i] > maxx)
                maxx = p->vx[i];
            if (p->vy[i] < miny)
                miny = p->vy[i];
            if (p->vy[i] > maxy)
                maxy = p->vy[i];
        }
        for (int i = 0; i < n; i++) {
            int k = i + 1 == n ? 0 : i + 1;
            area += p->vx[i] * p->vy[k] - p->vx[k] * p->vy[i];
        }
        if (!(fabs(area) > 1e-6) || !isfinite(area))
            continue;
        if (maxx < 0.0 || minx > (double) W || maxy < 0.0 || miny > (double) H)
            continue;
        p->y0 = miny <= 0.0 ? 0 : (int) miny;
        p->y1 = maxy >= (double) (H - 1) ? H - 1 : (int) maxy;
        if (p->y0 > p->y1)
            continue;

        int orient = area > 0.0 ? 1 : -1;
        p->ne = 0;
        for (int i = 0; i < n; i++) {
            int k = i + 1 == n ? 0 : i + 1;
            if (edge_setup(&p->e[p->ne], p->vx[i], p->vy[i], p->vx[k], p->vy[k], orient, ox, oy))
                p->ne++;
        }
        if (p->ne < 3)
            continue;

        p->layer = outside;
        double k_shade = 1.0 - shade * (1.0 - cosv);
        p->k_rgb = (uint32_t) (256.0 * k_shade * opacity + 0.5);
        p->k_a = (uint32_t) (256.0 * opacity + 0.5);

        if (face->tex) {
            c3d_texture *tex = face->tex;
            double tw = (double) tex->w[0], th = (double) tex->h[0];
            /* u,v in 0..1 across the face: u = (VxO).r / (N.r), v = (OxU).r / (N.r) */
            double fu[3], fv[3], fq[3];
            ray_form(fu, v3_cross(V, O), g, d, W, H);
            ray_form(fv, v3_cross(O, U), g, d, W, H);
            ray_form(fq, N, g, d, W, H);

            /* face (u,v) -> picture (u',v') = offset + scale * (u,v) */
            double su = 1.0, sv = 1.0, ou = 0.0, ov = 0.0;
            double af = fw / fh, as = tex->aspect;
            p->border = 0;
            if (s->fit == C3D_FIT_FILL) {
                if (af > as)
                    sv = as / af;
                else
                    su = af / as;
            } else if (s->fit == C3D_FIT_FIT) {
                if (af > as)
                    su = af / as;
                else
                    sv = as / af;
                p->border = 1;
            }
            ou = 0.5 * (1.0 - su);
            ov = 0.5 * (1.0 - sv);
            for (int i = 0; i < 3; i++) {
                p->fs[i] = tw * (su * fu[i] + ou * fq[i]);
                p->ft[i] = th * (sv * fv[i] + ov * fq[i]);
                p->fq[i] = fq[i];
            }
            /* make the denominator positive over the polygon */
            double qc = 0.0, qv_min = 1e300;
            for (int i = 0; i < n; i++)
                qc += form_at(p->fq, p->vx[i], p->vy[i]);
            if (qc < 0.0)
                for (int i = 0; i < 3; i++) {
                    p->fs[i] = -p->fs[i];
                    p->ft[i] = -p->ft[i];
                    p->fq[i] = -p->fq[i];
                }
            for (int i = 0; i < n; i++) {
                double q = form_at(p->fq, p->vx[i], p->vy[i]);
                if (q < qv_min)
                    qv_min = q;
            }
            if (!(qv_min > 0.0) || !isfinite(qv_min))
                continue;
            p->qmin = 0.25 * qv_min;
            p->smax = tw;
            p->tmax = th;
            p->tex = tex;
        } else {
            uint32_t a = face->color[3];
            p->color = div255(face->color[0] * a) | div255(face->color[1] * a) << 8
                       | div255(face->color[2] * a) << 16 | a << 24;
        }
        np++;
    }
    return np;
}

/* How many mip levels the textures need for this frame: the strongest
 * minification of a planar face is reached at one of its corners. */
static void prepare_textures(poly_t *polys, int np, int quality)
{
    for (int i = 0; i < np; i++) {
        poly_t *p = &polys[i];
        if (!p->tex)
            continue;
        float worst = 0.0f;
        for (int k = 0; k < p->n; k++) {
            double q = form_at(p->fq, p->vx[k], p->vy[k]);
            if (!(q > p->qmin))
                q = p->qmin;
            double inv = 1.0 / q;
            double sd = form_at(p->fs, p->vx[k], p->vy[k]) * inv;
            double td = form_at(p->ft, p->vx[k], p->vy[k]) * inv;
            float l1, l2, A, B, C;
            footprint((float) ((p->fs[0] - sd * p->fq[0]) * inv),
                      (float) ((p->ft[0] - td * p->fq[0]) * inv),
                      (float) ((p->fs[1] - sd * p->fq[1]) * inv),
                      (float) ((p->ft[1] - td * p->fq[1]) * inv), &l1, &l2, &A, &B, &C);
            if (l1 > worst)
                worst = l1;
        }
        int need = 1;
        if (worst > 1.0f && isfinite(worst))
            need = (int) (0.5f * fast_log2(worst)) + 3;
        else if (!isfinite(worst))
            need = MAX_LEVELS;
        (void) quality;
        while (p->tex->nlevels < need && tex_add_level(p->tex)) {
        }
    }
}

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (n > 64 ? 64 : (int) n);
}

void c3d_render(const c3d_scene *s, uint8_t *out, int stride, int width, int height,
                int max_threads)
{
    if (!s || !out || width <= 0 || height <= 0)
        return;

    job_t job;
    memset(&job, 0, sizeof(job));
    job.out = out;
    job.stride = stride;
    job.W = width;
    job.H = height;
    job.quality = s->quality < C3D_QUALITY_DRAFT
                      ? C3D_QUALITY_DRAFT
                      : (s->quality > C3D_QUALITY_BEST ? C3D_QUALITY_BEST : s->quality);
    job.bg_mode = s->bg_mode;
    memcpy(job.bg_color, s->bg_color, 4);
    job.bg_image = s->bg_image;
    job.bg_stride = s->bg_stride;
    if (job.bg_mode == C3D_BG_IMAGE && !job.bg_image)
        job.bg_mode = C3D_BG_TRANSPARENT;
    if (job.bg_mode != C3D_BG_IMAGE && job.bg_mode != C3D_BG_COLOR)
        job.bg_mode = C3D_BG_TRANSPARENT;

    poly_t *polys = malloc(sizeof(poly_t) * C3D_NUM_FACES);
    int np = polys ? build_polys(s, width, height, polys) : 0;
    if (np > 0)
        prepare_textures(polys, np, job.quality);
    job.polys = polys;
    job.npolys = np;

    /* rows that contain any geometry, to decide how much help is worth it */
    long work = 0;
    for (int i = 0; i < np; i++)
        work += (long) (polys[i].y1 - polys[i].y0 + 1) * width;

    int nthreads = max_threads > 0 ? max_threads : cpu_count();
    if (nthreads > 16)
        nthreads = 16;
    if (nthreads > height / ROW_CHUNK)
        nthreads = height / ROW_CHUNK;
    if (work < 200000)
        nthreads = 1;
    if (nthreads < 1)
        nthreads = 1;

    worker_t workers[16];
    pthread_t tid[16];
    int started[16] = {0};
    int nw = 0;
    for (int i = 0; i < nthreads; i++) {
        workers[nw].job = &job;
        if (!rowbuf_init(&workers[nw].rb, width)) {
            rowbuf_free(&workers[nw].rb);
            break;
        }
        nw++;
    }
    if (nw == 0) {
        /* out of memory: at least leave a defined picture behind */
        job.npolys = 0;
        for (int y = 0; y < height; y++)
            bg_fill(&job, y, 0, width);
        free(polys);
        return;
    }
    for (int i = 1; i < nw; i++)
        started[i] = pthread_create(&tid[i], NULL, worker_main, &workers[i]) == 0;
    worker_main(&workers[0]);
    for (int i = 1; i < nw; i++)
        if (started[i])
            pthread_join(tid[i], NULL);
    for (int i = 0; i < nw; i++)
        rowbuf_free(&workers[i].rb);
    free(polys);
}
