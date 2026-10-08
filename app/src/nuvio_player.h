/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * The Nuvio Player: full-screen native playback on EVO Player's engine, with
 * Jelly5's player interface drawn over it (ui::PlayerUi, behind the
 * nuvio_osd.h contract) and subtitles by libass (nuvio_subs.c).
 *
 * Hardware HEVC/H.264 (sceVideodec2) up to 4K, 10-bit HDR10/HLG output,
 * software AV1/VP9/MPEG-2/VC-1, and every audio format FFmpeg decodes (TrueHD,
 * DTS-HD MA, E-AC-3, FLAC, ...) out as multichannel PCM.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* After the display is up: creates the engine and the overlay canvases.
 * user_id owns the controller (opened only while a stream plays, because the
 * browser dialog gets no input while the app holds it). */
void nuvio_player_init(int user_id);

/* Plays one request (the page's JSON) until it ends or the viewer leaves,
 * then posts the result for Nuvio's page. Blocks for the whole playback. */
void nuvio_player_run(const char *request_json);

/* Jelly5: music while the app's menus stay up. Set before nuvio_player_run (on
 * its own thread): the player draws nothing and reads no controller - the app
 * draws (from nuvio_player_now_playing) and sends commands through app/remote.
 * Only for audio: there is no picture to show. */
void nuvio_player_set_headless(int headless);
/* Jelly5: after music, leave player mode (a music run keeps it on, so the next
 * track follows without a blank screen). Safe to call when nothing is pending. */
void nuvio_player_leave(void);

#ifdef __cplusplus
}

#include <atomic>
/* Jelly5: a stop of the caller's own (null: none), set before nuvio_player_run on
 * the thread that runs it: the run ends once *stop is set, and returns at once when
 * it already is. A theme song or the music stops this way, not through the shared
 * remote queue, so a late Stop can never reach the playback after it. */
void nuvio_player_set_stop(const std::atomic<bool> *stop);
/* Jelly5: that stop is set (on the thread that set it): a chain checks it between
 * its requests to the server, so a stopped theme or music lets go of the player soon. */
bool nuvio_player_stop_requested(void);
#endif
