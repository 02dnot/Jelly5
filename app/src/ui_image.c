/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui_image.h"

#include "evo_boot_trace.h"

#include <stdio.h>    /* jpeglib.h needs FILE */
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <libavformat/avio.h>
#include <libavutil/dict.h>

/* The SVG decoder is compiled here (Jelly5: it was in the old interface's ui_icons.c). */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include "evo_readdir.h"

#include <dirent.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 128   /* < 256: a handle keeps the slot in its low byte */
#define MAX_FETCH (16 * 1024 * 1024)

enum { EMPTY = 0, PENDING, LOADING, READY, FAILED };

typedef struct slot {
    char url[1024];
    int max_w, max_h, blur;
    int state;
    int discard;           /* cleared while loading: drop the result */
    unsigned seq;          /* request order, for FIFO and eviction */
    ui_image img;
} slot;

static slot s_slots[SLOTS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_worker;
static unsigned s_seq;
static volatile unsigned s_generation;

/* ---- decoding --------------------------------------------------------------- */

static void premultiply(ui_image *img)
{
    const size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = img->px[i];
        uint32_t a = p >> 24;
        if (a == 255)
            continue;
        uint32_t r = ((p & 0xff) * a + 127) / 255;
        uint32_t g = (((p >> 8) & 0xff) * a + 127) / 255;
        uint32_t b = (((p >> 16) & 0xff) * a + 127) / 255;
        img->px[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

static int decode_png(const uint8_t *d, size_t n, ui_image *out)
{
    png_image pi;
    memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&pi, d, n))
        return -1;
    pi.format = PNG_FORMAT_RGBA;
    if (ui_image_alloc(out, (int)pi.width, (int)pi.height) != 0) {
        png_image_free(&pi);
        return -1;
    }
    if (!png_image_finish_read(&pi, NULL, out->px, (png_int_32)(pi.width * 4), NULL)) {
        ui_image_free(out);
        return -1;
    }
    return 0;
}

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((struct jerr *)cinfo->err)->jump, 1);
}

/* Jelly5: the EXIF orientation of a JPEG (1 = as stored; 2..8 mirrored and/or
 * rotated). Phone photos - profile pictures above all - are stored sideways
 * with this tag; Jellyfin passes it through, even when it scales the image. */
static int jpeg_orientation(const uint8_t *d, size_t n)
{
    size_t i = 2;
    while (i + 4 <= n && d[i] == 0xff) {
        const int marker = d[i + 1];
        const size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (marker == 0xda || len < 2 || i + 2 + len > n)
            break;   /* image data, or a broken segment: no orientation found */
        if (marker == 0xe1 && len >= 16 && memcmp(d + i + 4, "Exif\0\0", 6) == 0) {
            const uint8_t *t = d + i + 10;
            const size_t tn = len - 8;
            const int le = t[0] == 'I';
            #define RD16(p) (le ? (uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) : ((uint32_t)(p)[0] << 8) | (p)[1])
            #define RD32(p) (le ? (uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24) \
                            : ((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (p)[3])
            const uint32_t ifd = RD32(t + 4);
            if (ifd + 2 > tn)
                return 1;
            const uint32_t count = RD16(t + ifd);
            for (uint32_t k = 0; k < count && ifd + 2 + 12 * (k + 1) <= tn; k++) {
                const uint8_t *e = t + ifd + 2 + 12 * k;
                if (RD16(e) == 0x0112) {
                    const uint32_t v = RD16(e + 8);
                    return v >= 1 && v <= 8 ? (int)v : 1;
                }
            }
            #undef RD16
            #undef RD32
            return 1;
        }
        i += 2 + len;
    }
    return 1;
}

/* Turns a decoded image upright for its EXIF orientation. */
static int orient(ui_image *img, int o)
{
    if (o <= 1)
        return 0;
    const int w = img->w, h = img->h;
    const int swap = o >= 5;   /* 5..8 rotate by 90 degrees: width and height trade places */
    ui_image r;
    if (ui_image_alloc(&r, swap ? h : w, swap ? w : h) != 0)
        return -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int nx, ny;
            switch (o) {
            case 2: nx = w - 1 - x; ny = y; break;              /* mirrored */
            case 3: nx = w - 1 - x; ny = h - 1 - y; break;      /* 180 */
            case 4: nx = x; ny = h - 1 - y; break;              /* mirrored vertically */
            case 5: nx = y; ny = x; break;                      /* transposed */
            case 6: nx = h - 1 - y; ny = x; break;              /* 90 clockwise */
            case 7: nx = h - 1 - y; ny = w - 1 - x; break;      /* transversed */
            default: nx = y; ny = w - 1 - x; break;             /* 8: 90 counter-clockwise */
            }
            r.px[(size_t)ny * r.w + nx] = img->px[(size_t)y * w + x];
        }
    ui_image_free(img);
    *img = r;
    return 0;
}

static int decode_jpeg(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    struct jpeg_decompress_struct ci;
    struct jerr je;
    memset(out, 0, sizeof *out);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_fail;
    if (setjmp(je.jump)) {
        jpeg_destroy_decompress(&ci);
        ui_image_free(out);
        return -1;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, d, (unsigned long)n);
    jpeg_read_header(&ci, TRUE);
    /* Decode no larger than needed: libjpeg scales by 1/2, 1/4, 1/8 for free. */
    ci.scale_num = 1;
    ci.scale_denom = 1;
    while (ci.scale_denom < 8 && (int)(ci.image_width / (ci.scale_denom * 2)) >= max_w &&
           (int)(ci.image_height / (ci.scale_denom * 2)) >= max_h)
        ci.scale_denom *= 2;
    ci.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&ci);
    if (ui_image_alloc(out, (int)ci.output_width, (int)ci.output_height) != 0) {
        jpeg_destroy_decompress(&ci);
        return -1;
    }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = (JSAMPROW)(out->px + (size_t)ci.output_scanline * out->w);
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    orient(out, jpeg_orientation(d, n));
    return 0;
}

static int decode_webp(const uint8_t *d, size_t n, ui_image *out)
{
    int w = 0, h = 0;
    uint8_t *rgba = WebPDecodeRGBA(d, n, &w, &h);
    if (!rgba)
        return -1;
    if (ui_image_alloc(out, w, h) != 0) {
        WebPFree(rgba);
        return -1;
    }
    memcpy(out->px, rgba, (size_t)w * h * 4);
    WebPFree(rgba);
    return 0;
}

static int decode_svg(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    char *text = (char *)malloc(n + 1);
    if (!text)
        return -1;
    memcpy(text, d, n);
    text[n] = 0;
    NSVGimage *svg = nsvgParse(text, "px", 96.0f);
    free(text);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f) {
        if (svg) nsvgDelete(svg);
        return -1;
    }
    float s = (float)max_w / svg->width;
    if (svg->height * s > (float)max_h)
        s = (float)max_h / svg->height;
    const int w = (int)(svg->width * s + 0.5f), h = (int)(svg->height * s + 0.5f);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    int rc = -1;
    if (r && w > 0 && h > 0 && ui_image_alloc(out, w, h) == 0) {
        nsvgRasterize(r, svg, 0, 0, s, (unsigned char *)out->px, w, h, w * 4);
        rc = 0;
    }
    if (r) nsvgDeleteRasterizer(r);
    nsvgDelete(svg);
    return rc;
}

static int looks_like_svg(const uint8_t *d, size_t n)
{
    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (d[i] == '<' && d[i + 1] == 's' && d[i + 2] == 'v' && d[i + 3] == 'g')
            return 1;
    return 0;
}

int ui_image_decode(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    int rc;
    ui_image raw;
    memset(&raw, 0, sizeof raw);
    memset(out, 0, sizeof *out);
    if (!d || n < 12)
        return -1;
    if (!memcmp(d, "\x89PNG", 4))
        rc = decode_png(d, n, &raw);
    else if (d[0] == 0xff && d[1] == 0xd8)
        rc = decode_jpeg(d, n, max_w, max_h, &raw);
    else if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        rc = decode_webp(d, n, &raw);
    else if (looks_like_svg(d, n))
        rc = decode_svg(d, n, max_w, max_h, &raw);
    else
        rc = -1;
    if (rc != 0)
        return -1;
    premultiply(&raw);
    if (raw.w > max_w || raw.h > max_h) {
        float s = (float)max_w / raw.w;
        if (raw.h * s > (float)max_h)
            s = (float)max_h / raw.h;
        int w = (int)(raw.w * s + 0.5f), h = (int)(raw.h * s + 0.5f);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        rc = ui_image_resize(&raw, w, h, out);
        ui_image_free(&raw);
        return rc;
    }
    *out = raw;
    return 0;
}

/* ---- Jelly5: the disk cache ----------------------------------------------------
 *
 * Jellyfin's image URLs carry the image's tag, which changes when the image
 * does: such a response never goes stale, so it is kept on disk and the next
 * launch draws from there instead of the network. Only tagged image URLs are
 * kept (never trickplay sheets or anything carrying a token). The cache lives
 * in the app's own data (/download0, 256 MB quota) and keeps under CACHE_CAP,
 * dropping the oldest files first. */

#define CACHE_DIR "/download0/jelly5/img"
#define CACHE_CAP (96ull * 1024 * 1024)

static pthread_mutex_t s_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_cache_ready;
static unsigned long long s_cache_bytes;

static int cacheable(const char *url)
{
    return strstr(url, "/Images/") && strstr(url, "tag=") && !strstr(url, "ApiKey") &&
           !strstr(url, "api_key") && !strstr(url, "/Trickplay/");
}

static void cache_path(const char *url, char *out, size_t cap)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    snprintf(out, cap, CACHE_DIR "/%016llx.img", (unsigned long long)h);
}

typedef struct cache_file {
    char name[32];
    time_t mtime;
    off_t size;
} cache_file;

static int by_age(const void *a, const void *b)
{
    const time_t x = ((const cache_file *)a)->mtime, y = ((const cache_file *)b)->mtime;
    return x < y ? -1 : x > y;
}

/* Sums the cache; over the cap, drops the oldest until it is at 3/4 of it. */
static void cache_trim(void)
{
    evo_dir_t *d = evo_opendir(CACHE_DIR);   /* opendir() is refused in the app sandbox */
    if (!d)
        return;
    size_t n = 0, cap = 0;
    cache_file *files = NULL;
    unsigned long long total = 0;
    struct dirent *e;
    while ((e = evo_readdir(d))) {
        if (!strstr(e->d_name, ".img") || strlen(e->d_name) >= sizeof files[0].name)
            continue;
        char path[160];
        snprintf(path, sizeof path, CACHE_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 512;
            cache_file *nf = (cache_file *)realloc(files, cap * sizeof *files);
            if (!nf)
                break;
            files = nf;
        }
        snprintf(files[n].name, sizeof files[n].name, "%s", e->d_name);
        files[n].mtime = st.st_mtime;
        files[n].size = st.st_size;
        total += (unsigned long long)st.st_size;
        n++;
    }
    evo_closedir(d);
    if (total > CACHE_CAP && n) {
        qsort(files, n, sizeof *files, by_age);
        for (size_t i = 0; i < n && total > CACHE_CAP * 3 / 4; i++) {
            char path[160];
            snprintf(path, sizeof path, CACHE_DIR "/%s", files[i].name);
            if (unlink(path) == 0)
                total -= (unsigned long long)files[i].size;
        }
    }
    free(files);
    s_cache_bytes = total;
}

static uint8_t *cache_read(const char *url, size_t *len)
{
    char path[160];
    cache_path(url, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && n <= MAX_FETCH && fseek(f, 0, SEEK_SET) == 0 && (buf = (uint8_t *)malloc((size_t)n))) {
            if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
                *len = (size_t)n;
            } else {
                free(buf);
                buf = NULL;
            }
        }
    }
    fclose(f);
    if (!buf)
        unlink(path);        /* unreadable: fetch it again */
    return buf;
}

static void cache_write(const char *url, const uint8_t *data, size_t len)
{
    char path[160], tmp[176];
    cache_path(url, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.%p", path, (void *)pthread_self());
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    const int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    pthread_mutex_lock(&s_cache_lock);
    s_cache_bytes += len;
    if (s_cache_bytes > CACHE_CAP)
        cache_trim();
    pthread_mutex_unlock(&s_cache_lock);
}

/* A cached file that did not decode is dropped (the next request fetches it). */
static void cache_forget(const char *url)
{
    if (!cacheable(url))
        return;
    char path[160];
    cache_path(url, path, sizeof path);
    unlink(path);
}

static void cache_init(void)
{
    pthread_mutex_lock(&s_cache_lock);
    if (!s_cache_ready) {
        mkdir("/download0/jelly5", 0777);
        mkdir(CACHE_DIR, 0777);
        cache_trim();
        s_cache_ready = 1;
    }
    pthread_mutex_unlock(&s_cache_lock);
}

/* ---- fetching ------------------------------------------------------------------ */

static uint8_t *fetch_network(const char *url, size_t *len);

static uint8_t *fetch(const char *url, size_t *len)
{
    *len = 0;
    const int keep = cacheable(url);
    if (keep) {
        cache_init();
        uint8_t *hit = cache_read(url, len);
        if (hit)
            return hit;
    }
    uint8_t *data = fetch_network(url, len);
    if (data && keep)
        cache_write(url, data, *len);
    return data;
}

static uint8_t *fetch_network(const char *url, size_t *len)
{
    AVIOContext *io = NULL;
    AVDictionary *opts = NULL;
    uint8_t *buf = NULL;
    size_t got = 0, cap = 0;

    *len = 0;
    av_dict_set(&opts, "rw_timeout", "10000000", 0);
    av_dict_set(&opts, "user_agent", "Mozilla/5.0 (PlayStation 5) NuvioPS5", 0);
    if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return NULL;
    }
    av_dict_free(&opts);
    for (;;) {
        if (got + 65536 > cap) {
            size_t ncap = cap ? cap * 2 : 256 * 1024;
            if (ncap > MAX_FETCH) ncap = MAX_FETCH;
            if (ncap <= got) break;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        int r = avio_read(io, buf + got, (int)(cap - got));
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    avio_closep(&io);
    if (!got) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/* Jelly5: a logo's transparent margins cut away. Logos come with any amount of
 * empty canvas around them (one was 600x614 for a 390x185 mark in a corner);
 * cropped to what is drawn, every logo fills the space it is laid out in. */
static void trim_transparent(ui_image *img)
{
    int x0 = img->w, y0 = img->h, x1 = -1, y1 = -1;
    for (int y = 0; y < img->h; y++) {
        const uint32_t *row = img->px + (size_t)y * img->w;
        for (int x = 0; x < img->w; x++)
            if ((row[x] >> 24) > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    }
    if (x1 < 0 || (x0 == 0 && y0 == 0 && x1 == img->w - 1 && y1 == img->h - 1))
        return;   /* empty, or nothing to cut */
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    ui_image t;
    if (ui_image_alloc(&t, w, h) != 0)
        return;
    for (int y = 0; y < h; y++)
        memcpy(t.px + (size_t)y * w, img->px + (size_t)(y0 + y) * img->w + x0, (size_t)w * 4);
    ui_image_free(img);
    *img = t;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        slot *next = NULL;
        while (!next) {
            /* Jelly5: newest first - the art for what is focused now beats the
             * art the viewer has already scrolled past. */
            for (int i = 0; i < SLOTS; i++)
                if (s_slots[i].state == PENDING && (!next || s_slots[i].seq > next->seq))
                    next = &s_slots[i];
            if (!next)
                pthread_cond_wait(&s_cond, &s_lock);
        }
        char url[1024];
        memcpy(url, next->url, sizeof url);
        const int mw = next->max_w, mh = next->max_h, blur = next->blur;
        next->state = LOADING;
        next->discard = 0;
        pthread_mutex_unlock(&s_lock);

        size_t len = 0;
        ui_image img;
        memset(&img, 0, sizeof img);
        uint8_t *data = fetch(url, &len);
        int ok = data && ui_image_decode(data, len, mw, mh, &img) == 0;
        free(data);
        if (ok && blur > 0)
            ui_image_blur(&img, blur);
        if (ok && strstr(url, "/Images/Logo"))
            trim_transparent(&img);
        if (!ok) {
            evo_bt("image: FAILED %.100s", url);
            cache_forget(url);
        }

        pthread_mutex_lock(&s_lock);
        if (next->discard || next->state != LOADING) {
            ui_image_free(&img);
            if (next->discard) {
                next->state = EMPTY;
                next->discard = 0;
            }
        } else {
            next->img = img;
            next->state = ok ? READY : FAILED;
        }
        s_generation++;
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

int ui_image_request(const char *url, int max_w, int max_h, int blur)
{
    if (!url || !*url || strlen(url) >= sizeof s_slots[0].url)
        return -1;
    pthread_mutex_lock(&s_lock);
    /* Jelly5: four fetch/decode workers (the server's first resize of an
     * image takes ~1 s, so one worker made a row of art arrive one by one). */
    while (s_worker < 4) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0)
            break;
        pthread_detach(t);
        s_worker++;
    }
    int found = -1, free_i = -1, oldest = -1;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &s_slots[i];
        if (s->state != EMPTY && !s->discard && s->max_w == max_w && s->max_h == max_h &&
            s->blur == blur && !strcmp(s->url, url)) {
            found = i;
            break;
        }
        if (s->state == EMPTY && free_i < 0)
            free_i = i;
        if ((s->state == READY || s->state == FAILED) &&
            (oldest < 0 || s->seq < s_slots[oldest].seq))
            oldest = i;
    }
    if (found < 0) {
        found = free_i >= 0 ? free_i : oldest;
        if (found >= 0) {
            slot *s = &s_slots[found];
            ui_image_free(&s->img);
            snprintf(s->url, sizeof s->url, "%s", url);
            s->max_w = max_w;
            s->max_h = max_h;
            s->blur = blur;
            s->state = PENDING;
            s->discard = 0;
            s->seq = ++s_seq;
            pthread_cond_signal(&s_cond);
        }
    }
    /* Jelly5: the handle carries the request's sequence number, so a handle
     * whose slot has since been reused for another image stops resolving
     * (it used to return that other image). */
    const int handle = found < 0 ? -1 : (int)(((s_slots[found].seq & 0x7fffffu) << 8) | (unsigned)found);
    pthread_mutex_unlock(&s_lock);
    return handle;
}

/* Index of a still-current handle, else -1. Caller holds s_lock. */
static int slot_of(int handle)
{
    if (handle < 0)
        return -1;
    const int i = handle & 0xff;
    if (i >= SLOTS || (s_slots[i].seq & 0x7fffffu) != ((unsigned)handle >> 8) || s_slots[i].state == EMPTY)
        return -1;
    return i;
}

int ui_image_alive(int handle)
{
    pthread_mutex_lock(&s_lock);
    const int ok = slot_of(handle) >= 0;
    pthread_mutex_unlock(&s_lock);
    return ok;
}

const ui_image *ui_image_get(int handle, int *failed)
{
    const ui_image *out = NULL;
    if (failed)
        *failed = handle < 0;
    pthread_mutex_lock(&s_lock);
    const int i = slot_of(handle);
    if (i >= 0 && s_slots[i].state == READY)
        out = &s_slots[i].img;
    else if (failed && (i < 0 || s_slots[i].state == FAILED))
        *failed = 1;
    pthread_mutex_unlock(&s_lock);
    return out;
}

unsigned ui_image_generation(void)
{
    return s_generation;
}

void ui_image_clear(void)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &s_slots[i];
        if (s->state == LOADING) {
            s->discard = 1;
        } else {
            ui_image_free(&s->img);
            s->state = EMPTY;
        }
        s->url[0] = 0;
    }
    pthread_mutex_unlock(&s_lock);
}
