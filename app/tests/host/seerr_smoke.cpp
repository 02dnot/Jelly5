/*
 * Jelly5 — host smoke test for the Seerr client against a real server.
 * Signs in with the Jellyfin password from .env.local (--auth quickconnect
 * signs in the way the console does, approving a Quick Connect code in
 * Jellyfin: a write, so only when asked), then reads: search, discover, a film's
 * and a series' pages, the Radarr/Sonarr options, the quota and the user's
 * requests, and fetches a poster through Seerr's image cache. A request is
 * only shown (dry run) unless --for-real is given.
 *
 *   tests/host/seerr.sh [--query TEXT] [--lang fr] [--auth quickconnect|jellyfin|local]
 *                       [--request TMDB_ID [--tv] [--seasons 1,2] [--server N]
 *                        [--profile N] [--folder PATH] [--for-real]] [--keep]
 *
 * .env.local: SEERR_URL; JF_URL, JF_USER, JF_PASS (Quick Connect and the
 * Jellyfin password); SEERR_EMAIL, SEERR_PASS (a local account).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf/jf_client.h"
#include "jf/jf_http.h"
#include "seerr/seerr_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int s_failures = 0;

void check(bool ok, const char *what, const std::string &detail = std::string())
{
    std::printf("%s %s%s%s\n", ok ? "  ok  " : "  FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok)
        s_failures++;
}

const char *env(const char *k, const char *fallback = nullptr)
{
    const char *v = std::getenv(k);
    return v && *v ? v : fallback;
}

const char *status_name(seerr::Status s)
{
    switch (s) {
    case seerr::Status::Pending: return "pending";
    case seerr::Status::Processing: return "requested";
    case seerr::Status::PartiallyAvailable: return "partly available";
    case seerr::Status::Available: return "available";
    case seerr::Status::Blocklisted: return "blocklisted";
    case seerr::Status::Deleted: return "deleted";
    default: return "not requested";
    }
}

const char *outcome_name(seerr::RequestResult::Outcome o)
{
    using R = seerr::RequestResult;
    switch (o) {
    case R::Approved: return "approved";
    case R::Pending: return "pending approval";
    case R::NothingToRequest: return "nothing to request";
    case R::Duplicate: return "already requested";
    case R::QuotaReached: return "quota reached";
    case R::NotAllowed: return "not allowed";
    case R::SignedOut: return "signed out";
    case R::Unreachable: return "unreachable";
    default: return "failed";
    }
}

void print_titles(const std::vector<seerr::Title> &list, size_t max)
{
    for (size_t i = 0; i < list.size() && i < max; i++) {
        const seerr::Title &t = list[i];
        std::printf("        %-5s %7d  %4d  %-16s %-10.10s %s\n", t.tv ? "tv" : "movie", t.id, t.year,
                    status_name(t.status), t.jellyfin_id.empty() ? "-" : t.jellyfin_id.c_str(), t.name.c_str());
    }
}

/* The picture's first bytes: JPEG, PNG or WebP. */
bool looks_like_image(const std::string &b)
{
    return (b.size() > 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) ||
           (b.size() > 8 && b.compare(1, 3, "PNG") == 0) || (b.size() > 12 && b.compare(8, 4, "WEBP") == 0);
}

} // namespace

int main(int argc, char **argv)
{
    std::string query = env("SEERR_QUERY", "dune"), lang = env("SEERR_LANG", "fr");
    std::string auth = env("SEERR_AUTH", "jellyfin");   /* quickconnect approves a code in Jellyfin: only when asked */
    seerr::RequestOptions ro;
    bool for_real = false, keep = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--query") query = argv[++i];
        else if (a == "--lang") lang = argv[++i];
        else if (a == "--auth") auth = argv[++i];
        else if (a == "--request") ro.tmdb_id = std::atoi(argv[++i]);
        else if (a == "--tv") ro.tv = true;
        else if (a == "--server") ro.server_id = std::atoi(argv[++i]);
        else if (a == "--profile") ro.profile_id = std::atoi(argv[++i]);
        else if (a == "--folder") ro.root_folder = argv[++i];
        else if (a == "--for-real") for_real = true;
        else if (a == "--keep") keep = true;
        else if (a == "--seasons") {
            for (const char *p = next; *p;) {
                ro.seasons.push_back(std::atoi(p));
                const char *comma = std::strchr(p, ',');
                p = comma ? comma + 1 : p + std::strlen(p);
            }
            i++;
        } else {
            std::fprintf(stderr, "unknown option %s (see the comment at the top of seerr_smoke.cpp)\n", a.c_str());
            return 2;
        }
    }
    const char *url = env("SEERR_URL");
    if (!url) {
        std::fprintf(stderr, "SEERR_URL must be set (in .env.local)\n");
        return 2;
    }

    seerr::Client c(url);
    c.set_language(lang);
    std::printf("Seerr at %s\n", c.url().c_str());
    std::string version;
    const bool up = c.status(&version);
    check(up, "status (no sign-in)", up ? "Seerr " + version : c.last_error());
    if (!up)
        return 1;
    seerr::PublicSettings ps;
    if (c.public_settings(&ps))
        check(true, "public settings",
              "\"" + ps.title + "\", media server " + std::to_string(ps.media_server) + ", seasons one by one " +
                  (ps.partial_requests ? "yes" : "no") + ", specials " + (ps.special_episodes ? "yes" : "no") +
                  (ps.youtube_url.empty() ? "" : ", trailers on " + ps.youtube_url));
    else
        check(false, "public settings", c.last_error());
    seerr::User me;
    check(!c.me(&me), "signed out before signing in (401: no session cookie)", c.last_error());

    /* Sign in. */
    if (auth == "quickconnect" || auth == "jellyfin") {
        const char *jf_url = env("JF_URL"), *jf_user = env("JF_USER"), *jf_pass = env("JF_PASS", "");
        if (!jf_url || !jf_user) {
            std::fprintf(stderr, "JF_URL and JF_USER must be set (in .env.local)\n");
            return 2;
        }
        if (auth == "jellyfin") {
            check(c.sign_in_jellyfin(jf_user, jf_pass, &me), "sign-in with the Jellyfin password", c.last_error());
        } else {
            jf::Client jfc(jf_url, "jelly5-dev-host", "Jelly5 dev (host)");
            if (!jfc.authenticate(jf_user, jf_pass)) {
                std::fprintf(stderr, "Jellyfin sign-in failed: %s\n", jfc.last_error().c_str());
                return 1;
            }
            std::string shown;
            const bool ok = c.sign_in_quick_connect(
                [&](const std::string &code) {
                    shown = code;
                    return jfc.quick_connect_authorize(code);
                },
                &me);
            check(ok, "sign-in with Quick Connect", ok ? "code " + shown + " approved as " + jf_user
                                                        : c.last_error() + " / Jellyfin: " + jfc.last_error());
        }
    } else if (auth == "local") {
        check(c.sign_in_local(env("SEERR_EMAIL", ""), env("SEERR_PASS", ""), &me), "sign-in with a local account",
              c.last_error());
    } else {
        std::fprintf(stderr, "--auth: quickconnect, jellyfin or local\n");
        return 2;
    }
    if (!c.has_session()) {
        check(false, "session cookie", "none");
        return 1;
    }
    std::printf("        signed in as %s (Seerr user %d, permissions %u): request films %s, series %s, "
                "advanced options %s, auto-approved films %s, series %s\n",
                me.name.c_str(), me.id, me.permissions, me.can_request(false) ? "yes" : "no",
                me.can_request(true) ? "yes" : "no", me.advanced() ? "yes" : "no",
                me.auto_approved(false) ? "yes" : "no", me.auto_approved(true) ? "yes" : "no");
    {   /* What the app saves and reads back at the next launch. */
        seerr::Client again(url);
        again.set_cookies(c.cookies());
        seerr::User u;
        check(again.me(&u) && u.id == me.id, "the saved session works in a new client", again.last_error());
    }

    /* Search. */
    const std::vector<seerr::Title> found = c.search(query);
    check(!found.empty(), ("search \"" + query + "\"").c_str(), std::to_string(found.size()) + " films and series");
    print_titles(found, 10);

    /* Discover. */
    const struct {
        seerr::Client::Shelf shelf;
        const char *name;
    } shelves[] = {{seerr::Client::Shelf::Trending, "trending"},
                   {seerr::Client::Shelf::PopularMovies, "popular films"},
                   {seerr::Client::Shelf::PopularTv, "popular series"},
                   {seerr::Client::Shelf::UpcomingMovies, "upcoming films"},
                   {seerr::Client::Shelf::UpcomingTv, "upcoming series"}};
    std::string poster;
    for (const auto &s : shelves) {
        const std::vector<seerr::Title> list = c.discover(s.shelf);
        check(!list.empty(), (std::string("discover: ") + s.name).c_str(), std::to_string(list.size()) + " titles");
        print_titles(list, 3);
        if (poster.empty() && !list.empty())
            poster = list[0].poster;
    }

    /* Genre names (the Discover tab's info line). */
    const std::map<int, std::string> mg = c.genres(false), tg = c.genres(true);
    check(!mg.empty() && !tg.empty(), "genres", std::to_string(mg.size()) + " for films, " + std::to_string(tg.size()) +
                                                    " for series" + (mg.count(878) ? ", 878 = " + mg.at(878) : ""));

    /* Artwork through Seerr's cache (what the console shows, never TMDB itself). */
    if (!poster.empty()) {
        const std::string img = c.image_url(poster, "w342");
        const jf::HttpResponse r = jf::http_request("GET", img, {}, "", 10);
        check(r.ok() && looks_like_image(r.body), "poster through /imageproxy/tmdb",
              img + " -> " + std::to_string(r.status) + ", " + std::to_string(r.body.size()) + " bytes");
    }

    /* A film's and a series' pages. */
    int first_movie = 0, first_tv = 0;
    for (const seerr::Title &t : found) {
        if (!t.tv && !first_movie) first_movie = t.id;
        if (t.tv && !first_tv) first_tv = t.id;
    }
    seerr::Detail d;
    if (first_movie) {
        const bool ok = c.movie(first_movie, &d);
        check(ok, "film page", d.title.name + " (" + std::to_string(d.title.year) + "), " +
                                   std::to_string(d.runtime) + " min, " + std::to_string(d.genres.size()) +
                                   " genres, " + std::to_string(d.cast.size()) + " cast, " + status_name(d.title.status));
        const seerr::Video *v = d.trailer();
        std::printf("        trailer: %s\n", v ? (v->name + " " + v->url).c_str() : "none");
    }
    if (first_tv) {
        const bool ok = c.tv(first_tv, &d);
        check(ok, "series page", d.title.name + ", tvdb " + std::to_string(d.tvdb_id) +
                                     (d.anime ? ", anime" : "") + ", " + std::to_string(d.seasons.size()) + " seasons, " +
                                     status_name(d.title.status));
        for (const seerr::Season &s : d.seasons)
            std::printf("        season %2d  %3d episodes  %-16s %s %s\n", s.number, s.episodes, status_name(s.status),
                        s.requested ? "requested" : "         ", s.name.c_str());
    }

    /* Radarr and Sonarr: what the advanced options offer. */
    for (int tv = 0; tv < 2; tv++) {
        const std::vector<seerr::Server> list = c.servers(tv);
        check(!list.empty(), tv ? "Sonarr servers" : "Radarr servers",
              list.empty() ? c.last_error() : std::to_string(list.size()));
        for (const seerr::Server &s : list) {
            seerr::Server full;
            const bool ok = c.server_details(tv, s.id, &full);
            check(ok, ("  " + s.name).c_str(),
                  ok ? std::to_string(full.profiles.size()) + " profiles, " + std::to_string(full.folders.size()) +
                           " folders" + (s.is_default ? ", default" : "") + (s.is4k ? ", 4K" : "")
                     : c.last_error());
            for (const seerr::Profile &p : full.profiles)
                std::printf("        profile %3d  %s%s\n", p.id, p.name.c_str(),
                            p.id == full.profile_id ? "  <- default" : p.id == full.anime_profile_id ? "  <- anime" : "");
            for (const seerr::RootFolder &f : full.folders)
                std::printf("        folder  %3d  %s (%lld GB free)%s\n", f.id, f.path.c_str(),
                            (long long)(f.free_space >> 30),
                            f.path == full.folder ? "  <- default" : f.path == full.anime_folder ? "  <- anime" : "");
        }
    }

    /* Quota and the user's own requests. */
    seerr::Quota mq, tq;
    check(c.quota(me.id, &mq, &tq), "quota",
          "films " + std::to_string(mq.used) + "/" + (mq.limit ? std::to_string(mq.limit) : "no limit") +
              (mq.days ? " in " + std::to_string(mq.days) + " days" : "") + ", series " + std::to_string(tq.used) +
              "/" + (tq.limit ? std::to_string(tq.limit) : "no limit"));
    const std::vector<seerr::Request> mine = c.requests(me.id, 5);
    check(true, "my requests", std::to_string(mine.size()) + " (the 5 newest)");
    for (const seerr::Request &r : mine)
        std::printf("        #%-5d %-5s %7d  request status %d, %s\n", r.id, r.tv ? "tv" : "movie", r.tmdb_id,
                    (int)r.status, status_name(r.media_status));

    /* A request: shown, sent only with --for-real. */
    if (!ro.tmdb_id) {
        for (const seerr::Title &t : found)
            if (t.status == seerr::Status::Unknown) {
                ro.tmdb_id = t.id;
                ro.tv = t.tv;
                break;
            }
        for_real = false;   /* a title picked by the test is never really requested */
    }
    if (ro.tmdb_id) {
        if (ro.tv && !ro.tvdb_id && c.tv(ro.tmdb_id, &d))
            ro.tvdb_id = d.tvdb_id;
        std::printf("\n  request %s\n", seerr::Client::request_json(ro).c_str());
        if (for_real) {
            const seerr::RequestResult r = c.request(ro);
            check(r.outcome == seerr::RequestResult::Approved || r.outcome == seerr::RequestResult::Pending,
                  "request sent", std::string(outcome_name(r.outcome)) + (r.id ? ", #" + std::to_string(r.id) : "") +
                                      (c.last_error().empty() ? "" : " (" + c.last_error() + ")"));
        } else {
            std::printf("        dry run: not sent (--request <tmdb id> ... --for-real sends it)\n");
        }
    }

    if (!keep) {   /* the session ends on the server: its cookie no longer works */
        const std::string saved = c.cookies();
        c.sign_out();
        seerr::Client again(url);
        again.set_cookies(saved);
        seerr::User u;
        check(!again.me(&u), "signed out", again.last_error());
    }
    std::printf("\n%s: %d failure%s\n", s_failures ? "FAILED" : "PASSED", s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
