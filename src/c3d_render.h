/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube3d - software renderer for a textured box.
 *
 * Everything in here is plain C with no dependency on frei0r or MLT, so it can
 * be exercised by the stand-alone test program in tests/.
 *
 * Conventions
 *   - Images are 8-bit RGBA with straight (non-premultiplied) alpha, byte
 *     order R,G,B,A - what MLT hands to frei0r plugins.
 *   - Lengths are expressed relative to the frame so that a scene looks the
 *     same at preview resolution and at render resolution.
 */
#ifndef C3D_RENDER_H
#define C3D_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    C3D_FACE_FRONT = 0,
    C3D_FACE_RIGHT,
    C3D_FACE_BACK,
    C3D_FACE_LEFT,
    C3D_FACE_TOP,
    C3D_FACE_BOTTOM,
    C3D_NUM_FACES
};

/* Box proportions. The front face always has the listed width x height. */
enum {
    C3D_SHAPE_CUBE = 0, /* square faces, edge = frame height                    */
    C3D_SHAPE_FRAME_H,  /* frame-shaped front, depth = width  (spin left/right) */
    C3D_SHAPE_FRAME_V   /* frame-shaped front, depth = height (spin up/down)    */
};

/* How an image is placed on a face whose proportions differ from the image. */
enum {
    C3D_FIT_FILL = 0, /* cover the face, crop the overflow       */
    C3D_FIT_FIT,      /* whole image visible, rest of face empty */
    C3D_FIT_STRETCH   /* distort to the face                     */
};

/* Order in which the three rotations are applied (first one listed is the
 * innermost, i.e. it turns the box about its own axis). */
enum {
    C3D_ORDER_YXZ = 0, /* spin about the box's vertical axis, then tilt, then roll */
    C3D_ORDER_XYZ      /* tumble about the box's horizontal axis, then turn, then roll */
};

enum { C3D_BG_TRANSPARENT = 0, C3D_BG_COLOR, C3D_BG_IMAGE };

enum { C3D_QUALITY_DRAFT = 0, C3D_QUALITY_GOOD, C3D_QUALITY_BEST };

/* A face image, stored premultiplied together with its mip chain. */
typedef struct c3d_texture c3d_texture;

/*
 * Copies the rectangle (rx,ry,rw,rh) of an RGBA image into a new texture.
 * display_aspect is the width/height the picture is meant to be shown at
 * (pass 0 to use rw/rh, i.e. square pixels).
 * Returns NULL on bad arguments or allocation failure.
 */
c3d_texture *c3d_texture_create(const uint8_t *rgba, int stride, int width, int height,
                                int rx, int ry, int rw, int rh, double display_aspect);
void c3d_texture_destroy(c3d_texture *tex);

typedef struct c3d_face {
    c3d_texture *tex; /* picture for this face, or NULL                       */
    int solid;        /* tex == NULL: 1 = paint with color, 0 = leave it open */
    uint8_t color[4]; /* straight RGBA                                        */
} c3d_face;

typedef struct c3d_scene {
    double rot_x, rot_y, rot_z; /* degrees. +x: top tips towards the viewer,
                                   +y: front turns to the right, +z: clockwise */
    double pos_x, pos_y;        /* fraction of frame width / height, +right / +down */
    double pos_z;               /* fraction of frame height, + is away from the viewer */
    double size;                /* 1 = front face (unrotated) exactly matches the frame
                                       (cube: a square as tall as the frame)     */
    double depth;               /* multiplier for the box depth, 0 = flat card */
    double fov;                 /* vertical field of view in degrees           */
    double keep_in_frame;       /* 0..1: shrink so the whole box stays visible */
    double shade;               /* 0..1: darken faces that turn away           */
    double opacity;             /* 0..1: opacity of all faces                  */
    int shape, fit, order, quality;
    int inside; /* also draw faces seen from the inside (through open or
                   translucent faces) */
    c3d_face face[C3D_NUM_FACES];
    int bg_mode;
    uint8_t bg_color[4];     /* straight RGBA, for C3D_BG_COLOR                */
    const uint8_t *bg_image; /* width x height RGBA, for C3D_BG_IMAGE          */
    int bg_stride;
} c3d_scene;

void c3d_scene_defaults(c3d_scene *scene);

/*
 * Renders the scene into out (width x height RGBA, straight alpha).
 * max_threads <= 0 picks a sensible number.
 *
 * The textures referenced by the scene are completed lazily (mip levels), so
 * one texture must not be used by two c3d_render() calls at the same time.
 * out may be the same buffer as bg_image but must not overlap anything else.
 */
void c3d_render(const c3d_scene *scene, uint8_t *out, int stride, int width, int height,
                int max_threads);

#ifdef __cplusplus
}
#endif

#endif /* C3D_RENDER_H */
