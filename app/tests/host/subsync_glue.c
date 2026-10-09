/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host glue for tests/host/subsync.sh: the engine's subtitle auto-sync
 * (engine/media/src/evo_subsync.c) built against the Mac's FFmpeg, with the
 * console-only pieces (the log, the stream options) stood in for.
 */
#include <stdarg.h>
#include <stdio.h>

#include "../../engine/media/src/evo_subsync.c"

void evo_boot_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    | ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void evo_stream_io_apply_network_options(AVDictionary **opts, const char *url)
{
    (void)url;
    av_dict_set(opts, "rw_timeout", "5000000", 0);
}

/* One run, as the worker makes it. audio_url empty: media_path is read. */
int glue_run(const char *media_path, const char *audio_url, const char *cue_url, const char *stop_url,
             double duration_s, int sub_stream, const double *cs, const double *ce, int n,
             evo_subsync_result_t *res)
{
    volatile int cancel = 0, progress = 0;
    return ss_run(media_path, audio_url, cue_url, stop_url, duration_s, -1, sub_stream, cs, ce, n,
                  &cancel, &progress, res);
}

/* A subtitle file's cue times (as nuvio_subs has an external track's). */
int glue_cues(const char *url, double **cs, double **ce)
{
    ss_ctx c;
    memset(&c, 0, sizeof(c));
    const int n = ss_load_cues(&c, url);
    *cs = c.ecs;
    *ce = c.ece;
    return n;
}
