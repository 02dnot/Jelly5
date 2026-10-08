/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Nuvio Player talks to Nuvio's payload through nuvio_bridge.h and
 * nuvio_control.h. Jelly5 has no payload: these stand-ins route the player's
 * progress and final result to the Jellyfin playback reporting instead.
 */
#include "jelly5_playback.h"
#include "nuvio_bridge.h"
#include "nuvio_control.h"

#include <cstdlib>

extern "C" {

volatile unsigned nuvio_control_beats;
volatile int nuvio_control_stage;

void nuvio_control_start(void) {}
void nuvio_control_set_playing(int) {}
int nuvio_control_take_stop(void) { return 0; }
int nuvio_control_quit_requested(void) { return 0; }

void nuvio_control_report(const char *, double position, double duration, int paused, int audio_stream,
                          int subtitle_track)
{
    nuvio_control_beats++;
    jelly5_playback_progress(position, duration, paused != 0, audio_stream, subtitle_track);
}

int nuvio_service_up(void) { return 1; }
int nuvio_bridge_next(char **json) { *json = nullptr; return 0; }
void nuvio_bridge_state(const char *, const char *, double, double, const char *) {}
void nuvio_bridge_state_json(const char *json) { jelly5_playback_finished(json); }
void nuvio_bridge_source(int index) { jelly5_playback_source(index); }

int nuvio_bridge_get(const char *, char **body)
{
    *body = nullptr;
    return 404;
}

int nuvio_bridge_commands(nuvio_command *, int) { return 0; }
int nuvio_bridge_post_blob(const char *, const char *, const void *, size_t) { return -1; }

} // extern "C"
