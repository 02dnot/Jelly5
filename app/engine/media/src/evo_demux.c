/*
 * evo_demux.c — the demux thread + in-place seek path.
 *
 * Verbatim move of the PROSPERO_TRUE_AV_SEEK region, the two PacketQueue
 * instances and the stream indices from main.c (Track A step A5 of
 * docs/modularisation-plan.md). The only edits are `static` -> external
 * linkage on what main.c still touches and the transitional extern block
 * below.
 */
#include "jelly5_bitstream.h"
#include "evo_demux.h"
#include "evo_thread.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include "pp_playback.h"
#include "evo_packet_queue.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_subtitle.h"
#ifdef NUVIO_APP
#include "nuvio_subs.h"
#endif
#include "evo_vdec.h"
#include "evo_adec.h"
#include "pp_stage_breadcrumb.h"
#include "evo_boot_trace.h"
#include "evo_stream_io.h"

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core decode context + flags + the app playback
 * object, all still owned by main.c. Replaced by the evo_pb_*() façade and a
 * passed-in pp_playback* at A7/A8.
 * ------------------------------------------------------------------------ */
extern AVFormatContext *play_fmt;
extern AVCodecContext  *audio_ctx;
extern evo_vdec        *g_vdec;   /* owns the video codec context (A6) */

extern int      player_paused;
extern char     current_media_path[512];
extern double   media_duration_sec;
extern double   resume_base_offset_seconds;
extern volatile double resume_base_anchor_pending;
extern long long controls_last_used_ms;

extern int      video_decode_done;
extern int      video_decode_ready;
extern int      dbg_read_fail;
extern int      dbg_video_packets;
extern double   first_video_pts_seconds;
extern double   video_clock_seconds;

extern AVPacket *video_pending_pkt;
extern AVPacket *video_video_pending_pkt;
extern volatile int video_thread_running;  /* evo_playback.c */
extern volatile int video_decode_parked;
extern volatile int video_decode_hold;

extern int      playback_profile;
extern int      video_packet_cap;
extern int      audio_packet_cap;

/* Start-of-stream pre-buffer. Armed by PlaybackController for a network
 * source, cleared here - see the note in Bridge.cpp. */
extern volatile int pb_prebuffer_hold;
extern int          pb_prebuffer_packets;
extern int          pb_prebuffer_max_ms;

extern pp_playback g_pp_pb;

long long now_ms(void);
void      toast(const char *title, const char *msg);

/* ---------------------------------------------------------------------------
 * Demux state (exported via evo_demux.h).
 * ------------------------------------------------------------------------ */
PacketQueue video_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };
PacketQueue audio_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };

int video_stream_index = -1;
int audio_stream_index = -1;

volatile int demux_thread_running = 0;
/* What the demux thread is doing, for the status line: 1 reading, 2 waiting
 * for video room, 3 waiting for audio room, 4 seeking, 5 read failed,
 * 6 recovering from a failed read. */
volatile int evo_demux_state = 0;
pthread_t    demux_thread;


static pthread_mutex_t prospero_seek_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static volatile int prospero_seek_pending = 0;
static volatile int prospero_seek_in_progress = 0;

static double prospero_seek_target_seconds = 0.0;
static int prospero_seek_restore_paused = 0;
static volatile long long s_seek_done_ms = 0;

/* A seek is queued, running, or finished less than a moment ago - the video
 * queue is empty on purpose then, not starved. */
int evo_demux_seek_busy(void)
{
    return prospero_seek_pending || prospero_seek_in_progress ||
           now_ms() - s_seek_done_ms < 1500;
}


int prospero_request_inplace_seek(
    double target_seconds,
    int restore_paused
) {
#ifdef NUVIO_APP
    /* Nuvio streams are provider sources, which keep current_media_path empty
     * (no sidecar scan, no favourites); they seek like any open file. */
    if (!play_fmt || (video_stream_index < 0 && audio_stream_index < 0))
        return 0;
#else
    if (
        !play_fmt ||
        (video_stream_index < 0 && audio_stream_index < 0) ||
        !current_media_path[0]
    ) {
        return 0;
    }
#endif

    if (target_seconds < 0.0) {
        target_seconds = 0.0;
    }

    if (media_duration_sec > 1.0) {
        double maximum =
            media_duration_sec - 1.0;

        if (maximum < 0.0) {
            maximum = 0.0;
        }

        if (target_seconds > maximum) {
            target_seconds = maximum;
        }
    }

    player_paused = 1;
    controls_last_used_ms = now_ms();

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    prospero_seek_target_seconds =
        target_seconds;

    prospero_seek_restore_paused =
        restore_paused;

    prospero_seek_pending = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    return 1;
}




static int prospero_process_seek_request(void) {
    double target_seconds;
    int restore_paused;

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    if (!prospero_seek_pending) {
        pthread_mutex_unlock(
            &prospero_seek_mutex
        );

        return 0;
    }

    target_seconds =
        prospero_seek_target_seconds;

    restore_paused =
        prospero_seek_restore_paused;

    prospero_seek_pending = 0;
    prospero_seek_in_progress = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    pp_playback_notify_seek_begin(
        &g_pp_pb,
        (int64_t)(target_seconds * 1000000.0)
    );

    /*
     * Wait for the video decode thread to be out of the decoder before
     * anything below touches it: evo_vdec_flush() and the pending-packet free
     * both race a decode call still in flight. The old fixed 5 ms was enough
     * while a decode call was short; a 4K AV1 frame in dav1d is not, and a
     * .mkv seek (av_seek_frame ~0 ms) flushed dav1d under a live
     * dav1d_send_data - SIGSEGV in dav1d_parse_obus (#94, hardware
     * 2026-09-26). Bounded, so a wedged decode (#39) cannot hang the seek.
     *
     * video_decode_hold, not player_paused: a committed scrub requests the
     * seek and then moves the FSM straight to Playing, whose entry clears
     * player_paused before this thread even gets here - so the decode thread
     * never parked and every seek sat out the full 2 s timeout, then flushed
     * unsynchronized anyway. The hold belongs to the seek alone.
     */
    video_decode_hold = 1;
    usleep(5000);
    if (video_thread_running) {
        int waited_ms = 0;
        while (!video_decode_parked && waited_ms < 2000) {
            usleep(1000);
            waited_ms++;
        }
        if (!video_decode_parked || waited_ms > 50) {
            char d[64];
            snprintf(d, sizeof d, "parked=%d waited_ms=%d",
                     (int)video_decode_parked, waited_ms);
            pp_stage_bc("SEEK_PARK", d);
        }
    }

    
    /*
     * Clear EOF immediately. This wakes the video and audio decoder
     * loops while the seek and queue reset are being completed.
     */
    video_decode_done = 0;
    video_decode_ready = 1;
    dbg_read_fail = 0;

packet_queue_clear(
        &video_packet_queue
    );

    packet_queue_clear(
        &audio_packet_queue
    );

    if (video_pending_pkt) {
        av_packet_free(
            &video_pending_pkt
        );

        video_pending_pkt = NULL;
    }

    if (video_video_pending_pkt) {
        av_packet_free(
            &video_video_pending_pkt
        );

        video_video_pending_pkt = NULL;
    }

    audio_queue_count = 0;
    evo_audio_flush_speed();   /* Jelly5: the stretcher's leftovers too */
    jelly5_bs_reset();         /* Jelly5: and an HDMI bitstream's buffered bursts */
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    double decoder_seek_seconds =
        target_seconds;

    /*
     * No extra backstep. AVSEEK_FLAG_BACKWARD already lands on the keyframe at
     * or before this timestamp, which is exactly what an inter-frame codec
     * needs to restart. The 0.5 s that used to be subtracted here only widened
     * the run-up the decoder then has to chew through and throw away - and
     * when the target sat just after a keyframe it pushed the seek back a
     * whole extra GOP, which is what made a longer seek hitch harder than a
     * short one.
     */

    int seek_stream =
        video_stream_index >= 0
            ? video_stream_index
            : audio_stream_index;

    AVRational time_base =
        play_fmt->streams[
            seek_stream
        ]->time_base;

    int64_t seek_timestamp =
        (int64_t)(
            decoder_seek_seconds /
            av_q2d(time_base)
        );
#ifdef NUVIO_APP
    /* Jelly5: an HLS transcode's timestamps need not start at 0 (Emby's start at
     * 10 s): the hls demuxer seeks by timestamp, so the target is counted from
     * the stream's start. Without it a seek landed 10 s early, and one to the
     * first 10 s was refused (2026-10-07). */
    if (play_fmt->iformat && strcmp(play_fmt->iformat->name, "hls") == 0 &&
        play_fmt->streams[seek_stream]->start_time != AV_NOPTS_VALUE &&
        play_fmt->streams[seek_stream]->start_time > 0)
        seek_timestamp += play_fmt->streams[seek_stream]->start_time;
#endif

    /* #94: a seek in a raw .obu (no index - the demuxer scans forward from
     * the last keyframe it has seen) took EVO down with nothing after it in
     * evo.log. This line and the ms= on SEEK_AVFRAME bracket the call. */
    {
        char d[112];
        snprintf(d, sizeof d, "fmt=%s ts=%lld target=%.3f",
                 play_fmt->iformat ? play_fmt->iformat->name : "?",
                 (long long)seek_timestamp, target_seconds);
        pp_stage_bc("SEEK_BEGIN", d);
    }
    const int64_t seek_t0 = av_gettime_relative();

    int result =
        av_seek_frame(
            play_fmt,
            seek_stream,
            seek_timestamp,
            AVSEEK_FLAG_BACKWARD
        );

    /* Big files (14 GB GTA trailer) whose stream index doesn't cover the byte
     * range can fail the timestamp seek; fall back to a byte seek. */
    if (result < 0) {
        result = av_seek_frame(play_fmt, seek_stream, seek_timestamp,
                               AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    }
    {
        char d[112];
        snprintf(d, sizeof d, "rc=%d ts=%lld strm=%d target=%.3f ms=%lld",
                 result, (long long)seek_timestamp, seek_stream, target_seconds,
                 (long long)((av_gettime_relative() - seek_t0) / 1000));
        pp_stage_bc("SEEK_AVFRAME", d);   /* #32 diagnostics -> /mnt/usb0/evo.log */
    }

    if (result >= 0) {
        /*
         * av_seek_frame() already flushes the demuxer before it repositions,
         * then sets the stream's running dts to the timestamp it landed on.
         * Flushing again here resets that dts. A container stamps its own
         * packets so it never mattered - but a stream with no timestamps (raw
         * .obu, AVFMT_NOTIMESTAMPS) has only that dts, and after the extra
         * flush restarted at 0: every frame then read as before the target
         * and the whole seek was decoded and thrown away in the dark (#94,
         * Chimera: a jump to 118 s discarded 1005 frames and never played).
         */
        if (!(play_fmt->iformat->flags & AVFMT_NOTIMESTAMPS))
            avformat_flush(play_fmt);

        evo_vdec_flush(g_vdec);   /* video codec + scratch frame/packet (A6) */
        if (g_adec) {
            evo_adec_flush(g_adec);
        }

        if (audio_ctx) {
            avcodec_flush_buffers(
                audio_ctx
            );
        }

        prospero_audio_resampler_reset();

        if (
            prospero_embedded_subtitle_ctx
        ) {
            avcodec_flush_buffers(
                prospero_embedded_subtitle_ctx
            );
        }

        prospero_embedded_subtitle_reset();
#ifdef NUVIO_APP
        nuvio_subs_on_seek();
#endif

        /*
         * The UI position is base offset plus the new audio clock.
         */
        resume_base_offset_seconds =
            target_seconds;
        resume_base_anchor_pending = -1.0;

        /*
         * Arm the audio discard window before the decode threads are let go,
         * so the run-up between the keyframe this seek landed on and the
         * target is dropped on the audio side too. Both clocks then restart
         * from the target and the picture resumes without waiting for audio.
         */
        if (target_seconds > 0.05)
            audio_seek_discard_until = target_seconds;
        else
            audio_seek_discard_until = -1.0;

        audio_samples_played = 0;
        audio_samples_decoded = 0;

        audio_clock_seconds = 0.0;
        audio_pts_seconds = 0.0;
        video_clock_seconds = 0.0;

        first_audio_pts_seconds = -1.0;
        first_video_pts_seconds = -1.0;

        video_decode_done = 0;
        dbg_read_fail = 0;

        /*
         * Older builds allowed the audio decoder thread to exit at
         * EOF. Restart it if this session reached EOF before seeking.
         */
        if (
            audio_ctx &&
            !audio_decode_thread_running
        ) {
            audio_decode_thread_running = 1;

            evo_thread_create(
                &audio_decode_thread,
                audio_decode_thread_func,
                NULL
            );
        }

    } else {
        audio_seek_discard_until = -1.0;
        toast(
            "SEEK",
            "Decoder seek failed"
        );
    }

    prospero_seek_in_progress = 0;
    s_seek_done_ms = now_ms();
    video_decode_hold = 0;

    pp_playback_notify_seek_end(
        &g_pp_pb,
        result >= 0,
        0,
        0
    );
    /* notify_seek_begin() paused the clock. Always lift that if we were
     * playing — on a FAILED seek notify_seek_end() only clears seek_discarding
     * and leaves the clock paused, which drops every frame -> frozen picture. */
    if (!restore_paused)
        pp_playback_resume(&g_pp_pb);

    player_paused =
        restore_paused ? 1 : 0;

    controls_last_used_ms =
        now_ms();

    return result >= 0;
}


/* ---------------------------------------------------------------------------
 * Read-ahead budget.
 *
 * A queue is full by the playback time it holds, not by its packet count.
 * Packet rates run from 24/s (film video) to ~250/s (TrueHD in Matroska), so
 * any one count cap suits one stream and throttles the other: EVO's 96/96
 * held a TrueHD remux to under two seconds of video read-ahead, because the
 * audio queue capped out first and av_read_frame() hands packets back in
 * interleave order. An internet stream with two seconds of cushion stalls on
 * every dip in throughput.
 *
 * Bytes bound it as well, for the stream whose bitrate makes thirty seconds
 * expensive (a 4K remux peaks near 100 Mbit/s). The packets themselves come
 * from direct memory - see the malloc shim's DIRECT_FIRST_MIN in
 * scripts/build.sh - so the flexible pool the decoders live in is not what
 * pays for it.
 * ------------------------------------------------------------------------ */
#define READAHEAD_NET_US     (30LL * 1000000)  /* internet / LAN stream */
#define READAHEAD_LOCAL_US   (8LL * 1000000)   /* file: reads far faster than real time */
#define VIDEO_RING_BYTES     ((size_t)512 << 20)
#define AUDIO_RING_BYTES     ((size_t)96 << 20)
#define VIDEO_BYTES_CAP      (384LL << 20)
#define AUDIO_BYTES_CAP      (64LL << 20)
/* No ring (direct memory refused): the packets stay in the demuxer's own
 * buffers, in flexible memory, which the decoders need most of. */
#define NO_RING_BYTES_CAP    (48LL << 20)
#define QUEUE_FLOOR_PACKETS  16                /* always room for this many */

/* Pre-buffer target at open: enough to absorb the first second of TCP ramp. */
#define PREBUFFER_OPEN_US    (2LL * 1000000)

static int64_t s_readahead_us = READAHEAD_LOCAL_US;
static int64_t s_video_bytes_cap = NO_RING_BYTES_CAP;
static int64_t s_audio_bytes_cap = NO_RING_BYTES_CAP;

/*
 * Low-water mark on the OTHER stream's queue. Below this, that decoder is
 * within a fraction of a second of running dry and the demux thread must not
 * stay parked on a full queue - see demux_wait_for_room().
 */
#define DEMUX_STARVE_LOW_PACKETS 12

/* What the current pre-buffer hold is waiting for. Written before the hold is
 * raised (evo_demux_rebuffer), read by the demux thread. */
static volatile long long s_prebuffer_deadline_ms = 0;
static volatile int64_t   s_prebuffer_target_us   = PREBUFFER_OPEN_US;

/*
 * At or over its budget. `scale` > 1 is the bounded overshoot allowed while
 * the other stream starves (demux_wait_for_room). Durations come from the
 * demuxer; a stream that never supplies them falls back to the packet caps.
 */
static int queue_full(PacketQueue *q, int is_video, int scale)
{
    int     n     = 0;
    int64_t dur   = 0;
    int64_t bytes = 0;
    packet_queue_level(q, &n, &dur, &bytes);

    if (n >= PACKET_QUEUE_SIZE - 1)
        return 1;
    /* Bytes overshoot by a quarter at most: the ring has that and no more. */
    const int64_t bcap = is_video ? s_video_bytes_cap : s_audio_bytes_cap;
    if (bytes >= bcap + (scale > 1 ? bcap / 4 : 0))
        return 1;
    if (n < QUEUE_FLOOR_PACKETS)
        return 0;
    if (dur > 0)
        return dur >= s_readahead_us * scale;
    return n >= (is_video ? video_packet_cap : audio_packet_cap) * scale;
}

/* Under half a second left, or nearly no packets: that decoder is about to
 * run dry and only the demux thread can feed it. */
static int queue_starving(PacketQueue *q)
{
    int     n   = 0;
    int64_t dur = 0;
    packet_queue_level(q, &n, &dur, NULL);
    return n < DEMUX_STARVE_LOW_PACKETS || (dur > 0 && dur < 500000);
}

/* Playback time a packet carries, in microseconds. The container's duration
 * when it has one; otherwise the stream's average packet spacing, measured
 * from successive timestamps (TrueHD in Matroska carries no durations, and
 * laced frames carry no timestamps either), with the frame rate as video's
 * first guess. 0 means unknown yet and only the byte cap applies. */
static int64_t packet_dur_us(const AVPacket *pkt)
{
    static struct { int64_t last_pts; int since; int64_t avg_us; } s_rate[2] = {
        { AV_NOPTS_VALUE, 0, 0 }, { AV_NOPTS_VALUE, 0, 0 } };
    const int k = (pkt->stream_index == video_stream_index) ? 0 : 1;
    AVStream *st = play_fmt->streams[pkt->stream_index];

    if (pkt->pts != AV_NOPTS_VALUE) {
        if (s_rate[k].last_pts != AV_NOPTS_VALUE && s_rate[k].since > 0) {
            int64_t d = av_rescale_q(pkt->pts - s_rate[k].last_pts, st->time_base,
                                     AV_TIME_BASE_Q);
            /* A seek or a discontinuity jumps; only steady spacing counts. */
            if (d > 0 && d < 2000000)
                s_rate[k].avg_us = d / s_rate[k].since;
        }
        s_rate[k].last_pts = pkt->pts;
        s_rate[k].since = 0;
    }
    s_rate[k].since++;

    if (pkt->duration > 0) {
        int64_t us = av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q);
        if (us > 0 && us < 10 * 1000000)
            return us;
    }
    if (k == 0) {
        AVRational r = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        if (r.num > 0 && r.den > 0)
            return av_rescale(1000000, r.den, r.num);
    }
    return s_rate[k].avg_us;
}

/*
 * Release the pre-buffer hold once the queue has a cushion, the deadline has
 * passed, or the stream ended. Called from the demux loop after each packet.
 * `ended` is set on a read failure, where waiting for depth that will never
 * arrive would park the decode threads for the whole deadline.
 */
static void prebuffer_check(int ended)
{
    if (!pb_prebuffer_hold)
        return;

    /* An audio-only stream never fills the video queue, so measure whichever
     * queue this stream actually feeds. */
    const int have_video = (video_stream_index >= 0);
    PacketQueue *q = have_video ? &video_packet_queue : &audio_packet_queue;
    int     depth = 0;
    int64_t held  = 0;
    packet_queue_level(q, &depth, &held, NULL);

    const int filled = (held > 0) ? (held >= s_prebuffer_target_us)
                                  : (depth >= pb_prebuffer_packets);

    /*
     * A queue at its budget is as much cushion as this stream will ever get,
     * so release on that too rather than sitting out the deadline.
     */
    const int any_full = queue_full(&video_packet_queue, 1, 1) ||
                         queue_full(&audio_packet_queue, 0, 1);

    const int timed_out = (now_ms() >= s_prebuffer_deadline_ms);
    if (!ended && !timed_out && !any_full && !filled)
        return;

    pb_prebuffer_hold = 0;
    {
        char d[80];
        snprintf(d, sizeof d, "%s packets=%d held_ms=%lld target_ms=%lld",
                 ended     ? "ended"
                 : timed_out ? "deadline"
                 : any_full  ? "queue-full"
                             : "filled",
                 depth, (long long)(held / 1000),
                 (long long)(s_prebuffer_target_us / 1000));
        pp_stage_bc("P8_03_PREBUFFER_DONE", d);
    }
}

/* Re-arm the pre-buffer mid-play: the decoders park, the clocks hold, and the
 * demux thread refills `target_us` of video (or gives up at `max_ms`) before
 * playback carries on - one clean pause instead of a stutter per frame. */
void evo_demux_rebuffer(int64_t target_us, int max_ms)
{
    if (target_us > s_readahead_us - 1000000)
        target_us = s_readahead_us - 1000000;
    s_prebuffer_target_us   = target_us;
    s_prebuffer_deadline_ms = now_ms() + max_ms;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pb_prebuffer_hold = 1;
    {
        char d[48];
        snprintf(d, sizeof d, "target_ms=%lld", (long long)(target_us / 1000));
        pp_stage_bc("P8_03_REBUFFER", d);
    }
}

/* 0..1 toward the pre-buffer target, for the loading screen. */
float evo_demux_prebuffer_progress(void)
{
    PacketQueue *q = (video_stream_index >= 0) ? &video_packet_queue
                                               : &audio_packet_queue;
    int     n    = 0;
    int64_t held = 0;
    packet_queue_level(q, &n, &held, NULL);
    float p = (held > 0 && s_prebuffer_target_us > 0)
                  ? (float)held / (float)s_prebuffer_target_us
                  : (pb_prebuffer_packets > 0 ? (float)n / (float)pb_prebuffer_packets : 0.0f);
    return p > 1.0f ? 1.0f : p;
}

/* Seconds of playback held in the video (or, audio-only, audio) queue. */
double evo_demux_buffered_s(void)
{
    PacketQueue *q = (video_stream_index >= 0) ? &video_packet_queue
                                               : &audio_packet_queue;
    int64_t held = 0;
    packet_queue_level(q, NULL, &held, NULL);
    return (double)held / 1000000.0;
}

/*
 * Hard ceiling on how far a queue may overshoot its budget while the other
 * stream is starving. The budget is a memory guard, not a correctness
 * invariant, and PACKET_QUEUE_SIZE is the real limit - packet_queue_push()
 * simply returns 0 there, so overshooting can never corrupt the ring.
 */
#define DEMUX_OVERSHOOT_FACTOR 2

/*
 * Wait for room in `q`, but never at the cost of starving the other stream.
 *
 * av_read_frame() hands packets back in interleave order, so parking here on a
 * full queue also stops the OTHER stream's packets arriving. That is one leg of
 * a four-way deadlock hit on hardware after seeking into 4K HEVC + E-AC-3
 * (2026-09-28, Avatar UHD remux):
 *
 *   audio output parks because the audio clock is >0.5 s ahead of video
 *     -> the decoded-PCM ring stays above its high-water mark
 *     -> the audio decode thread stops popping packets
 *     -> the audio packet queue caps out
 *     -> the demux thread parks HERE
 *     -> the video packet queue drains to empty
 *     -> video stops decoding, so video_clock_seconds stops advancing
 *     -> audio output's "ahead of video" test is now permanently true.
 *
 * Releasing as soon as the other queue runs dry cuts that cycle: the queue in
 * hand overshoots its budget by a bounded amount instead, which costs memory
 * and keeps both decoders fed.
 */
static void demux_wait_for_room(PacketQueue *q, int is_video, PacketQueue *other,
                                int sleep_us)
{
    /* Not released by a pause: the packet in hand would be dropped, and a
     * lost reference frame smears the picture until the next keyframe. */
    while (demux_thread_running &&
           queue_full(q, is_video, 1)) {
        evo_demux_state = is_video ? 2 : 3;
        /* Still re-check the pre-buffer here: this loop does not return to the
         * top of the demux loop, so it is the only place the deadline can fire
         * once a queue is full. */
        prebuffer_check(0);

        /* A seek is waiting: return so the loop can run it. */
        if (prospero_seek_pending)
            return;

        if (other && queue_starving(other) && !queue_full(q, is_video, DEMUX_OVERSHOOT_FACTOR)) {
            /* Rate-limited: one line per 2 s is enough to tell a starved
             * interleave apart from a healthy one in evo.log. */
            static long long s_last_bc_ms = 0;
            long long now = now_ms();
            if (now - s_last_bc_ms >= 2000) {
                char d[80];
                snprintf(d, sizeof d, "q=%d other=%d video=%d",
                         packet_queue_count(q), packet_queue_count(other), is_video);
                pp_stage_bc("DEMUX_OVERSHOOT", d);
                s_last_bc_ms = now;
            }
            return;
        }

        usleep(sleep_us);
    }
}

/* ---------------------------------------------------------------------------
 * Network read recovery (from EVO Player 4b2dbde, reworked for Jelly5).
 *
 * A failed read used to be the end of the file: the AVIOContext latches its
 * error, every later av_read_frame() fails, and video stops while the queues
 * drain. Only then did the player notice the early end and reopen the stream
 * (nuvio_player.cpp, connection_dropped): a freeze, a spinner and a fresh
 * open, though the network was usually back within seconds and thirty
 * seconds of read-ahead were still queued. EVO hit it on hardware 2026-10-07:
 * a Jellyfin stream lost one chunk and never showed another frame.
 *
 * So on a seekable network file, a read error short of the real end clears
 * the latch, seeks back to just before the last packets demuxed, and drops
 * what it has already queued until each stream is past that point again. The viewer sees
 * nothing while the queue lasts. After DEMUX_RECOVER_TRIES failures in a row
 * it ends as before, and the player's reopen takes over.
 *
 * Per stream, not just audio and video as in EVO: an embedded subtitle read
 * again would show twice, and one dropped while the video catches up would
 * not show at all. "Already have" is by file position first: Matroska video
 * has no DTS of its own (libavformat guesses one from the PTS, and has none
 * for the first frames after a seek), and the later frames of a laced block
 * carry no timestamp at all. Packets at the same position (laces, or frames
 * a parser split from one PES) are told apart by timestamp.
 * ------------------------------------------------------------------------ */
#define DEMUX_RECOVER_TRIES       10
#define DEMUX_RECOVER_MAX_STREAMS 64

static int     s_recover_allowed;        /* this source may recover (set per file) */
static int     s_recover_tries;          /* failures since the last new anchor packet */
static int     s_recover_gave_up;        /* logged once per outage */
static int     s_recover_catching;       /* some stream is dropping what it has already */
static int64_t s_last_ts[DEMUX_RECOVER_MAX_STREAMS];   /* last timestamp handed on */
static int64_t s_last_pos[DEMUX_RECOVER_MAX_STREAMS];  /* and file position, -1 unknown */
static int64_t s_seek_ts[DEMUX_RECOVER_MAX_STREAMS];   /* last real timestamp, to seek back to */
static uint8_t s_catching[DEMUX_RECOVER_MAX_STREAMS];  /* dropping up to s_last_ts */

static int64_t pkt_ts(const AVPacket *pkt)
{
    return pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
}

/* The stream the recovery seeks by: video, or audio for music. */
static int recover_anchor(void)
{
    const int a = video_stream_index >= 0 ? video_stream_index : audio_stream_index;
    return a >= 0 && a < DEMUX_RECOVER_MAX_STREAMS ? a : -1;
}

static void demux_recover_reset(void)
{
    for (int i = 0; i < DEMUX_RECOVER_MAX_STREAMS; i++) {
        s_last_ts[i] = AV_NOPTS_VALUE;
        s_last_pos[i] = -1;
        s_seek_ts[i] = AV_NOPTS_VALUE;
        s_catching[i] = 0;
    }
    s_recover_tries = 0;
    s_recover_gave_up = 0;
    s_recover_catching = 0;
}

/*
 * Only a seekable network file: a local file's error is the disc's, an HLS
 * or DASH playlist reconnects per segment inside its own demuxer, and a
 * stream that cannot seek (a progressive transcode, a live channel) cannot
 * go back to where it broke. A Live TV channel played directly is seekable
 * http too, but has no past to go back to: its http layer reconnects at EOF
 * and the player joins it again where it airs (evo_stream_io.c).
 */
#ifdef NUVIO_APP
extern int nuvio_stream_live;   /* evo_stream_io.c: the player opened a Live TV channel */
#endif

static int demux_recover_allowed_for(const AVFormatContext *fmt)
{
    if (!fmt || !fmt->pb || !fmt->url)
        return 0;
    const char *u = fmt->url;
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0)
        return 0;
    if (evo_stream_io_url_is_playlist(u))
        return 0;
    if (strstr(u, "liveStreamId=") || strstr(u, "LiveStreamId="))
        return 0;
#ifdef NUVIO_APP
    if (nuvio_stream_live)
        return 0;
#endif
    if (fmt->iformat && (strcmp(fmt->iformat->name, "hls") == 0 ||
                         strcmp(fmt->iformat->name, "dash") == 0))
        return 0;
    return (fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
}

/* 1 when the read ended the stream for good (or recovery is not ours to try). */
static int demux_read_is_final(int read_result)
{
    const int a = recover_anchor();
    if (!s_recover_allowed || !demux_thread_running || !play_fmt || !play_fmt->pb ||
        a < 0 || s_seek_ts[a] == AV_NOPTS_VALUE)
        return 1;
    /* Stop (evo_stream_io_abort) or a deadline: not the network's doing. */
    if (read_result == AVERROR_EXIT)
        return 1;
    if (read_result == AVERROR_EOF) {
        /* A clean end of the file: the http layer reports a connection that
         * closed early as an error (and has already tried to reconnect). */
        if (!play_fmt->pb->error)
            return 1;
        if (media_duration_sec > 0.0) {
            const AVStream *st = play_fmt->streams[a];
            const double start = st->start_time != AV_NOPTS_VALUE
                                     ? st->start_time * av_q2d(st->time_base) : 0.0;
            if (s_seek_ts[a] * av_q2d(st->time_base) - start >= media_duration_sec - 3.0)
                return 1;
        }
    }
    return s_recover_tries >= DEMUX_RECOVER_TRIES;
}

/*
 * Where to go back to: a second before the earlier of the anchor's and the
 * audio's last packet. Not the anchor's own timestamp: a parser holds the
 * frame it is assembling until the next one starts (AC-3 and H.264 in
 * MPEG-TS), the seek flushes it, and in MPEG-TS the audio of a moment may sit
 * before the video's keyframe in the file. Going back further only costs a
 * re-read: what was already handed on is dropped by position and timestamp.
 */
static int64_t recover_seek_target(int a)
{
    const AVRational tb = play_fmt->streams[a]->time_base;
    int64_t t = s_seek_ts[a];
    const int au = audio_stream_index;
    if (au >= 0 && au != a && au < DEMUX_RECOVER_MAX_STREAMS && s_seek_ts[au] != AV_NOPTS_VALUE) {
        const int64_t ta = av_rescale_q(s_seek_ts[au], play_fmt->streams[au]->time_base, tb);
        if (ta < t)
            t = ta;
    }
    return t - av_rescale_q(1, (AVRational){1, 1}, tb);
}

/* Clear the latch, wait a moment (shorter the first time), and seek back
 * (recover_seek_target); a seek that fails (the server is still away) is
 * the next try, without reading on from where the stream broke, which would
 * lose what the demuxer had half read. A stop or a seek cuts the wait short:
 * the seek at the top of the loop then starts from a clean latch. */
static void demux_recover(int read_result)
{
    const int a = recover_anchor();
    const AVStream *st = play_fmt->streams[a];
    char err[64];

    evo_demux_state = 6;
    for (;;) {
        s_recover_tries++;
        av_strerror(read_result, err, sizeof err);
        evo_bt("demux: read failed (%s) at %.2f s - recovering, try %d/%d", err,
               s_seek_ts[a] * av_q2d(st->time_base), s_recover_tries, DEMUX_RECOVER_TRIES);
        {
            char d[64];
            snprintf(d, sizeof d, "try=%d", s_recover_tries);
            pp_stage_bc("DEMUX_RECOVER", d);
        }

        play_fmt->pb->error = 0;
        play_fmt->pb->eof_reached = 0;
        for (int waited = 0, wait_ms = s_recover_tries == 1 ? 200 : 1000; waited < wait_ms;
             waited += 50) {
            if (!demux_thread_running || prospero_seek_pending)
                return;
            prebuffer_check(0);   /* a rebuffer's deadline still holds */
            usleep(50000);
        }

        const int64_t pos_before = avio_tell(play_fmt->pb);
        int rc = av_seek_frame(play_fmt, a, recover_seek_target(a), AVSEEK_FLAG_BACKWARD);
        /* Matroska ignores a failed avio_seek and reports success, and its next
         * read would start mid-cluster and skip the rest of it. A demuxer that
         * reads the file in order must have gone back; MP4 reads each sample
         * at its own offset, so its seek leaves the position where it was. */
        if (rc >= 0 && avio_tell(play_fmt->pb) >= pos_before &&
            !(play_fmt->iformat && strcmp(play_fmt->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0))
            rc = AVERROR(EIO);
        play_fmt->pb->error = 0;
        play_fmt->pb->eof_reached = 0;
        if (rc >= 0)
            break;
        av_strerror(rc, err, sizeof err);
        evo_bt("demux: recovery seek failed (%s)", err);
        if (s_recover_tries >= DEMUX_RECOVER_TRIES || !demux_thread_running || prospero_seek_pending)
            return;
        read_result = rc;
    }
    s_recover_catching = 0;
    for (int i = 0; i < DEMUX_RECOVER_MAX_STREAMS; i++) {
        s_catching[i] = s_last_ts[i] != AV_NOPTS_VALUE || s_last_pos[i] >= 0;
        s_recover_catching |= s_catching[i];
    }
}

/*
 * A packet that came back although the read under it failed. MP4 hands back
 * what arrived (flagged corrupt), MPEG-TS flushes the half PES it had, and
 * Matroska resyncs to the next cluster and skips the rest of the broken one.
 * Each is either a broken copy (kept, the whole one read again later would be
 * dropped as a duplicate) or a jump past packets that never came. So while
 * recovery can run, a set error latch makes the read a failed one, whatever
 * the demuxer returned; when it cannot, the latch is cleared and the packet
 * goes on as before, or the real end of the file would read as an error.
 */
static int demux_read_broke(const AVPacket *pkt)
{
    (void)pkt;
    if (!s_recover_allowed || !play_fmt->pb || !play_fmt->pb->error)
        return 0;
    if (!demux_read_is_final(play_fmt->pb->error))
        return 1;
    char err[64];
    av_strerror(play_fmt->pb->error, err, sizeof err);
    evo_bt("demux: read on past a read error (%s)", err);
    play_fmt->pb->error = 0;
    return 0;
}

/* While catching up after a recovery seek: 1 = already handed on, drop it. */
static int demux_already_have(const AVPacket *pkt)
{
    const int i = pkt->stream_index;
    if (!s_recover_catching || i < 0 || i >= DEMUX_RECOVER_MAX_STREAMS || !s_catching[i])
        return 0;
    const int64_t ts = pkt_ts(pkt);
    const int ts_old = ts == AV_NOPTS_VALUE || s_last_ts[i] == AV_NOPTS_VALUE || ts <= s_last_ts[i];
    if (pkt->pos >= 0 && s_last_pos[i] >= 0) {
        if (pkt->pos < s_last_pos[i] || (pkt->pos == s_last_pos[i] && ts_old))
            return 1;
    } else if (ts_old) {
        return 1;
    }
    s_catching[i] = 0;
    s_recover_catching = 0;
    for (int k = 0; k < DEMUX_RECOVER_MAX_STREAMS; k++)
        s_recover_catching |= s_catching[k];
    if (i == recover_anchor()) {
        evo_bt("demux: recovered - picking up after %.2f s",
               s_seek_ts[i] * av_q2d(play_fmt->streams[i]->time_base));
        pp_stage_bc("DEMUX_RECOVERED", "");
    }
    return 0;
}

/* A packet that goes on: where each stream has got to. */
static void demux_note_packet(const AVPacket *pkt)
{
    const int i = pkt->stream_index;
    if (i < 0 || i >= DEMUX_RECOVER_MAX_STREAMS)
        return;
    const int64_t ts = pkt_ts(pkt);
    if (pkt->pos >= 0 && pkt->pos != s_last_pos[i]) {
        s_last_pos[i] = pkt->pos;
        s_last_ts[i] = ts;     /* the timestamp at this position, NOPTS or not */
    } else if (ts != AV_NOPTS_VALUE) {
        s_last_ts[i] = ts;
    }
    if (ts != AV_NOPTS_VALUE)
        s_seek_ts[i] = ts;
    if (i == recover_anchor()) {
        s_recover_tries = 0;
        s_recover_gave_up = 0;
    }
}

void *demux_thread_func(void *arg) {
    (void)arg;

    AVPacket *pkt =
        av_packet_alloc();

    if (!pkt) {
        /* Nothing will ever fill the queue, so do not leave the decode
         * threads parked on a hold that can no longer be cleared. */
        pb_prebuffer_hold = 0;
        return NULL;
    }

    /* Deadline for the pre-buffer, measured from when this thread actually
     * starts reading rather than from when it was created. */
    s_prebuffer_target_us   = PREBUFFER_OPEN_US;
    s_prebuffer_deadline_ms = now_ms() + (long long)pb_prebuffer_max_ms;

    {
        const char *u = (play_fmt && play_fmt->url) ? play_fmt->url : "";
        const int net = strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0 ||
                        strncmp(u, "ftp://", 6) == 0 || strncmp(u, "smb://", 6) == 0;
        s_readahead_us = net ? READAHEAD_NET_US : READAHEAD_LOCAL_US;
    }
    demux_recover_reset();
    s_recover_allowed = demux_recover_allowed_for(play_fmt);
    /* Budget by what the rings actually got: cap plus the quarter of
     * overshoot must still fit, or the packets past it fall back to the
     * plain allocator. */
    if (packet_queue_use_ring(&video_packet_queue, VIDEO_RING_BYTES)) {
        int64_t cap = (int64_t)packet_queue_ring_size(&video_packet_queue) * 3 / 4;
        s_video_bytes_cap = cap < VIDEO_BYTES_CAP ? cap : VIDEO_BYTES_CAP;
    } else {
        s_video_bytes_cap = NO_RING_BYTES_CAP;
    }
    if (packet_queue_use_ring(&audio_packet_queue, AUDIO_RING_BYTES)) {
        int64_t cap = (int64_t)packet_queue_ring_size(&audio_packet_queue) * 3 / 4;
        s_audio_bytes_cap = cap < AUDIO_BYTES_CAP ? cap : AUDIO_BYTES_CAP;
    } else {
        s_audio_bytes_cap = NO_RING_BYTES_CAP;
    }

    while (demux_thread_running) {
        /*
         * Process seek requests before checking the paused state.
         */
        if (prospero_seek_pending)
            evo_demux_state = 4;
        if (prospero_process_seek_request()) {
            av_packet_unref(pkt);
            demux_recover_reset();
            continue;
        }

        /* Paused or not, keep reading up to the budget: a pause is the
         * cheapest time to build cushion on a network stream, and the queue
         * budget (demux_wait_for_room) bounds it either way. */

        evo_demux_state = 1;
        int read_result =
            av_read_frame(
                play_fmt,
                pkt
            );

        if (read_result >= 0 && demux_read_broke(pkt)) {
            av_packet_unref(pkt);
            read_result = play_fmt->pb->error;
        }
        if (read_result < 0) {
            if (!demux_read_is_final(read_result)) {
                demux_recover(read_result);
                continue;
            }
            if (s_recover_tries >= DEMUX_RECOVER_TRIES && !s_recover_gave_up) {
                s_recover_gave_up = 1;
                evo_bt("demux: gave up after %d tries - the stream stopped", s_recover_tries);
            }
            /*
             * Keep the demux thread alive so seeking backward from EOF
             * does not require reopening the file.
             */
            video_decode_done = 1;
            evo_demux_state = 5;
            prebuffer_check(1);
            usleep(5000);
            continue;
        }

        if (demux_already_have(pkt)) {
            av_packet_unref(pkt);
            continue;
        }
        demux_note_packet(pkt);

        video_decode_done = 0;
        prebuffer_check(0);

        if (
            pkt->stream_index ==
            video_stream_index
        ) {
            /*
             * Do not drop non-keyframes in demux — that freezes for a full GOP
             * (often every 1–2s). Cap queue by waiting only.
             */
            demux_wait_for_room(&video_packet_queue, 1, &audio_packet_queue,
                                playback_profile >= 3 ? 300 : 500);

            if (demux_thread_running) {
                packet_queue_push_timed(&video_packet_queue, pkt,
                                        packet_dur_us(pkt));
                dbg_video_packets++;
            }
        } else if (
            pkt->stream_index ==
            audio_stream_index
        ) {
            demux_wait_for_room(&audio_packet_queue, 0, &video_packet_queue, 1000);

            if (demux_thread_running)
                packet_queue_push_timed(&audio_packet_queue, pkt,
                                        packet_dur_us(pkt));
        }

#ifdef NUVIO_APP
        /* The Nuvio Player renders subtitles itself (src/nuvio_subs.c). */
        nuvio_subs_on_packet(play_fmt, pkt);
        if (0) {
#else
        if (
            prospero_subtitle_wants_stream(
                pkt->stream_index
            )
        ) {
#endif
            if (
                pkt->stream_index ==
                prospero_embedded_subtitle_stream_index
            ) {
                dbg_sub_demuxed++;
            }

            prospero_embedded_subtitle_decode_packet(
                pkt
            );
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

