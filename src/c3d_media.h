/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube3d - pictures for cube faces taken from media files.
 *
 * frei0r gives a plugin its one or two input frames and nothing else. To put
 * other clips on the cube, the plugin opens them itself through the MLT
 * framework that is hosting it (melt, Kdenlive, kdenlive_render): video,
 * stills, image sequences, title files, "color:..." and the like.
 *
 * Not accepted: MLT XML and Kdenlive project files (and other sources that
 * are a whole graph of producers rather than one). Each producer inside such a
 * file needs room in MLT's decoder caches that cannot be reserved from here,
 * and MLT crashes when those caches overflow. To show a timeline on a face,
 * render it, or feed it through the grid cells of the cube's own input.
 *
 * MLT is not linked in. Its functions are looked up in the running process
 * the first time they are needed; in a host that is not built on MLT the
 * lookup fails and file faces are simply reported as unavailable.
 */
#ifndef C3D_MEDIA_H
#define C3D_MEDIA_H

#include "c3d_render.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c3d_media c3d_media;

/* 1 when the hosting process offers the MLT API. */
int c3d_media_supported(void);

/*
 * One shared, reference counted object per path. The file is not touched
 * until the first picture is requested. Returns NULL for an empty path or
 * when out of memory.
 */
c3d_media *c3d_media_acquire(const char *path);
void c3d_media_release(c3d_media *media);

/*
 * Picture of the file at `time` seconds from its start (it loops), as a
 * texture. out_width x out_height is the size of the frame being rendered:
 * sources taller than that are scaled down while decoding, and sources that
 * have no size of their own (colours, titles) are made in that shape.
 *
 * *serial identifies the picture the caller already holds a texture for
 * (0 = none). Returns
 *    1  a new texture was stored in *tex and *serial was updated,
 *    0  the picture is the one identified by *serial: keep using your texture,
 *   -1  no picture (file missing, unreadable, without video or of a kind that
 *       is not accepted, or MLT not available). A file that could not be
 *       opened is tried again every couple of seconds.
 * Safe to call from several threads at once.
 */
int c3d_media_texture(c3d_media *media, double time, int out_width, int out_height,
                      uint64_t *serial, c3d_texture **tex);

#ifdef __cplusplus
}
#endif

#endif /* C3D_MEDIA_H */
