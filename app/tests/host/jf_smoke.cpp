/*
 * Jelly5 — host smoke test for the Jellyfin client against a real server
 * (Jellyfin or Emby). Read-only apart from sign-in: the kind of server, the
 * user's settings, the home rows, a library, an item's details, seasons and
 * chapters, search, and how each "continue watching" item would play with the
 * PS5 profile (and whether its URL is let in).
 *   tests/host/run.sh          (JF_URL, JF_USER, JF_PASS from .env.local)
 *   tests/host/run.sh emby     (EMBY_URL, EMBY_USER, EMBY_PASS)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_client.h"
#include "jf_http.h"

#include <cstdio>
#include <cstdlib>

static int s_failed = 0;

static void check(bool ok, const char *what, const std::string &detail = std::string())
{
    std::printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok)
        s_failed++;
}

/* Emby's intro/credits markers (an Emby Premiere feature: no test server has them). */
static void emby_markers()
{
    std::printf("Emby markers (no server)\n");
    auto at = [](double s) { return std::to_string((long long)(s * jf::kTicksPerSecond)); };
    const std::string item = "{\"RunTimeTicks\":" + at(1500) + ",\"Chapters\":["
        "{\"StartPositionTicks\":0,\"MarkerType\":\"Chapter\"},"
        "{\"StartPositionTicks\":" + at(30) + ",\"MarkerType\":\"IntroStart\"},"
        "{\"StartPositionTicks\":" + at(95) + ",\"MarkerType\":\"IntroEnd\"},"
        "{\"StartPositionTicks\":" + at(600) + ",\"MarkerType\":\"Chapter\"},"
        "{\"StartPositionTicks\":" + at(1400) + ",\"MarkerType\":\"CreditsStart\"}]}";
    const std::vector<jf::Segment> s = jf::Client::emby_markers_of(item);
    check(s.size() == 2 && s[0].type == "Intro" && s[0].start == 30 && s[0].end == 95 && s[1].type == "Outro" &&
              s[1].start == 1400 && s[1].end == 1500,
          "intro 30-95, credits 1400-1500");
    /* No IntroEnd: the intro ends where the next chapter starts. */
    const std::string open_end = "{\"RunTimeTicks\":" + at(900) + ",\"Chapters\":["
        "{\"StartPositionTicks\":" + at(10) + ",\"MarkerType\":\"IntroStart\"},"
        "{\"StartPositionTicks\":" + at(70) + ",\"MarkerType\":\"Chapter\"}]}";
    const std::vector<jf::Segment> o = jf::Client::emby_markers_of(open_end);
    check(o.size() == 1 && o[0].start == 10 && o[0].end == 70, "intro without an end runs to the next chapter");
    check(jf::Client::emby_markers_of("{\"Chapters\":[]}").empty(), "no markers, no segments");
    std::printf("\n");
}

int main()
{
    emby_markers();
    const char *server = std::getenv("JF_URL"), *user = std::getenv("JF_USER"), *pass = std::getenv("JF_PASS");
    if (!server || !user || !pass) {
        std::fprintf(stderr, "JF_URL, JF_USER and JF_PASS must be set\n");
        return 2;
    }
    jf::Client c(server, "jelly5-dev-mac", "Mac (Jelly5 dev)");
    std::string name, version;
    jf::Kind kind = jf::Kind::Jellyfin;
    if (!c.public_info(&name, &version, nullptr, &kind)) {
        std::fprintf(stderr, "server unreachable: %s\n", c.last_error().c_str());
        return 1;
    }
    c.set_kind(kind);
    std::printf("server: %s (%s %s)\n", name.c_str(), jf::kind_key(kind), version.c_str());
    if (!c.authenticate(user, pass)) {
        std::fprintf(stderr, "sign-in failed: %s\n", c.last_error().c_str());
        return 1;
    }
    std::printf("signed in as %s\n\n", c.user_name().c_str());

    std::printf("browsing\n");
    check(c.validate(), "validate (the user)", c.last_error());
    jf::UserPrefs prefs;
    check(c.get_prefs(&prefs), "user prefs", "subtitles " + prefs.subtitle_mode);
    const std::vector<jf::Item> views = c.views();
    check(!views.empty(), "views", std::to_string(views.size()));
    std::string series_id, movie_id;
    for (const jf::Item &v : views) {
        const std::string types = v.collection_type == "music" ? "MusicAlbum"
                                  : v.collection_type == "tvshows" ? "Series" : "Movie";
        const jf::Page page = c.library(v.id, types, "SortName", false, 0, 20);
        const std::vector<jf::Item> latest = c.latest(v.id, 6);
        check(!page.items.empty() && page.total > 0, ("library " + v.name).c_str(),
              std::to_string(page.items.size()) + " of " + std::to_string(page.total) + ", latest " +
                  std::to_string(latest.size()));
        for (const jf::Item &it : page.items) {
            if (it.type == "Series" && series_id.empty()) series_id = it.id;
            if (it.type == "Movie" && movie_id.empty()) movie_id = it.id;
        }
        const int before = c.count_before(v.id, types, "", "M");
        check(before >= 0, ("A-Z jump in " + v.name).c_str(), std::to_string(before) + " before M");
    }
    std::printf("  rows: resume %zu, next up %zu, featured %zu, favourites %zu, genres %zu, home sections %zu\n",
                c.resume(12).size(), c.next_up(12).size(), c.featured(6).size(), c.favorites(12).size(),
                c.genres().size(), c.home_sections().size());
    if (!movie_id.empty()) {
        jf::Item it;
        jf::Detail d;
        check(c.item(movie_id, &it, &d), "movie details",
              it.name + ", " + std::to_string(d.streams.size()) + " streams");
        std::vector<jf::Chapter> chapters;
        jf::Trickplay tp;
        check(c.media_extras(movie_id, "", &chapters, &tp), "chapters",
              std::to_string(chapters.size()) + (tp.valid() ? ", trickplay" : ", no trickplay"));
        if (tp.valid()) {   /* the second sheet (Emby: thumbnail), as the player asks for it, without headers */
            const std::string url = tp.sheet_ticks > 0 ? tp.url_base + std::to_string(tp.first_ticks + tp.sheet_ticks) + tp.url_query
                                                       : tp.url_base + "1.jpg" + tp.url_query;
            const jf::HttpResponse r = jf::http_request("GET", url, {}, "", 15);
            check(r.ok() && r.body.size() > 1000, "trickplay sheet",
                  std::to_string(tp.count) + " thumbnails of " + std::to_string(tp.width) + "x" +
                      std::to_string(tp.height) + " every " + std::to_string((int)tp.interval) + " s, " +
                      std::to_string(r.body.size()) + " bytes");
        }
        std::printf("  similar %zu, extras %zu, trailers %zu\n", c.similar(movie_id, 6).size(),
                    c.special_features(movie_id).size(), c.local_trailers(movie_id).size());
    }
    if (!series_id.empty()) {
        const std::vector<jf::Item> seasons = c.seasons(series_id);
        check(!seasons.empty(), "seasons", std::to_string(seasons.size()));
        if (!seasons.empty())
            check(!c.episodes(series_id, seasons[0].id).empty(), "episodes of the first season");
    }
    std::printf("  search \"test\": %zu\n", c.search("test", "Movie,Series,MusicAlbum,Episode", 10).size());

    std::printf("\nplayback (continue watching, else the first movie)\n");
    std::vector<jf::Item> to_play = c.resume(12);
    if (to_play.empty() && !movie_id.empty()) {
        jf::Item it;
        if (c.item(movie_id, &it))
            to_play.push_back(it);
    }
    int direct = 0, total = 0;
    for (const jf::Item &it : to_play) {
        jf::Playback pb;
        const bool ok = c.playback_info(it.id, it.position_ticks, -1, -2, &pb);
        const jf::MediaStream *v = nullptr, *a = nullptr;
        for (const auto &s : pb.streams) {
            if (!v && s.type == "Video") v = &s;
            if (!a && s.type == "Audio" && s.index == pb.default_audio) a = &s;
        }
        total++;
        if (ok && pb.play_method == "DirectPlay") direct++;
        /* The player sends no headers: the URL alone must be let in. */
        const int status = ok ? jf::http_request("GET", pb.url, {"Range: bytes=0-15"}, "", 15).status : 0;
        std::printf("%-44.44s %-5s %-9s %-6s %4dx%-4d %-8s %-6s %dch  %s %s  url %d\n",
                    (it.series_name.empty() ? it.name : it.series_name + " - " + it.name).c_str(),
                    pb.container.c_str(), v ? v->codec.c_str() : "-", v ? v->video_range_type.c_str() : "-",
                    v ? v->width : 0, v ? v->height : 0, a ? a->codec.c_str() : "-",
                    a ? a->language.c_str() : "", a ? a->channels : 0,
                    ok ? pb.play_method.c_str() : "ERROR", ok ? pb.transcode_reasons.c_str() : c.last_error().c_str(),
                    status);
        if (ok && status != 200 && status != 206)
            s_failed++;
    }
    std::printf("\n%d of %d would play directly on the PS5\n", direct, total);
    auto next = c.next_up(1);
    if (!next.empty())
        std::printf("segments for %s: %zu\n", next[0].name.c_str(), c.segments(next[0].id).size());
    std::printf("%s\n", s_failed ? "SOME CHECKS FAILED" : "all checks passed");
    return s_failed ? 1 : 0;
}
