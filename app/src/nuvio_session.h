/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * One playback as Nuvio's page describes it (the JSON body of
 * POST /api/player/play, see NuvioTVSmart js/platform/ps5/ps5NativePlayer.js):
 * the stream, what it is, the other sources, the episodes around it, the
 * subtitle addons to ask, and the viewer's preferences and language.
 */
#include "nuvio_subs.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct NuvioSource {
    std::string id, title, description, addon, url, quality;
    std::string headers;          /* "Name: value\r\n" lines */
    std::string user_agent;
};

struct NuvioEpisode {
    int season = 0, episode = 0;
    std::string title, thumbnail, video_id, overview, released_label;
    bool released = true;
    bool watched = false;
    double progress = 0;           /* Jelly5: percent watched (0..100) */
    std::string runtime;           /* Jelly5: "23 min" */
    std::string blurhash;          /* Jelly5: the still's placeholder */
};

struct NuvioSubtitleRef {
    std::string url, lang, label, headers;
    std::string source;           /* Jelly5: only with this source (id); empty: with every one */
};

struct NuvioSkip {
    std::string type;             /* intro, recap, outro, credits, preview */
    double start = 0, end = 0;
};

/* Jelly5: a chapter mark, and the server's scrub previews. */
struct NuvioChapter {
    double start = 0;
    std::string name;
    std::string image;            /* Jelly5: the chapter's picture, if the server has one */
};

struct NuvioLyric {
    double start = -1;            /* seconds; < 0: not timed */
    std::string text;
    struct Cue {                  /* a word: when it is sung, and its bytes in text */
        double start = 0;
        size_t from = 0, to = 0;
    };
    std::vector<Cue> cues;        /* empty: the line is timed as a whole */
};

struct NuvioTrickplay {
    int width = 0, height = 0, tile_w = 0, tile_h = 0, count = 0;
    double interval = 0;
    std::string url_base, url_query;
    int64_t sheet_ticks = 0;      /* Jelly5: Emby's thumbnails are found by time (jf::Trickplay) */
    int64_t first_ticks = 0;
    /* The thumbnail for a moment (seconds), counted from the first one's time. */
    int index_at(double pos) const
    {
        const int i = (int)((pos - first_ticks / 1e7) / interval);
        return i < 0 ? 0 : i > count - 1 ? count - 1 : i;
    }
    std::string sheet_url(int sheet) const
    {
        return sheet_ticks > 0 ? url_base + std::to_string((long long)(first_ticks + sheet * sheet_ticks)) + url_query
                               : url_base + std::to_string(sheet) + ".jpg" + url_query;
    }
    bool valid() const { return width > 0 && height > 0 && tile_w > 0 && tile_h > 0 && count > 0 && interval > 0; }
};

struct NuvioPrefs {
    std::vector<std::string> audio_langs;     /* preferred, in order */
    std::vector<std::string> subtitle_langs;
    bool subtitles_enabled = true;
    bool forced_only_when_off = true;
    nuvio_sub_style style = {100, 0xffffff, 0, 1, 0.0f, 0.0f};
    bool autoplay_next = true;
    /* Nuvio's next-episode card rule (playerNextEpisodeRules.js). */
    bool next_by_minutes = false;            /* MINUTES_BEFORE_END, else PERCENTAGE */
    double next_percent = 99.0;
    double next_minutes = 2.0;
    bool show_clock = true;
    bool clock_24h = true;
    bool skip_intro = true;
    bool auto_skip = false;                  /* Jelly5: skip intros without asking */
    int still_watching_episodes = 3;         /* 0 = never ask */
    bool has_tz = false;                     /* the page's UTC offset, for the clock */
    int tz_offset_min = 0;
};

struct NuvioRequest {
    std::string id, url, headers, user_agent;
    std::string title, episode_title, year, description, genres, runtime, rating, item_type;
    std::string logo, poster, backdrop, thumbnail;
    std::string artist, album, cover, cover_blurhash;   /* Jelly5: music */
    uint32_t light_color = 0;                           /* Jelly5: the DualSense light bar, 0 = leave it */
    int season = 0, episode = 0;
    double start_position = 0;
    std::string stream_title, stream_description, stream_addon;
    std::string play_method, transcode_reasons;         /* Jelly5: PlaybackInfo's decision, for the L3 panel */
    std::vector<NuvioSource> sources;
    int source_index = 0;
    std::vector<NuvioSubtitleRef> subtitles;
    struct SubtitleRequest { std::string url, addon; };
    std::vector<SubtitleRequest> subtitle_requests;   /* Stremio addon subtitle lookups */
    std::vector<NuvioEpisode> episodes;
    bool has_next = false;
    NuvioEpisode next;
    std::vector<NuvioSkip> skips;
    std::vector<NuvioChapter> chapters;
    std::vector<NuvioLyric> lyrics;
    NuvioTrickplay trickplay;
    NuvioPrefs prefs;
    std::map<std::string, std::string> strings;
    int autoplay_count = 0;       /* episodes played back to back so far */
    bool not_group = false;       /* Jelly5: never a SyncPlay group's item (a theme song) */

    /* A UI string in the viewer's language (Nuvio's translation), else fallback. */
    const char *str(const char *key, const char *fallback) const;
    /* "Show", "S1 E3 · Title" lines as Nuvio's player header shows them. */
    std::string header_title() const;
    std::string header_subtitle() const;
};

/* Parses the request; false when it carries no playable url. */
bool nuvio_request_parse(const char *json, NuvioRequest &out);

/* "Name: value\r\n" lines from a JSON headers object; User-Agent apart. */
std::string nuvio_headers_from_json(const void *cjson_object, std::string *user_agent);

/* How the playback ended, for Nuvio's page (POST /api/player/state). */
struct NuvioResult {
    std::string state;            /* stopped | ended | error */
    double position = 0, duration = 0;
    std::string error;
    std::string action;           /* "", next, episode, source */
    int season = 0, episode = 0;  /* action episode */
    std::string video_id;         /* Jelly5: action next/episode: the item, when known */
    int source_index = -1;        /* action source */
    std::string audio_lang, subtitle_lang;
    bool subtitles_on = false;
    int subtitle_delay_ms = 0;
    bool group_end = false;       /* Jelly5: ended as a SyncPlay group's item: the group plays on */
};

std::string nuvio_result_json(const NuvioRequest &req, const NuvioResult &res);
