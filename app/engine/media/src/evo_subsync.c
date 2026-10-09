/*
 * evo_subsync.c - auto-sync an external SRT to the film's audio (#102).
 * See evo_subsync.h for the model and the split with evo_subtitle.c.
 *
 * Pipeline per run:
 *   1. Own AVFormatContext on the media path, every stream but the selected
 *      audio one discarded. Playback's demux is never touched.
 *   2. Three windows (~5 min at 20% / 50% / 80% of the film). Each is seeked
 *      to, decoded, resampled to mono 8 kHz (centre channel only when the
 *      layout has one - that is where the dialogue is), band-passed
 *      300-3400 Hz and cut into 10 ms frames. An energy VAD with an adaptive
 *      noise floor turns the frames into a bit-packed speech vector.
 *   3. The cues are rasterised into the same 10 ms grid (in media time, for a
 *      given scale) and slid over each window, +-60 s, scored by bit
 *      covariance (popcount); the windows are summed into one curve.
 *      Confidence = how far the peak stands out, in SDs of that curve.
 *   4. Scale 1.0 first, then each standard framerate ratio that would move the
 *      cues at least 1 s across the analysed span. A drifting SRT smears the
 *      peak inside a window and puts each window at a different offset, so
 *      only the right ratio lines the windows up - which doubles as the
 *      re-score that confirms it. The winner needs a peak that stands out
 *      (z, sharpness) and two windows whose own peaks sit on it; an SRT from
 *      another film manages that under no scale and reports
 *      EVO_SUBSYNC_LOW_CONF.
 *
 * Only libc/libm calls EVO already makes (no log10/cos/sin: the filter
 * coefficients are precomputed and the VAD works in linear energy).
 */
#include "evo_subsync.h"
#include "evo_stream_io.h"
#include "evo_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "evo_boot_log.h"

#define SS_RATE         8000
#define SS_FRAME_SMP    80                  /* 10 ms at 8 kHz              */
#define SS_FPS          100                 /* bit-vector frames / second  */
#define SS_WINDOWS      3
#define SS_WINDOW_S     300.0
/* Embedded tracks are offset-only (no ratio search), and every window also
 * reads SS_SEARCH_S of interleaved file on each side to gather its cues -
 * on a big remux that read, not the decode, is the cost (a 1080p HEVC 5.1
 * MKV took ~45 s a window at 300 s). Half the window is plenty for one
 * offset and cuts the read by about a third. */
#define SS_WINDOW_EMB_S 150.0
#define SS_SEARCH_S     60
#define SS_SEARCH_F     (SS_SEARCH_S * SS_FPS) /* +-60 s                   */
#define SS_EXCL_F       (SS_FPS / 2)        /* runner-up must be > 0.5 s off */
#define SS_AGREE_F      (SS_FPS / 2)        /* a window's own peak within 0.5 s */
/* Acceptance, in standard deviations of the summed correlation curve.
 * Calibrated on real films (tools/subsync_host.sh): see docs/build/tooling.md. */
/* Tears of Steel + the synthetic film: right answers z 5.1-6.1, sharp
 * 0.40-1.6; wrong SRTs z <= 2.5, sharp <= 0.20, never 2 windows agreeing. */
#define SS_MIN_Z        4.0
#define SS_MIN_SHARP    0.35
#define SS_RATIO_GAIN   1.10                /* a ratio must beat 1.0's z by 10% */
#define SS_MIN_DRIFT_S  1.0                 /* ratio must move cues this far */
#define SS_MIN_CUES     8
#define SS_MIN_SPEECH   0.02                /* fraction of window frames   */
/* Cap on how fast the worker pulls the file, so a 4K remux being played from
 * the same USB stick keeps its bandwidth. The window decode has to read every
 * interleaved video packet to reach the audio. */
#define SS_READ_CAP_BPS (40.0 * 1024.0 * 1024.0)
/* Jelly5: from the media server (direct play over http) the read shares the
 * network with the stream being watched: a lower cap, and half-length windows
 * (as for embedded tracks), which still find one offset or ratio reliably. */
#define SS_NET_READ_CAP_BPS (16.0 * 1024.0 * 1024.0)
#define SS_NET_WINDOW_S     150.0

static const struct {
    double      srt_fps;
    double      media_fps;
    const char *label;
} k_ratios[] = {
    { 25.0,            24000.0 / 1001.0, "25->23.976 fps" },
    { 24000.0 / 1001.0, 25.0,            "23.976->25 fps" },
    { 24.0,            24000.0 / 1001.0, "24->23.976 fps" },
    { 24000.0 / 1001.0, 24.0,            "23.976->24 fps" },
    { 25.0,            24.0,             "25->24 fps" },
    { 24.0,            25.0,             "24->25 fps" },
};
#define SS_RATIO_COUNT ((int)(sizeof(k_ratios) / sizeof(k_ratios[0])))

static double ss_abs(double v) { return v < 0.0 ? -v : v; }

static double ss_now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

const char *evo_subsync_ratio_label(double scale)
{
    for (int i = 0; i < SS_RATIO_COUNT; i++) {
        if (ss_abs(scale - k_ratios[i].media_fps / k_ratios[i].srt_fps) < 1e-4)
            return k_ratios[i].label;
    }
    return NULL;
}

static int ss_is_http(const char *p)
{
    return p && (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8));
}

/* Jelly5: an http(s) URL too (a direct-play stream from the server); it is
 * opened with the playback's network options and request headers. */
int evo_subsync_path_supported(const char *media_path)
{
    return media_path && media_path[0] && (!strstr(media_path, "://") || ss_is_http(media_path));
}

/* The request headers for an http(s) media path: set before evo_subsync_start,
 * read only by the worker (ignored while one runs). */
static char s_req_headers[4096];
static char s_req_ua[512];
/* Jelly5: the server's audio-only source for the next run (evo_subsync_set_server_source). */
static char   s_audio_url[4096];
static char   s_cue_url[4096];
static double s_url_duration_s;
static int    s_static_fallback;
static char   s_stop_url[4096];

/* ------------------------------------------------------------------------
 * Bit vectors. Bit i lives in word i >> 6, bit i & 63.
 * ---------------------------------------------------------------------- */

static void ss_set_range(uint64_t *v, int64_t nbits, int64_t from, int64_t to)
{
    if (from < 0) from = 0;
    if (to > nbits) to = nbits;
    for (int64_t i = from; i < to; i++)
        v[i >> 6] |= 1ULL << (i & 63);
}

static uint64_t ss_get64(const uint64_t *v, int64_t nwords, int64_t pos)
{
    int64_t wi = pos >> 6;
    int     sh = (int)(pos & 63);
    uint64_t lo = (wi >= 0 && wi < nwords) ? v[wi] : 0;
    if (!sh) return lo;
    uint64_t hi = (wi + 1 >= 0 && wi + 1 < nwords) ? v[wi + 1] : 0;
    return (lo >> sh) | (hi << (64 - sh));
}

/* ------------------------------------------------------------------------
 * Audio -> speech vector
 * ---------------------------------------------------------------------- */

typedef struct {
    double    start_s;         /* media time of frame 0                    */
    int       frames;
    int       words;
    uint64_t *bits;            /* speech/no-speech per 10 ms               */
    int       speech_frames;
    int       valid;
} ss_window;

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    SwrContext      *swr;
    AVStream        *st;
    int              stream;
    double           start_offset_s;   /* fmt->start_time, so media time starts at 0 */
    double           duration_s;

    int              swr_fmt, swr_rate, swr_channels;
    float           *out;
    int              out_cap;

    volatile int    *cancel;
    volatile int    *progress;

    /* Embedded track: cues are gathered from its packets while the windows
     * are read, so each read starts SS_SEARCH_S early and runs SS_SEARCH_S
     * late - every cue the +-60 s search can reach. */
    int              sub_stream;
    AVStream        *sub_st;
    double          *ecs, *ece;
    int              en, ecap;

    double           read_cap;         /* bytes/s */
    int              url_windows;      /* Jelly5: each window its own input, from 0 */
    double           io_t0;
    double           io_bytes;
    int64_t          io_last;
} ss_ctx;

static int ss_interrupt(void *opaque)
{
    volatile int *cancel = (volatile int *)opaque;
    return cancel && *cancel;
}

static int ss_cancelled(const ss_ctx *c) { return c->cancel && *c->cancel; }

/* Band-pass: RBJ biquads, HPF 300 Hz + LPF 3400 Hz, Q 0.7071, fs 8000. */
typedef struct { float x1, x2, y1, y2; } ss_biquad;

static float ss_bq(ss_biquad *s, float x, float b0, float b1, float b2,
                   float a1, float a2)
{
    float y = b0 * x + b1 * s->x1 + b2 * s->x2 - a1 * s->y1 - a2 * s->y2;
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

static int ss_swr_setup(ss_ctx *c, const AVFrame *f)
{
    int nch = f->ch_layout.nb_channels;
    if (c->swr && c->swr_fmt == f->format && c->swr_rate == f->sample_rate &&
        c->swr_channels == nch)
        return 0;
    swr_free(&c->swr);

    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(&c->swr, &mono, AV_SAMPLE_FMT_FLT, SS_RATE,
                            &f->ch_layout, (enum AVSampleFormat)f->format,
                            f->sample_rate, 0, NULL) < 0 || !c->swr)
        return -1;

    /* Dialogue lives in the centre channel of a surround mix; a plain
     * downmix buries it under music and effects. */
    int fc = av_channel_layout_index_from_channel(&f->ch_layout,
                                                  AV_CHAN_FRONT_CENTER);
    if (fc >= 0 && nch >= 3 && nch <= 64) {
        double matrix[64];
        memset(matrix, 0, sizeof(matrix));
        matrix[fc] = 1.0;
        swr_set_matrix(c->swr, matrix, nch);
    }
    if (swr_init(c->swr) < 0) {
        swr_free(&c->swr);
        return -1;
    }
    c->swr_fmt = f->format;
    c->swr_rate = f->sample_rate;
    c->swr_channels = nch;
    return 0;
}

static void ss_throttle(ss_ctx *c)
{
    if (!c->fmt->pb) return;
    int64_t pos = avio_tell(c->fmt->pb);
    if (pos > c->io_last) c->io_bytes += (double)(pos - c->io_last);
    c->io_last = pos;
    double due = c->io_t0 + c->io_bytes / c->read_cap;
    double now = ss_now_s();
    if (due > now) {
        double wait = due - now;
        if (wait > 0.05) wait = 0.05;
        usleep((useconds_t)(wait * 1e6));
    }
}

static int ss_cue_push(ss_ctx *c, double s, double e)
{
    if (c->en == c->ecap) {
        int cap = c->ecap ? c->ecap * 2 : 1024;
        double *ns = (double *)realloc(c->ecs, sizeof(double) * (size_t)cap);
        if (!ns) return -1;
        c->ecs = ns;
        double *ne = (double *)realloc(c->ece, sizeof(double) * (size_t)cap);
        if (!ne) return -1;
        c->ece = ne;
        c->ecap = cap;
    }
    c->ecs[c->en] = s;
    c->ece[c->en] = e;
    c->en++;
    return 0;
}

static int ss_decode_window(ss_ctx *c, ss_window *w, int widx)
{
    const int64_t total_smp = (int64_t)w->frames * SS_FRAME_SMP;
    float    *esum = (float *)calloc((size_t)w->frames, sizeof(float));
    uint16_t *ecnt = (uint16_t *)calloc((size_t)w->frames, sizeof(uint16_t));
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    int rc = -1;
    if (!esum || !ecnt || !pkt || !frm) goto out;

    const double margin = c->sub_stream >= 0 ? SS_SEARCH_S + 1.0 : 0.0;
    const double wlen = (double)w->frames / SS_FPS;
    double seek_s = w->start_s - margin;
    if (seek_s < 0.0) seek_s = 0.0;
    int64_t ts = (int64_t)((seek_s + c->start_offset_s) * AV_TIME_BASE);
    if (!c->url_windows && seek_s > 0.0 &&
        av_seek_frame(c->fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0 &&
        avformat_seek_file(c->fmt, -1, INT64_MIN, ts, ts, 0) < 0) {
        evo_log("subsync: window %d seek to %.1fs failed", widx, w->start_s);
        goto out;
    }
    avcodec_flush_buffers(c->dec);
    swr_free(&c->swr);
    c->io_last = c->fmt->pb ? avio_tell(c->fmt->pb) : 0;

    const float hb0 = 0.846459254f, hb1 = -1.692918508f, hb2 = 0.846459254f;
    const float ha1 = -1.669203143f, ha2 = 0.716633874f;
    const float lb0 = 0.715737410f, lb1 = 1.431474820f, lb2 = 0.715737410f;
    const float la1 = 1.348967745f, la2 = 0.513981894f;
    ss_biquad hp = {0}, lp = {0};

    const double tb = av_q2d(c->st->time_base);
    const double sub_tb = c->sub_st ? av_q2d(c->sub_st->time_base) : 0.0;
    int64_t cursor = -1;           /* window-relative output sample index */
    int done = 0;                  /* audio window filled */
    int subs_done = c->sub_stream < 0;

    while (!(done && subs_done) && !ss_cancelled(c)) {
        int r = av_read_frame(c->fmt, pkt);
        if (r < 0) break;          /* EOF / error: keep what we have */
        ss_throttle(c);
        int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;

        if (pkt->stream_index == c->sub_stream) {
            if (pts != AV_NOPTS_VALUE) {
                double t = (double)pts * sub_tb - c->start_offset_s;
                double rel = t - w->start_s;
                if (rel > wlen + margin) {
                    subs_done = 1;
                } else if (rel >= -margin) {
                    double d = pkt->duration > 0 ? (double)pkt->duration * sub_tb : 3.0;
                    if (ss_cue_push(c, t, t + d) < 0) { av_packet_unref(pkt); goto out; }
                }
            }
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->stream_index != c->stream) {
            av_packet_unref(pkt);
            continue;
        }
        if (pts != AV_NOPTS_VALUE) {
            double rel = (double)pts * tb - c->start_offset_s - w->start_s;
            /* Interleaved: audio this far past the range means every cue
             * inside it has been read. */
            if (rel > wlen + margin + 5.0) subs_done = 1;
            /* The lead-in read for cues costs no decode. */
            if (done || rel < -1.0) {
                av_packet_unref(pkt);
                continue;
            }
        } else if (done) {
            av_packet_unref(pkt);
            continue;
        }
        r = avcodec_send_packet(c->dec, pkt);
        av_packet_unref(pkt);
        if (r < 0 && r != AVERROR(EAGAIN))
            continue;              /* one bad packet is not fatal */

        while (avcodec_receive_frame(c->dec, frm) >= 0) {
            if (done) {
                av_frame_unref(frm);
                continue;
            }
            if (ss_swr_setup(c, frm) < 0) {
                av_frame_unref(frm);
                goto out;
            }
            int64_t fpts = frm->best_effort_timestamp;
            if (fpts != AV_NOPTS_VALUE) {
                double t = (double)fpts * tb - c->start_offset_s - w->start_s;
                int64_t expect = (int64_t)(t * SS_RATE);
                if (cursor < 0 || expect - cursor > SS_RATE / 50 ||
                    cursor - expect > SS_RATE / 50)
                    cursor = expect;
                if (t > (double)w->frames / SS_FPS + 1.0) done = 1;
            } else if (cursor < 0) {
                av_frame_unref(frm);
                continue;          /* nothing to anchor it to yet */
            }

            int need = swr_get_out_samples(c->swr, frm->nb_samples);
            if (need > c->out_cap) {
                float *grown = (float *)realloc(c->out, (size_t)need * sizeof(float));
                if (!grown) { av_frame_unref(frm); goto out; }
                c->out = grown;
                c->out_cap = need;
            }
            uint8_t *outp = (uint8_t *)c->out;
            int got = swr_convert(c->swr, &outp, c->out_cap,
                                  (const uint8_t **)frm->extended_data,
                                  frm->nb_samples);
            av_frame_unref(frm);

            for (int i = 0; i < got; i++, cursor++) {
                float x = ss_bq(&hp, c->out[i], hb0, hb1, hb2, ha1, ha2);
                x = ss_bq(&lp, x, lb0, lb1, lb2, la1, la2);
                if (cursor < 0) continue;
                if (cursor >= total_smp) { done = 1; break; }
                int64_t fi = cursor / SS_FRAME_SMP;
                esum[fi] += x * x;
                if (ecnt[fi] < 0xFFFF) ecnt[fi]++;
            }
            if (c->progress && cursor > 0) {
                double frac = (double)cursor / (double)total_smp;
                if (frac > 1.0) frac = 1.0;
                *c->progress = (int)(((double)widx + frac) * 90.0 / SS_WINDOWS);
            }
        }
    }
    if (ss_cancelled(c)) goto out;

    /* Energy VAD. The noise floor drops quickly (a quiet stretch resets it)
     * and rises ~1 dB/s, so a long loud scene cannot drag it up to speech
     * level. Speech = 6 dB over the floor and above -60 dBFS. */
    float floor_e = -1.0f;
    for (int i = 0; i < w->frames; i++) {
        if (!ecnt[i]) continue;
        float e = esum[i] / (float)ecnt[i];
        if (floor_e < 0.0f) floor_e = e;
        if (e < floor_e) floor_e += 0.2f * (e - floor_e);
        else             floor_e *= 1.0023f;
        if (floor_e < 1e-9f) floor_e = 1e-9f;
        if (e > floor_e * 4.0f && e > 1e-6f) {
            w->bits[i >> 6] |= 1ULL << (i & 63);
            w->speech_frames++;
        }
    }
    rc = 0;

out:
    av_frame_free(&frm);
    av_packet_free(&pkt);
    free(esum);
    free(ecnt);
    return rc;
}

/* ------------------------------------------------------------------------
 * Correlation
 * ---------------------------------------------------------------------- */

/* Cues rasterised in media time for `scale`, with SS_SEARCH_F + 64 frames
 * of padding in front so a shifted read never goes negative. */
typedef struct {
    uint64_t *bits;
    int64_t   nbits, nwords, origin;
} ss_cuevec;

static int ss_cuevec_build(ss_cuevec *cv, const double *cs, const double *ce,
                           int n, double scale, double media_s)
{
    double last = media_s;
    for (int i = 0; i < n; i++)
        if (ce[i] / scale > last) last = ce[i] / scale;
    cv->origin = SS_SEARCH_F + 64;
    cv->nbits = cv->origin + (int64_t)(last * SS_FPS) + SS_SEARCH_F + 128;
    cv->nwords = (cv->nbits + 63) >> 6;
    cv->bits = (uint64_t *)calloc((size_t)cv->nwords, sizeof(uint64_t));
    if (!cv->bits) return -1;
    for (int i = 0; i < n; i++) {
        if (ce[i] <= cs[i]) continue;
        int64_t a = (int64_t)(cs[i] / scale * SS_FPS) + cv->origin;
        int64_t b = (int64_t)(ce[i] / scale * SS_FPS + 0.999) + cv->origin;
        ss_set_range(cv->bits, cv->nbits, a, b);
    }
    return 0;
}

static int ss_cues_near(const double *cs, int n, double scale,
                        double from_s, double to_s)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        double t = cs[i] / scale;
        if (t >= from_s && t <= to_s) count++;
    }
    return count;
}

/* Adds one window's correlation to acc[] for every shift k (frames; the cue
 * shows k frames after its own time) and returns the window's own best k.
 *
 * The score is the covariance of the two bit vectors,
 *     popcount(A & C) - |A| * |C| / N,
 * rather than raw XNOR agreement. XNOR = N - |A| - |C| + 2|A & C|, and |C|
 * (how many cue frames the shift pulls into the window) swings with k on
 * sparse dialogue, which on a real film buried the true peak. The covariance
 * is the same popcount work with that term normalised out. */
static int ss_score_window(const ss_window *w, const ss_cuevec *cv, double *acc)
{
    const int nk = 2 * SS_SEARCH_F + 1;
    const int64_t base = (int64_t)(w->start_s * SS_FPS + 0.5) + cv->origin;
    const int tail = w->frames & 63;
    const uint64_t last_mask = tail ? ((1ULL << tail) - 1) : ~0ULL;
    const double pa_n = (double)w->speech_frames / (double)w->frames;
    double best = -1e18;
    int best_k = 0;

    for (int ki = 0; ki < nk; ki++) {
        int k = ki - SS_SEARCH_F;
        int64_t pos = base - k;
        int both = 0, cues = 0;
        for (int j = 0; j < w->words; j++) {
            uint64_t cw = ss_get64(cv->bits, cv->nwords, pos + (int64_t)j * 64);
            if (j == w->words - 1) cw &= last_mask;
            both += __builtin_popcountll(w->bits[j] & cw);
            cues += __builtin_popcountll(cw);
        }
        double s = (double)both - pa_n * (double)cues;
        acc[ki] += s;
        if (s > best) { best = s; best_k = k; }
    }
    return best_k;
}

/* How far the curve's peak stands out, in standard deviations of the curve:
 * z against the whole +-60 s, sharp against the best shift > 0.5 s away (a
 * correlation peak is a few hundred ms wide, so its own shoulders are not a
 * rival). */
typedef struct {
    int    peak_k;
    double z, sharp;
} ss_curve;

static ss_curve ss_curve_stats(const double *acc)
{
    const int nk = 2 * SS_SEARCH_F + 1;
    ss_curve r = { 0, 0.0, 0.0 };
    double sum = 0.0, sum2 = 0.0, best = -1e18;
    for (int ki = 0; ki < nk; ki++) {
        sum += acc[ki];
        sum2 += acc[ki] * acc[ki];
        if (acc[ki] > best) { best = acc[ki]; r.peak_k = ki - SS_SEARCH_F; }
    }
    double mean = sum / nk;
    double var = sum2 / nk - mean * mean;
    if (var <= 0.0) return r;
    double sd = sqrt(var);
    double second = -1e18;
    for (int ki = 0; ki < nk; ki++) {
        int d = (ki - SS_SEARCH_F) - r.peak_k;
        if (d < 0) d = -d;
        if (d > SS_EXCL_F && acc[ki] > second) second = acc[ki];
    }
    r.z = (best - mean) / sd;
    r.sharp = (best - second) / sd;
    return r;
}
/* ------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

/* Opens `url` into c: its input, the audio stream (audio_stream, else the
 * best one), the embedded subtitle stream when sub_stream >= 0, and a
 * decoder. ss_close() undoes it. */
static int ss_open(ss_ctx *c, const char *url, int audio_stream, int sub_stream)
{
    const int net = ss_is_http(url);
    c->fmt = avformat_alloc_context();
    if (!c->fmt) return -1;
    c->fmt->interrupt_callback.callback = ss_interrupt;
    c->fmt->interrupt_callback.opaque = (void *)c->cancel;
    AVDictionary *opts = NULL;
    if (net) {
        evo_stream_io_apply_network_options(&opts, url);
        /* This worker's own copy, not what the player holds right now. */
        av_dict_set(&opts, "headers", s_req_headers[0] ? s_req_headers : NULL, 0);
        av_dict_set(&opts, "user_agent", s_req_ua[0] ? s_req_ua : NULL, 0);
        /* Jelly5: a server's audio window starts an encoder before its first byte,
         * and is a stream of unknown length: a reconnect would start it over from
         * the window's start, a second copy of its audio. A dropped window ends
         * where it dropped (its frames so far count). */
        if (c->url_windows) {
            av_dict_set(&opts, "rw_timeout", "20000000", 0);
            av_dict_set(&opts, "reconnect", "0", 0);
            av_dict_set(&opts, "reconnect_streamed", "0", 0);
            av_dict_set(&opts, "reconnect_at_eof", "0", 0);
            av_dict_set(&opts, "reconnect_on_network_error", "0", 0);
        }
    }
    const int open_rc = avformat_open_input(&c->fmt, url, NULL, &opts);
    av_dict_free(&opts);
    if (open_rc < 0) {
        /* (Never the URL: a server's carries the account's token.) */
        evo_log("subsync: open failed (%s)", net ? "network" : "file");
        c->fmt = NULL;      /* freed by avformat_open_input on failure */
        return -1;
    }
    if (avformat_find_stream_info(c->fmt, NULL) < 0) {
        evo_log("subsync: no stream info");
        return -1;
    }

    c->stream = audio_stream;
    if (c->stream < 0 || c->stream >= (int)c->fmt->nb_streams ||
        c->fmt->streams[c->stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        c->stream = av_find_best_stream(c->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (c->stream < 0) {
        evo_log("subsync: no audio stream");
        return -1;
    }
    if (sub_stream >= 0) {
        if (sub_stream >= (int)c->fmt->nb_streams ||
            c->fmt->streams[sub_stream]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            evo_log("subsync: stream %d is not a subtitle track", sub_stream);
            return -1;
        }
        c->sub_stream = sub_stream;
        c->sub_st = c->fmt->streams[sub_stream];
    }
    for (unsigned i = 0; i < c->fmt->nb_streams; i++)
        c->fmt->streams[i]->discard = ((int)i == c->stream || (int)i == c->sub_stream)
                                    ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    c->st = c->fmt->streams[c->stream];

    const AVCodec *codec = avcodec_find_decoder(c->st->codecpar->codec_id);
    c->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!c->dec || avcodec_parameters_to_context(c->dec, c->st->codecpar) < 0) {
        evo_log("subsync: no decoder for codec %d", (int)c->st->codecpar->codec_id);
        return -1;
    }
    c->dec->pkt_timebase = c->st->time_base;
    c->dec->thread_count = 1;          /* stay off the playback cores */
    if (avcodec_open2(c->dec, codec, NULL) < 0) {
        evo_log("subsync: decoder open failed");
        return -1;
    }
    return 0;
}

static void ss_close(ss_ctx *c)
{
    swr_free(&c->swr);
    avcodec_free_context(&c->dec);
    if (c->fmt) avformat_close_input(&c->fmt);
    c->st = NULL;
    c->stream = -1;
}

/* Jelly5: ends the server's encoder for one audio window (stop_url +
 * "&playSessionId=<it>", a DELETE of /Videos/ActiveEncodings): it would
 * otherwise go on through the rest of the film after the window was read.
 * Not interruptible by a cancel (it is part of one); 3 s at most. */
static void ss_stop_encoder(const char *stop_url, const char *session)
{
    if (!stop_url || !stop_url[0] || !session || !session[0]) return;
    char u[sizeof(s_stop_url) + 64];
    snprintf(u, sizeof(u), "%s&playSessionId=%s", stop_url, session);
    AVDictionary *o = NULL;
    av_dict_set(&o, "method", "DELETE", 0);
    av_dict_set(&o, "rw_timeout", "3000000", 0);
    av_dict_set(&o, "timeout", "3000000", 0);
    if (s_req_headers[0]) av_dict_set(&o, "headers", s_req_headers, 0);
    if (s_req_ua[0]) av_dict_set(&o, "user_agent", s_req_ua, 0);
    AVIOContext *pb = NULL;
    const int rc = avio_open2(&pb, u, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (pb) avio_closep(&pb);
    /* (A 204 No Content can read as an error to FFmpeg: the request went out.) */
    evo_log("subsync: stopped the server's encoder (%d)", rc);
}

/* Jelly5: an embedded track's cues from the server's own copy of it (a
 * subtitle file the server cuts from the container), into c->ecs/ece.
 * Returns how many, -1 when it could not be read. */
static int ss_load_cues(ss_ctx *c, const char *url)
{
    AVFormatContext *f = avformat_alloc_context();
    if (!f) return -1;
    f->interrupt_callback.callback = ss_interrupt;
    f->interrupt_callback.opaque = (void *)c->cancel;
    AVDictionary *opts = NULL;
    evo_stream_io_apply_network_options(&opts, url);
    av_dict_set(&opts, "headers", s_req_headers[0] ? s_req_headers : NULL, 0);
    av_dict_set(&opts, "user_agent", s_req_ua[0] ? s_req_ua : NULL, 0);
    av_dict_set(&opts, "rw_timeout", "20000000", 0);   /* the server extracts it first */
    const int rc = avformat_open_input(&f, url, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        evo_log("subsync: the server's subtitle file did not open");
        return -1;
    }
    AVPacket *pkt = av_packet_alloc();
    int n = pkt ? 0 : -1;
    while (pkt && !ss_cancelled(c) && av_read_frame(f, pkt) >= 0) {
        const AVStream *st = f->streams[pkt->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && pkt->pts != AV_NOPTS_VALUE &&
            pkt->duration > 0) {
            const double tb = av_q2d(st->time_base);
            const double t = (double)pkt->pts * tb;
            if (ss_cue_push(c, t, t + (double)pkt->duration * tb) < 0) {
                n = -1;
                av_packet_unref(pkt);
                break;
            }
            n++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&f);
    return n;
}

/* The analysis. media_path is a file (or URL) read window by window; with
 * audio_url (Jelly5) each window is instead fetched from the server as audio
 * only, audio_url + "&startTimeTicks=<window start>", which the server seeks
 * to and returns from 0 - so a window costs a few MB, not a stretch of the
 * whole file. Its cues come from cs/ce or, for an embedded track, cue_url. */
static int ss_run(const char *media_path, const char *audio_url, const char *cue_url,
                  const char *stop_url,
                  double url_duration_s, int audio_stream, int sub_stream,
                  const double *cs, const double *ce, int n,
                  volatile int *cancel, volatile int *progress,
                  evo_subsync_result_t *res)
{
    ss_ctx c;
    ss_window win[SS_WINDOWS];
    double *acc = NULL;
    const double t0 = ss_now_s();
    const int url_mode = audio_url && audio_url[0];
    /* An embedded track's cues (from the file's packets, or the server's file of
     * it): it is muxed with the video, so offset only, no framerate ratio. */
    const int embedded = url_mode ? (cue_url && cue_url[0]) : sub_stream >= 0;
    char session[48] = "";              /* url_mode: the window being read */
    int net = 0;

    memset(&c, 0, sizeof(c));
    memset(win, 0, sizeof(win));
    memset(res, 0, sizeof(*res));
    res->status = EVO_SUBSYNC_ERROR;
    res->scale = 1.0;
    c.cancel = cancel;
    c.progress = progress;
    c.stream = -1;
    c.sub_stream = -1;

    if (url_mode) {
        c.url_windows = 1;
        c.read_cap = SS_NET_READ_CAP_BPS;
        if (cue_url && cue_url[0]) {
            const int got = ss_load_cues(&c, cue_url);
            evo_log("subsync: %d cues from the server's subtitle file", got);
            if (got < 0) goto done;
            cs = c.ecs;
            ce = c.ece;
            n = c.en;
        }
        if (!cs || !ce || n < SS_MIN_CUES) {
            res->status = EVO_SUBSYNC_LOW_CONF;
            goto done;
        }
        c.duration_s = url_duration_s;
        evo_log("subsync: start (audio from the server) dur=%.0fs cues=%d", c.duration_s, n);
    } else {
        if (!media_path ||
            (sub_stream < 0 && (!cs || !ce || n < SS_MIN_CUES))) {
            res->status = EVO_SUBSYNC_LOW_CONF;
            goto done;
        }
        net = ss_is_http(media_path);
        c.read_cap = net ? SS_NET_READ_CAP_BPS : SS_READ_CAP_BPS;
        if (ss_open(&c, media_path, audio_stream, sub_stream) < 0) goto done;
        c.start_offset_s = c.fmt->start_time != AV_NOPTS_VALUE
                         ? (double)c.fmt->start_time / AV_TIME_BASE : 0.0;
        c.duration_s = c.fmt->duration > 0 ? (double)c.fmt->duration / AV_TIME_BASE
                     : (c.sub_stream < 0 ? ce[n - 1] * 1.05 : 0.0);
        evo_log("subsync: start stream=%d codec=%s ch=%d dur=%.0fs cues=%s%d",
                c.stream, c.dec->codec->name, c.st->codecpar->ch_layout.nb_channels,
                c.duration_s, c.sub_stream >= 0 ? "embedded#" : "",
                c.sub_stream >= 0 ? c.sub_stream : n);
    }
    if (c.duration_s < 60.0) {
        evo_log("subsync: %.0fs is too short to analyse", c.duration_s);
        res->status = EVO_SUBSYNC_LOW_CONF;
        goto done;
    }

    /* Windows at 20/50/80%: far apart for drift, clear of opening/closing
     * credits where there is music but no dialogue. */
    /* (A server's audio windows are cheap: the full length EVO calibrated.) */
    double wlen = url_mode ? SS_WINDOW_S : c.sub_stream >= 0 ? SS_WINDOW_EMB_S
                : net ? SS_NET_WINDOW_S : SS_WINDOW_S;
    if (wlen > c.duration_s / 4.0) wlen = c.duration_s / 4.0;
    static const double centres[SS_WINDOWS] = { 0.2, 0.5, 0.8 };
    for (int i = 0; i < SS_WINDOWS; i++) {
        double s = c.duration_s * centres[i] - wlen / 2.0;
        if (s < 0.0) s = 0.0;
        if (s + wlen > c.duration_s) s = c.duration_s - wlen;
        win[i].start_s = s;
        win[i].frames = (int)(wlen * SS_FPS);
        win[i].words = (win[i].frames + 63) >> 6;
        win[i].bits = (uint64_t *)calloc((size_t)win[i].words, sizeof(uint64_t));
        if (!win[i].bits) goto done;
    }

    c.io_t0 = ss_now_s();
    for (int i = 0; i < SS_WINDOWS; i++) {
        double wt = ss_now_s();
        if (url_mode) {
            /* The window from the server: its audio starts at 0, which is
             * win[i].start_s in the film. Each its own play session: Jellyfin
             * (12.2) names an encoder's output by device and session only, and
             * would serve the first window's again for the next. */
            ss_close(&c);
            ss_stop_encoder(stop_url, session);   /* the last window's */
            snprintf(session, sizeof(session), "jelly5sync%llx%d", (unsigned long long)(t0 * 1000.0), i);
            char u[sizeof(s_audio_url) + 96];
            snprintf(u, sizeof(u), "%s&startTimeTicks=%lld&playSessionId=%s", audio_url,
                     (long long)llround(win[i].start_s * 1e7), session);
            if (ss_open(&c, u, -1, -1) < 0) goto done;
            c.start_offset_s = -win[i].start_s;
        }
        if (ss_decode_window(&c, &win[i], i) < 0) goto done;
        if (ss_cancelled(&c)) break;
        win[i].valid = win[i].speech_frames >= (int)(win[i].frames * SS_MIN_SPEECH);
        evo_log("subsync: window %d @%.0fs speech=%.1f%% %.1fs",
                i, win[i].start_s,
                100.0 * win[i].speech_frames / (win[i].frames ? win[i].frames : 1),
                ss_now_s() - wt);
    }
    if (ss_cancelled(&c)) {
        res->status = EVO_SUBSYNC_CANCELLED;
        goto done;
    }

    if (c.sub_stream >= 0) {
        /* The track's own cues, as read around the windows. */
        cs = c.ecs;
        ce = c.ece;
        n = c.en;
        evo_log("subsync: %d embedded cues gathered", n);
        if (n < SS_MIN_CUES) {
            res->status = EVO_SUBSYNC_LOW_CONF;
            goto done;
        }
    }

    acc = (double *)malloc(sizeof(double) * (2 * SS_SEARCH_F + 1));
    if (!acc) goto done;

    /* Offset only for an embedded track: it is muxed with the video, and
     * checking a ratio would mean reading minutes more around each window
     * to reach the cues a 4% drift moves there. */
    const int ratios = embedded ? 0 : SS_RATIO_COUNT;

    /* Each scale's windows are summed into one curve - a constant offset (at
     * the right scale) lines their peaks up, so the sum is ~sqrt(3) cleaner
     * than any one window.
     *
     * Ranking: more windows whose own peak sits on the answer, then z. A
     * ratio must beat 1.0's z by SS_RATIO_GAIN at equal agreement, and two
     * ratios scoring alike (25->23.976 vs 25->24) are split by how tightly
     * their windows agree. A ratio is not tried at all unless it moves the
     * cues >= SS_MIN_DRIFT_S across the analysed span: on a 12 min film
     * 24->23.976 shifts them ~0.4 s, inside the noise, and on Tears of
     * Steel it "won" against an SRT that is in sync. */
    const double span = win[SS_WINDOWS - 1].start_s - win[0].start_s;
    ss_curve best = { 0, 0.0, 0.0 };
    double   best_scale = 1.0, best_spread = 0.0;
    int      best_agree = -1, best_usable = 0;
    for (int ci = -1; ci < ratios && !ss_cancelled(&c); ci++) {
        double scale = ci < 0 ? 1.0 : k_ratios[ci].media_fps / k_ratios[ci].srt_fps;
        if (ci >= 0 && ss_abs(1.0 - scale) * span < SS_MIN_DRIFT_S) {
            evo_log("subsync: scale=%.5f skipped (%.2fs drift over the span)",
                    scale, ss_abs(1.0 - scale) * span);
            continue;
        }
        ss_cuevec cv;
        if (ss_cuevec_build(&cv, cs, ce, n, scale, c.duration_s) < 0) goto done;

        memset(acc, 0, sizeof(double) * (2 * SS_SEARCH_F + 1));
        int wk[SS_WINDOWS], used[SS_WINDOWS], usable = 0;
        for (int i = 0; i < SS_WINDOWS; i++) {
            double ws = win[i].start_s, we = ws + (double)win[i].frames / SS_FPS;
            used[i] = win[i].valid &&
                      ss_cues_near(cs, n, scale, ws - 60.0, we + 60.0) >= SS_MIN_CUES;
            if (!used[i]) continue;
            wk[i] = ss_score_window(&win[i], &cv, acc);
            usable++;
        }
        free(cv.bits);
        if (!usable) continue;

        ss_curve cur = ss_curve_stats(acc);
        int agree = 0, lo = 1 << 30, hi = -(1 << 30);
        for (int i = 0; i < SS_WINDOWS; i++) {
            if (!used[i]) continue;
            int d = wk[i] - cur.peak_k;
            if (d < 0) d = -d;
            if (d <= SS_AGREE_F) agree++;
            if (wk[i] < lo) lo = wk[i];
            if (wk[i] > hi) hi = wk[i];
        }
        double spread = (double)(hi - lo) / SS_FPS;
        evo_log("subsync: scale=%.5f offset=%+.2fs z=%.1f sharp=%.2f windows %d/%d "
                "(%+.2f %+.2f %+.2f)",
                scale, cur.peak_k / (double)SS_FPS, cur.z, cur.sharp, agree, usable,
                used[0] ? wk[0] / (double)SS_FPS : 0.0,
                used[1] ? wk[1] / (double)SS_FPS : 0.0,
                used[2] ? wk[2] / (double)SS_FPS : 0.0);

        int better;
        if (agree != best_agree)
            better = agree > best_agree;
        else if (best_scale == 1.0)
            better = cur.z > best.z * SS_RATIO_GAIN;
        else if (ss_abs(cur.z - best.z) <= 0.05 * best.z)
            better = spread < best_spread;
        else
            better = cur.z > best.z;
        if (better) {
            best = cur;
            best_scale = scale;
            best_agree = agree;
            best_usable = usable;
            best_spread = spread;
        }
        if (progress) *progress = 90 + (ci + 2) * 10 / (ratios + 1);
    }
    if (ss_cancelled(&c)) {
        res->status = EVO_SUBSYNC_CANCELLED;
        goto done;
    }

    /* Two windows must put their own peak on the answer (one, if only one
     * window had dialogue and cues to score). */
    int need_agree = best_usable >= 2 ? 2 : 1;
    if (best_usable && best.z >= SS_MIN_Z && best.sharp >= SS_MIN_SHARP &&
        best_agree >= need_agree) {
        res->status = EVO_SUBSYNC_OK;
        res->scale = best_scale;
        res->delay_s = best_scale * best.peak_k / SS_FPS;
        res->confidence = best.sharp;
        res->windows_used = best_agree;
    } else {
        res->status = EVO_SUBSYNC_LOW_CONF;
        res->confidence = best.sharp;
    }

done:
    free(acc);
    free(c.ecs);
    free(c.ece);
    for (int i = 0; i < SS_WINDOWS; i++) free(win[i].bits);
    free(c.out);
    ss_close(&c);
    ss_stop_encoder(stop_url, session);   /* also after a cancel or a failed window */
    if (cancel && *cancel) res->status = EVO_SUBSYNC_CANCELLED;
    res->elapsed_s = ss_now_s() - t0;
    if (progress) *progress = 100;
    evo_log("subsync: result status=%d offset=%+.3fs scale=%.5f conf=%.3f windows=%d elapsed=%.1fs",
            res->status, res->delay_s, res->scale, res->confidence,
            res->windows_used, res->elapsed_s);
    return res->status;
}

int evo_subsync_analyse(const char *media_path, int audio_stream,
                        int sub_stream,
                        const double *cs, const double *ce, int n,
                        volatile int *cancel, volatile int *progress,
                        evo_subsync_result_t *res)
{
    return ss_run(media_path, NULL, NULL, NULL, 0.0, audio_stream, sub_stream, cs, ce, n,
                  cancel, progress, res);
}

/* ------------------------------------------------------------------------
 * Background worker
 * ---------------------------------------------------------------------- */

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_thread;
static int             s_thread_live;          /* UI thread only */
static volatile int    s_running;
static volatile int    s_cancel;
static volatile int    s_progress;
static int             s_have_result;          /* under s_mx */
static evo_subsync_result_t s_result;          /* under s_mx */

static char    s_path[4096];
static int     s_stream;
static int     s_sub_stream;
static double *s_cs, *s_ce;
static int     s_n;

static void *ss_worker(void *arg)
{
    (void)arg;
    evo_subsync_result_t r;
    if (s_audio_url[0]) {
        ss_run(s_path, s_audio_url, s_cue_url, s_stop_url, s_url_duration_s, s_stream, s_sub_stream,
               s_cs, s_ce, s_n, &s_cancel, &s_progress, &r);
        /* The server would not give the audio: the file itself, where the
         * caller allowed it (a small one). */
        if (r.status == EVO_SUBSYNC_ERROR && s_static_fallback && !s_cancel) {
            evo_log("subsync: no audio from the server, reading the file");
            evo_subsync_analyse(s_path, s_stream, s_sub_stream, s_cs, s_ce, s_n,
                                &s_cancel, &s_progress, &r);
        }
    } else {
        evo_subsync_analyse(s_path, s_stream, s_sub_stream, s_cs, s_ce, s_n,
                            &s_cancel, &s_progress, &r);
    }
    pthread_mutex_lock(&s_mx);
    if (!s_cancel) {
        s_result = r;
        s_have_result = 1;
    }
    pthread_mutex_unlock(&s_mx);
    s_running = 0;
    return NULL;
}

static void ss_reap(void)
{
    if (!s_thread_live) return;
    pthread_join(s_thread, NULL);
    s_thread_live = 0;
    free(s_cs); s_cs = NULL;
    free(s_ce); s_ce = NULL;
    s_n = 0;
}

int evo_subsync_start(const char *media_path, int audio_stream,
                      int sub_stream,
                      const double *cue_start, const double *cue_end,
                      int cue_count)
{
    if (s_running) return 0;
    ss_reap();
    if (!evo_subsync_path_supported(media_path) || strlen(media_path) >= sizeof(s_path))
        return 0;

    if (sub_stream < 0) {
        if (!cue_start || !cue_end || cue_count < SS_MIN_CUES)
            return 0;
        s_cs = (double *)malloc(sizeof(double) * (size_t)cue_count);
        s_ce = (double *)malloc(sizeof(double) * (size_t)cue_count);
        if (!s_cs || !s_ce) {
            free(s_cs); free(s_ce);
            s_cs = s_ce = NULL;
            return 0;
        }
        memcpy(s_cs, cue_start, sizeof(double) * (size_t)cue_count);
        memcpy(s_ce, cue_end, sizeof(double) * (size_t)cue_count);
        s_n = cue_count;
    }
    snprintf(s_path, sizeof(s_path), "%s", media_path);
    s_stream = audio_stream;
    s_sub_stream = sub_stream;

    pthread_mutex_lock(&s_mx);
    s_have_result = 0;
    pthread_mutex_unlock(&s_mx);
    s_cancel = 0;
    s_progress = 0;
    s_running = 1;
    if (evo_thread_create(&s_thread, ss_worker, NULL) != 0) {   /* FFmpeg wants a big stack */
        s_running = 0;
        free(s_cs); free(s_ce);
        s_cs = s_ce = NULL;
        s_n = 0;
        evo_log("subsync: worker thread create failed");
        return 0;
    }
    s_thread_live = 1;
    return 1;
}

void evo_subsync_cancel(void)
{
    if (s_thread_live) {
        s_cancel = 1;
        ss_reap();
    }
    pthread_mutex_lock(&s_mx);
    s_have_result = 0;
    pthread_mutex_unlock(&s_mx);
    s_running = 0;
}

int evo_subsync_running(void) { return s_running; }

void evo_subsync_set_server_source(const char *audio_url, const char *cue_url,
                                   const char *stop_url,
                                   double duration_s, int static_fallback)
{
    if (s_running) return;
    snprintf(s_stop_url, sizeof(s_stop_url), "%s", stop_url ? stop_url : "");
    snprintf(s_audio_url, sizeof(s_audio_url), "%s", audio_url ? audio_url : "");
    snprintf(s_cue_url, sizeof(s_cue_url), "%s", cue_url ? cue_url : "");
    s_url_duration_s = duration_s;
    s_static_fallback = static_fallback;
}

void evo_subsync_set_request_headers(const char *headers, const char *user_agent)
{
    if (s_running) return;
    snprintf(s_req_headers, sizeof(s_req_headers), "%s", headers ? headers : "");
    snprintf(s_req_ua, sizeof(s_req_ua), "%s", user_agent ? user_agent : "");
}

int evo_subsync_progress(void) { return s_progress; }

int evo_subsync_take_result(evo_subsync_result_t *out)
{
    int got = 0;
    pthread_mutex_lock(&s_mx);
    if (s_have_result) {
        *out = s_result;
        s_have_result = 0;
        got = 1;
    }
    pthread_mutex_unlock(&s_mx);
    if (got && !s_running) ss_reap();
    return got;
}
