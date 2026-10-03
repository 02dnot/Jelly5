/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jelly5_playback.h"

#include "nuvio_player.h"
#include "app/settings.h"

#include "evo_boot_trace.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
struct Session {
    jf::Client *client = nullptr;
    jf::Playback pb;
    std::mutex lock;
    double position = 0, duration = 0;
    double reported = -1;              /* position of the last progress report */
    std::string result;                /* the player's final result JSON */
    std::atomic<bool> active{false};
};
Session s_session;

int64_t ticks(double seconds) { return (int64_t)std::llround(seconds * jf::kTicksPerSecond); }

void *reporter_thread(void *)
{
    while (s_session.active) {
        for (int i = 0; i < 100 && s_session.active; i++)
            usleep(100 * 1000);   /* every 10 s, as Jellyfin's own clients do */
        if (!s_session.active)
            break;
        double pos;
        bool paused;
        {
            std::lock_guard<std::mutex> g(s_session.lock);
            pos = s_session.position;
            paused = std::fabs(pos - s_session.reported) < 0.5;
            s_session.reported = pos;
        }
        s_session.client->report_progress(s_session.pb, ticks(pos), paused);
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
    if (min >= 60)
        std::snprintf(b, sizeof b, "%d t %d min", min / 60, min % 60);
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
    const char *how = pb.play_method == "DirectPlay" ? "Direktespilling"
                      : pb.play_method == "DirectStream" ? "Direktestrøm" : "Transkodet av serveren";
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

/* The request the Nuvio Player plays (see nuvio_request_parse). */
struct Extras {
    std::vector<jf::Segment> segments;
    std::vector<jf::Chapter> chapters;
    jf::Trickplay trickplay;
};

std::string request_json(jf::Client &c, const jf::Item &it, const jf::Playback &pb,
                         const std::vector<jf::Item> &episodes, const Extras &ex)
{
    const std::vector<jf::Segment> &segs = ex.segments;
    const bool episode = it.type == "Episode", audio = it.type == "Audio";
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", it.id.c_str());
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
    cJSON_AddStringToObject(o, "title", (episode ? it.series_name : it.name).c_str());
    if (episode) {
        cJSON_AddStringToObject(o, "episodeTitle", it.name.c_str());
        cJSON_AddNumberToObject(o, "season", it.parent_index);
        cJSON_AddNumberToObject(o, "episode", it.index);
    }
    if (it.year)
        cJSON_AddStringToObject(o, "year", std::to_string(it.year).c_str());
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
    cJSON_AddStringToObject(o, "itemType", episode ? "series" : audio ? "audio" : "movie");
    cJSON_AddStringToObject(o, "logo", c.image_url(it.logo_owner, "Logo", it.logo_tag, 800).c_str());
    cJSON_AddStringToObject(o, "poster", c.image_url(episode ? it.series_id : it.id, "Primary",
                                                      episode ? std::string() : it.primary_tag, 400).c_str());
    cJSON_AddStringToObject(o, "background",
                            c.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920).c_str());
    if (episode)
        cJSON_AddStringToObject(o, "thumbnail", c.image_url(it.id, "Primary", it.primary_tag, 640).c_str());
    cJSON_AddNumberToObject(o, "startPosition", (double)it.position_ticks / jf::kTicksPerSecond);

    cJSON *stream = cJSON_CreateObject();
    cJSON_AddStringToObject(stream, "title", method_label(pb).c_str());
    cJSON_AddStringToObject(stream, "addon", "Jellyfin");
    cJSON_AddItemToObject(o, "stream", stream);

    cJSON *sources = cJSON_CreateArray();
    cJSON *src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "id", pb.media_source_id.c_str());
    cJSON_AddStringToObject(src, "title", method_label(pb).c_str());
    cJSON_AddStringToObject(src, "addon", "Jellyfin");
    cJSON_AddStringToObject(src, "url", pb.url.c_str());
    cJSON_AddItemToArray(sources, src);
    cJSON_AddItemToObject(o, "sources", sources);

    /* Embedded tracks come out of the container; external text subtitles are fetched. */
    cJSON *subs = cJSON_CreateArray();
    for (const auto &s : pb.streams) {
        if (s.type != "Subtitle" || !s.is_external || s.delivery_url.empty())
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "url", s.delivery_url.c_str());
        cJSON_AddStringToObject(e, "lang", s.language.c_str());
        cJSON_AddStringToObject(e, "label", s.display_title.c_str());
        cJSON_AddItemToArray(subs, e);
    }
    cJSON_AddItemToObject(o, "subtitles", subs);

    if ((episode || audio) && !episodes.empty()) {
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
        for (const auto &ch : ex.chapters) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "start", ch.start);
            cJSON_AddStringToObject(e, "name", ch.name.c_str());
            cJSON_AddItemToArray(chs, e);
        }
        cJSON_AddItemToObject(o, "chapters", chs);
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
    cJSON_AddItemToObject(prefs, "autoSkipIntro", cJSON_CreateBool(set.local.auto_skip_intro));
    cJSON_AddItemToObject(prefs, "clock24h", cJSON_CreateBool(1));
    cJSON_AddItemToObject(o, "prefs", prefs);

    /* The player's interface text, in Norwegian. */
    static const char *const kStrings[][2] = {
        {"advanced", "Avansert"}, {"advanced_style", "Stil og timing"},
        {"advanced_hint", "Forsinkelse, størrelse, posisjon \xE2\x80\xA6"},
        {"audio", "Lyd"}, {"background", "Bakgrunn"}, {"bold", "Fet skrift"}, {"built_in", "Innebygd"},
        {"default", "Standard"}, {"delay", "Forsinkelse"}, {"ends_at", "Slutter kl. %1$s"},
        {"episode", "Episode"}, {"forced", "Tvungen"}, {"go_back", "Tilbake"}, {"language", "Språk"},
        {"loading", "Laster \xE2\x80\xA6"}, {"next_episode", "Neste episode"}, {"next_in", "Spilles om %1$s"},
        {"no_audio_tracks", "Ingen andre lydspor"}, {"no_subtitles", "Ingen undertekster for denne strømmen"},
        {"off", "Av"}, {"on", "På"}, {"outline", "Kontur"}, {"play", "Spill av"},
        {"playback_error", "Avspillingsfeil"}, {"playing", "Spiller"}, {"position", "Posisjon"},
        {"press_to_play_next", "Trykk \xE2\x9C\x95 for å spille"}, {"season", "Sesong"}, {"size", "Størrelse"},
        {"skip_intro", "Hopp over intro"}, {"skip_preview", "Hopp over forhåndsvisning"},
        {"skip_recap", "Hopp over oppsummering"}, {"sources", "Kilder"}, {"specials", "Spesialer"},
        {"subtitles_off", "Undertekster er av"}, {"subtitles", "Undertekster"}, {"track", "Spor"},
        {"unavailable", "Utilgjengelig"}, {"unknown_language", "Ukjent"}, {"upcoming", "Kommer"},
        {"youre_watching", "Du ser på"}, {"addon", "Kilde"},
    };
    cJSON *strings = cJSON_CreateObject();
    for (const auto &kv : kStrings)
        cJSON_AddStringToObject(strings, kv[0], kv[1]);
    cJSON_AddItemToObject(o, "strings", strings);

    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    std::free(s);
    cJSON_Delete(o);
    return out;
}

/* What the viewer asked for at the end: another episode by number, or nothing. */
bool next_from_result(const std::string &result, int *season, int *episode)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "action");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "type");
    bool want = false;
    if (cJSON_IsString(type) && (std::string(type->valuestring) == "next" ||
                                 std::string(type->valuestring) == "episode")) {
        *season = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(a, "season"));
        *episode = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(a, "episode"));
        want = true;
    }
    cJSON_Delete(j);
    return want;
}

double position_from_result(const std::string &result, double fallback)
{
    cJSON *j = cJSON_Parse(result.c_str());
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "position");
    const double pos = cJSON_IsNumber(p) ? p->valuedouble : fallback;
    cJSON_Delete(j);
    return pos;
}

} // namespace

extern "C" void jelly5_playback_progress(double position, double duration)
{
    std::lock_guard<std::mutex> g(s_session.lock);
    s_session.position = position;
    s_session.duration = duration;
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

bool jelly5_play(jf::Client &client, const jf::Item &first, std::string *error, bool shuffle)
{
    jf::Item item = first;
    std::vector<jf::Item> episodes;
    if (item.type == "Episode" && !item.series_id.empty())
        episodes = client.episodes(item.series_id, std::string());
    if (item.type == "Audio" && !item.album_id.empty())
        episodes = album_queue(client, &item, shuffle);

    for (int chain = 0; chain < 50; chain++) {
        jf::Playback pb;
        const int mbps = settings::get().local.max_mbps;
        if (!client.playback_info(item.id, item.position_ticks, -1, -2, &pb, (int64_t)mbps * 1000000)) {
            *error = client.last_error();
            evo_bt("jelly5: playback info failed: %s", error->c_str());
            return chain > 0;
        }
        evo_bt("jelly5: play %s (%s) %s %s", item.name.c_str(), item.id.c_str(), pb.play_method.c_str(),
               pb.transcode_reasons.c_str());
        Extras ex;
        if (item.type != "Audio") {   /* music has no intros, chapters or previews */
            std::thread chapters([&] { client.media_extras(item.id, pb.media_source_id, &ex.chapters, &ex.trickplay); });
            ex.segments = client.segments(item.id);
            chapters.join();
        }
        if (ex.trickplay.valid())
            evo_bt("jelly5: trickplay %dx%d, %d thumbnails", ex.trickplay.width, ex.trickplay.height, ex.trickplay.count);
        const std::string req = request_json(client, item, pb, episodes, ex);

        s_session.client = &client;
        s_session.pb = pb;
        s_session.position = (double)item.position_ticks / jf::kTicksPerSecond;
        s_session.reported = -1;
        s_session.result.clear();
        s_session.active = true;
        client.report_start(pb, item.position_ticks);
        pthread_t reporter;
        pthread_create(&reporter, nullptr, reporter_thread, nullptr);

        nuvio_player_run(req.c_str());

        s_session.active = false;
        pthread_join(reporter, nullptr);
        std::string result;
        double pos;
        {
            std::lock_guard<std::mutex> g(s_session.lock);
            result = s_session.result;
            pos = position_from_result(result, s_session.position);
        }
        client.report_stopped(pb, ticks(pos));
        client.stop_encoding(pb);
        evo_bt("jelly5: playback done at %.1f s: %s", pos, result.c_str());

        int season = 0, number = 0;
        if (!next_from_result(result, &season, &number))
            return true;
        const jf::Item *next = nullptr;
        for (const auto &e : episodes)
            if (e.parent_index == season && e.index == number)
                next = &e;
        if (!next)
            return true;
        item = *next;
        item.position_ticks = 0;
    }
    return true;
}
