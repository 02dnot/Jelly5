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

#include <vector>

/* Blocks for the whole playback (and any episodes played after it). Returns
 * false with *error when the item could not be started. A music track plays
 * on through its album; shuffle: the album in random order from this track. */
bool jelly5_play(jf::Client &client, const jf::Item &item, std::string *error, bool shuffle = false);
/* The stop report goes to the server in the background after playback; this
 * waits (up to max_ms) until it has landed, before reading positions back. */
void jelly5_wait_reports(int max_ms);
/* A queue (a playlist, an Instant Mix, what a phone sent): plays from queue[start]
 * through the rest. */
bool jelly5_play_queue(jf::Client &client, const std::vector<jf::Item> &queue, size_t start, std::string *error);

/* Subtitle search while something plays (the player's "Søk etter undertekster"):
 * Jellyfin's subtitle plugins search, the server downloads the choice, and it
 * is added to the player as an external track. All of it runs off the
 * player's thread; the player polls the state. */
namespace jelly5_subs {
enum State { Idle, Busy, Done, Failed };
bool available();                                 /* this title, this account, this server */
void search(const std::string &language);         /* three-letter code */
State results(std::vector<jf::RemoteSubtitle> *out, std::string *language);
void download(const jf::RemoteSubtitle &s);
/* Done: *track is the player's new subtitle track (to select). */
State download_state(int *track);
} // namespace jelly5_subs

extern "C" {
/* Called by the player through the bridge stand-ins (jelly5_bridge.cpp). */
void jelly5_playback_progress(double position, double duration);
void jelly5_playback_finished(const char *result_json);
}
