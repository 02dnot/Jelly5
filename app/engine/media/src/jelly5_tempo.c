/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Playback speed without the chipmunk: a WSOLA time-stretcher on the player's
 * 48 kHz interleaved float PCM. Frames of 1024 samples (21 ms) are laid down
 * every 512 output samples with a Hann window (which sums to one at 50 %
 * overlap); each is taken from the input `speed` times further on, nudged up
 * to +/-256 samples to where it lines up best with the natural continuation of
 * the frame before (cross-correlation on the front left and right), so the
 * waveform stays continuous and the pitch stays put.
 */
#include "jelly5_tempo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TN   1024        /* frame */
#define HS   (TN / 2)    /* output hop: frames overlap by half */
#define SEEK 256         /* how far a frame may move to line up */
#define OVL  (TN / 2)    /* how much of it is compared */

static struct {
    int ch;
    float *in;           /* interleaved input not yet consumed */
    long in_len, in_cap; /* in frames (samples per channel) */
    double pos;          /* where the next frame would start at this speed */
    long prev;           /* where the last frame did start */
    int have_prev;
    float ola[HS * 8];   /* the second half of the last windowed frame */
    float *out;          /* this call's output */
    long out_cap;
    float win[TN];
    int ready;
} T;

void jelly5_tempo_reset(void)
{
    T.in_len = 0;
    T.pos = SEEK;
    T.prev = 0;
    T.have_prev = 0;
    memset(T.ola, 0, sizeof T.ola);
}

static int grow(float **buf, long *cap, long need_frames, int ch)
{
    if (need_frames <= *cap)
        return 1;
    long cap2 = *cap ? *cap : 8192;
    while (cap2 < need_frames)
        cap2 *= 2;
    float *b = (float *)realloc(*buf, (size_t)cap2 * (size_t)ch * sizeof(float));
    if (!b)
        return 0;
    *buf = b;
    *cap = cap2;
    return 1;
}

/* Front left + right as one signal, for lining frames up. */
static inline float mono(const float *f, int ch) { return ch >= 2 ? f[0] + f[1] : f[0]; }

int jelly5_tempo_process(const float *in, int frames, int ch, float speed, const float **out)
{
    if (!T.ready) {
        for (int i = 0; i < TN; i++)
            T.win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float)i / (float)TN);
        T.ready = 1;
        jelly5_tempo_reset();
    }
    if (ch < 1 || ch > 8)
        return 0;
    if (ch != T.ch) {
        T.ch = ch;
        T.in_cap = 0;
        free(T.in);
        T.in = NULL;
        T.out_cap = 0;
        free(T.out);
        T.out = NULL;
        jelly5_tempo_reset();
    }
    if (!grow(&T.in, &T.in_cap, T.in_len + frames, ch))
        return 0;
    memcpy(T.in + (size_t)T.in_len * ch, in, (size_t)frames * ch * sizeof(float));
    T.in_len += frames;

    const long max_frames = (long)((double)T.in_len / (HS * speed)) + 2;
    if (!grow(&T.out, &T.out_cap, max_frames * HS, ch))
        return 0;
    long n_out = 0;

    for (;;) {
        const long p = (long)T.pos;
        if (p + SEEK + TN > T.in_len)
            break;
        long s = p;
        if (T.have_prev) {   /* line up with where the last frame naturally goes on */
            const long target = T.prev + HS;
            if (target + OVL <= T.in_len) {
                float best = -1e30f;
                for (long d = -SEEK; d <= SEEK; d += 2) {
                    const long c = p + d;
                    if (c < 0)
                        continue;
                    float acc = 0.f;
                    const float *a = T.in + (size_t)target * ch, *b = T.in + (size_t)c * ch;
                    for (int i = 0; i < OVL; i += 2)
                        acc += mono(a + (size_t)i * ch, ch) * mono(b + (size_t)i * ch, ch);
                    if (acc > best) {
                        best = acc;
                        s = c;
                    }
                }
            }
        }
        /* Overlap-add: emit the first half, keep the second for the next frame. */
        float *o = T.out + (size_t)n_out * ch;
        const float *src = T.in + (size_t)s * ch;
        for (int i = 0; i < HS; i++)
            for (int c = 0; c < ch; c++) {
                o[(size_t)i * ch + c] = T.ola[i * ch + c] + T.win[i] * src[(size_t)i * ch + c];
                T.ola[i * ch + c] = T.win[i + HS] * src[(size_t)(i + HS) * ch + c];
            }
        n_out += HS;
        T.prev = s;
        T.have_prev = 1;
        T.pos += HS * speed;
    }

    /* Drop input nothing will read again. */
    long keep_from = (long)T.pos - SEEK;
    if (T.prev < keep_from)
        keep_from = T.prev;
    if (keep_from > 4096) {
        memmove(T.in, T.in + (size_t)keep_from * ch, (size_t)(T.in_len - keep_from) * ch * sizeof(float));
        T.in_len -= keep_from;
        T.pos -= keep_from;
        T.prev -= keep_from;
    }
    *out = T.out;
    return (int)n_out;
}
