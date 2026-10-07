/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jelly5_playback.h"
#include "evo_audio_out.h"

#include "nuvio_player.h"
#include "nuvio_subs.h"
#include "app/settings.h"
#include "app/i18n.h"

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
    double reported = -1;              /* position of the last progress report */
    std::string result;                /* the player's final result JSON */
    std::atomic<bool> active{false};
};
Session s_session;

int64_t ticks(double seconds) { return (int64_t)std::llround(seconds * jf::kTicksPerSecond); }

/* Progress every 10 s, as Jellyfin's own clients do, and at once when playback
 * pauses or resumes (a phone controlling the PS5 shows the state it is in).
 * Paused: the position (posted once a second) has not moved for 1.5 s. */
void *reporter_thread(void *)
{
    double last_pos = -1, still_since = 0, t = 0, last_report = 0;
    bool reported_paused = false;
    while (s_session.active) {
        for (int i = 0; i < 10 && s_session.active; i++)   /* 250 ms, but gone at once when playback ends */
            usleep(25 * 1000);
        t += 0.25;
        if (!s_session.active)
            break;
        double pos;
        {
            std::lock_guard<std::mutex> g(s_session.lock);
            pos = s_session.position;
        }
        if (std::fabs(pos - last_pos) > 0.05) {
            last_pos = pos;
            still_since = t;
        }
        const bool paused = t - still_since >= 1.5;
        if (paused != reported_paused || t - last_report >= 10.0) {
            reported_paused = paused;
            last_report = t;
            s_session.client->report_progress(s_session.pb, ticks(pos), paused);
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
    if (min >= 60)
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
    {   /* the controller's light: the picture's colour */
        const std::string &hash = audio ? (!it.album_blurhash.empty() ? it.album_blurhash : it.primary_blurhash)
                                        : it.backdrop_blurhash;
        cJSON_AddNumberToObject(o, "lightColor", (double)light_of(hash));
    }
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
        cJSON_AddItemToArray(sources, src);
    }
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
    cJSON *style = cJSON_CreateObject();   /* how text subtitles look (Innstillinger) */
    cJSON_AddNumberToObject(style, "size", set.local.sub_size);
    cJSON_AddNumberToObject(style, "offset", set.local.sub_offset);
    cJSON_AddNumberToObject(style, "background", set.local.sub_background);
    cJSON_AddItemToObject(style, "outline", cJSON_CreateBool(set.local.sub_outline));
    cJSON_AddItemToObject(prefs, "subtitleStyle", style);
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

/* ---- subtitle search -------------------------------------------------------------- */

namespace jelly5_subs {
namespace {
std::mutex s_lock;
State s_search = Idle, s_download = Idle;
std::vector<jf::RemoteSubtitle> s_found;
std::string s_lang;
int s_track = -1;
unsigned s_gen = 0;              /* a new search or title drops older answers */
}

bool available()
{
    return s_session.active && s_session.client && s_session.client->can_search_subtitles();
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
    std::thread([c, id, language, gen] {
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
    }).detach();
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
    const jf::Playback pb = s_session.pb;
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_download = Busy;
        s_track = -1;
    }
    const unsigned gen = s_gen;
    std::thread([c, pb, sub, gen] {
        /* The new file shows as an external subtitle stream the next time the server
         * is asked how to play the title: the one that was not there before. */
        std::set<std::string> known;
        for (const jf::MediaStream &m : pb.streams)
            if (m.type == "Subtitle" && m.is_external)
                known.insert(m.delivery_url.substr(0, m.delivery_url.find('?')));
        int track = -1;
        if (c->download_subtitle(pb.item_id, sub.id)) {
            for (int attempt = 0; attempt < 5 && track < 0; attempt++) {
                if (attempt)
                    usleep(1000 * 1000);   /* the server may still be writing it */
                jf::Playback now;
                if (!c->playback_info(pb.item_id, 0, -1, -1, &now))
                    continue;
                for (const jf::MediaStream &m : now.streams)
                    if (m.type == "Subtitle" && m.is_external && !m.delivery_url.empty() &&
                        !known.count(m.delivery_url.substr(0, m.delivery_url.find('?')))) {
                        track = nuvio_subs_add_external(m.delivery_url.c_str(), m.language.c_str(),
                                                        sub.name.c_str(), "");
                        break;
                    }
            }
        }
        evo_bt("jelly5: subtitle download %s -> track %d", track >= 0 ? "ok" : "failed", track);
        std::lock_guard<std::mutex> g(s_lock);
        if (gen != s_gen && track < 0)
            return;
        s_track = track;
        s_download = track >= 0 ? Done : Failed;
    }).detach();
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

static bool play_chain_tracks(jf::Client &client, jf::Item item, std::vector<jf::Item> episodes, std::string *error);

/* The chain, then out of player mode (music keeps it on between tracks). */
static bool play_chain(jf::Client &client, jf::Item item, std::vector<jf::Item> episodes, std::string *error)
{
    const bool ok = play_chain_tracks(client, std::move(item), std::move(episodes), error);
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

/* A title's theme song on its page: the player headless (the caller set that),
 * quiet, and never reported to Jellyfin - it is background, not listening. */
bool jelly5_play_theme(jf::Client &client, const jf::Item &song)
{
    jf::Playback pb;
    if (!client.playback_info(song.id, 0, -1, -2, &pb, 0))
        return false;
    Extras ex;
    const std::string req = request_json(client, song, pb, {}, ex);
    evo_audio_set_night(0);
    evo_audio_set_gain(0.28f);
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
        int cur = 0;
        for (size_t i = 0; i < episodes.size(); i++)
            if (episodes[i].id == item.id)
                cur = (int)i;
        reorder_locked(cur);
    }
    evo_audio_set_gain(1.0f);                                    /* full volume (a theme may have been playing) */
    evo_audio_set_speed(1.0f);                                   /* each playback starts at normal speed */
    evo_audio_set_night(settings::get().local.night_mode ? 1 : 0);   /* Innstillinger: Nattmodus */

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
        if (item.type == "Audio") {
            ex.lyrics = client.lyrics(item.id);
        } else {   /* music has no intros, chapters or previews */
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
        /* Tell the server in the background: the menus come back at once
         * (jelly5_wait_reports lets the home refresh wait for the position). */
        {
            jf::Client *c = &client;
            const jf::Playback stopped = pb;
            const int64_t at = ticks(pos);
            s_reports++;
            std::thread([c, stopped, at] {
                c->report_stopped(stopped, at);
                c->stop_encoding(stopped);
                s_reports--;
            }).detach();
        }
        evo_bt("jelly5: playback done at %.1f s: %s", pos, result.c_str());

        int season = 0, number = 0;
        if (item.type == "Audio" && !episodes.empty()) {   /* music: the queue decides */
            const bool natural = state_from_result(result) == "ended";
            const bool asked = next_from_result(result, &season, &number);
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
            } else if (asked && !natural && !s_music.shuffle) {   /* Next / Previous: the player's pick */
                for (int i = 0; i < n; i++)
                    if (s_music.queue[i].parent_index == season && s_music.queue[i].index == number)
                        next_index = i;
                for (int i = 0; i < (int)s_music.order.size(); i++)
                    if (s_music.order[i] == next_index)
                        s_music.pos = i;
            }
            if (next_index < 0) {   /* on through the play order */
                /* Previous while shuffled: the player asked for the track before
                 * in the queue's own order; step back in the play order instead. */
                bool back = false;
                if (asked && !natural && s_music.shuffle) {
                    const int was = current_locked();
                    for (int i = 0; i < n; i++)
                        if (s_music.queue[i].parent_index == season && s_music.queue[i].index == number)
                            back = i < was;
                }
                if (back) {
                    s_music.pos = std::max(0, s_music.pos - 1);
                } else if (++s_music.pos >= (int)s_music.order.size()) {
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

void jelly5_wait_reports(int max_ms)
{
    for (int waited = 0; s_reports > 0 && waited < max_ms; waited += 20)
        usleep(20 * 1000);
}
