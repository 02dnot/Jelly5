/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nuvio_session.h"

#include "cJSON.h"
#include "jf/json_num.h"
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

std::string str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring)
        return v->valuestring;
    if (cJSON_IsNumber(v)) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", v->valuedouble);
        return b;
    }
    return std::string();
}

/* Jelly5: finite only (strtod takes "inf" and "nan", and 1e999 parses as inf). */
double num_of(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v))
        return std::isfinite(v->valuedouble) ? v->valuedouble : fallback;
    if (cJSON_IsString(v) && v->valuestring && *v->valuestring) {
        char *end = nullptr;
        double d = std::strtod(v->valuestring, &end);
        if (end && end != v->valuestring && std::isfinite(d))
            return d;
    }
    return fallback;
}

/* Jelly5: an integer field without an undefined cast (jf/json_num.h). */
template <typename T>
T int_of(const cJSON *o, const char *key, T fallback)
{
    return jf::to_int<T>(num_of(o, key, (double)fallback), fallback);
}

bool bool_of(const cJSON *o, const char *key, bool fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0.0;
    return fallback;
}

std::vector<std::string> strings_of(const cJSON *o, const char *key)
{
    std::vector<std::string> out;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && it->valuestring && *it->valuestring)
            out.push_back(it->valuestring);
    }
    return out;
}

/* "#RRGGBB" / "RRGGBB" / "#RGB" -> 0xRRGGBB; fallback when unparsable. */
uint32_t colour_of(const std::string &s, uint32_t fallback)
{
    std::string h = s;
    if (!h.empty() && h[0] == '#')
        h.erase(0, 1);
    if (h.size() == 3)
        h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() < 6)
        return fallback;
    char *end = nullptr;
    const std::string hex = h.substr(0, 6);   /* named: end points into it */
    unsigned long v = std::strtoul(hex.c_str(), &end, 16);
    return (end && *end == 0) ? (uint32_t)v : fallback;
}

NuvioEpisode episode_of(const cJSON *e)
{
    NuvioEpisode ep;
    ep.season = int_of<int>(e, "season", 0);
    ep.episode = int_of<int>(e, "episode", 0);
    ep.title = str_of(e, "title");
    ep.thumbnail = str_of(e, "thumbnail");
    ep.video_id = str_of(e, "videoId");
    ep.overview = str_of(e, "overview");
    ep.released_label = str_of(e, "releasedLabel");
    ep.released = bool_of(e, "released", true);
    ep.watched = bool_of(e, "watched", false);
    ep.progress = num_of(e, "progress", 0.0);
    ep.runtime = str_of(e, "runtime");
    ep.blurhash = str_of(e, "blurhash");
    return ep;
}

} // namespace

std::string nuvio_headers_from_json(const void *cjson_object, std::string *user_agent)
{
    std::string out;
    const cJSON *h;
    cJSON_ArrayForEach(h, (const cJSON *)cjson_object) {
        const char *v = cJSON_GetStringValue(h);
        if (!h->string || !v || std::strpbrk(h->string, "\r\n") || std::strpbrk(v, "\r\n"))
            continue;
        if (!strcasecmp(h->string, "User-Agent")) {
            if (user_agent)
                *user_agent = v;
            continue;
        }
        out += h->string;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    return out;
}

const char *NuvioRequest::str(const char *key, const char *fallback) const
{
    auto it = strings.find(key);
    return (it != strings.end() && !it->second.empty()) ? it->second.c_str() : fallback;
}

std::string NuvioRequest::header_title() const
{
    return title.empty() ? std::string("Nuvio") : title;
}

std::string NuvioRequest::header_subtitle() const
{
    std::string s;
    if (season > 0 && episode > 0) {
        char b[32];
        std::snprintf(b, sizeof b, "S%d E%d", season, episode);
        s = b;
    }
    if (!episode_title.empty())
        s += (s.empty() ? "" : " \xC2\xB7 ") + episode_title;   /* middle dot */
    return s;
}

bool nuvio_request_parse(const char *json, NuvioRequest &r)
{
    cJSON *root = json ? cJSON_Parse(json) : nullptr;
    if (!root)
        return false;

    r = NuvioRequest();
    r.id = str_of(root, "id");
    r.url = str_of(root, "url");
    r.play_method = str_of(root, "playMethod");
    r.sync_stop_url = str_of(root, "syncStop");
    r.transcode_reasons = str_of(root, "transcodeReasons");
    r.headers = nuvio_headers_from_json(cJSON_GetObjectItemCaseSensitive(root, "headers"), &r.user_agent);
    r.title = str_of(root, "title");
    r.episode_title = str_of(root, "episodeTitle");
    r.season = int_of<int>(root, "season", 0);
    r.episode = int_of<int>(root, "episode", 0);
    r.year = str_of(root, "year");
    r.description = str_of(root, "description");
    r.genres = str_of(root, "genres");
    r.runtime = str_of(root, "runtime");
    r.rating = str_of(root, "rating");
    r.item_type = str_of(root, "itemType");
    r.logo = str_of(root, "logo");
    r.poster = str_of(root, "poster");
    r.artist = str_of(root, "artist");
    r.light_color = int_of<uint32_t>(root, "lightColor", 0);
    r.album = str_of(root, "album");
    r.cover = str_of(root, "cover");
    r.cover_blurhash = str_of(root, "coverBlurhash");
    r.backdrop = str_of(root, "background");
    r.thumbnail = str_of(root, "thumbnail");
    r.start_position = num_of(root, "startPosition", 0.0);
    if (r.start_position < 0)
        r.start_position = 0;
    r.autoplay_count = std::max(0, int_of<int>(root, "autoplayCount", 0));
    r.autoplay_idle = std::max(0.0, num_of(root, "autoplayIdle", 0.0));
    r.not_group = bool_of(root, "notGroup", false);
    r.live = bool_of(root, "live", false);

    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(root, "stream");
    r.stream_title = str_of(stream, "title");
    r.stream_description = str_of(stream, "description");
    r.stream_addon = str_of(stream, "addon");

    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "sources")) {
        NuvioSource s;
        s.id = str_of(it, "id");
        s.title = str_of(it, "title");
        s.description = str_of(it, "description");
        s.addon = str_of(it, "addon");
        s.url = str_of(it, "url");
        s.quality = str_of(it, "quality");
        s.headers = nuvio_headers_from_json(cJSON_GetObjectItemCaseSensitive(it, "headers"), &s.user_agent);
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(it, "syncAudio"))
            s.sync_audio[int_of<int>(e, "stream", -1)] = str_of(e, "url");
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(it, "syncSubtitles"))
            s.sync_subtitles[int_of<int>(e, "stream", -1)] = str_of(e, "url");
        if (!s.url.empty())
            r.sources.push_back(s);
    }
    r.source_index = int_of<int>(root, "sourceIndex", 0);
    if (r.source_index < 0 || r.source_index >= (int)r.sources.size())
        r.source_index = r.sources.empty() ? -1 : 0;

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "subtitles")) {
        NuvioSubtitleRef s;
        s.url = str_of(it, "url");
        s.lang = str_of(it, "lang");
        s.label = str_of(it, "label");
        s.headers = nuvio_headers_from_json(cJSON_GetObjectItemCaseSensitive(it, "headers"), nullptr);
        s.source = str_of(it, "source");
        if (!s.url.empty())
            r.subtitles.push_back(s);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "subtitleRequests")) {
        NuvioRequest::SubtitleRequest q;
        if (cJSON_IsString(it) && it->valuestring) {
            q.url = it->valuestring;
        } else {
            q.url = str_of(it, "url");
            q.addon = str_of(it, "addon");
        }
        if (!q.url.empty())
            r.subtitle_requests.push_back(q);
    }

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "episodes"))
        r.episodes.push_back(episode_of(it));
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "nextEpisode");
    if (cJSON_IsObject(next)) {
        r.next = episode_of(next);
        /* Jelly5: or by its id (episodes without numbers are all 0) */
        r.has_next = r.next.season > 0 || r.next.episode > 0 || !r.next.video_id.empty();
    }

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "skipIntervals")) {
        NuvioSkip k;
        k.type = str_of(it, "type");
        k.start = num_of(it, "start", -1);
        k.end = num_of(it, "end", -1);
        if (k.end > k.start && k.start >= 0)
            r.skips.push_back(k);
    }

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "chapters")) {
        NuvioChapter ch;
        ch.start = num_of(it, "start", 0);
        ch.name = str_of(it, "name");
        ch.image = str_of(it, "image");
        r.chapters.push_back(ch);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "lyrics")) {
        NuvioLyric l;
        l.start = num_of(it, "start", -1);
        l.text = str_of(it, "text");
        const cJSON *cue;
        cJSON_ArrayForEach(cue, cJSON_GetObjectItemCaseSensitive(it, "cues")) {
            NuvioLyric::Cue c;
            c.start = num_of(cue, "start", 0);
            c.from = int_of<size_t>(cue, "from", 0);
            c.to = std::min(int_of<size_t>(cue, "to", 0), l.text.size());
            if (c.to > c.from)
                l.cues.push_back(c);
        }
        r.lyrics.push_back(l);
    }
    if (const cJSON *tp = cJSON_GetObjectItemCaseSensitive(root, "trickplay")) {
        NuvioTrickplay &t = r.trickplay;
        t.width = int_of<int>(tp, "width", 0);
        t.height = int_of<int>(tp, "height", 0);
        t.tile_w = int_of<int>(tp, "tileWidth", 0);
        t.tile_h = int_of<int>(tp, "tileHeight", 0);
        t.count = int_of<int>(tp, "count", 0);
        t.interval = num_of(tp, "interval", 0);
        t.url_base = str_of(tp, "urlBase");
        t.url_query = str_of(tp, "urlQuery");
        t.sheet_ticks = int_of<int64_t>(tp, "sheetTicks", 0);
        t.first_ticks = int_of<int64_t>(tp, "firstTicks", 0);
    }

    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "prefs");
    if (cJSON_IsObject(p)) {
        NuvioPrefs &pr = r.prefs;
        pr.audio_langs = strings_of(p, "audioLanguages");
        pr.subtitle_langs = strings_of(p, "subtitleLanguages");
        pr.subtitles_enabled = bool_of(p, "subtitlesEnabled", true);
        pr.autoplay_next = bool_of(p, "autoplayNext", true);
        pr.next_by_minutes = str_of(p, "nextThresholdMode") == "MINUTES_BEFORE_END";
        pr.next_percent = num_of(p, "nextThresholdPercent", 99.0);
        pr.next_minutes = num_of(p, "nextThresholdMinutes", 2.0);
        if (pr.next_percent < 50.0 || pr.next_percent > 100.0)
            pr.next_percent = 99.0;
        if (pr.next_minutes <= 0.0 || pr.next_minutes > 30.0)
            pr.next_minutes = 2.0;
        pr.show_clock = bool_of(p, "clock", true);
        pr.clock_24h = bool_of(p, "clock24h", true);
        pr.skip_intro = bool_of(p, "skipIntro", true);
        const cJSON *seg = cJSON_GetObjectItemCaseSensitive(p, "segments");   /* Jelly5 */
        for (int t = 0; t < segments::TypeCount; t++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(seg, segments::key_of(t));
            pr.segment[t] = cJSON_IsString(v) ? segments::action_of(v->valuestring, pr.segment[t]) : pr.segment[t];
        }
        pr.forced_only_when_off = bool_of(p, "forcedOnlyWhenOff", true);
        pr.still_watching_episodes = std::max(0, int_of<int>(p, "stillWatchingEpisodes", 0));
        pr.still_watching_seconds = std::max(0.0, num_of(p, "stillWatchingSeconds", 0.0));
        pr.keep_audio_stream = std::max(-1, int_of<int>(p, "keepAudioStream", -1));
        if (const cJSON *k = cJSON_GetObjectItemCaseSensitive(p, "keepSubtitle")) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(k, "off"))) {
                pr.keep_subtitle = 1;
            } else if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(k, "external"))) {
                pr.keep_subtitle = 2;
                pr.keep_subtitle_at = int_of<int>(k, "external", -1);
            } else if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(k, "stream"))) {
                pr.keep_subtitle = 3;
                pr.keep_subtitle_at = int_of<int>(k, "stream", -1);
            }
            if (pr.keep_subtitle > 1 && pr.keep_subtitle_at < 0)
                pr.keep_subtitle = 0;
        }
        if (const cJSON *k = cJSON_GetObjectItemCaseSensitive(p, "serverSubtitle")) {
            if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(k, "external"))) {
                pr.server_subtitle = 2;
                pr.server_subtitle_at = int_of<int>(k, "external", -1);
            } else if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(k, "stream"))) {
                pr.server_subtitle = 3;
                pr.server_subtitle_at = int_of<int>(k, "stream", -1);
            }
            if (pr.server_subtitle_at < 0)
                pr.server_subtitle = 0;
        }
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(p, "tzOffsetMinutes"))) {
            pr.has_tz = true;
            pr.tz_offset_min = int_of<int>(p, "tzOffsetMinutes", 0);
        }
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "subtitleStyle");
        if (cJSON_IsObject(st)) {
            pr.style.size_pct = int_of<int>(st, "size", 100);
            pr.style.color = colour_of(str_of(st, "color"), 0xffffff);
            pr.style.bold = bool_of(st, "bold", false) ? 1 : 0;
            pr.style.outline = bool_of(st, "outline", true) ? 1 : 0;
            pr.style.background = (float)std::max(0.0, std::min(1.0, num_of(st, "background", 0.0)));
            pr.style.offset_pct = (float)std::max(0.0, std::min(40.0, num_of(st, "offset", 0.0)));
        }
    }

    const cJSON *strs = cJSON_GetObjectItemCaseSensitive(root, "strings");
    cJSON_ArrayForEach(it, strs) {
        if (it->string && cJSON_IsString(it) && it->valuestring)
            r.strings[it->string] = it->valuestring;
    }

    cJSON_Delete(root);
    return !r.url.empty();
}

std::string nuvio_result_json(const NuvioRequest &req, const NuvioResult &res)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", req.id.c_str());
    cJSON_AddStringToObject(o, "state", res.state.c_str());
    cJSON_AddNumberToObject(o, "position", res.position);
    cJSON_AddNumberToObject(o, "duration", res.duration);
    if (!res.error.empty())
        cJSON_AddStringToObject(o, "error", res.error.c_str());
    if (res.group_end)
        cJSON_AddBoolToObject(o, "groupEnd", 1);
    cJSON_AddNumberToObject(o, "autoplayCount", res.autoplay_count);
    cJSON_AddNumberToObject(o, "autoplayIdle", res.autoplay_idle);
    if (!res.action.empty()) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "type", res.action.c_str());
        if (res.action == "episode" || res.action == "next") {
            cJSON_AddNumberToObject(a, "season", res.season);
            cJSON_AddNumberToObject(a, "episode", res.episode);
            if (!res.video_id.empty())
                cJSON_AddStringToObject(a, "videoId", res.video_id.c_str());
        }
        if (res.action == "source")
            cJSON_AddNumberToObject(a, "sourceIndex", res.source_index);
        cJSON_AddItemToObject(o, "action", a);
    }
    cJSON *tracks = cJSON_CreateObject();
    cJSON_AddStringToObject(tracks, "audioLanguage", res.audio_lang.c_str());
    cJSON_AddStringToObject(tracks, "subtitleLanguage", res.subtitle_lang.c_str());
    cJSON_AddItemToObject(tracks, "subtitlesOn", cJSON_CreateBool(res.subtitles_on));
    cJSON_AddItemToObject(tracks, "audioPicked", cJSON_CreateBool(res.audio_picked));
    cJSON_AddItemToObject(tracks, "subtitlePicked", cJSON_CreateBool(res.subtitle_picked));
    cJSON_AddNumberToObject(tracks, "subtitleDelayMs", res.subtitle_delay_ms);
    cJSON_AddItemToObject(o, "tracks", tracks);
    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    std::free(s);
    cJSON_Delete(o);
    return out;
}
