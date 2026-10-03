/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Jellyfin API as Jelly5 uses it: sign-in (password or Quick Connect),
 * the home rows, items, playback negotiation with the PS5 device profile and
 * playback reporting. Plain blocking calls; callers keep them off the render
 * thread. Builds on the console and on a development machine.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace jf {

constexpr int64_t kTicksPerSecond = 10000000;

struct Item {
    std::string id, name, type;               /* Movie, Series, Episode, ... */
    std::string overview, official_rating;
    std::string series_id, series_name, season_id, season_name;
    int index = -1, parent_index = -1;        /* episode / season numbers */
    int year = 0;
    double community_rating = 0;
    int64_t runtime_ticks = 0;
    int64_t position_ticks = 0;               /* resume point */
    double played_percent = 0;
    bool played = false, favorite = false;
    std::vector<std::string> genres;

    /* Image owners and tags (an episode's logo/backdrop belong to its series). */
    std::string primary_tag, thumb_tag, logo_tag, backdrop_tag;
    std::string logo_owner, backdrop_owner, thumb_owner;
    std::string primary_blurhash, backdrop_blurhash, thumb_blurhash;
    std::string collection_type;              /* views: movies, tvshows, music ... */
    std::string premiere_date;                /* ISO date; a person's birth date */
    std::vector<std::string> locations;       /* a person's birthplace */
};

struct MediaStream {
    int index = -1;
    std::string type;                         /* Video, Audio, Subtitle */
    std::string codec, language, title, display_title, profile;
    std::string video_range, video_range_type;
    int width = 0, height = 0, channels = 0, bit_depth = 0;
    bool is_default = false, is_forced = false, is_external = false, is_text = false;
    std::string delivery_url;                 /* external subtitles */
};

struct Segment {
    std::string type;                         /* Intro, Outro, Recap, Preview, Commercial */
    double start = 0, end = 0;                /* seconds */
};

/* What PlaybackInfo decided for one item. */
struct Playback {
    std::string item_id, media_source_id, play_session_id;
    std::string play_method;                  /* DirectPlay, DirectStream, Transcode */
    std::string url;                          /* absolute, authorised */
    std::string container, transcode_reasons;
    int default_audio = -1, default_subtitle = -1;
    std::vector<MediaStream> streams;
};

struct Person {
    std::string id, name, role, type;         /* type: Actor, Director, Writer ... */
    std::string image_tag, blurhash;
};

/* Everything the detail page shows beyond the row fields. */
struct Detail {
    std::vector<Person> people;
    std::vector<std::string> studios;
    std::string tagline;
    std::vector<MediaStream> streams;         /* the default source's, for badges */
};

/* The user's playback preferences, kept on the server (shared with every client). */
struct UserPrefs {
    std::string audio_language;               /* ISO 639-2, e.g. "nor"; empty = any */
    std::string subtitle_language;
    std::string subtitle_mode;                /* Default, Always, OnlyForced, None, Smart */
    bool autoplay_next = true;
};

struct PublicUser {
    std::string id, name, image_tag;
    bool has_password = true;
};

struct Page {
    std::vector<Item> items;
    int total = 0;
};

struct QuickConnect {
    std::string code, secret;
};

class Client {
public:
    Client(std::string server, std::string device_id, std::string device_name);

    const std::string &server() const { return server_; }
    const std::string &device_id() const { return device_id_; }
    const std::string &device_name() const { return device_name_; }
    void set_server(std::string server);
    const std::string &user_image_tag() const { return user_image_tag_; }
    const std::string &token() const { return token_; }
    const std::string &user_id() const { return user_id_; }
    const std::string &user_name() const { return user_name_; }
    void set_session(std::string token, std::string user_id, std::string user_name);
    void note_image_tag(std::string tag) { user_image_tag_ = std::move(tag); }
    bool signed_in() const { return !token_.empty(); }
    /* Safe from any thread: requests may run in parallel. */
    std::string last_error() const { std::lock_guard<std::mutex> g(error_lock_); return error_; }

    /* Server name and version from /System/Info/Public; false if unreachable. */
    bool public_info(std::string *name, std::string *version);

    bool authenticate(const std::string &user, const std::string &password);
    bool quick_connect_start(QuickConnect *out);
    /* true once the code was approved (and the session is set). */
    bool quick_connect_poll(const QuickConnect &qc, bool *approved);
    /* The token still works (GET /Users/Me). */
    bool validate();
    std::vector<PublicUser> public_users();
    bool get_prefs(UserPrefs *out);
    bool set_prefs(const UserPrefs &p);

    std::vector<Item> resume(int limit, const std::string &parent_id = std::string());
    std::vector<Item> next_up(int limit, const std::string &series_id = std::string());
    std::vector<Item> views();
    /* Random movies and series that have both a logo and a backdrop (the hero). */
    std::vector<Item> featured(int limit, std::string *raw = nullptr);
    /* The same from a response saved earlier (the hero is cached between launches). */
    std::vector<Item> featured_from(const std::string &raw, int limit);
    std::vector<Item> latest(const std::string &parent_id, int limit);
    std::vector<Item> episodes(const std::string &series_id, const std::string &season_id);
    /* A library page: types e.g. "Movie" or "Series"; sort_by e.g. "DateCreated,SortName". */
    Page library(const std::string &parent_id, const std::string &types, const std::string &sort_by,
                 bool descending, int start, int limit);
    std::vector<Item> search(const std::string &term, int limit);
    bool item(const std::string &id, Item *out, Detail *detail = nullptr);
    std::vector<Item> seasons(const std::string &series_id);
    std::vector<Item> similar(const std::string &id, int limit);
    /* What a person is in, in this library: types e.g. "Movie" or "Series", newest first. */
    std::vector<Item> person_items(const std::string &person_id, const std::string &types, int limit);
    bool set_favorite(const std::string &id, bool favorite);
    bool set_played(const std::string &id, bool played);
    /* Drops the resume point: the title leaves "Fortsett å se". */
    bool clear_position(const std::string &id);
    /* Min liste: the user's favourite movies, series and collections, newest first. */
    std::vector<Item> favorites(int limit);
    /* A folder's or collection's direct children, e.g. sort_by "PremiereDate,SortName". */
    std::vector<Item> children(const std::string &parent_id, const std::string &sort_by, int limit);
    std::vector<Segment> segments(const std::string &item_id);

    /* audio_index < 0: the server's default track; subtitle_index -2: the
     * server's default, -1: none. */
    /* max_bitrate (bits/s, 0 = no cap): above it the server transcodes down. */
    bool playback_info(const std::string &item_id, int64_t start_ticks, int audio_index,
                       int subtitle_index, Playback *out, int64_t max_bitrate = 0);
    std::string image_url(const std::string &owner, const char *type, const std::string &tag,
                          int width) const;

    void report_start(const Playback &pb, int64_t position_ticks);
    void report_progress(const Playback &pb, int64_t position_ticks, bool paused);
    void report_stopped(const Playback &pb, int64_t position_ticks);
    /* Ends a server transcode (no-op for direct play). */
    void stop_encoding(const Playback &pb);

    /* The PS5 device profile (JSON) sent with PlaybackInfo. */
    static std::string device_profile_json(int64_t max_bitrate = 0);

private:
    std::string auth_header() const;
    bool get_json(const std::string &path, std::string *body);
    bool post_json(const std::string &path, const std::string &json, std::string *body);
    std::vector<Item> items_of(const std::string &body);

    std::string server_, device_id_, device_name_;
    std::string token_, user_id_, user_name_, user_image_tag_;
    void set_error(std::string e) { std::lock_guard<std::mutex> g(error_lock_); error_ = std::move(e); }
    std::string error_;
    mutable std::mutex error_lock_;
};

} // namespace jf
