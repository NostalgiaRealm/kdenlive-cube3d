/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube3d - a 3D box with a clip on every face, as frei0r plugins.
 *
 * This file is compiled twice:
 *   cube3d.so             frei0r filter  (one input)  -> MLT filter     frei0r.cube3d
 *   cube3d_transition.so  frei0r mixer2  (two inputs) -> MLT transition frei0r.cube3d_transition
 * (build the second one with -DCUBE3D_MIXER).
 *
 * Parameter values are NOT squeezed into frei0r's customary 0..1 range. They
 * are plain degrees, percent and list indices: Kdenlive's effect XML has no
 * way to shift a range (only to scale it), and MLT - the only host this is
 * written for - passes numbers through untouched and can animate them with
 * keyframes. Every number parameter may therefore be keyframed.
 *
 * The parameters that hold a file name have odd names (resource,
 * producer.resource, ...). That is on purpose: when MLT writes a project it
 * turns every value that starts with the project's folder into a relative
 * path, and when it reads the project back it makes only a fixed list of
 * property names absolute again. A file parameter called anything else would
 * lose its folder the first time a project is saved, reopened or rendered.
 * The transition has five file parameters instead of six because MLT's list
 * for transitions has only five names.
 */
#include <frei0r.h>

#include "c3d_media.h"
#include "c3d_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CUBE3D_MIXER
#define NUM_FILES 5
#else
#define NUM_FILES 6
#endif
#define MAX_FILES 6 /* the face codes always leave room for six */
#define NUM_CELLS 6 /* clip A read as a 3 x 2 grid of pictures */

/* what a face (or the background) shows */
enum {
    SRC_NONE = 0,
    SRC_A = 1,     /* filter: the clip; transition: lower track / outgoing clip */
    SRC_B = 2,     /* transition: upper track / incoming clip (filter: same as A) */
    SRC_FILE = 3,  /* 3..8: file 1..6 (the transition has no file 6) */
    SRC_CELL = 9,  /* 9..14: cell 1..6 of clip A, counted left to right, top row first */
    SRC_COLOR = 15,
    SRC_LAST = SRC_COLOR
};

enum { BG_TRANSPARENT = 0, BG_COLOR, BG_A, BG_B, BG_LAST = BG_B };

/* softens keyframed turns without help from the host, see ease_angle() */
enum { EASE_NONE = 0, EASE_QUARTER, EASE_HALF, EASE_LAST = EASE_HALF };

enum {
    P_ROT_X,
    P_ROT_Y,
    P_ROT_Z,
    P_POS_X,
    P_POS_Y,
    P_POS_Z,
    P_SCALE,
    P_DEPTH,
    P_FOV,
    P_SPIN_X,
    P_SPIN_Y,
    P_SPIN_Z,
    P_SHAPE,
    P_ROT_ORDER,
    P_EASING,
    P_FIT,
    P_KEEP_IN_FRAME,
    P_SHADING,
    P_FACE_OPACITY,
    P_SHOW_INSIDE,
    P_QUALITY,
    P_BACKGROUND,
    P_BG_COLOR,
    P_FACE_FRONT, /* the six faces stay together, in c3d_render.h order */
    P_FACE_RIGHT,
    P_FACE_BACK,
    P_FACE_LEFT,
    P_FACE_TOP,
    P_FACE_BOTTOM,
    P_FACE_COLOR,
    P_FILE_1, /* the files stay together */
    P_FILE_2,
    P_FILE_3,
    P_FILE_4,
    P_FILE_5,
#ifndef CUBE3D_MIXER
    P_FILE_6,
#endif
    P_COUNT
};

#ifdef CUBE3D_MIXER
#define DEF(filter, mixer) (mixer)
#else
#define DEF(filter, mixer) (filter)
#endif

typedef struct
{
    const char *name;
    int type;
    double def; /* numbers and booleans */
    const char *explanation;
} param_desc;

static const param_desc PARAMS[P_COUNT] = {
    [P_ROT_X] = {"rot_x", F0R_PARAM_DOUBLE, 0,
                 "Rotation about the horizontal axis in degrees; positive tips the top towards the viewer"},
    [P_ROT_Y] = {"rot_y", F0R_PARAM_DOUBLE, 0,
                 "Rotation about the vertical axis in degrees; positive turns the front to the right"},
    [P_ROT_Z] = {"rot_z", F0R_PARAM_DOUBLE, 0, "Roll in degrees; positive is clockwise"},
    [P_POS_X] = {"pos_x", F0R_PARAM_DOUBLE, 0,
                 "Horizontal position in percent of the frame width; positive is right"},
    [P_POS_Y] = {"pos_y", F0R_PARAM_DOUBLE, 0,
                 "Vertical position in percent of the frame height; positive is down"},
    [P_POS_Z] = {"pos_z", F0R_PARAM_DOUBLE, 0,
                 "Distance in percent of the frame height; positive is away from the viewer"},
    [P_SCALE] = {"scale", F0R_PARAM_DOUBLE, DEF(50, 100),
                 "Size in percent; at 100 the unrotated front face matches the frame"},
    [P_DEPTH] = {"depth", F0R_PARAM_DOUBLE, 100, "Depth of the box in percent; 0 is a flat card"},
    [P_FOV] = {"fov", F0R_PARAM_DOUBLE, DEF(40, 15),
               "Field of view in degrees (1-160); larger gives stronger perspective"},
    [P_SPIN_X] = {"spin_x", F0R_PARAM_DOUBLE, 0,
                  "Constant spin about the horizontal axis in degrees per second, added to rot_x"},
    [P_SPIN_Y] = {"spin_y", F0R_PARAM_DOUBLE, 0,
                  "Constant spin about the vertical axis in degrees per second, added to rot_y"},
    [P_SPIN_Z] = {"spin_z", F0R_PARAM_DOUBLE, 0,
                  "Constant roll in degrees per second, added to rot_z"},
    [P_SHAPE] = {"shape", F0R_PARAM_DOUBLE, DEF(0, 1),
                 "0 = cube, 1 = frame-shaped box for left/right spins, 2 = frame-shaped box for up/down spins"},
    [P_ROT_ORDER] = {"rot_order", F0R_PARAM_DOUBLE, 0,
                     "0 = turntable (spin about the box's vertical axis, then tilt), 1 = tumble (spin about its horizontal axis, then turn)"},
    [P_EASING] = {"easing", F0R_PARAM_DOUBLE, DEF(EASE_NONE, EASE_QUARTER),
                  "Softens rot_x, rot_y and rot_z so that a steady turn starts and stops gently: 0 = off, 1 = at every quarter turn, 2 = at every half turn"},
    [P_FIT] = {"fit", F0R_PARAM_DOUBLE, 0,
               "How a picture is placed on a face: 0 = fill (crop), 1 = fit (whole picture), 2 = stretch"},
    [P_KEEP_IN_FRAME] = {"keep_in_frame", F0R_PARAM_DOUBLE, DEF(0, 100),
                         "Percent; 100 shrinks the box just enough to keep all of it inside the frame"},
    [P_SHADING] = {"shading", F0R_PARAM_DOUBLE, 35,
                   "Percent; how much faces darken as they turn away"},
    [P_FACE_OPACITY] = {"face_opacity", F0R_PARAM_DOUBLE, 100, "Opacity of the faces in percent"},
    [P_SHOW_INSIDE] = {"show_inside", F0R_PARAM_BOOL, 1,
                       "Draw the inside of faces seen through empty or see-through faces"},
    [P_QUALITY] = {"quality", F0R_PARAM_DOUBLE, 1, "0 = draft, 1 = good, 2 = best"},
    [P_BACKGROUND] = {"background", F0R_PARAM_DOUBLE, DEF(BG_TRANSPARENT, BG_COLOR),
                      "Behind the box: 0 = transparent, 1 = bg_color, 2 = clip A, 3 = clip B"},
    [P_BG_COLOR] = {"bg_color", F0R_PARAM_COLOR, 0, "Background colour"},
    [P_FACE_FRONT] = {"face_front", F0R_PARAM_DOUBLE, SRC_A,
                      "Front face: 0 = empty, 1 = clip A, 2 = clip B, 3-8 = file 1-6, 9-14 = grid cell 1-6 of clip A, 15 = face_color"},
    [P_FACE_RIGHT] = {"face_right", F0R_PARAM_DOUBLE, DEF(SRC_A, SRC_B), "Right face, values as face_front"},
    [P_FACE_BACK] = {"face_back", F0R_PARAM_DOUBLE, DEF(SRC_A, SRC_B), "Back face, values as face_front"},
    [P_FACE_LEFT] = {"face_left", F0R_PARAM_DOUBLE, DEF(SRC_A, SRC_B), "Left face, values as face_front"},
    [P_FACE_TOP] = {"face_top", F0R_PARAM_DOUBLE, DEF(SRC_A, SRC_B), "Top face, values as face_front"},
    [P_FACE_BOTTOM] = {"face_bottom", F0R_PARAM_DOUBLE, DEF(SRC_A, SRC_B), "Bottom face, values as face_front"},
    [P_FACE_COLOR] = {"face_color", F0R_PARAM_COLOR, 0, "Colour of faces set to 15"},
    /* names MLT keeps absolute across saving a project - see the top of this file */
#ifdef CUBE3D_MIXER
    [P_FILE_1] = {"resource", F0R_PARAM_STRING, 0, "File 1: a video or image file to show on faces"},
    [P_FILE_2] = {"producer.resource", F0R_PARAM_STRING, 0, "File 2"},
    [P_FILE_3] = {"luma", F0R_PARAM_STRING, 0, "File 3"},
    [P_FILE_4] = {"luma.resource", F0R_PARAM_STRING, 0, "File 4"},
    [P_FILE_5] = {"composite.luma", F0R_PARAM_STRING, 0, "File 5"},
#else
    [P_FILE_1] = {"resource", F0R_PARAM_STRING, 0, "File 1: a video or image file to show on faces"},
    [P_FILE_2] = {"producer.resource", F0R_PARAM_STRING, 0, "File 2"},
    [P_FILE_3] = {"filename", F0R_PARAM_STRING, 0, "File 3"},
    [P_FILE_4] = {"av.file", F0R_PARAM_STRING, 0, "File 4"},
    [P_FILE_5] = {"av.filename", F0R_PARAM_STRING, 0, "File 5"},
    [P_FILE_6] = {"filter.resource", F0R_PARAM_STRING, 0, "File 6"},
#endif
};

typedef struct
{
    int w, h;
    double v[P_COUNT]; /* numbers, booleans, list indices */
    f0r_param_color_t bg_color, face_color;
    char *file[NUM_FILES];        /* never NULL */
    c3d_media *media[NUM_FILES];  /* opened when a face first needs it */
    c3d_texture *ftex[NUM_FILES]; /* last picture of each file ... */
    uint64_t fserial[NUM_FILES];  /* ... and which picture that is */
    c3d_texture *missing;         /* shown when a file cannot be read */
} cube_t;

/* ------------------------------------------------------------------ */

int f0r_init(void)
{
    return 1;
}

void f0r_deinit(void)
{
    /* Nothing: MLT calls this whenever one service using the plugin goes
     * away, while other instances may well be alive. */
}

void f0r_get_plugin_info(f0r_plugin_info_t *info)
{
#ifdef CUBE3D_MIXER
    info->name = "3D Cube Transition";
    info->plugin_type = F0R_PLUGIN_TYPE_MIXER2;
    info->explanation = "Turns from one clip to another on the faces of a spinning 3D box";
#else
    info->name = "3D Cube";
    info->plugin_type = F0R_PLUGIN_TYPE_FILTER;
    info->explanation = "Shows clips on the faces of a 3D box that can be spun and moved";
#endif
    info->author = "NostalgiaRealm";
    info->color_model = F0R_COLOR_MODEL_RGBA8888;
    info->frei0r_version = FREI0R_MAJOR_VERSION;
    info->major_version = 1;
    info->minor_version = 0;
    info->num_params = P_COUNT;
}

void f0r_get_param_info(f0r_param_info_t *info, int index)
{
    if (index < 0 || index >= P_COUNT)
        return;
    info->name = PARAMS[index].name;
    info->type = PARAMS[index].type;
    info->explanation = PARAMS[index].explanation;
}

f0r_instance_t f0r_construct(unsigned int width, unsigned int height)
{
    cube_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->w = (int) width;
    c->h = (int) height;
    for (int i = 0; i < P_COUNT; i++)
        c->v[i] = PARAMS[i].def;
    c->face_color.r = c->face_color.g = c->face_color.b = 0.25f;
    for (int i = 0; i < NUM_FILES; i++) {
        c->file[i] = strdup("");
        if (!c->file[i]) {
            while (i--)
                free(c->file[i]);
            free(c);
            return NULL;
        }
    }
    return c;
}

static void drop_file(cube_t *c, int i)
{
    c3d_texture_destroy(c->ftex[i]);
    c->ftex[i] = NULL;
    c->fserial[i] = 0;
    c3d_media_release(c->media[i]);
    c->media[i] = NULL;
}

void f0r_destruct(f0r_instance_t instance)
{
    cube_t *c = instance;
    if (!c)
        return;
    for (int i = 0; i < NUM_FILES; i++) {
        drop_file(c, i);
        free(c->file[i]);
    }
    c3d_texture_destroy(c->missing);
    free(c);
}

void f0r_set_param_value(f0r_instance_t instance, f0r_param_t param, int index)
{
    cube_t *c = instance;
    if (!c || !param || index < 0 || index >= P_COUNT)
        return;
    switch (PARAMS[index].type) {
    case F0R_PARAM_DOUBLE:
    case F0R_PARAM_BOOL:
        c->v[index] = *(const double *) param;
        break;
    case F0R_PARAM_COLOR:
        *(index == P_BG_COLOR ? &c->bg_color : &c->face_color) = *(const f0r_param_color_t *) param;
        break;
    case F0R_PARAM_STRING: {
        int i = index - P_FILE_1;
        const char *s = *(const f0r_param_string *) param;
        if (!s)
            s = "";
        if (!strncmp(s, "file://", 7))
            s += 7;
        if (strcmp(s, c->file[i])) {
            char *copy = strdup(s);
            if (copy) {
                drop_file(c, i);
                free(c->file[i]);
                c->file[i] = copy;
            }
        }
        break;
    }
    }
}

void f0r_get_param_value(f0r_instance_t instance, f0r_param_t param, int index)
{
    cube_t *c = instance;
    if (!c || !param || index < 0 || index >= P_COUNT)
        return;
    switch (PARAMS[index].type) {
    case F0R_PARAM_DOUBLE:
        *(double *) param = c->v[index];
        break;
    case F0R_PARAM_BOOL:
        *(double *) param = c->v[index] > 0.5 ? 1.0 : 0.0;
        break;
    case F0R_PARAM_COLOR:
        *(f0r_param_color_t *) param = index == P_BG_COLOR ? c->bg_color : c->face_color;
        break;
    case F0R_PARAM_STRING:
        *(f0r_param_string *) param = c->file[index - P_FILE_1];
        break;
    }
}

/* ------------------------------------------------------------------ */

static int to_index(double v, int last)
{
    if (!(v > 0.0)) /* also NaN */
        return 0;
    if (v >= (double) last)
        return last;
    return (int) (v + 0.5);
}

static uint8_t to_byte(float v)
{
    return v <= 0.0f ? 0 : (v >= 1.0f ? 255 : (uint8_t) (v * 255.0f + 0.5f));
}

/* grey tiles with a red cross: "this file could not be shown" */
static c3d_texture *missing_texture(void)
{
    enum { N = 96 };
    uint8_t *img = malloc(N * N * 4);
    if (!img)
        return NULL;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint8_t *p = img + (y * N + x) * 4;
            uint8_t g = ((x / 12) + (y / 12)) & 1 ? 70 : 45;
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
            int d1 = abs(x - y), d2 = abs(x + y - (N - 1));
            if (d1 < 4 || d2 < 4) {
                p[0] = 200;
                p[1] = 40;
                p[2] = 40;
            }
        }
    c3d_texture *t = c3d_texture_create(img, N * 4, N, N, 0, 0, N, N, 1.0);
    free(img);
    return t;
}

/*
 * A frei0r plugin is told neither how long a transition is nor how far it has
 * got, so it cannot ease a move the usual way. But a turn can be eased from
 * the angle alone: within every quarter (or half) turn the angle is run
 * through a smooth step. Plain two-keyframe moves of 90 or 180 degrees then
 * start and stop gently, and stay that way however the host rewrites or
 * stretches the keyframes.
 */
static double ease_angle(double degrees, int mode)
{
    if (mode == EASE_NONE || !isfinite(degrees))
        return degrees;
    double period = mode == EASE_HALF ? 180.0 : 90.0;
    double turns = degrees / period;
    double whole = floor(turns), f = turns - whole;
    return (whole + f * f * (3.0 - 2.0 * f)) * period;
}

/* Renders may run on several threads at once (MLT keeps one instance per
 * thread); share the cores between them instead of each taking all. */
static int active_renders = 0;

static int thread_budget(int active)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int n = cpus < 1 ? 1 : (cpus > 64 ? 64 : (int) cpus);
    const char *env = getenv("CUBE3D_THREADS");
    if (env && atoi(env) > 0 && atoi(env) < n)
        n = atoi(env);
    n /= active > 0 ? active : 1;
    return n < 1 ? 1 : n;
}

static void render(cube_t *c, double time, const uint32_t *in1, const uint32_t *in2, uint32_t *out)
{
    const int W = c->w, H = c->h;
    if (!out || W <= 0 || H <= 0)
        return;
    const uint8_t *in[2] = {(const uint8_t *) in1, (const uint8_t *) (in2 ? in2 : in1)};
    if (!isfinite(time))
        time = 0.0;

    c3d_scene s;
    c3d_scene_defaults(&s);
    int ease = to_index(c->v[P_EASING], EASE_LAST);
    s.rot_x = ease_angle(c->v[P_ROT_X], ease) + c->v[P_SPIN_X] * time;
    s.rot_y = ease_angle(c->v[P_ROT_Y], ease) + c->v[P_SPIN_Y] * time;
    s.rot_z = ease_angle(c->v[P_ROT_Z], ease) + c->v[P_SPIN_Z] * time;
    s.pos_x = c->v[P_POS_X] / 100.0;
    s.pos_y = c->v[P_POS_Y] / 100.0;
    s.pos_z = c->v[P_POS_Z] / 100.0;
    s.size = c->v[P_SCALE] / 100.0;
    s.depth = c->v[P_DEPTH] / 100.0;
    s.fov = c->v[P_FOV];
    s.keep_in_frame = c->v[P_KEEP_IN_FRAME] / 100.0;
    s.shade = c->v[P_SHADING] / 100.0;
    s.opacity = c->v[P_FACE_OPACITY] / 100.0;
    s.shape = to_index(c->v[P_SHAPE], C3D_SHAPE_FRAME_V);
    s.order = to_index(c->v[P_ROT_ORDER], C3D_ORDER_XYZ);
    s.fit = to_index(c->v[P_FIT], C3D_FIT_STRETCH);
    s.quality = to_index(c->v[P_QUALITY], C3D_QUALITY_BEST);
    s.inside = c->v[P_SHOW_INSIDE] > 0.5;

    switch (to_index(c->v[P_BACKGROUND], BG_LAST)) {
    case BG_COLOR:
        s.bg_mode = C3D_BG_COLOR;
        s.bg_color[0] = to_byte(c->bg_color.r);
        s.bg_color[1] = to_byte(c->bg_color.g);
        s.bg_color[2] = to_byte(c->bg_color.b);
        s.bg_color[3] = 255;
        break;
    case BG_A:
    case BG_B: {
        const uint8_t *img = in[to_index(c->v[P_BACKGROUND], BG_LAST) == BG_B];
        if (img) {
            s.bg_mode = C3D_BG_IMAGE;
            s.bg_image = img;
            s.bg_stride = W * 4;
        }
        break;
    }
    default:
        break;
    }

    /* textures made for this frame only: the two inputs and the grid cells */
    c3d_texture *tin[2] = {NULL, NULL};
    c3d_texture *tcell[NUM_CELLS] = {NULL};
    int tried_in[2] = {0, 0}, tried_cell[NUM_CELLS] = {0}, tried_file[NUM_FILES] = {0};

    for (int f = 0; f < C3D_NUM_FACES; f++) {
        int src = to_index(c->v[P_FACE_FRONT + f], SRC_LAST);
        c3d_face *face = &s.face[f];
        if (src == SRC_A || src == SRC_B) {
            int k = src == SRC_B;
            if (in[1] == in[0])
                k = 0;
            if (!tried_in[k] && in[k]) {
                tried_in[k] = 1;
                tin[k] = c3d_texture_create(in[k], W * 4, W, H, 0, 0, W, H, 0);
            }
            face->tex = tin[k];
        } else if (src >= SRC_CELL && src < SRC_CELL + NUM_CELLS) {
            int k = src - SRC_CELL;
            if (!tried_cell[k] && in[0]) {
                tried_cell[k] = 1;
                int col = k % 3, row = k / 3;
                int x0 = col * W / 3, x1 = (col + 1) * W / 3;
                int y0 = row * H / 2, y1 = (row + 1) * H / 2;
                /* stay a pixel clear of the neighbours: whatever scaled the
                 * clips into the grid has blended them along the borders */
                if (x1 - x0 > 8 && y1 - y0 > 8) {
                    x0++;
                    x1--;
                    y0++;
                    y1--;
                }
                /* a cell holds a whole frame squeezed into it */
                tcell[k] = c3d_texture_create(in[0], W * 4, W, H, x0, y0, x1 - x0, y1 - y0,
                                              (double) W / (double) H);
            }
            face->tex = tcell[k];
        } else if (src >= SRC_FILE && src < SRC_FILE + MAX_FILES) {
            int k = src - SRC_FILE;
            if (k >= NUM_FILES || !c->file[k][0])
                continue; /* no such file, or none chosen yet: leave the face empty */
            if (!tried_file[k]) {
                tried_file[k] = 1;
                if (!c->media[k])
                    c->media[k] = c3d_media_acquire(c->file[k]);
                c3d_texture *t = NULL;
                int r = c3d_media_texture(c->media[k], time, W, H, &c->fserial[k], &t);
                if (r > 0) {
                    c3d_texture_destroy(c->ftex[k]);
                    c->ftex[k] = t;
                } else if (r < 0) {
                    c3d_texture_destroy(c->ftex[k]);
                    c->ftex[k] = NULL;
                    c->fserial[k] = 0;
                }
            }
            if (c->ftex[k]) {
                face->tex = c->ftex[k];
            } else {
                if (!c->missing)
                    c->missing = missing_texture();
                face->tex = c->missing;
            }
        } else if (src == SRC_COLOR) {
            face->solid = 1;
            face->color[0] = to_byte(c->face_color.r);
            face->color[1] = to_byte(c->face_color.g);
            face->color[2] = to_byte(c->face_color.b);
            face->color[3] = 255;
        }
    }

    int active = __atomic_add_fetch(&active_renders, 1, __ATOMIC_RELAXED);
    c3d_render(&s, (uint8_t *) out, W * 4, W, H, thread_budget(active));
    __atomic_sub_fetch(&active_renders, 1, __ATOMIC_RELAXED);

    for (int i = 0; i < 2; i++)
        c3d_texture_destroy(tin[i]);
    for (int i = 0; i < NUM_CELLS; i++)
        c3d_texture_destroy(tcell[i]);
}

void f0r_update(f0r_instance_t instance, double time, const uint32_t *inframe, uint32_t *outframe)
{
    if (instance)
        render(instance, time, inframe, NULL, outframe);
}

void f0r_update2(f0r_instance_t instance, double time, const uint32_t *inframe1,
                 const uint32_t *inframe2, const uint32_t *inframe3, uint32_t *outframe)
{
    (void) inframe3;
    if (instance)
        render(instance, time, inframe1, inframe2, outframe);
}
