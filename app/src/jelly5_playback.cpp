/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jelly5_bitstream.h"
#include "jelly5_playback.h"
#include "evo_audio_out.h"

#include "nuvio_player.h"
#include "nuvio_subs.h"
#include "app/settings.h"
#include "app/i18n.h"
#include "app/livetv.h"
#include "ui/livetv.h"
#include "app/track_memory.h"
#include "app/spawn.h"
#include "jf/json_num.h"

#include "evo_boot_trace.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <set>
#include <thread>
#include <ctime>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace {

/* The playback in progress, shared with the player's callbacks. */
std::atomic<int> s_reports{0};   /* stop reports still on their way to the server */

struct Session {
    jf::Client *client = nullptr;
    jf::Playback pb;
    std::mutex lock;
    double position = 0, duration = 0;
    bool heard = false;                /* the player has reported (it has started) */
    bool paused = false;
    int audio_stream = -1;             /* the player's audio: the container's stream index */
    int subtitle_track = -1;           /* the player's subtitle: nuvio_subs' id, -1 off */
    /* The tracks as the server's indices, as last reported (server_tracks; -1 and -2
     * unknown): what the viewer had at the end (app/track_memory). The subtitle's
     * is for the player's track subtitle_known_for. */
    int server_audio = -1, server_subtitle = -2, subtitle_known_for = -1;
    double reported = -1;              /* position of the last progress report */
    std::string result;                /* the player's final result JSON */
    std::atomic<bool> active{false};
};
Session s_session;

int64_t ticks(double seconds) { return (int64_t)std::llround(seconds * jf::kTicksPerSecond); }

/* The server's index of each subtitle file the player was given, in the player's
 * order: request_json lists the real external files (the first version's, which
 * the player keeps through a version switch), then each version's served
 * subtitles tagged with it, and the player adds only the playing version's of
 * those (add_request_subtitles). The k-th external track is the k-th entry here;
 * -2 (not told) for a file of another version than the one playing. */
std::vector<int> request_subtitle_indices(const jf::Playback &pb)
{
    std::vector<int> out;
    const jf::Version *first = pb.versions.empty() ? nullptr : &pb.versions.front();
    const bool first_plays = !first || first->id == pb.media_source_id;
    for (const jf::MediaStream &m : first ? first->streams : pb.streams)
        if (m.type == "Subtitle" && m.is_external && !m.delivery_url.empty())
            out.push_back(first_plays ? m.index : -2);
    for (const jf::Version &v : pb.versions)
        if (v.id == pb.media_source_id)
            for (const jf::MediaStream &m : v.served_subtitles)
                out.push_back(m.index);
    return out;
}

/* A stream of the file as the file numbers it (the container's index): Jellyfin
 * numbers the external files first and the file's own streams after them, Emby
 * puts the external files last. */
int container_index(const jf::Playback &pb, int server_index)
{
    int before = 0;
    for (const jf::MediaStream &m : pb.streams)
        if (m.is_external && m.index >= 0 && m.index < server_index)
            before++;
    return server_index - before;
}

/* The player's tracks as the server's stream indices (PlaybackProgressInfo's
 * AudioStreamIndex and SubtitleStreamIndex): *audio -1 and *subtitle -2 where
 * they cannot be told. Playing the file itself, its streams are the server's
 * (container_index); an encoded stream carries the audio the server picked.
 * External subtitles were added first, in pb's order. */
void server_tracks(const jf::Playback &pb, int audio_stream, int subtitle_track, int *audio, int *subtitle)
{
    const bool direct = pb.play_method == "DirectPlay";
    auto embedded = [&pb](const char *type, int stream) {
        for (const jf::MediaStream &m : pb.streams)
            if (stream >= 0 && m.type == type && !m.is_external && container_index(pb, m.index) == stream)
                return m.index;
        return -1;
    };
    *audio = -1;
    if (direct) {
        *audio = embedded("Audio", audio_stream);
    } else {
        const size_t at = pb.url.find("AudioStreamIndex=");
        *audio = at != std::string::npos ? std::atoi(pb.url.c_str() + at + 17) : pb.default_audio;
    }
    *subtitle = -2;
    nuvio_sub_track t;
    if (subtitle_track < 0) {
        *subtitle = -1;   /* off */
    } else if (nuvio_subs_track(subtitle_track, &t) == 0 && t.external) {
        int k = 0;   /* its place among the external tracks */
        for (int i = 0; i < subtitle_track; i++) {
            nuvio_sub_track o;
            if (nuvio_subs_track(i, &o) == 0 && o.external)
                k++;
        }
        const std::vector<int> files = request_subtitle_indices(pb);
        if (k < (int)files.size())   /* (one downloaded during playback comes after them: not told) */
            *subtitle = files[k];
    } else if (direct && nuvio_subs_track(subtitle_track, &t) == 0 && embedded("Subtitle", t.stream) >= 0) {
        *subtitle = embedded("Subtitle", t.stream);
    }
}

/* Progress every 10 s, as Jellyfin's own clients do, and at once when playback
 * pauses or resumes (a phone controlling the PS5 shows the state it is in) or
 * another audio or subtitle track is chosen (the server remembers the choice). */
void *reporter_thread(void *)
{
    double t = 0, last_report = 0;
    bool reported_paused = false;
    int reported_audio = -1, reported_subtitle = -2;
    while (s_session.active) {
        for (int i = 0; i < 10 && s_session.active; i++)   /* 250 ms, but gone at once when playback ends */
            usleep(25 * 1000);
        t += 0.25;
        if (!s_session.active)
            break;
        double pos;
        bool heard, paused;
        int audio_stream, subtitle_track;
        jf::Playback pb;   /* the version playing (Versjon may switch it) */
        {
            std::lock_guard<std::mutex> g(s_session.lock);
            pb = s_session.pb;
            pos = s_session.position;
            heard = s_session.heard;
            paused = s_session.paused;
            audio_stream = s_session.audio_stream;
            subtitle_track = s_session.subtitle_track;
        }
        int audio = -1, subtitle = -2;   /* (not known before the player has started) */
        if (heard) {
            server_tracks(pb, audio_stream, subtitle_track, &audio, &subtitle);
            /* Kept for the series' memory. Once the player has closed its subtitles a
             * track cannot be told any more: the same track keeps what it was. */
            std::lock_guard<std::mutex> g(s_session.lock);
            s_session.server_audio = audio;
            if (subtitle != -2 || subtitle_track != s_session.subtitle_known_for) {
                s_session.server_subtitle = subtitle;
                s_session.subtitle_known_for = subtitle_track;
            }
        }
        if (paused != reported_paused || audio != reported_audio || subtitle != reported_subtitle ||
            t - last_report >= 10.0) {
            reported_paused = paused;
            reported_audio = audio;
            reported_subtitle = subtitle;
            last_report = t;
            s_session.client->report_progress(pb, ticks(pos), paused, audio, subtitle);
        }
    }
    return nullptr;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string runtime_label(int64_t runtime_ticks)
{
    const int min = (int)(runtime_ticks / jf::kTicksPerSecond / 60);
    char b[32];
    if (min >= 60 && min % 60 == 0)
        std::snprintf(b, sizeof b, T("%d t"), min / 60);
    else if (min >= 60)
        std::snprintf(b, sizeof b, T("%d t %d min"), min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return min > 0 ? b : "";
}

std::string method_label(const jf::Playback &pb)
{
    std::string video;
    for (const auto &s : pb.streams)
        if (s.type == "Video") {
            std::string codec = s.codec;
            for (char &c : codec)
                c = (char)std::toupper((unsigned char)c);
            char b[64];
            std::snprintf(b, sizeof b, "%s %dp%s", codec.c_str(), s.height,
                          s.video_range == "HDR" ? " HDR" : "");
            video = b;
            break;
        }
    const char *how = pb.play_method == "DirectPlay" ? T("Direktespilling")
                      : pb.play_method == "DirectStream" ? T("Direktestrøm") : T("Transkodet av serveren");
    return video.empty() ? how : std::string(how) + " \xC2\xB7 " + video;
}

cJSON *episode_json(jf::Client &c, const jf::Item &e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "season", e.parent_index);
    cJSON_AddNumberToObject(o, "episode", e.index);
    cJSON_AddStringToObject(o, "title", e.name.c_str());
    cJSON_AddStringToObject(o, "thumbnail", c.image_url(e.id, "Primary", e.primary_tag, 480).c_str());
    cJSON_AddStringToObject(o, "videoId", e.id.c_str());
    cJSON_AddStringToObject(o, "overview", e.overview.c_str());
    cJSON_AddItemToObject(o, "watched", cJSON_CreateBool(e.played));
    cJSON_AddNumberToObject(o, "progress", e.played_percent);
    cJSON_AddStringToObject(o, "runtime", runtime_label(e.runtime_ticks).c_str());
    cJSON_AddStringToObject(o, "blurhash", e.primary_blurhash.c_str());
    return o;
}

/* The DualSense light bar's colour for a title: a BlurHash starts with the
 * picture's average colour (characters 2-5, base 83, sRGB). Lifted so a dark
 * picture still glows; 0 when there is nothing to go on. */
uint32_t light_of(const std::string &hash)
{
    static const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";
    if (hash.size() < 6)
        return 0;
    uint32_t v = 0;
    for (size_t i = 2; i < 6; i++) {
        const char *p = std::strchr(digits, hash[i]);
        if (!p)
            return 0;
        v = v * 83 + (uint32_t)(p - digits);
    }
    /* The picture's average is usually muted, and the LED shows a muted colour
     * as white: keep its hue, at full saturation and brightness. Grey has no
     * hue to keep, so that gets the brand's blue. */
    const float r = (float)((v >> 16) & 255) / 255.f, g = (float)((v >> 8) & 255) / 255.f, b = (float)(v & 255) / 255.f;
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), c = mx - mn;
    if (mx < 0.06f || c < 0.04f || c / mx < 0.12f)
        return 0x2f6bff;
    float h = mx == r ? std::fmod((g - b) / c, 6.f) : mx == g ? (b - r) / c + 2.f : (r - g) / c + 4.f;
    if (h < 0)
        h += 6.f;
    const float x = 1.f - std::fabs(std::fmod(h, 2.f) - 1.f);   /* HSV with S = V = 1 */
    float o[3];
    switch ((int)h) {
    case 0: o[0] = 1, o[1] = x, o[2] = 0; break;
    case 1: o[0] = x, o[1] = 1, o[2] = 0; break;
    case 2: o[0] = 0, o[1] = 1, o[2] = x; break;
    case 3: o[0] = 0, o[1] = x, o[2] = 1; break;
    case 4: o[0] = x, o[1] = 0, o[2] = 1; break;
    default: o[0] = 1, o[1] = 0, o[2] = x; break;
    }
    return ((uint32_t)(o[0] * 255) << 16) | ((uint32_t)(o[1] * 255) << 8) | (uint32_t)(o[2] * 255);
}

/* The request the Nuvio Player plays (see nuvio_request_parse). */
struct Extras {
    std::vector<jf::Segment> segments;
    std::vector<jf::Chapter> chapters;
    jf::Trickplay trickplay;
    std::vector<jf::LyricLine> lyrics;
    bool not_group = false;   /* not a SyncPlay group's item (a theme song) */
    /* "Ser du fortsatt på?" (ui/still_watching.h): episodes autoplayed and seconds
     * played since the viewer's last press, carried from the episode before. */
    int autoplay_count = 0;
    double autoplay_idle = 0;
    /* The series' remembered tracks in this title (remembered_tracks): server indices. */
    int keep_audio = -1;
    int keep_subtitle = -2;   /* -1 off */
};

std::string request_json(jf::Client &c, const jf::Item &it, const jf::Playback &pb,
                         const std::vector<jf::Item> &episodes, const Extras &ex)
{
    const std::vector<jf::Segment> &segs = ex.segments;
    const bool episode = it.type == "Episode", audio = it.type == "Audio";
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", it.id.c_str());
    if (pb.live) {
        /* A channel: what airs on it now names it (the player follows the guide from
         * here, app/livetv), its logo is the title's. */
        const livetv::GuideRef g = livetv::guide();
        const jf::Item *p = g->on_at(it.id, livetv::now());
        if (!p)
            p = it.now_on();
        cJSON_AddBoolToObject(o, "live", 1);
        cJSON_AddStringToObject(o, "description", p ? p->overview.c_str() : "");
        if (p && !p->primary_tag.empty())
            cJSON_AddStringToObject(o, "background", c.image_url(p->id, "Primary", p->primary_tag, 1920).c_str());
        if (it.type == "TvChannel")
            cJSON_AddStringToObject(o, "logo", c.image_url(it.id, "Primary", it.primary_tag, 400).c_str());
    }
    if (audio) {   /* the player's music screen: the album's cover, artist and album */
        cJSON_AddStringToObject(o, "artist", it.album_artist.c_str());
        cJSON_AddStringToObject(o, "album", it.album.c_str());
        const std::string cover = !it.album_primary_tag.empty()
                                      ? c.image_url(it.album_id, "Primary", it.album_primary_tag, 800)
                                      : c.image_url(it.id, "Primary", it.primary_tag, 800);
        cJSON_AddStringToObject(o, "cover", cover.c_str());
        cJSON_AddStringToObject(o, "coverBlurhash",
                                (!it.album_blurhash.empty() ? it.album_blurhash : it.primary_blurhash).c_str());
        cJSON_AddNumberToObject(o, "season", it.parent_index);   /* its place in the album queue */
        cJSON_AddNumberToObject(o, "episode", it.index);
    }
    cJSON_AddStringToObject(o, "url", pb.url.c_str());
    cJSON_AddStringToObject(o, "playMethod", pb.play_method.c_str());
    cJSON_AddStringToObject(o, "transcodeReasons", pb.transcode_reasons.c_str());
    cJSON_AddStringToObject(o, "title", (episode ? it.series_name : it.name).c_str());
    if (episode) {
        cJSON_AddStringToObject(o, "episodeTitle", it.name.c_str());
        cJSON_AddNumberToObject(o, "season", it.parent_index);
        cJSON_AddNumberToObject(o, "episode", it.index);
    }
    if (it.year)
        cJSON_AddStringToObject(o, "year", std::to_string(it.year).c_str());
    if (!pb.live)
        cJSON_AddStringToObject(o, "description", it.overview.c_str());
    std::string genres;
    for (const auto &g : it.genres)
        genres += (genres.empty() ? "" : ", ") + g;
    cJSON_AddStringToObject(o, "genres", genres.c_str());
    cJSON_AddStringToObject(o, "runtime", runtime_label(it.runtime_ticks).c_str());
    if (it.community_rating > 0) {
        char r[16];
        std::snprintf(r, sizeof r, "%.1f", it.community_rating);
        cJSON_AddStringToObject(o, "rating", r);
    }
    /* "live": a channel (zapping, its guide); a recording still being made is live too
     * (no end yet, no seeking) but no channel. */
    cJSON_AddStringToObject(o, "itemType", pb.live && it.type == "TvChannel" ? "live"
                                           : episode                        ? "series"
                                           : audio                          ? "audio"
                                                                            : "movie");
    {   /* the controller's light: the picture's colour */
        const std::string &hash = audio ? (!it.album_blurhash.empty() ? it.album_blurhash : it.primary_blurhash)
                                        : it.backdrop_blurhash;
        cJSON_AddNumberToObject(o, "lightColor", (double)light_of(hash));
    }
    if (!pb.live) {
        cJSON_AddStringToObject(o, "logo", c.image_url(it.logo_owner, "Logo", it.logo_tag, 800).c_str());
        cJSON_AddStringToObject(o, "poster", c.image_url(episode ? it.series_id : it.id, "Primary",
                                                          episode ? std::string() : it.primary_tag, 400).c_str());
        cJSON_AddStringToObject(o, "background",
                                c.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920).c_str());
    }
    if (episode)
        cJSON_AddStringToObject(o, "thumbnail", c.image_url(it.id, "Primary", it.primary_tag, 640).c_str());
    cJSON_AddNumberToObject(o, "startPosition", (double)it.position_ticks / jf::kTicksPerSecond);

    cJSON *stream = cJSON_CreateObject();
    cJSON_AddStringToObject(stream, "title", method_label(pb).c_str());
    cJSON_AddStringToObject(stream, "addon", "Jellyfin");
    cJSON_AddItemToObject(o, "stream", stream);

    /* Every version of the title, the chosen (best) one first: the player can switch
     * between them (Lyd og undertekster -> Versjon) at the same moment. */
    cJSON *sources = cJSON_CreateArray();
    for (const jf::Version &v : pb.versions) {
        cJSON *src = cJSON_CreateObject();
        cJSON_AddStringToObject(src, "id", v.id.c_str());
        std::string title = v.label.empty() ? v.name : v.label;
        if (!v.name.empty() && v.name != it.name && title != v.name)
            title = v.name + " \xC2\xB7 " + title;   /* Jellyfin's own name for the version */
        cJSON_AddStringToObject(src, "title", title.c_str());
        cJSON_AddStringToObject(src, "description",
                                T(v.play_method == "DirectPlay" ? "Direktespilling"
                                  : v.play_method == "DirectStream" ? "Direktestr\xC3\xB8m" : "Transkodet av serveren"));
        cJSON_AddStringToObject(src, "addon", "Jellyfin");
        cJSON_AddStringToObject(src, "url", v.url.c_str());
        if (v.play_method == "DirectPlay") {
            /* Subtitle auto-sync: each audio track the server gives alone, and each embedded
             * text track as a file, by its place in the file (the player's stream index). */
            cJSON *sync_audio = cJSON_CreateArray(), *sync_subs = cJSON_CreateArray();
            for (const jf::MediaStream &m : v.streams) {
                const std::string u = m.type == "Audio" ? c.sync_audio_url(pb.item_id, v, m)
                                                        : c.subtitle_file_url(pb.item_id, v, m);
                const int at = jf::Client::container_index(v, m);
                if (u.empty() || at < 0)
                    continue;
                cJSON *e = cJSON_CreateObject();
                cJSON_AddNumberToObject(e, "stream", at);
                cJSON_AddStringToObject(e, "url", u.c_str());
                cJSON_AddItemToArray(m.type == "Audio" ? sync_audio : sync_subs, e);
            }
            cJSON_AddItemToObject(src, "syncAudio", sync_audio);
            cJSON_AddItemToObject(src, "syncSubtitles", sync_subs);
        }
        cJSON_AddItemToArray(sources, src);
    }
    cJSON_AddItemToObject(o, "sources", sources);
    cJSON_AddStringToObject(o, "syncStop", c.sync_stop_url().c_str());   /* auto-sync's encoders */

    /* Embedded tracks come out of the container; external text subtitles are fetched,
     * and so is an embedded one a transcoded version's server serves as a file: that
     * one only with its own version ("source"). */
    cJSON *subs = cJSON_CreateArray();
    auto add_sub = [subs](const jf::MediaStream &s, const std::string &source) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "url", s.delivery_url.c_str());
        cJSON_AddStringToObject(e, "lang", s.language.c_str());
        cJSON_AddStringToObject(e, "label", s.display_title.c_str());
        if (!source.empty())
            cJSON_AddStringToObject(e, "source", source.c_str());
        cJSON_AddItemToArray(subs, e);
    };
    for (const auto &s : pb.streams)
        if (s.type == "Subtitle" && s.is_external && !s.delivery_url.empty())
            add_sub(s, std::string());
    for (const jf::Version &v : pb.versions)
        for (const auto &s : v.served_subtitles)
            add_sub(s, v.id);
    cJSON_AddItemToObject(o, "subtitles", subs);

    if (!episodes.empty()) {   /* a series' episodes, an album, or a queue */
        cJSON *eps = cJSON_CreateArray();
        size_t here = episodes.size();
        for (size_t i = 0; i < episodes.size(); i++) {
            if (episodes[i].id == it.id)
                here = i;
            if (i < 400)   /* every season: the player's picker switches between them */
                cJSON_AddItemToArray(eps, episode_json(c, episodes[i]));
        }
        cJSON_AddItemToObject(o, "episodes", eps);
        if (here + 1 < episodes.size())
            cJSON_AddItemToObject(o, "nextEpisode", episode_json(c, episodes[here + 1]));
    }

    /* Segments come from the server's detection (Intro Skipper), which can be
     * badly wrong (two-story cartoons get minutes-long "credits" mid-episode).
     * Only plausible ones are used: an intro early and at most 3 min; credits
     * ending near the end and no longer than 3 min or 12 % of the runtime. */
    const double runtime = (double)it.runtime_ticks / jf::kTicksPerSecond;
    auto plausible = [runtime](const jf::Segment &sg) {
        const double len = sg.end - sg.start;
        if (len <= 0 || runtime <= 0)
            return len > 0;
        const std::string t = lower(sg.type);
        if (t == "outro" || t == "credits")
            return len <= std::max(180.0, 0.12 * runtime) && sg.end >= runtime - 120.0;
        if (t == "intro" || t == "recap")
            return len <= 180.0 && sg.start <= runtime * 0.4;
        return true;
    };
    cJSON *skips = cJSON_CreateArray();
    for (const auto &sg : segs) {
        if (!plausible(sg)) {
            evo_bt("jelly5: ignoring implausible %s segment %.0f-%.0f s (runtime %.0f s)", sg.type.c_str(), sg.start,
                   sg.end, runtime);
            continue;
        }
        cJSON *k = cJSON_CreateObject();
        const std::string t = lower(sg.type);
        cJSON_AddStringToObject(k, "type", t == "outro" ? "outro" : t.c_str());
        cJSON_AddNumberToObject(k, "start", sg.start);
        cJSON_AddNumberToObject(k, "end", sg.end);
        cJSON_AddItemToArray(skips, k);
    }
    cJSON_AddItemToObject(o, "skipIntervals", skips);

    /* Chapters (marks on the bar, their name while scrubbing); a single "chapter"
     * spanning the whole title says nothing. */
    if (ex.chapters.size() > 1) {
        cJSON *chs = cJSON_CreateArray();
        for (size_t i = 0; i < ex.chapters.size(); i++) {
            const jf::Chapter &ch = ex.chapters[i];
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "start", ch.start);
            cJSON_AddStringToObject(e, "name", ch.name.c_str());
            if (!ch.image_tag.empty())   /* Jellyfin's chapter image, when it extracted one */
                cJSON_AddStringToObject(
                    e, "image",
                    c.image_url(it.id, ("Chapter/" + std::to_string(i)).c_str(), ch.image_tag, 480).c_str());
            cJSON_AddItemToArray(chs, e);
        }
        cJSON_AddItemToObject(o, "chapters", chs);
    }
    /* Lyrics (music): timed lines follow the song, untimed ones just show. */
    if (!ex.lyrics.empty()) {
        cJSON *ly = cJSON_CreateArray();
        for (const auto &l : ex.lyrics) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "start", l.start);
            cJSON_AddStringToObject(e, "text", l.text.c_str());
            if (!l.cues.empty()) {   /* word by word */
                cJSON *cs = cJSON_CreateArray();
                for (const auto &c : l.cues) {
                    cJSON *ce = cJSON_CreateObject();
                    cJSON_AddNumberToObject(ce, "start", c.start);
                    cJSON_AddNumberToObject(ce, "from", (double)c.from);
                    cJSON_AddNumberToObject(ce, "to", (double)c.to);
                    cJSON_AddItemToArray(cs, ce);
                }
                cJSON_AddItemToObject(e, "cues", cs);
            }
            cJSON_AddItemToArray(ly, e);
        }
        cJSON_AddItemToObject(o, "lyrics", ly);
    }
    /* Scrub previews, when the server has made them. */
    if (ex.trickplay.valid()) {
        const jf::Trickplay &t = ex.trickplay;
        cJSON *tp = cJSON_CreateObject();
        cJSON_AddNumberToObject(tp, "width", t.width);
        cJSON_AddNumberToObject(tp, "height", t.height);
        cJSON_AddNumberToObject(tp, "tileWidth", t.tile_w);
        cJSON_AddNumberToObject(tp, "tileHeight", t.tile_h);
        cJSON_AddNumberToObject(tp, "count", t.count);
        cJSON_AddNumberToObject(tp, "interval", t.interval);
        cJSON_AddStringToObject(tp, "urlBase", t.url_base.c_str());
        cJSON_AddStringToObject(tp, "urlQuery", t.url_query.c_str());
        cJSON_AddNumberToObject(tp, "sheetTicks", (double)t.sheet_ticks);
        cJSON_AddNumberToObject(tp, "firstTicks", (double)t.first_ticks);
        cJSON_AddItemToObject(o, "trickplay", tp);
    }

    /* The account's preferences (settings), as the player's track rules. */
    const settings::All set = settings::get();
    cJSON *prefs = cJSON_CreateObject();
    cJSON *al = cJSON_CreateArray(), *sl = cJSON_CreateArray();
    auto langs = [](cJSON *arr, const std::string &first) {
        std::vector<std::string> l;
        if (!first.empty())
            l.push_back(first);
        if (first == "nor" || first == "nob" || first == "nno")
            l.insert(l.end(), {"nob", "nor", "no", "nb"});
        for (const auto &x : l)
            cJSON_AddItemToArray(arr, cJSON_CreateString(x.c_str()));
    };
    langs(al, set.server.audio_language);
    langs(sl, set.server.subtitle_language);
    const std::string mode = set.server.subtitle_mode;
    cJSON_AddItemToObject(prefs, "audioLanguages", al);
    cJSON_AddItemToObject(prefs, "subtitleLanguages", sl);
    /* Always: on in the preferred language. OnlyForced/Default/Smart: forced
     * subtitles only (Default/Smart also turn them on when the audio is not in
     * the viewer's language - the player has no such rule yet). None: off. */
    cJSON_AddItemToObject(prefs, "subtitlesEnabled", cJSON_CreateBool(mode == "Always"));
    cJSON_AddItemToObject(prefs, "forcedOnlyWhenOff", cJSON_CreateBool(mode != "None"));
    cJSON_AddItemToObject(prefs, "autoplayNext", cJSON_CreateBool(set.server.autoplay_next));
    cJSON_AddItemToObject(prefs, "skipIntro", cJSON_CreateBool(1));
    cJSON *seg = cJSON_CreateObject();   /* Innstillinger: what to do at each segment type */
    for (int t = 0; t < segments::TypeCount; t++)
        cJSON_AddStringToObject(seg, segments::key_of(t), segments::action_key(set.local.segment[t]));
    cJSON_AddItemToObject(prefs, "segments", seg);
    /* Innstillinger: Spør om du fortsatt ser på (Av, after 3 episodes, after 2 hours). */
    cJSON_AddNumberToObject(prefs, "stillWatchingEpisodes", set.local.still_watching == 1 ? 3 : 0);
    cJSON_AddNumberToObject(prefs, "stillWatchingSeconds", set.local.still_watching == 2 ? 2 * 3600 : 0);
    cJSON_AddItemToObject(prefs, "clock24h", cJSON_CreateBool(1));
    cJSON *style = cJSON_CreateObject();   /* how text subtitles look (Innstillinger) */
    cJSON_AddNumberToObject(style, "size", set.local.sub_size);
    cJSON_AddNumberToObject(style, "offset", set.local.sub_offset);
    cJSON_AddNumberToObject(style, "background", set.local.sub_background);
    cJSON_AddItemToObject(style, "outline", cJSON_CreateBool(set.local.sub_outline));
    cJSON_AddItemToObject(prefs, "subtitleStyle", style);
    /* The series' remembered tracks, as the player finds them: the file's own stream
     * (played directly), or the request's subtitle file by its place. */
    const bool direct = pb.play_method == "DirectPlay";
    if (ex.keep_audio >= 0 && direct)
        cJSON_AddNumberToObject(prefs, "keepAudioStream", container_index(pb, ex.keep_audio));
    if (ex.keep_subtitle >= -1) {
        cJSON *k = cJSON_CreateObject();
        const std::vector<int> files = request_subtitle_indices(pb);
        const auto file = std::find(files.begin(), files.end(), ex.keep_subtitle);
        if (ex.keep_subtitle == -1) {
            cJSON_AddTrueToObject(k, "off");
        } else if (file != files.end()) {
            cJSON_AddNumberToObject(k, "external", (double)(file - files.begin()));
        } else if (direct) {
            for (const jf::MediaStream &m : pb.streams)
                if (m.type == "Subtitle" && !m.is_external && m.index == ex.keep_subtitle)
                    cJSON_AddNumberToObject(k, "stream", container_index(pb, m.index));
        }
        if (k->child)
            cJSON_AddItemToObject(prefs, "keepSubtitle", k);
        else
            cJSON_Delete(k);
    }
    cJSON_AddItemToObject(o, "prefs", prefs);

    /* The player's interface text, in the interface's language (built per playback). */
    const char *const kStrings[][2] = {
        {"advanced", T("Avansert")}, {"advanced_style", T("Stil og timing")},
        {"advanced_hint", T("Forsinkelse, størrelse, posisjon \xE2\x80\xA6")},
        {"audio", T("Lyd")}, {"background", T("Bakgrunn")}, {"bold", T("Fet skrift")}, {"built_in", T("Innebygd")},
        {"default", T("Standard")}, {"delay", T("Forsinkelse")}, {"ends_at", T("Slutter kl. %1$s")},
        {"episode", "Episode"}, {"forced", T("Tvungen")}, {"go_back", T("Tilbake")}, {"language", T("Språk")},
        {"loading", T("Laster \xE2\x80\xA6")}, {"next_episode", T("Neste episode")}, {"connection_lost", T("Mistet kontakten med serveren \xE2\x80\x93 pr\xC3\xB8ver igjen \xE2\x80\xA6")},
        {"connection_gone", T("Fikk ikke kontakt med serveren igjen.")}, {"next_in", T("Spilles om %1$s")},
        {"no_audio_tracks", T("Ingen andre lydspor")}, {"no_subtitles", T("Ingen undertekster for denne strømmen")},
        {"off", T("Av")}, {"on", T("På")}, {"outline", T("Kontur")}, {"play", T("Spill av")},
        {"playback_error", T("Avspillingsfeil")}, {"playing", T("Spiller")}, {"position", T("Posisjon")},
        {"press_to_play_next", T("Trykk \xE2\x9C\x95 for å spille")}, {"season", T("Sesong")}, {"size", T("Størrelse")},
        {"skip_intro", T("Hopp over intro")}, {"skip_preview", T("Hopp over forhåndsvisning")},
        {"skip_recap", T("Hopp over oppsummering")}, {"sources", T("Kilder")}, {"specials", T("Spesialer")},
        {"subtitles_off", T("Undertekster er av")}, {"subtitles", T("Undertekster")}, {"track", T("Spor")},
        {"unavailable", T("Utilgjengelig")}, {"unknown_language", T("Ukjent")}, {"upcoming", T("Kommer")},
        {"youre_watching", T("Du ser på")}, {"addon", T("Kilde")},
        {"open_failed", pb.live ? T("Kanalen kunne ikke åpnes. Den sender kanskje ikke akkurat nå.")
                                : T("Strømmen kunne ikke åpnes. Kilden er kanskje ikke tilgjengelig.")},
        {"opening", T("Åpner \xE2\x80\xA6")}, {"buffering", T("Bufrer \xE2\x80\xA6")},
        {"decode_failed", T("Denne videoen kunne ikke dekodes.")},
        {"did_not_start", T("Avspillingen startet ikke. Kilden er kanskje for treg eller utilgjengelig.")},
        {"audio_switch_failed", T("Kunne ikke spille av lydsporet")},
        {"decoder_hung", T("Maskinvaredekoderen svarer ikke. Start Jelly5 på nytt for å bruke den igjen.")},
    };
    cJSON *strings = cJSON_CreateObject();
    for (const auto &kv : kStrings)
        cJSON_AddStringToObject(strings, kv[0], kv[1]);
    cJSON_AddItemToObject(o, "strings", strings);

    if (ex.not_group)
        cJSON_AddBoolToObject(o, "notGroup", 1);
    cJSON_AddNumberToObject(o, "autoplayCount", ex.autoplay_count);
    cJSON_AddNumberToObject(o, "autoplayIdle", ex.autoplay_idle);
    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    std::free(s);
    cJSON_Delete(o);
    return out;
}

/* Ended as a SyncPlay group's item (PlayerUi::playback_ended): the group's queue goes on. */
bool group_end_of(const std::string &result)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const bool out = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "groupEnd"));
    cJSON_Delete(j);
    return out;
}

/* What the viewer asked for at the end: another episode by number, or nothing. */
bool next_from_result(const std::string &result, int *season, int *episode, std::string *id)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "action");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "type");
    bool want = false;
    if (cJSON_IsString(type) && (std::string(type->valuestring) == "next" ||
                                 std::string(type->valuestring) == "episode")) {
        *season = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(a, "season"));
        *episode = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(a, "episode"));
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, "videoId");
        *id = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
        want = true;
    }
    cJSON_Delete(j);
    return want;
}

/* The item the player asked for: by its id when the result carries one (season and
 * episode numbers can be missing - all 0 - or repeat), else by the numbers. */
bool is_pick(const jf::Item &e, int season, int number, const std::string &id)
{
    return !id.empty() ? e.id == id : e.parent_index == season && e.index == number;
}

/* The player's still-watching count and time at its end (ui/still_watching.h). */
void still_watching_from_result(const std::string &result, int *count, double *idle)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "autoplayCount");
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(j, "autoplayIdle");
    *count = cJSON_IsNumber(c) ? std::max(0, jf::to_int<int>(c->valuedouble)) : 0;
    *idle = cJSON_IsNumber(i) && i->valuedouble > 0 ? i->valuedouble : 0;
    cJSON_Delete(j);
}

double position_from_result(const std::string &result, double fallback)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "position");
    const double pos = cJSON_IsNumber(p) ? p->valuedouble : fallback;
    cJSON_Delete(j);
    return pos;
}

/* ---- the series' tracks (app/track_memory) ---------------------------------------- */

/* The tracks the viewer chose earlier in this episode's series, as this file's own
 * (server indices): *audio -1 and *subtitle -2 where none is remembered or the file
 * has no equivalent; *subtitle -1: off. Only what the account lets be remembered. */
void remembered_tracks(const jf::Client &c, const jf::Item &item, const jf::Playback &pb, int *audio,
                       int *subtitle)
{
    *audio = -1;
    *subtitle = -2;
    track_memory::Choice ch;
    if (item.type != "Episode" || !track_memory::get(c.user_id(), item.series_id, &ch))
        return;
    const jf::UserPrefs prefs = settings::get().server;
    if (ch.audio_set && prefs.remember_audio)
        *audio = track_memory::match(pb.streams, "Audio", ch.audio);
    if (ch.subtitle_set && prefs.remember_subtitles) {
        /* An encoded stream brings no picture subtitles to the player (ask_with_tracks):
         * there the text one in the same language. */
        track_memory::Track want = ch.subtitle;
        if (pb.play_method != "DirectPlay")
            want.image = false;
        const int m = ch.subtitle_off ? -1 : track_memory::match(pb.streams, "Subtitle", want);
        *subtitle = ch.subtitle_off ? -1 : m >= 0 ? m : -2;
    }
}

/* A stream the server encodes carries the audio it picks and leaves out a subtitle
 * turned off: when the remembered tracks are not its defaults, the version chosen
 * is asked for again with them (the server applies the indices only to a version
 * it is named). A text subtitle is still served as a file the player draws
 * itself; a picture one is not asked for, as the server would burn it into the
 * picture (the player shows those only from the file itself). One the request
 * already carries as a file is not asked for either, unless the server's default
 * is a picture one it would burn in. The other versions stay, under the new play
 * session; the first answer's encoding is stopped. */
void ask_with_tracks(jf::Client &c, const jf::Item &item, int64_t max_bitrate, int audio, int subtitle,
                     jf::Playback *pb)
{
    if (pb->play_method == "DirectPlay" || pb->versions.empty())
        return;   /* the player reads the file and picks the tracks itself */
    int sub = subtitle == -1 ? -1 : -2;
    for (const jf::MediaStream &m : pb->streams)
        if (subtitle >= 0 && m.index == subtitle && m.type == "Subtitle" && m.is_text)
            sub = subtitle;
    bool default_picture = false;
    for (const jf::MediaStream &m : pb->streams)
        if (m.type == "Subtitle" && m.index == pb->default_subtitle && pb->default_subtitle >= 0 && !m.is_text)
            default_picture = true;
    const std::vector<int> files = request_subtitle_indices(*pb);
    if (sub >= 0 && !default_picture && std::find(files.begin(), files.end(), sub) != files.end())
        sub = -2;   /* the player gets it as a file anyway (keepSubtitle) */
    if ((audio < 0 || audio == pb->default_audio) && (sub == -2 || sub == pb->default_subtitle))
        return;
    jf::Playback again;
    if (!c.playback_info(item.id, item.position_ticks, audio, sub, &again, max_bitrate, pb->media_source_id) ||
        again.media_source_id != pb->media_source_id || again.versions.empty()) {
        evo_bt("jelly5: playback info with the series' tracks failed (%s): the server's tracks",
               c.last_error().c_str());
        return;
    }
    for (size_t i = 1; i < pb->versions.size(); i++) {
        jf::Version v = pb->versions[i];
        for (size_t at; !pb->play_session_id.empty() && (at = v.url.find(pb->play_session_id)) != std::string::npos;)
            v.url.replace(at, pb->play_session_id.size(), again.play_session_id);
        again.versions.push_back(std::move(v));
    }
    c.stop_encoding(*pb);   /* the first answer's transcode, if it started one */
    *pb = std::move(again);
}

/* After a title: the tracks the viewer chose in it are the series' from now on. */
void remember_tracks(const jf::Client &c, const jf::Item &item, const std::string &result)
{
    if (item.type != "Episode" || item.series_id.empty())
        return;
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "tracks");
    const bool audio_picked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "audioPicked"));
    const bool subtitle_picked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "subtitlePicked"));
    cJSON_Delete(j);
    if (!audio_picked && !subtitle_picked)
        return;
    jf::Playback pb;
    int audio, subtitle;
    {
        std::lock_guard<std::mutex> g(s_session.lock);
        pb = s_session.pb;   /* the version playing last */
        audio = s_session.server_audio;
        subtitle = s_session.subtitle_known_for == s_session.subtitle_track ? s_session.server_subtitle : -2;
    }
    const jf::UserPrefs prefs = settings::get().server;
    auto stream = [&pb](const char *type, int index) -> const jf::MediaStream * {
        for (const jf::MediaStream &m : pb.streams)
            if (m.type == type && m.index == index)
                return &m;
        return nullptr;
    };
    /* Audio only from the file itself: an encoded stream's is the one the server picked. */
    const jf::MediaStream *a = pb.play_method == "DirectPlay" ? stream("Audio", audio) : nullptr;
    if (audio_picked && prefs.remember_audio && a) {
        track_memory::set_audio(c.user_id(), item.series_id, track_memory::describe(*a));
        evo_bt("jelly5: the series' audio from now on: stream %d", audio);
    }
    if (subtitle_picked && prefs.remember_subtitles && subtitle == -1) {
        track_memory::set_subtitle(c.user_id(), item.series_id, true, track_memory::Track());
        evo_bt("jelly5: the series' subtitles from now on: off");
    } else if (const jf::MediaStream *s = subtitle_picked && prefs.remember_subtitles ? stream("Subtitle", subtitle)
                                                                                         : nullptr) {
        track_memory::set_subtitle(c.user_id(), item.series_id, false, track_memory::describe(*s));
        evo_bt("jelly5: the series' subtitles from now on: stream %d", subtitle);
    }
}

} // namespace

/* ---- subtitle search -------------------------------------------------------------- */

namespace jelly5_subs {
namespace {
std::mutex s_lock;
State s_search = Idle, s_download = Idle;
std::vector<jf::RemoteSubtitle> s_found;
std::string s_lang;
int s_track = -1;
unsigned s_gen = 0;              /* a new search or title drops older answers */
unsigned s_title = 0;            /* a new title drops downloads for the last one */

/* A new title starts: what was searched or fetched for the last one is dropped. */
void new_title()
{
    std::lock_guard<std::mutex> g(s_lock);
    s_gen++;
    s_title++;
    s_search = s_download = Idle;
    s_found.clear();
    s_track = -1;
}
}

bool available()
{
    return s_session.active && s_session.client && s_session.client->can_search_subtitles() && !s_session.pb.live;
}

void search(const std::string &language)
{
    if (!available())
        return;
    jf::Client *c = s_session.client;
    const std::string id = s_session.pb.item_id;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        gen = ++s_gen;
        s_search = Busy;
        s_found.clear();
        s_lang = language;
    }
    const bool started = jelly5::spawn([c, id, language, gen] {
        std::vector<jf::RemoteSubtitle> found = c->search_subtitles(id, language);
        /* Best first: a match for this very file, then the most downloaded. */
        std::stable_sort(found.begin(), found.end(), [](const jf::RemoteSubtitle &a, const jf::RemoteSubtitle &b) {
            return a.hash_match != b.hash_match ? a.hash_match : a.downloads > b.downloads;
        });
        std::lock_guard<std::mutex> g(s_lock);
        if (gen != s_gen)
            return;
        s_found = std::move(found);
        s_search = Done;
    });
    if (!started) {   /* nothing found, as when the search finds nothing */
        std::lock_guard<std::mutex> g(s_lock);
        if (gen == s_gen)
            s_search = Done;
    }
}

State results(std::vector<jf::RemoteSubtitle> *out, std::string *language)
{
    std::lock_guard<std::mutex> g(s_lock);
    *out = s_found;
    *language = s_lang;
    return s_search;
}

void download(const jf::RemoteSubtitle &sub)
{
    if (!available())
        return;
    jf::Client *c = s_session.client;
    jf::Playback pb;
    {
        std::lock_guard<std::mutex> g(s_session.lock);
        pb = s_session.pb;
    }
    unsigned title;
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_download = Busy;
        s_track = -1;
        title = s_title;
    }
    const bool started = jelly5::spawn([c, pb, sub, title] {
        /* The new file shows as an external subtitle stream the next time the server
         * is asked how to play the title: the one that was not there before. */
        std::set<std::string> known;
        for (const jf::MediaStream &m : pb.streams)
            if (m.type == "Subtitle" && m.is_external)
                known.insert(m.delivery_url.substr(0, m.delivery_url.find('?')));
        int track = -1;
        bool gone = false;   /* the title it was for has ended */
        if (c->download_subtitle(pb.item_id, sub.id)) {
            for (int attempt = 0; attempt < 5 && track < 0 && !gone; attempt++) {
                if (attempt)
                    usleep(1000 * 1000);   /* the server may still be writing it */
                jf::Playback now;
                if (!c->playback_info(pb.item_id, 0, -1, -1, &now))
                    continue;
                const std::vector<jf::MediaStream> *streams = &now.streams;   /* the version playing */
                for (const jf::Version &v : now.versions)
                    if (v.id == pb.media_source_id)
                        streams = &v.streams;
                for (const jf::MediaStream &m : *streams)
                    /* A downloaded subtitle is a file beside the media (IsExternal); a
                     * transcode's served embedded tracks are not new ones. */
                    if (m.type == "Subtitle" && m.is_external && !m.delivery_url.empty() &&
                        !known.count(m.delivery_url.substr(0, m.delivery_url.find('?')))) {
                        /* Under the lock: a new title (new_title) cannot start in between
                         * and get this one's subtitle (adding only queues it). */
                        std::lock_guard<std::mutex> g(s_lock);
                        if (title == s_title)
                            track = nuvio_subs_add_external(m.delivery_url.c_str(), m.language.c_str(),
                                                            sub.name.c_str(), "");
                        else
                            gone = true;
                        break;
                    }
            }
        }
        evo_bt("jelly5: subtitle download %s -> track %d", track >= 0 ? "ok" : "failed", track);
        std::lock_guard<std::mutex> g(s_lock);
        if (title != s_title)
            return;
        s_track = track;
        s_download = track >= 0 ? Done : Failed;
    });
    if (!started) {
        std::lock_guard<std::mutex> g(s_lock);
        s_download = Failed;
    }
}

State download_state(int *track)
{
    std::lock_guard<std::mutex> g(s_lock);
    const State st = s_download;
    *track = s_track;
    if (st == Done || st == Failed)
        s_download = Idle;   /* reported once */
    return st;
}
} // namespace jelly5_subs

extern "C" void jelly5_playback_progress(double position, double duration, bool paused, int audio_stream,
                                         int subtitle_track)
{
    std::lock_guard<std::mutex> g(s_session.lock);
    s_session.position = position;
    s_session.duration = duration;
    s_session.heard = true;
    s_session.paused = paused;
    s_session.audio_stream = audio_stream;
    s_session.subtitle_track = subtitle_track;
}

/* The player switched to version `index` (Versjon): the reports name the one playing
 * now. All versions share the play session, and stopping its encodings stops them all:
 * a transcode left behind is stopped here only when the new version plays directly,
 * else by the stop at the end (which now sees a transcoded version, not the first). */
extern "C" void jelly5_playback_source(int index)
{
    jf::Playback left;
    {
        std::lock_guard<std::mutex> g(s_session.lock);
        if (!s_session.active || index < 0 || index >= (int)s_session.pb.versions.size())
            return;
        const jf::Version &v = s_session.pb.versions[index];
        if (v.id == s_session.pb.media_source_id)
            return;
        left = s_session.pb;
        s_session.pb.media_source_id = v.id;
        s_session.pb.play_method = v.play_method;
        s_session.pb.url = v.url;
        s_session.pb.streams = v.streams;   /* its tracks, not the first version's */
        s_session.pb.default_audio = v.default_audio;
        s_session.pb.default_subtitle = v.default_subtitle;
        /* The tracks known so far were the last file's (the reporter tells the new ones). */
        s_session.server_audio = -1;
        s_session.server_subtitle = -2;
        s_session.subtitle_known_for = -1;
        if (v.play_method != "DirectPlay" || left.play_method == "DirectPlay")
            return;
    }
    jf::Client *c = s_session.client;
    jelly5::spawn([c, left] { c->stop_encoding(left); });
}

extern "C" void jelly5_playback_finished(const char *result_json)
{
    std::lock_guard<std::mutex> g(s_session.lock);
    s_session.result = result_json ? result_json : "";
}

/* An album's tracks as the player's queue: disc as "season", track as "episode".
 * Missing or repeated numbers (and shuffle) number them in play order instead. */
std::vector<jf::Item> album_queue(jf::Client &client, jf::Item *item, bool shuffle)
{
    std::vector<jf::Item> tracks;
    for (jf::Item &t : client.children(item->album_id, "ParentIndexNumber,IndexNumber,SortName", 1000))
        if (t.type == "Audio")
            tracks.push_back(std::move(t));
    if (tracks.empty())
        return tracks;
    if (shuffle) {
        /* This track first, the rest in random order. */
        std::srand((unsigned)time(nullptr));
        for (size_t i = 0; i < tracks.size(); i++)
            if (tracks[i].id == item->id)
                std::swap(tracks[0], tracks[i]);
        for (size_t i = tracks.size() - 1; i > 1; i--)
            std::swap(tracks[i], tracks[1 + std::rand() % i]);
    }
    std::set<std::pair<int, int>> seen;
    bool renumber = shuffle;
    for (const jf::Item &t : tracks)
        if (t.index <= 0 || !seen.insert({t.parent_index, t.index}).second)
            renumber = true;
    for (size_t i = 0; i < tracks.size(); i++) {
        if (renumber) {
            tracks[i].parent_index = 1;
            tracks[i].index = (int)i + 1;
        }
        if (tracks[i].id == item->id)
            *item = tracks[i];   /* the numbering the queue uses */
    }
    return tracks;
}

static bool play_chain_tracks(jf::Client &client, jf::Item item, std::vector<jf::Item> episodes, std::string *error);
static void music_queue_end();

/* The chain, then out of player mode (music keeps it on between tracks). */
static bool play_chain(jf::Client &client, jf::Item item, std::vector<jf::Item> episodes, std::string *error)
{
    const bool ok = play_chain_tracks(client, std::move(item), std::move(episodes), error);
    music_queue_end();
    nuvio_player_leave();
    return ok;
}

bool jelly5_play(jf::Client &client, const jf::Item &first, std::string *error, bool shuffle)
{
    jf::Item item = first;
    std::vector<jf::Item> episodes;
    if (item.type == "Episode" && !item.series_id.empty())
        episodes = client.episodes(item.series_id, std::string());
    if (item.type == "Audio" && !item.album_id.empty())
        episodes = album_queue(client, &item, shuffle);
    return play_chain(client, item, std::move(episodes), error);
}

/* The queue as the player's episode list: one "season", numbered in play order. */
bool jelly5_play_queue(jf::Client &client, const std::vector<jf::Item> &queue, size_t start, std::string *error)
{
    if (queue.empty())
        return false;
    if (queue.size() == 1)
        return jelly5_play(client, queue.front(), error);
    std::vector<jf::Item> q = queue;
    for (size_t i = 0; i < q.size(); i++) {
        q[i].parent_index = 1;
        q[i].index = (int)i + 1;
    }
    const jf::Item item = q[std::min(start, q.size() - 1)];
    return play_chain(client, item, std::move(q), error);
}

/* ---- the music queue ---------------------------------------------------------
 * Music plays through `queue` in `order` (the queue's own order, or shuffled);
 * after each track this decides what comes next - repeat one, repeat all, a
 * jump picked on the queue sheet - instead of the player's album order. */
namespace {
struct MusicQueue {
    std::mutex lock;
    std::vector<jf::Item> queue;
    std::vector<int> order;   /* indices into queue, in play order */
    int pos = 0;              /* where in order the current track is */
    bool shuffle = false;
    int repeat = 0;           /* 0 off, 1 all, 2 one */
    int jump = -1;            /* a queue index to play next, picked on the sheet */
    bool active = false;      /* music plays through this queue now */
    unsigned generation = 0;  /* which playback the queue belongs to */
} s_music;

void reorder_locked(int current)
{
    const int n = (int)s_music.queue.size();
    s_music.order.resize(n);
    for (int i = 0; i < n; i++)
        s_music.order[i] = i;
    if (s_music.shuffle && n > 1) {   /* the current track first, the rest shuffled */
        std::swap(s_music.order[0], s_music.order[std::max(0, std::min(current, n - 1))]);
        for (int i = n - 1; i > 1; i--)
            std::swap(s_music.order[i], s_music.order[1 + std::rand() % i]);
        s_music.pos = 0;
    } else {
        s_music.pos = std::max(0, std::min(current, n - 1));
    }
}

int current_locked() { return s_music.order.empty() ? 0 : s_music.order[std::min(s_music.pos, (int)s_music.order.size() - 1)]; }

std::string state_from_result(const std::string &result)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "state");
    const std::string out = cJSON_IsString(s) && s->valuestring ? s->valuestring : "";
    cJSON_Delete(j);
    return out;
}

/* The player's action: "next" (a skip), "episode" (a pick or Previous), or "". */
std::string action_from_result(const std::string &result)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "action"), "type");
    const std::string out = cJSON_IsString(t) && t->valuestring ? t->valuestring : "";
    cJSON_Delete(j);
    return out;
}
} // namespace

void jelly5_music_state(std::vector<jf::Item> *upcoming, std::vector<int> *indices, int *current, bool *shuffle,
                        int *repeat)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    upcoming->clear();
    indices->clear();
    for (size_t i = s_music.pos + 1; i < s_music.order.size(); i++) {
        upcoming->push_back(s_music.queue[s_music.order[i]]);
        indices->push_back(s_music.order[i]);
    }
    *current = current_locked();
    *shuffle = s_music.shuffle;
    *repeat = s_music.repeat;
}

void jelly5_music_set_shuffle(bool on)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    if (s_music.shuffle == on)
        return;
    const int cur = current_locked();
    s_music.shuffle = on;
    reorder_locked(cur);
}

void jelly5_music_set_repeat(int mode)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    s_music.repeat = std::max(0, std::min(2, mode));
}

void jelly5_music_jump(int queue_index)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    s_music.jump = queue_index;
}

static void music_queue_end()
{
    std::lock_guard<std::mutex> g(s_music.lock);
    s_music.active = false;
}

bool jelly5_music_has_next(bool fallback)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    if (!s_music.active)
        return fallback;
    return s_music.repeat == 1 || (s_music.jump >= 0 && s_music.jump < (int)s_music.queue.size()) ||
           s_music.pos + 1 < (int)s_music.order.size();
}

bool jelly5_music_has_previous(bool fallback)
{
    std::lock_guard<std::mutex> g(s_music.lock);
    return s_music.active ? s_music.pos > 0 : fallback;
}

bool jelly5_music_enqueue(const std::vector<std::string> &ids, bool next)
{
    jf::Client *client;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_music.lock);
        if (!s_music.active || ids.empty())
            return false;
        client = s_session.client;
        gen = s_music.generation;
    }
    if (!client)
        return false;
    /* The items come from the server, off the player's thread. */
    return jelly5::spawn([client, ids, next, gen] {
        std::vector<jf::Item> items;
        for (const std::string &id : ids) {
            jf::Item it;
            if (client->item(id, &it) && it.type == "Audio") {
                it.parent_index = -1;   /* never one of the player's own picks (Previous) */
                it.index = -1;
                it.position_ticks = 0;
                items.push_back(std::move(it));
            }
        }
        std::lock_guard<std::mutex> g(s_music.lock);
        if (!s_music.active || s_music.generation != gen || items.empty())
            return;
        size_t at = next ? std::min(s_music.order.size(), (size_t)s_music.pos + 1) : s_music.order.size();
        for (jf::Item &it : items) {
            s_music.order.insert(s_music.order.begin() + at++, (int)s_music.queue.size());
            s_music.queue.push_back(std::move(it));
        }
        evo_bt("jelly5: remote %s: %zu track(s) queued", next ? "PlayNext" : "PlayLast", items.size());
    });
}

/* A title's theme song on its page: the player headless (the caller set that),
 * quiet, and never reported to Jellyfin - it is background, not listening. */
bool jelly5_play_theme(jf::Client &client, const jf::Item &song)
{
    jf::Playback pb;
    if (nuvio_player_stop_requested() || !client.playback_info(song.id, 0, -1, -2, &pb, 0))
        return false;
    if (nuvio_player_stop_requested())
        return true;   /* nothing opened yet */
    Extras ex;
    ex.not_group = true;   /* the page's theme, never the group's: the group's commands pass it by */
    const std::string req = request_json(client, song, pb, {}, ex);
    evo_audio_set_night(0);
    evo_audio_set_gain(0.28f);
    jelly5_bs_set_allowed(0);   /* a theme plays quietly: decoded, so the gain applies */
    nuvio_player_run(req.c_str());
    evo_audio_set_gain(1.0f);
    client.stop_encoding(pb);
    return true;
}

static bool play_chain_tracks(jf::Client &client, jf::Item item, std::vector<jf::Item> episodes, std::string *error)
{
    if (item.type == "Audio" && !episodes.empty()) {   /* the music queue starts on this track */
        std::lock_guard<std::mutex> g(s_music.lock);
        s_music.queue = episodes;
        s_music.jump = -1;
        s_music.active = true;
        s_music.generation++;
        int cur = 0;
        for (size_t i = 0; i < episodes.size(); i++)
            if (episodes[i].id == item.id)
                cur = (int)i;
        reorder_locked(cur);
    }
    evo_audio_set_gain(1.0f);                                    /* full volume (a theme may have been playing) */
    evo_audio_set_speed(1.0f);                                   /* each playback starts at normal speed */
    evo_audio_set_night(settings::get().local.night_mode ? 1 : 0);   /* Innstillinger: Nattmodus */
    /* Innstillinger: HDMI-bitstrøm. Night mode needs the sound decoded here, so it wins. */
    jelly5_bs_set_allowed(settings::get().local.hdmi_bitstream && !settings::get().local.night_mode
                              ? JELLY5_BS_AC3 | JELLY5_BS_EAC3 | JELLY5_BS_DTS
                              : 0);

    /* No cap on the chain: a playlist, an album on repeat or a binge plays on as
     * long as the viewer lets it. It cannot spin: a title that fails or is
     * stopped ends it, and only one that started can end by itself. */
    /* "Ser du fortsatt på?": what the last player counted goes on to the next. */
    int still_count = 0;
    double still_idle = 0;
    for (int chain = 0;; chain++) {
        if (nuvio_player_stop_requested())
            return true;   /* the music stopped between tracks (a film's chain has no such stop) */
        jf::Playback pb;
        const int mbps = settings::get().local.max_mbps;
        if (item.type == "TvChannel") {
            /* Changing channel: what was picked shows at once, while the server opens it
             * (the player's loading veil then carries the same screen on). */
            if (chain > 0)
                ui::show_tuning(item.id, item.name);
            jelly5_wait_reports(3000);   /* the last channel's stream closed before this one opens */
        }
        if (!client.playback_info(item.id, item.position_ticks, -1, -2, &pb, (int64_t)mbps * 1000000,
                                  std::string(), item.type == "TvChannel")) {
            *error = client.last_error();
            evo_bt("jelly5: playback info failed: %s", error->c_str());
            return chain > 0 && item.type != "TvChannel";   /* a channel zapped to says it could not open */
        }
        evo_bt("jelly5: playback info answered");
        int keep_audio, keep_subtitle;   /* an episode: the tracks chosen earlier in its series */
        remembered_tracks(client, item, pb, &keep_audio, &keep_subtitle);
        if (keep_audio >= 0 || keep_subtitle >= -1) {
            evo_bt("jelly5: the series' tracks here: audio %d, subtitle %d", keep_audio, keep_subtitle);
            ask_with_tracks(client, item, (int64_t)mbps * 1000000, keep_audio, keep_subtitle, &pb);
        }
        evo_bt("jelly5: play %s (%s) %s %s", item.name.c_str(), item.id.c_str(), pb.play_method.c_str(),
               pb.transcode_reasons.c_str());
        if (nuvio_player_stop_requested()) {
            client.close_live_stream(pb);   /* (a channel's stream is open from PlaybackInfo on) */
            return true;
        }
        Extras ex;
        if (pb.live && item.type != "TvChannel")
            pb.live = false;   /* a recording still being made: plays as a title (it may resume, seek and end);
                                * its stream still closes by LiveStreamId */
        if (pb.live) {
            /* A channel: no intros, chapters or previews; it plays from where it airs. */
            item.position_ticks = 0;
            ex.not_group = true;   /* never a SyncPlay group's (see main's play) */
            if (item.type == "TvChannel")   /* (not a recording still being made) */
                livetv::watched(item.id);
            livetv::refresh();   /* the guide the player shows (zapping, the channel list) */
        } else if (item.type == "Audio") {
            ex.lyrics = client.lyrics(item.id);
        } else {   /* music has no intros, chapters or previews */
            jelly5::run_all({
                [&] { client.media_extras(item.id, pb.media_source_id, &ex.chapters, &ex.trickplay); },
                [&] { ex.segments = client.segments(item.id); },
            });
        }
        if (nuvio_player_stop_requested()) {
            client.close_live_stream(pb);
            return true;   /* stopped while the server answered: nothing reported, nothing opened */
        }
        evo_bt("jelly5: chapters, segments and trickplay answered");
        if (ex.trickplay.valid())
            evo_bt("jelly5: trickplay %dx%d, %d thumbnails", ex.trickplay.width, ex.trickplay.height, ex.trickplay.count);
        ex.autoplay_count = still_count;
        ex.autoplay_idle = still_idle;
        ex.keep_audio = keep_audio;
        ex.keep_subtitle = keep_subtitle;
        const std::string req = request_json(client, item, pb, episodes, ex);

        jelly5_subs::new_title();
        s_session.client = &client;
        s_session.pb = pb;
        s_session.position = (double)item.position_ticks / jf::kTicksPerSecond;
        s_session.reported = -1;
        s_session.heard = false;
        s_session.paused = false;
        s_session.audio_stream = -1;
        s_session.subtitle_track = -1;
        s_session.server_audio = -1;
        s_session.server_subtitle = -2;
        s_session.subtitle_known_for = -1;
        s_session.result.clear();
        s_session.active = true;
        client.report_start(pb, item.position_ticks);
        pthread_t reporter;
        const bool reporting = pthread_create(&reporter, nullptr, reporter_thread, nullptr) == 0;
        if (!reporting)
            evo_bt("jelly5: no thread for progress reports");

        nuvio_player_run(req.c_str());

        s_session.active = false;
        if (reporting)
            pthread_join(reporter, nullptr);
        std::string result;
        double pos;
        {
            std::lock_guard<std::mutex> g(s_session.lock);
            result = s_session.result;
            pos = position_from_result(result, s_session.position);
        }
        /* Tell the server in the background: the menus come back at once
         * (jelly5_wait_reports lets the home refresh wait for the position). */
        {
            jf::Client *c = &client;
            jf::Playback stopped;
            {
                std::lock_guard<std::mutex> g(s_session.lock);
                stopped = s_session.pb;   /* the version playing last */
            }
            const int64_t at = ticks(pos);
            s_reports++;
            auto report = [c, stopped, at] {
                c->report_stopped(stopped, at);   /* (a channel's carries its stream: the server closes it) */
                c->stop_encoding(stopped);
                s_reports--;
            };
            /* Another channel next: this one's stream is closed first, so a tuner or an
             * IPTV account that takes one stream at a time is free for it. (A channel
             * picked in the guide after leaving waits for it too: see the top.) */
            int s0 = 0, e0 = 0;
            std::string to;
            if (stopped.live && next_from_result(result, &s0, &e0, &to))
                report();
            else if (!jelly5::spawn(report))
                report();   /* no thread: tell the server here, the menus wait a moment */
        }
        evo_bt("jelly5: playback done at %.1f s: %s", pos, result.c_str());
        remember_tracks(client, item, result);

        int season = 0, number = 0;
        std::string pick;
        if (item.type == "Audio" && !episodes.empty()) {   /* music: the queue decides */
            if (group_end_of(result)) {   /* a SyncPlay group's: its queue plays on, not this one */
                std::lock_guard<std::mutex> g(s_music.lock);
                s_music.jump = -1;
                return true;
            }
            const bool natural = state_from_result(result) == "ended";
            const bool asked = next_from_result(result, &season, &number, &pick);
            /* Next: on through the play order (not the player's pick by numbers,
             * which can repeat in a playlist), whatever repeat-one says. */
            const bool skip = !natural && action_from_result(result) == "next";
            std::lock_guard<std::mutex> g(s_music.lock);
            const int n = (int)s_music.queue.size();
            int next_index = -1;
            if (s_music.jump >= 0 && s_music.jump < n) {   /* picked on the queue sheet */
                next_index = s_music.jump;
                for (int i = 0; i < (int)s_music.order.size(); i++)
                    if (s_music.order[i] == next_index)
                        s_music.pos = i;
            } else if (!natural && !asked) {
                s_music.jump = -1;
                return true;   /* stopped */
            } else if (natural && s_music.repeat == 2) {
                next_index = current_locked();   /* repeat one */
            } else if (asked && !natural && !skip) {
                /* Previous: one back in the play order (shuffled, or with tracks a phone
                 * queued, the player's album numbers name another track or none). */
                s_music.pos = std::max(0, std::min(s_music.pos - 1, (int)s_music.order.size() - 1));
                next_index = current_locked();
            }
            if (next_index < 0) {   /* on through the play order */
                if (++s_music.pos >= (int)s_music.order.size()) {
                    if (s_music.repeat != 1)
                        return true;   /* the end of the queue */
                    reorder_locked(s_music.shuffle ? std::rand() % std::max(1, n) : 0);
                    s_music.pos = 0;
                }
                next_index = s_music.order[s_music.pos];
            }
            s_music.jump = -1;
            item = s_music.queue[next_index];
            item.position_ticks = 0;
            continue;
        }
        if (!next_from_result(result, &season, &number, &pick))
            return true;
        if (pb.live) {   /* zapping: the channel the player asked for */
            if (pick.empty())
                return true;
            const livetv::GuideRef g = livetv::guide();
            if (const jf::Item *ch = g->channel(pick)) {
                item = *ch;
            } else {
                item = jf::Item();
                item.id = pick;
                item.type = "TvChannel";
            }
            item.position_ticks = 0;
            continue;
        }
        still_watching_from_result(result, &still_count, &still_idle);
        const jf::Item *next = nullptr;
        for (const auto &e : episodes)
            if (!next && is_pick(e, season, number, pick))
                next = &e;
        if (!next)
            return true;
        item = *next;
        item.position_ticks = 0;
    }
}

void jelly5_wait_reports(int max_ms)
{
    for (int waited = 0; s_reports > 0 && waited < max_ms; waited += 20)
        usleep(20 * 1000);
}
