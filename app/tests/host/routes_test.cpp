/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every request the client makes, against a fake server that records them:
 * no network. Prints one "METHOD path" line per request. routes.sh builds it
 * against main's client too and compares the Jellyfin lines, so a change for
 * Emby that alters a Jellyfin request shows up here; the Emby lines are
 * checked against what Emby 4.10 answers (tests/host/run.sh emby).
 */
#include "jf_client.h"
#include "jf_http.h"

#include <cstdio>
#include <string>
#include <vector>

namespace jf {

static std::string s_server = "http://srv";

/* A JSON answer good enough for the parsers: a user, a playable item, else {}. */
HttpResponse http_request(const std::string &method, const std::string &url, const std::vector<std::string> &,
                          const std::string &, int)
{
    const std::string path = url.rfind(s_server, 0) == 0 ? url.substr(s_server.size()) : url;
    std::printf("%s %s\n", method.c_str(), path.c_str());
    HttpResponse r;
    r.status = 200;
    if (path.find("/PlaybackInfo") != std::string::npos)
        r.body = R"({"PlaySessionId":"ps1","MediaSources":[{"Id":"ms1","SupportsDirectPlay":true,)"
                 R"("MediaStreams":[{"Type":"Video","Codec":"hevc","Height":2160,"Index":0}]}]})";
    else if (path.rfind("/Users/Me", 0) == 0 || path.rfind("/Users/u1", 0) == 0)
        r.body = R"({"Name":"u","Configuration":{"AudioLanguagePreference":"nor","SubtitleMode":"Default"},"Policy":{}})";
    else if (path.find("/Lyrics") != std::string::npos || path.find("/ThumbnailSet") != std::string::npos)
        r.status = 404;
    else
        r.body = "{}";
    return r;
}

} // namespace jf

static void run(jf::Client &c)
{
    c.set_session("TOKEN", "u1", "u");
    std::string s;
    c.public_info(&s, &s);
    c.validate();
    jf::UserPrefs p;
    c.get_prefs(&p);
    c.set_prefs(p);
    c.public_users();
    c.resume(12);
    c.resume(12, "p1");
    c.next_up(12);
    c.next_up(1, "s1");
    c.views();
    c.featured(6);
    c.latest("v1", 20);
    c.episodes("s1", "se1");
    c.library("v1", "Movie", "SortName", false, 0, 50, "&Filters=IsFavorite");
    c.album_artists("v1", "SortName", false, 0, 50);
    c.genres_in("v1", "Movie");
    c.count_before("v1", "Movie", "", "M");
    c.home_sections();
    c.playlist_items("pl1");
    c.lyrics("a1");
    c.search("test", "Movie,Series", 10);
    c.recommendations(4, 10);
    c.genres();
    c.genre_items("Drama", 10);
    c.special_features("i1");
    c.theme_songs("i1");
    jf::Item it;
    jf::Detail d;
    c.item("i1", &it, &d);
    c.seasons("s1");
    c.similar("i1", 6);
    c.person_items("pe1", "Movie", 10);
    c.set_favorite("i1", true);
    c.set_favorite("i1", false);
    c.local_trailers("i1");
    c.set_played("i1", true);
    c.set_played("i1", false);
    c.clear_position("i1");
    c.favorites(30);
    c.children("b1", "SortName", 100);
    std::vector<jf::Chapter> ch;
    jf::Trickplay tp;
    c.media_extras("i1", "ms1", &ch, &tp);
    c.segments("i1");
    jf::Playback pb;
    c.playback_info("i1", 0, -1, -2, &pb);
    std::printf("URL %s\n", pb.url.substr(jf::s_server.size()).c_str());
    c.report_start(pb, 0);
    c.report_progress(pb, 10, false);
    c.report_stopped(pb, 20);
    c.instant_mix("a1", 50);
    c.post_capabilities();
    std::printf("IMAGE %s\n", c.image_url("i1", "Primary", "t1", 300).substr(jf::s_server.size()).c_str());
    std::printf("AUTH %s\n", c.auth_header().c_str());
}

int main()
{
    std::printf("== jellyfin\n");
    jf::Client jelly(jf::s_server, "dev1", "PS5");
    run(jelly);
#ifdef ROUTES_EMBY
    std::printf("== emby\n");
    jf::Client emby(jf::s_server, "dev1", "PS5");
    emby.set_kind(jf::Kind::Emby);
    run(emby);
    std::printf("SOCKET %s\n", emby.socket_url().substr(jf::s_server.size()).c_str());
#endif
    return 0;
}
