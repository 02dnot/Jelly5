/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The audio and subtitle track the viewer chose in a series, so its next
 * episodes start on the same kind of track: kept per account and series in
 * /download0/jelly5/tracks.json (the last 200 series). A track is remembered
 * by what it is (language, forced, SDH, commentary ...), not by its index,
 * which differs from one episode's file to the next. "Subtitles off" is a
 * choice too. Follows the account's "Remember audio/subtitle selections".
 */
#pragma once

#include "jf/jf_client.h"

#include <string>
#include <vector>

namespace track_memory {

/* What a track is, apart from where it sits in a file. */
struct Track {
    std::string lang;          /* language key ("en", "no" for every Norwegian code); empty: none given */
    bool forced = false;       /* only the foreign parts */
    bool sdh = false;          /* for the hearing impaired */
    bool commentary = false;   /* a commentary track (audio, or its subtitles) */
    bool description = false;  /* audio: described for the visually impaired */
    bool image = false;        /* subtitles: a picture format (PGS, VobSub), else text */
    int channels = 0;          /* audio */
};

struct Choice {
    bool audio_set = false;
    Track audio;
    bool subtitle_set = false;
    bool subtitle_off = false;   /* the viewer turned subtitles off */
    Track subtitle;
};

/* The track a stream is. */
Track describe(const jf::MediaStream &m);
/* The stream ("Audio" or "Subtitle") in streams that is the remembered track's
 * equivalent: the same language, forced and commentary/description alike;
 * then the same SDH, text or picture, the server's default, the most channels.
 * A separate audio file is never one (the player plays the file's own). The
 * stream's index, or -1 when no stream is an equivalent. */
int match(const std::vector<jf::MediaStream> &streams, const char *type, const Track &want);

/* account: the user's id on its server (unique per server). */
bool get(const std::string &account, const std::string &series, Choice *out);
void set_audio(const std::string &account, const std::string &series, const Track &t);
/* off: subtitles turned off (t is then not used). */
void set_subtitle(const std::string &account, const std::string &series, bool off, const Track &t);

} // namespace track_memory
