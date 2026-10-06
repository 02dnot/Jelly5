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
    std::string video3d;                      /* Video3DFormat: HalfSideBySide, MVC ... (empty: 2D) */
    std::string series_id, series_name, season_id, season_name;
    int index = -1, parent_index = -1;        /* episode / season numbers */
    int year = 0;
    double community_rating = 0;
    int64_t runtime_ticks = 0;
    int64_t position_ticks = 0;               /* resume point */
    double played_percent = 0;
    bool played = false, favorite = false;
    int local_trailers = 0;                   /* trailer files next to the title */
    int unplayed = 0;                         /* a series' or season's episodes not yet watched */
    int special_features = 0;                 /* extras: behind the scenes, deleted scenes ... */
    std::vector<std::string> genres;

    /* Image owners and tags (an episode's logo/backdrop belong to its series). */
    std::string primary_tag, thumb_tag, logo_tag, backdrop_tag;
    std::string series_primary_tag;           /* an episode's series poster */
    std::string logo_owner, backdrop_owner, thumb_owner;
    std::string primary_blurhash, backdrop_blurhash, thumb_blurhash;
    std::string collection_type;              /* views: movies, tvshows, music ... */
    /* Music: a track's album and artist (the album's cover is its art). */
    std::string album_id, album, album_artist, album_artist_id;
    std::string album_primary_tag, album_blurhash;
    std::string premiere_date;                /* ISO date; a person's birth date */
    std::vector<std::string> locations;       /* a person's birthplace */
    std::string tmdb_id, tvdb_id;             /* ProviderIds (search and item ask for them) */

    /* A title from Seerr rather than this server (search, Discover): its TMDB
     * id, where it stands in Seerr (a seerr::Status), its art as absolute URLs
     * (through Seerr's image cache; empty when there is none to be had), and
     * the server's own item when it has the title. Empty for the server's items. */
    struct External {
        int tmdb_id = 0;
        int status = 0;                       /* Seerr's (also on a library item Discover shows) */
        int request = 0;                      /* "Mine forespørsler": the request's own state (seerr::RequestStatus) */
        std::string poster, backdrop, thumb;
        std::string jellyfin_id;
    } ext;
    bool external() const { return ext.tmdb_id != 0; }
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

struct Chapter {
    double start = 0;                         /* seconds */
    std::string name;
    std::string image_tag;                    /* the server's chapter image, if it made one */
};

/* Scrubbing previews: sheets of tile_w x tile_h thumbnails, one every interval. */
struct Trickplay {
    int width = 0, height = 0;                /* one thumbnail */
    int tile_w = 0, tile_h = 0, count = 0;
    double interval = 0;                      /* seconds between thumbnails */
    std::string url_base;                     /* + "<sheet>.jpg?..." (see sheet_url) */
    std::string url_query;
    bool valid() const { return width > 0 && height > 0 && tile_w > 0 && tile_h > 0 && count > 0 && interval > 0; }
};

/* What PlaybackInfo decided for one item. */
/* One version of a title (Jellyfin: a media source), as it would play. */
struct Version {
    std::string id, name;                     /* name: "4K", "1080p" ... (Jellyfin's) */
    std::string play_method, url;
    int height = 0;
    int64_t bitrate = 0;
    std::string label;                        /* "2160p · HEVC · HDR" */
};

struct Playback {
    std::string item_id, media_source_id, play_session_id;
    std::string play_method;                  /* DirectPlay, DirectStream, Transcode */
    std::string url;                          /* absolute, authorised */
    std::string container, transcode_reasons;
    int default_audio = -1, default_subtitle = -1;
    std::vector<MediaStream> streams;
    /* Every version, the chosen one (the best the PS5 plays, see playback_info) first. */
    std::vector<Version> versions;
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

/* A subtitle Jellyfin's subtitle plugins found for a title. */
struct RemoteSubtitle {
    std::string id, name, provider, language, format;
    int downloads = 0;
    bool hash_match = false, forced = false, hearing_impaired = false;
};

/* A line of a song's lyrics; start < 0 when the lyrics are not timed. */
struct LyricLine {
    double start = -1;                        /* seconds */
    std::string text;
    struct Cue {                              /* a word's timing (enhanced LRC), when the file has it */
        double start = 0;                     /* seconds */
        size_t from = 0, to = 0;              /* byte range in text */
    };
    std::vector<Cue> cues;
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
    /* Approves another app's Quick Connect code for this session's user, as
     * the phone does (Seerr signs in this way: app/seerr_service). */
    bool quick_connect_authorize(const std::string &code);
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
    /* filter: extra query, e.g. "&AlbumArtistIds=<id>" (an artist's albums). */
    Page library(const std::string &parent_id, const std::string &types, const std::string &sort_by,
                 bool descending, int start, int limit, const std::string &filter = std::string());
    /* A music library's album artists (Jellyfin's "Album Artists"), paged like library(). */
    Page album_artists(const std::string &parent_id, const std::string &sort_by, bool descending, int start,
                       int limit);
    /* The genres in one library (for its filter). */
    std::vector<std::string> genres_in(const std::string &parent_id, const std::string &types);
    /* How many titles of a library (with its filter) sort before `letter` by name:
     * the index of the first one from that letter on (A-Å jumps). -1 on failure. */
    int count_before(const std::string &parent_id, const std::string &types, const std::string &filter,
                     const std::string &letter);
    static std::string escape(const std::string &s);   /* for a query value */
    /* A cheap request that needs no sign-in: is the server there? */
    bool ping();
    /* The home screen's sections as the user ordered them in Jellyfin (Settings ->
     * Home: "resume", "nextup", "latestmedia", "smalllibrarytiles", "none" ...),
     * empty when they kept the default. */
    std::vector<std::string> home_sections();
    /* The user's display settings for libraries (from /Users/Me): ids of libraries
     * left out of "Nylig lagt til". */
    const std::vector<std::string> &latest_excludes() const { return latest_excludes_; }
    /* A playlist's entries, in its order. */
    std::vector<Item> playlist_items(const std::string &playlist_id);
    /* A song's lyrics (Jellyfin's .lrc / lyric plugins); empty when it has none. */
    std::vector<LyricLine> lyrics(const std::string &item_id);
    /* types e.g. "Movie,Series" or "Person" (Jellyfin's own matching and order). */
    std::vector<Item> search(const std::string &term, const std::string &types, int limit);

    /* Jellyfin's recommendations: "because you watched X" and the like. */
    struct Recommendation {
        std::string type;                     /* SimilarToRecentlyPlayed, HasActorFromRecentlyPlayed, ... */
        std::string baseline;                 /* the title or person it is built on */
        std::vector<Item> items;
    };
    std::vector<Recommendation> recommendations(int categories, int items);
    /* Genre names in the library (movies and series). */
    std::vector<std::string> genres();
    /* Titles in a genre, in random order. */
    std::vector<Item> genre_items(const std::string &genre, int limit);
    /* A title's extras: behind the scenes, deleted scenes, featurettes. */
    std::vector<Item> special_features(const std::string &id);
    /* A title's theme songs (an episode's or season's come from its series). */
    std::vector<Item> theme_songs(const std::string &id);
    bool item(const std::string &id, Item *out, Detail *detail = nullptr);
    std::vector<Item> seasons(const std::string &series_id);
    std::vector<Item> similar(const std::string &id, int limit);
    /* What a person is in, in this library: types e.g. "Movie" or "Series", newest first. */
    std::vector<Item> person_items(const std::string &person_id, const std::string &types, int limit);
    bool set_favorite(const std::string &id, bool favorite);
    bool set_played(const std::string &id, bool played);
    /* A title's own trailer files (YouTube trailers are not played: not Jellyfin's). */
    std::vector<Item> local_trailers(const std::string &id);
    /* Drops the resume point: the title leaves "Fortsett å se". */
    bool clear_position(const std::string &id);
    /* Min liste: the user's favourite movies, series and collections, newest first. */
    std::vector<Item> favorites(int limit);
    /* A folder's or collection's direct children, e.g. sort_by "PremiereDate,SortName". */
    std::vector<Item> children(const std::string &parent_id, const std::string &sort_by, int limit);
    std::vector<Segment> segments(const std::string &item_id);
    /* Chapters and trickplay of what plays (media_source_id picks the version). */
    bool media_extras(const std::string &item_id, const std::string &media_source_id, std::vector<Chapter> *chapters,
                      Trickplay *trickplay);

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

    /* "Authorization: MediaBrowser ..." with this session's token (also for /socket). */
    std::string auth_header() const;
    /* Remote control: this device plays video and audio and takes playstate
     * commands and messages (POST /Sessions/Capabilities/Full). */
    bool post_capabilities();
    /* Subtitle search through the server's plugins (Open Subtitles and the like):
     * the user may manage subtitles (an administrator, or the policy allows it)
     * and, as far as an administrator can see, a subtitle plugin is installed.
     * Checked by check_subtitle_search() after sign-in. */
    bool can_search_subtitles() const { return subtitle_search_; }
    void check_subtitle_search();
    std::vector<RemoteSubtitle> search_subtitles(const std::string &item_id, const std::string &language);
    /* The server downloads it next to the video (a new external subtitle stream). */
    bool download_subtitle(const std::string &item_id, const std::string &subtitle_id);

    /* Jellyfin's Instant Mix: songs like this one (or album, artist, genre). */
    std::vector<Item> instant_mix(const std::string &id, int limit);

    /* A GET / POST of the API, for modules with their own endpoints (app/syncplay). */
    bool get_json(const std::string &path, std::string *body);
    bool post_json(const std::string &path, const std::string &json, std::string *body);

private:
    std::vector<Item> items_of(const std::string &body);

    std::string server_, device_id_, device_name_;
    std::string token_, user_id_, user_name_, user_image_tag_;
    bool is_admin_ = false, manages_subtitles_ = false, subtitle_search_ = false;
    std::vector<std::string> latest_excludes_;
    void set_error(std::string e) { std::lock_guard<std::mutex> g(error_lock_); error_ = std::move(e); }
    std::string error_;
    mutable std::mutex error_lock_;
};

/* Requests in a row (from any client) that got no answer at all: the server or
 * the network is gone. 0 once anything answers. */
int unreachable_streak();

} // namespace jf
