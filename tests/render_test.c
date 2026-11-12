/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-alone checks for the cube renderer (no frei0r, no MLT).
 *
 *   render_test <outdir>
 *
 * Writes raw RGBA pictures (<name>_<w>x<h>.rgba) into outdir for visual
 * inspection (tests/topng.py converts them) and runs numeric checks.
 * Exit status is the number of failed checks.
 */
#include "../src/c3d_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static const char *outdir = ".";

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            failures++; \
            printf("FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

/* 5x7 glyphs for the letters used to label faces */
static const char *glyph(char c)
{
    switch (c) {
    case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'D': return "11100" "10010" "10001" "10001" "10001" "10010" "11100";
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    default: return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

/* A picture that makes orientation obvious: coloured gradient, a letter that
 * is not mirror-symmetric, a white frame, and a red dot in the top-left. */
static uint8_t *make_label(int w, int h, char letter, const uint8_t base[3])
{
    uint8_t *img = malloc((size_t) w * h * 4);
    int cell = (h < w ? h : w) / 10;
    int gx = (w - 5 * cell) / 2, gy = (h - 7 * cell) / 2;
    const char *g = glyph(letter);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + ((size_t) y * w + x) * 4;
            double fx = (double) x / (w - 1), fy = (double) y / (h - 1);
            p[0] = (uint8_t) (base[0] * (0.55 + 0.45 * fx));
            p[1] = (uint8_t) (base[1] * (0.55 + 0.45 * fy));
            p[2] = (uint8_t) (base[2] * (0.55 + 0.45 * (1.0 - fx)));
            p[3] = 255;
            int border = cell / 4 > 0 ? cell / 4 : 1;
            if (x < border || y < border || x >= w - border || y >= h - border)
                p[0] = p[1] = p[2] = 255;
            int cx = x - gx, cy = y - gy;
            if (cx >= 0 && cy >= 0 && cx < 5 * cell && cy < 7 * cell
                && g[(cy / cell) * 5 + cx / cell] == '1')
                p[0] = p[1] = p[2] = 20;
            int dx = x - cell, dy = y - cell;
            if (dx * dx + dy * dy < cell * cell / 4) {
                p[0] = 255;
                p[1] = 0;
                p[2] = 0;
            }
        }
    return img;
}

/* high-frequency picture for the filtering / aliasing checks */
static uint8_t *make_checker(int w, int h, int period)
{
    uint8_t *img = malloc((size_t) w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + ((size_t) y * w + x) * 4;
            int on = ((x / period) + (y / period)) & 1;
            p[0] = p[1] = p[2] = on ? 235 : 20;
            p[3] = 255;
        }
    return img;
}

static void save(const char *name, const uint8_t *img, int w, int h)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_%dx%d.rgba", outdir, name, w, h);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite(img, 4, (size_t) w * h, f);
    fclose(f);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char LETTERS[6] = {'F', 'R', 'B', 'L', 'T', 'D'};
static const uint8_t COLORS[6][3] = {
    {230, 70, 70}, {70, 200, 90}, {80, 110, 240}, {230, 200, 60}, {200, 90, 220}, {70, 210, 220},
};

typedef struct
{
    uint8_t *img[6];
    c3d_texture *tex[6];
    int w, h;
} faces_t;

static void faces_make(faces_t *f, int w, int h)
{
    f->w = w;
    f->h = h;
    for (int i = 0; i < 6; i++) {
        f->img[i] = make_label(w, h, LETTERS[i], COLORS[i]);
        f->tex[i] = NULL;
    }
}

/* textures are rebuilt for every render, as the plugin does */
static void faces_bind(faces_t *f, c3d_scene *s)
{
    for (int i = 0; i < 6; i++) {
        c3d_texture_destroy(f->tex[i]);
        f->tex[i] = c3d_texture_create(f->img[i], f->w * 4, f->w, f->h, 0, 0, f->w, f->h, 0);
        s->face[i].tex = f->tex[i];
    }
}

static void faces_free(faces_t *f)
{
    for (int i = 0; i < 6; i++) {
        c3d_texture_destroy(f->tex[i]);
        free(f->img[i]);
    }
}

static long max_diff(const uint8_t *a, const uint8_t *b, size_t n, double *mean)
{
    long worst = 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        long d = labs((long) a[i] - (long) b[i]);
        if (d > worst)
            worst = d;
        sum += d;
    }
    if (mean)
        *mean = sum / (double) n;
    return worst;
}

/* 1. A face turned exactly to the front must reproduce its picture exactly. */
static void test_identity(void)
{
    const int W = 640, H = 360;
    faces_t f;
    faces_make(&f, W, H);
    uint8_t *out = malloc((size_t) W * H * 4);
    struct
    {
        const char *name;
        int shape;
        double rx, ry;
        int face;
    } cases[] = {
        {"front", C3D_SHAPE_FRAME_H, 0, 0, C3D_FACE_FRONT},
        {"right", C3D_SHAPE_FRAME_H, 0, -90, C3D_FACE_RIGHT},
        {"back", C3D_SHAPE_FRAME_H, 0, 180, C3D_FACE_BACK},
        {"back_neg", C3D_SHAPE_FRAME_H, 0, -180, C3D_FACE_BACK},
        {"left", C3D_SHAPE_FRAME_H, 0, 90, C3D_FACE_LEFT},
        {"top", C3D_SHAPE_FRAME_V, 90, 0, C3D_FACE_TOP},
        {"bottom", C3D_SHAPE_FRAME_V, -90, 0, C3D_FACE_BOTTOM},
        {"turn360", C3D_SHAPE_FRAME_H, 0, 360, C3D_FACE_FRONT},
        {"turn_m720", C3D_SHAPE_FRAME_H, 0, -720, C3D_FACE_FRONT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int fov = 20; fov <= 90; fov += 35) {
            c3d_scene s;
            c3d_scene_defaults(&s);
            s.shape = cases[i].shape;
            s.size = 1.0;
            s.rot_x = cases[i].rx;
            s.rot_y = cases[i].ry;
            s.fov = fov;
            s.shade = 0.5; /* a face seen head-on must not be darkened */
            s.bg_mode = C3D_BG_COLOR;
            s.bg_color[0] = 255;
            s.bg_color[1] = 0;
            s.bg_color[2] = 255;
            s.bg_color[3] = 255;
            faces_bind(&f, &s);
            memset(out, 0x55, (size_t) W * H * 4);
            c3d_render(&s, out, W * 4, W, H, 0);
            double mean;
            long d = max_diff(out, f.img[cases[i].face], (size_t) W * H * 4, &mean);
            CHECK(d <= 1, "identity %s fov %d: max diff %ld, mean %.4f", cases[i].name, fov, d,
                  mean);
            if (d > 1) {
                char nm[64];
                snprintf(nm, sizeof(nm), "fail_identity_%s_fov%d", cases[i].name, fov);
                save(nm, out, W, H);
            }
        }
    }
    free(out);
    faces_free(&f);
}

/* 2. Pictures for looking at. */
static void test_gallery(void)
{
    const int W = 960, H = 540;
    faces_t f;
    faces_make(&f, 640, 360);
    uint8_t *out = malloc((size_t) W * H * 4);
    c3d_scene s;

    /* cube, three faces visible */
    c3d_scene_defaults(&s);
    s.size = 0.55;
    s.rot_y = -35;
    s.rot_x = 25;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_top_right", out, W, H);

    s.rot_y = 40;
    s.rot_x = -25;
    s.rot_z = 10;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_bottom_left_roll", out, W, H);

    /* fit modes on a cube */
    c3d_scene_defaults(&s);
    s.size = 0.6;
    s.rot_y = -30;
    s.rot_x = 20;
    s.fit = C3D_FIT_FIT;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_fit", out, W, H);
    s.fit = C3D_FIT_STRETCH;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_stretch", out, W, H);

    /* open box: front and top removed, inside faces visible */
    c3d_scene_defaults(&s);
    s.size = 0.6;
    s.rot_y = -25;
    s.rot_x = 30;
    faces_bind(&f, &s);
    s.face[C3D_FACE_FRONT].tex = NULL;
    s.face[C3D_FACE_TOP].tex = NULL;
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_open", out, W, H);

    /* translucent cube */
    c3d_scene_defaults(&s);
    s.size = 0.6;
    s.rot_y = -25;
    s.rot_x = 30;
    s.opacity = 0.55;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_translucent", out, W, H);

    /* transition pose: frame-shaped box half way through a left spin */
    c3d_scene_defaults(&s);
    s.shape = C3D_SHAPE_FRAME_H;
    s.size = 1.0;
    s.rot_y = -45;
    s.bg_mode = C3D_BG_COLOR;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("spin_half", out, W, H);
    s.keep_in_frame = 1.0;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("spin_half_keep", out, W, H);
    s.rot_y = -20;
    s.keep_in_frame = 0.0;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("spin_20", out, W, H);

    /* the default transition move at several fields of view */
    for (int fi = 0; fi < 3; fi++) {
        static const double fovs[3] = {12, 25, 40};
        for (int k = 0; k <= 8; k++) {
            c3d_scene_defaults(&s);
            s.shape = C3D_SHAPE_FRAME_H;
            s.size = 1.0;
            s.fov = fovs[fi];
            s.rot_y = -90.0 * k / 8.0;
            s.keep_in_frame = 1.0;
            s.bg_mode = C3D_BG_COLOR;
            faces_bind(&f, &s);
            c3d_render(&s, out, W * 4, W, H, 0);
            char nm[64];
            snprintf(nm, sizeof(nm), "strip_fov%02d_step%d", (int) fovs[fi], k);
            save(nm, out, W, H);
        }
    }

    /* moved in space, strong perspective */
    c3d_scene_defaults(&s);
    s.size = 0.5;
    s.pos_x = 0.25;
    s.pos_y = -0.15;
    s.pos_z = 0.6;
    s.rot_y = 30;
    s.rot_x = 15;
    s.fov = 75;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("cube_moved", out, W, H);

    /* very close: a corner pokes through the near plane */
    c3d_scene_defaults(&s);
    s.size = 0.8;
    s.pos_z = -0.35;
    s.rot_y = 40;
    s.rot_x = 30;
    s.fov = 90;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("near_clip_corner", out, W, H);

    /* the eye is inside the box: only inside faces are visible */
    c3d_scene_defaults(&s);
    s.size = 1.5;
    s.pos_z = -1.0;
    s.rot_y = 25;
    s.rot_x = 10;
    s.fov = 70;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("eye_inside_box", out, W, H);

    /* flat card half way through a flip */
    c3d_scene_defaults(&s);
    s.shape = C3D_SHAPE_FRAME_H;
    s.size = 0.8;
    s.depth = 0.0;
    s.rot_y = -60;
    faces_bind(&f, &s);
    c3d_render(&s, out, W * 4, W, H, 0);
    save("card_flip", out, W, H);

    free(out);
    faces_free(&f);
}

/* 3. No gaps or double coverage along shared edges: an opaque cube rendered
 *    over a contrasting background must not let the background bleed through
 *    anywhere inside its silhouette. Compared against a 6x supersampled
 *    silhouette rendered by this same code (solid faces), which also checks
 *    that coverage is close to the true area. */
static void test_seams(void)
{
    const int W = 400, H = 300, SS = 6;
    uint8_t *out = malloc((size_t) W * H * 4);
    uint8_t *big = malloc((size_t) W * SS * H * SS * 4);
    double worst_bleed = 0, worst_cov = 0;
    for (int k = 0; k < 40; k++) {
        c3d_scene s;
        c3d_scene_defaults(&s);
        s.size = 0.5;
        s.rot_y = k * 37.3 + 3.1;
        s.rot_x = k * 21.7 - 11.0;
        s.rot_z = k * 9.1;
        s.pos_x = 0.013 * (k % 7) - 0.04;
        s.pos_y = 0.011 * (k % 5) - 0.02;
        s.shade = 0.0;
        s.inside = k & 1;
        for (int i = 0; i < 6; i++) {
            s.face[i].solid = 1;
            s.face[i].color[0] = 255;
            s.face[i].color[1] = 255;
            s.face[i].color[2] = 255;
            s.face[i].color[3] = 255;
        }
        c3d_render(&s, out, W * 4, W, H, 0);
        c3d_render(&s, big, W * SS * 4, W * SS, H * SS, 0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *p = out + ((size_t) y * W + x) * 4;
                /* reference coverage from the supersampled alpha */
                double ref = 0;
                int solid = 1;
                for (int yy = 0; yy < SS; yy++)
                    for (int xx = 0; xx < SS; xx++) {
                        int a = big[(((size_t) y * SS + yy) * W * SS + x * SS + xx) * 4 + 3];
                        ref += a;
                        if (a != 255)
                            solid = 0;
                    }
                ref /= SS * SS * 255.0;
                double cov = p[3] / 255.0;
                if (fabs(cov - ref) > worst_cov)
                    worst_cov = fabs(cov - ref);
                /* deep inside the silhouette the result must be solid white */
                if (solid && ref == 1.0) {
                    double bleed = 255 - p[3];
                    if (255 - p[0] > bleed)
                        bleed = 255 - p[0];
                    if (bleed > worst_bleed)
                        worst_bleed = bleed;
                }
            }
    }
    printf("seams: worst bleed inside silhouette %.0f/255, worst coverage error %.3f\n",
           worst_bleed, worst_cov);
    CHECK(worst_bleed <= 1.0, "background shows through inside the cube (%.0f/255)", worst_bleed);
    CHECK(worst_cov <= 0.12, "edge coverage differs from supersampled reference by %.3f", worst_cov);
    free(out);
    free(big);
}

/* 4. Minified high-frequency texture: the average brightness must stay close
 *    to the texture's average (aliasing would make it swing). */
static void test_filtering(void)
{
    const int TW = 1024, TH = 1024, W = 640, H = 360;
    uint8_t *tex_img = make_checker(TW, TH, 2);
    uint8_t *out = malloc((size_t) W * H * 4);
    for (int q = C3D_QUALITY_DRAFT; q <= C3D_QUALITY_BEST; q++) {
        double lo = 1e9, hi = -1e9;
        int worst_dev = 0;
        for (int k = 0; k < 24; k++) {
            c3d_scene s;
            c3d_scene_defaults(&s);
            s.size = 0.18 + 0.01 * k;
            s.rot_y = -20 - 2.5 * k;
            s.rot_x = 10 + k;
            s.shade = 0;
            s.quality = q;
            c3d_texture *t = c3d_texture_create(tex_img, TW * 4, TW, TH, 0, 0, TW, TH, 0);
            for (int i = 0; i < 6; i++)
                s.face[i].tex = t;
            c3d_render(&s, out, W * 4, W, H, 0);
            c3d_texture_destroy(t);
            double sum = 0, wsum = 0;
            for (size_t i = 0; i < (size_t) W * H; i++)
                if (out[i * 4 + 3] == 255) {
                    sum += out[i * 4];
                    wsum += 1;
                    /* a 2-texel checker shrunk this far is plain grey; single
                     * pixels far from grey mean the texture was point sampled */
                    int dev = abs((int) out[i * 4] - 128);
                    if (dev > worst_dev)
                        worst_dev = dev;
                }
            double avg = wsum > 0 ? sum / wsum : 0;
            if (avg < lo)
                lo = avg;
            if (avg > hi)
                hi = avg;
            if (k == 12) {
                char nm[64];
                snprintf(nm, sizeof(nm), "checker_q%d", q);
                save(nm, out, W, H);
            }
        }
        printf("filtering quality %d: mean face brightness between %.1f and %.1f (ideal 127.5)\n",
               q, lo, hi);
        CHECK(lo > 110 && hi < 145, "quality %d: minified checker average drifts (%.1f..%.1f)", q,
              lo, hi);
        CHECK(worst_dev <= 24, "quality %d: minified checker is not smooth (a pixel is %d away from grey)",
              q, worst_dev);
    }
    free(out);
    free(tex_img);
}

/* 5. Alpha: transparent background stays transparent, translucent picture
 *    pixels do not get dark fringes. */
static void test_alpha(void)
{
    const int W = 320, H = 240, TW = 64, TH = 64;
    uint8_t *img = malloc((size_t) TW * TH * 4);
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            uint8_t *p = img + ((size_t) y * TW + x) * 4;
            int in = x >= 16 && x < 48 && y >= 16 && y < 48;
            /* white square on transparent GREEN: straight-alpha filtering
             * would pull green into the square's edge */
            p[0] = in ? 255 : 0;
            p[1] = 255;
            p[2] = in ? 255 : 0;
            p[3] = in ? 255 : 0;
        }
    uint8_t *out = malloc((size_t) W * H * 4);
    c3d_scene s;
    c3d_scene_defaults(&s);
    s.size = 0.7;
    s.rot_y = -20;
    s.rot_x = 15;
    s.shade = 0;
    s.inside = 0;
    c3d_texture *t = c3d_texture_create(img, TW * 4, TW, TH, 0, 0, TW, TH, 0);
    for (int i = 0; i < 6; i++)
        s.face[i].tex = t;
    c3d_render(&s, out, W * 4, W, H, 0);
    c3d_texture_destroy(t);
    save("alpha_square", out, W, H);
    int fringe = 0, corner_a = out[3];
    for (size_t i = 0; i < (size_t) W * H; i++) {
        const uint8_t *p = out + i * 4;
        if (p[3] > 8 && (p[0] < 250 || p[2] < 250))
            fringe++;
    }
    CHECK(corner_a == 0, "transparent background has alpha %d", corner_a);
    CHECK(fringe == 0, "%d pixels have a coloured fringe at alpha edges", fringe);
    free(out);
    free(img);
}

/* 6. Robustness: silly parameters must not crash or hang. */
static void test_robustness(void)
{
    const int W = 160, H = 90;
    faces_t f;
    faces_make(&f, 64, 36);
    uint8_t *out = malloc((size_t) W * H * 4);
    double vals[] = {0, -1, 1, 1e-9, 1e9, -1e9, NAN, INFINITY, 0.5, 179.9, 3600.123};
    int n = sizeof(vals) / sizeof(vals[0]);
    unsigned seed = 12345;
    for (int k = 0; k < 4000; k++) {
        c3d_scene s;
        c3d_scene_defaults(&s);
        double *fields[] = {&s.rot_x, &s.rot_y, &s.rot_z, &s.pos_x, &s.pos_y, &s.pos_z, &s.size,
                            &s.depth, &s.fov,   &s.keep_in_frame, &s.shade, &s.opacity};
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 3 == 0)
                *fields[i] = vals[(seed >> 8) % n];
            else
                *fields[i] = ((double) ((seed >> 4) % 20001) - 10000.0) / 2500.0;
        }
        seed = seed * 1103515245u + 12345u;
        s.shape = (int) ((seed >> 16) % 5) - 1;
        s.fit = (int) ((seed >> 19) % 5) - 1;
        s.order = (int) ((seed >> 22) % 4) - 1;
        s.quality = (int) ((seed >> 25) % 5) - 1;
        s.inside = (seed >> 28) & 1;
        faces_bind(&f, &s);
        if (k % 3 == 0)
            s.face[k % 6].tex = NULL;
        c3d_render(&s, out, W * 4, W, H, 1 + k % 3);
    }
    /* odd sizes */
    for (int w = 1; w <= 9; w++)
        for (int h = 1; h <= 9; h++) {
            c3d_scene s;
            c3d_scene_defaults(&s);
            s.rot_y = 33;
            s.rot_x = 21;
            faces_bind(&f, &s);
            uint8_t small[9 * 9 * 4];
            c3d_render(&s, small, w * 4, w, h, 0);
        }
    printf("robustness: 4000 random scenes and 81 tiny frames rendered\n");
    free(out);
    faces_free(&f);
}

/* 7. Speed. */
static void test_speed(void)
{
    static const int sizes[][2] = {{1920, 1080}, {3840, 2160}};
    for (int si = 0; si < 2; si++) {
        int W = sizes[si][0], H = sizes[si][1];
        faces_t f;
        faces_make(&f, W, H);
        uint8_t *out = malloc((size_t) W * H * 4);
        for (int q = C3D_QUALITY_DRAFT; q <= C3D_QUALITY_BEST; q++) {
            for (int threads = 1; threads <= 16; threads *= 16) {
                c3d_scene s;
                c3d_scene_defaults(&s);
                s.shape = C3D_SHAPE_FRAME_H;
                s.size = 1.0;
                s.quality = q;
                s.bg_mode = C3D_BG_COLOR;
                double t0 = now(), ttex = 0;
                int frames = 12;
                for (int k = 0; k < frames; k++) {
                    s.rot_y = -90.0 * (k + 0.5) / frames;
                    double a = now();
                    faces_bind(&f, &s);
                    ttex += now() - a;
                    c3d_render(&s, out, W * 4, W, H, threads);
                }
                double dt = (now() - t0) / frames;
                printf("speed %dx%d quality %d threads %2d: %6.1f ms/frame (%.1f ms of it building 6 textures)\n",
                       W, H, q, threads, dt * 1e3, ttex / frames * 1e3);
            }
        }
        free(out);
        faces_free(&f);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        outdir = argv[1];
    test_identity();
    test_gallery();
    test_seams();
    test_filtering();
    test_alpha();
    test_robustness();
    if (argc > 2 && !strcmp(argv[2], "speed"))
        test_speed();
    printf("%s (%d failed checks)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures;
}
