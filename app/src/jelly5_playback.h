/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Playback of a Jellyfin item on the native player (the Nuvio Player on the
 * EVO engine): negotiates the stream with PlaybackInfo, hands the player a
 * request built from the item, reports start/progress/stop to Jellyfin, and
 * follows "next episode" from one episode (or album track) to the next.
 */
#pragma once

#include "jf/jf_client.h"

/* Blocks for the whole playback (and any episodes played after it). Returns
 * false with *error when the item could not be started. A music track plays
 * on through its album; shuffle: the album in random order from this track. */
bool jelly5_play(jf::Client &client, const jf::Item &item, std::string *error, bool shuffle = false);

extern "C" {
/* Called by the player through the bridge stand-ins (jelly5_bridge.cpp). */
void jelly5_playback_progress(double position, double duration);
void jelly5_playback_finished(const char *result_json);
}
