/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif

#include "c3d_media.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* the handful of MLT functions used, resolved at run time             */
/* ------------------------------------------------------------------ */

/* Values from MLT's public headers (framework/mlt_types.h, mlt_log.h). */
#define MLT_IMAGE_RGBA 2
#define MLT_SERVICE_PRODUCER_TYPE 2
#define MLT_SERVICE_CHAIN_TYPE 11
#define MLT_LOG_WARNING 24

static struct
{
    int ok;
    void *(*factory_repository)(void);
    void *(*global_properties)(void);
    void *(*profile_init)(const char *name);
    void (*profile_close)(void *profile);
    void (*profile_from_producer)(void *profile, void *producer);
    void *(*factory_producer)(void *profile, const char *service, const void *resource);
    void (*producer_close)(void *producer);
    int (*producer_seek)(void *producer, int32_t position);
    int32_t (*producer_get_playtime)(void *producer);
    double (*producer_get_fps)(void *producer);
    void *(*producer_properties)(void *producer);
    void *(*producer_service)(void *producer);
    int (*service_identify)(void *service);
    int (*service_get_frame)(void *service, void **frame, int index);
    int (*frame_get_image)(void *frame, uint8_t **buffer, int *format, int *width, int *height,
                           int writable);
    void (*frame_close)(void *frame);
    void *(*frame_properties)(void *frame);
    int (*frame_is_test_card)(void *frame);
    int (*properties_set)(void *properties, const char *name, const char *value);
    int (*properties_set_int)(void *properties, const char *name, int value);
    char *(*properties_get)(void *properties, const char *name);
    int (*properties_get_int)(void *properties, const char *name);
    int (*cache_get_size)(void *service, const char *name);
    void (*cache_set_size)(void *service, const char *name, int size);
    void (*log)(void *service, int level, const char *fmt, ...);
} mlt;

static pthread_once_t mlt_once = PTHREAD_ONCE_INIT;

static void mlt_resolve(void)
{
    int missing = 0;
#define NEED(field, symbol) \
    do { \
        *(void **) (&mlt.field) = dlsym(RTLD_DEFAULT, symbol); \
        if (!mlt.field) \
            missing = 1; \
    } while (0)
    NEED(factory_repository, "mlt_factory_repository");
    NEED(global_properties, "mlt_global_properties");
    NEED(profile_init, "mlt_profile_init");
    NEED(profile_close, "mlt_profile_close");
    NEED(profile_from_producer, "mlt_profile_from_producer");
    NEED(factory_producer, "mlt_factory_producer");
    NEED(producer_close, "mlt_producer_close");
    NEED(producer_seek, "mlt_producer_seek");
    NEED(producer_get_playtime, "mlt_producer_get_playtime");
    NEED(producer_get_fps, "mlt_producer_get_fps");
    NEED(producer_properties, "mlt_producer_properties");
    NEED(producer_service, "mlt_producer_service");
    NEED(service_identify, "mlt_service_identify");
    NEED(service_get_frame, "mlt_service_get_frame");
    NEED(frame_get_image, "mlt_frame_get_image");
    NEED(frame_close, "mlt_frame_close");
    NEED(frame_properties, "mlt_frame_properties");
    NEED(frame_is_test_card, "mlt_frame_is_test_card");
    NEED(properties_set, "mlt_properties_set");
    NEED(properties_set_int, "mlt_properties_set_int");
    NEED(properties_get, "mlt_properties_get");
    NEED(properties_get_int, "mlt_properties_get_int");
    NEED(cache_get_size, "mlt_service_cache_get_size");
    NEED(cache_set_size, "mlt_service_cache_set_size");
#undef NEED
    /* optional, only used for a diagnostic */
    *(void **) (&mlt.log) = dlsym(RTLD_DEFAULT, "mlt_log");
    mlt.ok = !missing;
}

int c3d_media_supported(void)
{
    pthread_once(&mlt_once, mlt_resolve);
    /* the library being present is not enough: the host must have
     * initialised it, otherwise there is no service repository to ask */
    return mlt.ok && mlt.factory_repository() != NULL;
}

static void warn(const char *what, const char *path)
{
    if (mlt.log)
        mlt.log(NULL, MLT_LOG_WARNING, "[frei0r cube3d] %s: \"%s\"\n", what, path);
}

/* ------------------------------------------------------------------ */
/* room in MLT's per-producer caches                                   */
/* ------------------------------------------------------------------ */

/*
 * MLT keeps the decoder state of every producer in small process-wide LRU
 * caches (one entry per producer). Hosts size them for the producers they
 * know about - MLT itself uses twice the number of tracks, Kdenlive adds the
 * thread count - and the default is just 4. The producers opened here are on
 * top of that. If a cache overflows while an evicted entry is still referenced
 * by a frame in flight, MLT mixes up the reference counts of the old and the
 * re-created entry and frees a decoder that is in use: with four video faces
 * melt crashed inside libavcodec. So every producer opened here adds a slot
 * to the caches it can occupy, on top of whatever size the host last chose.
 *
 * The filter and the transition are two libraries, each with its own copy of
 * this file, yet the caches are one per process. The bookkeeping therefore
 * lives in MLT's global properties, where both copies see it: how many
 * producers each copy has open, the size a copy last set, and the host's size
 * that was derived from. Without that each copy would take the other's
 * setting for the host's and add its own share on top, again and again.
 */
#ifdef CUBE3D_MIXER
#define LIVE_MINE "cube3d.open.transition"
#define LIVE_OTHER "cube3d.open.filter"
#else
#define LIVE_MINE "cube3d.open.filter"
#define LIVE_OTHER "cube3d.open.transition"
#endif

static const char *const CACHE_NAMES[] = {
    "producer_avformat", "qimage.image", "qimage.alpha", "qimage.qimage",
    "pixbuf.image",      "pixbuf.alpha", "pixbuf.pixbuf",
};
#define N_CACHES ((int) (sizeof(CACHE_NAMES) / sizeof(CACHE_NAMES[0])))
#define MLT_MAX_CACHE_SIZE 200 /* larger values are ignored by MLT */

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int live_producers = 0; /* producers this copy has open */

static void reserve_cache_slots(int delta)
{
    void *global = mlt.global_properties();
    if (!global)
        return;
    pthread_mutex_lock(&cache_lock);
    live_producers += delta;
    mlt.properties_set_int(global, LIVE_MINE, live_producers);
    int live = live_producers + mlt.properties_get_int(global, LIVE_OTHER);
    for (int i = 0; i < N_CACHES; i++) {
        char key_set[64], key_base[64];
        snprintf(key_set, sizeof(key_set), "cube3d.set.%s", CACHE_NAMES[i]);
        snprintf(key_base, sizeof(key_base), "cube3d.base.%s", CACHE_NAMES[i]);
        int cur = mlt.cache_get_size(NULL, CACHE_NAMES[i]);
        if (cur <= 0)
            continue;
        int base = mlt.properties_get_int(global, key_base);
        if (cur != mlt.properties_get_int(global, key_set) || base <= 0) {
            base = cur; /* the host (re)sized it since a cube3d copy last did */
            mlt.properties_set_int(global, key_base, base);
        }
        int want = base + live + 2;
        if (want > MLT_MAX_CACHE_SIZE)
            want = MLT_MAX_CACHE_SIZE;
        if (cur < want) {
            mlt.properties_set_int(global, key_set, want);
            mlt.cache_set_size(NULL, CACHE_NAMES[i], want);
            if (getenv("CUBE3D_DEBUG"))
                fprintf(stderr, "[cube3d] MLT cache %s: %d -> %d (host %d + %d face files + 2)\n",
                        CACHE_NAMES[i], cur, want, base, live);
        }
    }
    pthread_mutex_unlock(&cache_lock);
}

/* ------------------------------------------------------------------ */
/* shared media objects                                                */
/* ------------------------------------------------------------------ */

#define RECENT 8
#define RETRY_SECONDS 2

struct c3d_media
{
    struct c3d_media *next;
    char *path;
    int refcount;         /* guarded by registry_lock */
    pthread_mutex_t lock; /* guards everything below */
    int state;            /* 0 not opened yet, 1 ready, -1 failed */
    time_t failed_at;     /* monotonic seconds, when state became -1 */
    int refused;          /* a kind of source that is never opened */
    void *profile, *producer;
    double fps;
    int32_t length;
    int still;              /* every position shows the same picture */
    double zone_in, zone_len; /* seconds; zone_len 0 = the whole file (see read_link) */
    int native_w, native_h; /* 0: the source has no size of its own */
    double native_aspect;
    /* which picture got which serial, so a caller that still holds the
     * texture for a picture does not have to fetch it again */
    struct
    {
        int32_t pos;
        int w, h;
        uint64_t serial;
    } recent[RECENT];
    int recent_next;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static c3d_media *registry = NULL;
static uint64_t next_serial = 1; /* guarded by registry_lock */

static uint64_t new_serial(void)
{
    pthread_mutex_lock(&registry_lock);
    uint64_t s = next_serial++;
    pthread_mutex_unlock(&registry_lock);
    return s;
}

static time_t monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

c3d_media *c3d_media_acquire(const char *path)
{
    if (!path || !*path)
        return NULL;
    pthread_mutex_lock(&registry_lock);
    c3d_media *m;
    for (m = registry; m; m = m->next)
        if (!strcmp(m->path, path))
            break;
    if (m) {
        m->refcount++;
    } else {
        m = calloc(1, sizeof(*m));
        if (m)
            m->path = strdup(path);
        if (m && m->path) {
            pthread_mutex_init(&m->lock, NULL);
            m->refcount = 1;
            m->next = registry;
            registry = m;
        } else {
            free(m);
            m = NULL;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return m;
}

void c3d_media_release(c3d_media *m)
{
    if (!m)
        return;
    pthread_mutex_lock(&registry_lock);
    int dead = --m->refcount == 0;
    if (dead) {
        c3d_media **pp = &registry;
        while (*pp && *pp != m)
            pp = &(*pp)->next;
        if (*pp)
            *pp = m->next;
    }
    pthread_mutex_unlock(&registry_lock);
    if (!dead)
        return;
    /* nobody else can reach it any more */
    if (m->producer) {
        mlt.producer_close(m->producer);
        reserve_cache_slots(-1);
    }
    if (m->profile)
        mlt.profile_close(m->profile);
    pthread_mutex_destroy(&m->lock);
    free(m->path);
    free(m);
}

static int has_suffix(const char *s, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    return n >= k && !strncasecmp(s + n - k, suffix, k);
}

static int has_prefix(const char *s, const char *prefix)
{
    return !strncasecmp(s, prefix, strlen(prefix));
}

/* Sources that are a graph of producers rather than one (see c3d_media.h). */
static int is_compound(const char *path)
{
    static const char *const prefixes[] = {"xml:", "xml-string:", "xml-nogl:", "consumer:",
                                           "melt:", "melt_file:", "<"};
    static const char *const suffixes[] = {".mlt", ".kdenlive", ".xml", ".melt", ".westley"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (has_prefix(path, prefixes[i]))
            return 1;
    /* MLT lets parameters follow the name after a '?' */
    const char *q = strrchr(path, '?');
    size_t n = q ? (size_t) (q - path) : strlen(path);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (has_suffix(path, n, suffixes[i]))
            return 1;
    return 0;
}

static void media_open_target(c3d_media *m, const char *path);

/* "12.48" -> 12.48 whatever the process locale uses as decimal separator */
static double parse_seconds(const char *s)
{
    double v = 0.0, scale = 1.0;
    int seen = 0;
    for (; *s >= '0' && *s <= '9'; s++, seen = 1)
        v = v * 10.0 + (*s - '0');
    if (*s == '.' || *s == ',')
        for (s++; *s >= '0' && *s <= '9'; s++, seen = 1)
            v += (*s - '0') * (scale /= 10.0);
    return seen ? v : -1.0;
}

/*
 * A ".c3dface" file stands for a clip, or a stretch of one, that lives
 * somewhere else - which is how a zone of a clip in Kdenlive's project bin
 * gets onto a face (tools/cube3d-faces writes these files). Plain text:
 *
 *     cube3d-face 1
 *     file=/absolute/path/or/anything/MLT/opens
 *     in=12.48          seconds, optional
 *     out=20.0          seconds, optional (the last moment shown)
 *
 * Returns the target (to be freed) or NULL.
 */
static char *read_link(const char *path, double *in, double *len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char line[4200], *target = NULL;
    double a = 0.0, b = -1.0;
    int first = 1, ok = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (first) {
            first = 0;
            ok = !strncmp(line, "cube3d-face", 11);
            if (!ok)
                break;
        } else if (!strncmp(line, "file=", 5) && !target) {
            target = strdup(line + 5);
        } else if (!strncmp(line, "in=", 3)) {
            a = parse_seconds(line + 3);
        } else if (!strncmp(line, "out=", 4)) {
            b = parse_seconds(line + 4);
        }
    }
    fclose(f);
    if (!ok || !target || !*target) {
        free(target);
        return NULL;
    }
    if (!(a > 0.0) || !isfinite(a))
        a = 0.0;
    *in = a;
    *len = isfinite(b) && b > a ? b - a : 0.0;
    return target;
}

static void media_open(c3d_media *m)
{
    m->state = -1;
    m->failed_at = monotonic_seconds();
    if (!c3d_media_supported())
        return;
    const char *path = m->path;
    char *target = NULL;
    m->zone_in = m->zone_len = 0.0;
    if (has_suffix(path, strlen(path), ".c3dface")) {
        target = read_link(path, &m->zone_in, &m->zone_len);
        if (!target) {
            warn("not a usable face link file", m->path);
            return;
        }
        path = target;
    }
    media_open_target(m, path);
    free(target);
}

static void media_open_target(c3d_media *m, const char *path)
{
    if (is_compound(path)) {
        if (!m->refused)
            warn("project and MLT XML files cannot be shown on a face", m->path);
        m->refused = 1;
        return;
    }
    /* A private profile that takes on the file's own size and frame rate
     * where MLT can tell (video), so that pictures arrive unscaled and
     * positions count the file's frames. */
    void *profile = mlt.profile_init(NULL);
    if (!profile)
        return;
    reserve_cache_slots(1); /* before the producer exists: creating it already uses a slot */
    void *producer = mlt.factory_producer(profile, NULL, path);
    if (producer) {
        mlt.profile_from_producer(profile, producer);
        mlt.producer_close(producer);
        producer = mlt.factory_producer(profile, NULL, path);
    }
    if (producer) {
        int type = mlt.service_identify(mlt.producer_service(producer));
        if (type != MLT_SERVICE_PRODUCER_TYPE && type != MLT_SERVICE_CHAIN_TYPE) {
            /* a playlist or tractor after all, whatever the name looked like */
            mlt.producer_close(producer);
            producer = NULL;
            m->refused = 1;
        }
    }
    if (!producer) {
        reserve_cache_slots(-1);
        mlt.profile_close(profile);
        warn(m->refused ? "this kind of source cannot be shown on a face"
                        : "cannot open face file",
             m->path);
        return;
    }
    void *props = mlt.producer_properties(producer);
    m->fps = mlt.producer_get_fps(producer);
    if (!(m->fps > 0.0) || !isfinite(m->fps))
        m->fps = 25.0;
    m->length = mlt.producer_get_playtime(producer);
    const char *service = mlt.properties_get(props, "mlt_service");
    m->still = service && (!strcmp(service, "qimage") || !strcmp(service, "pixbuf"))
               && !strchr(path, '%') && !strstr(path, ".all.")
               && !has_suffix(path, strlen(path), ".gif")
               && !has_suffix(path, strlen(path), ".webp");
    /* The source's own size, where it has one. Video and stills publish it;
     * colours, titles and other generators do not, and are made in the shape
     * of the frame that is being rendered. (The private profile cannot be
     * relied on for this: MLT adapts it to video files only, for everything
     * else it stays at MLT's default, a 4:3 standard-definition format.) */
    int w = mlt.properties_get_int(props, "meta.media.width");
    int h = mlt.properties_get_int(props, "meta.media.height");
    m->native_w = m->native_h = 0;
    if (w > 0 && h > 0 && w <= 32768 && h <= 32768) {
        double sar = 1.0;
        int num = mlt.properties_get_int(props, "meta.media.sample_aspect_num");
        int den = mlt.properties_get_int(props, "meta.media.sample_aspect_den");
        if (num > 0 && den > 0)
            sar = (double) num / (double) den;
        m->native_w = w;
        m->native_h = h;
        m->native_aspect = sar * (double) w / (double) h;
    }
    m->profile = profile;
    m->producer = producer;
    m->state = 1;
}

static int media_fetch(c3d_media *m, double time, int out_w, int out_h, uint64_t *serial,
                       c3d_texture **tex)
{
    if (m->state < 0 && !m->refused && monotonic_seconds() - m->failed_at >= RETRY_SECONDS)
        m->state = 0; /* the file may have appeared in the meantime */
    if (m->state == 0)
        media_open(m);
    if (m->state < 0)
        return -1;

    int32_t pos = 0;
    if (!m->still && isfinite(time) && time >= 0.0) {
        /* a zone loops within itself; a whole file loops over its length */
        if (m->zone_len > 0.0)
            time = fmod(time, m->zone_len);
        double p = floor((m->zone_in + time) * m->fps + 0.5);
        if (m->length > 0)
            p = fmod(p, (double) m->length);
        if (p >= 0.0 && p < 2147483000.0)
            pos = (int32_t) p;
    }

    /* The size to ask for: the source's own, scaled down while decoding when
     * it is clearly larger than the frame it will be shown in. Always an
     * explicit, even size (parts of MLT's scaling path work in 4:2:2) and
     * always "distort": the whole picture at exactly that size, no padding. */
    double w = m->native_h > 0 ? m->native_w : out_w;
    double h = m->native_h > 0 ? m->native_h : out_h;
    double aspect = m->native_h > 0 ? m->native_aspect : w / h;
    if (h > out_h + out_h / 4) {
        w = w * out_h / h;
        h = out_h;
    }
    int req_w = ((int) (w + 0.5) + 1) & ~1, req_h = ((int) (h + 0.5) + 1) & ~1;
    if (req_w < 2)
        req_w = 2;
    if (req_h < 2)
        req_h = 2;

    int known = -1;
    for (int i = 0; i < RECENT; i++)
        if (m->recent[i].serial && m->recent[i].pos == pos && m->recent[i].w == req_w
            && m->recent[i].h == req_h) {
            if (*serial == m->recent[i].serial)
                return 0;
            known = i;
            break;
        }

    reserve_cache_slots(0); /* the host may have resized its caches meanwhile */
    if (mlt.producer_seek(m->producer, pos))
        return -1;
    void *frame = NULL;
    if (mlt.service_get_frame(mlt.producer_service(m->producer), &frame, 0) || !frame)
        return -1;
    c3d_texture *t = NULL;
    if (!mlt.frame_is_test_card(frame)) { /* e.g. a sound file: nothing to show */
        void *props = mlt.frame_properties(frame);
        mlt.properties_set(props, "consumer.rescale", "bilinear");
        mlt.properties_set_int(props, "consumer.progressive", 1);
        mlt.properties_set_int(props, "distort", 1);
        uint8_t *image = NULL;
        int format = MLT_IMAGE_RGBA, iw = req_w, ih = req_h;
        if (!mlt.frame_get_image(frame, &image, &format, &iw, &ih, 0) && image
            && format == MLT_IMAGE_RGBA && iw > 0 && ih > 0)
            t = c3d_texture_create(image, iw * 4, iw, ih, 0, 0, iw, ih, aspect);
    }
    mlt.frame_close(frame);
    if (!t)
        return -1;

    /* The same picture fetched again (by another render thread, say) keeps
     * its serial, so the textures others hold for it stay valid. */
    if (known < 0) {
        known = m->recent_next;
        m->recent_next = (known + 1) % RECENT;
        m->recent[known].pos = pos;
        m->recent[known].w = req_w;
        m->recent[known].h = req_h;
        m->recent[known].serial = new_serial();
    }
    *serial = m->recent[known].serial;
    *tex = t;
    return 1;
}

int c3d_media_texture(c3d_media *m, double time, int out_width, int out_height, uint64_t *serial,
                      c3d_texture **tex)
{
    if (!m || !serial || !tex || out_width <= 0 || out_height <= 0)
        return -1;
    pthread_mutex_lock(&m->lock);
    int r = media_fetch(m, time, out_width, out_height, serial, tex);
    pthread_mutex_unlock(&m->lock);
    return r;
}
