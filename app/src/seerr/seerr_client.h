/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Seerr (https://github.com/seerr-team/seerr, the successor of Overseerr and
 * Jellyseerr) as Jelly5 uses it: sign-in, search, discover, a title's details,
 * the Radarr/Sonarr options and requests, on its API (/api/v1). Seerr asks
 * TMDB for everything, so the console only ever talks to Seerr: artwork comes
 * through Seerr's image cache too (image_url).
 *
 * Plain blocking calls on jf::http_request; callers keep them off the render
 * thread. Builds on the console and on a development machine (tests/host).
 *
 * The session is Seerr's cookie (connect.sid), kept here and saved by the
 * app (cookies / set_cookies). Without one Seerr answers 401 (its API
 * validator); with one that has expired, 403, as for a missing permission.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

namespace seerr {

/* Where a title stands (Seerr's MediaStatus). */
enum class Status {
    Unknown = 1,            /* not requested (or no media row yet) */
    Pending,                /* requested, waiting for approval */
    Processing,             /* approved, on its way (Radarr/Sonarr) */
    PartiallyAvailable,     /* some seasons are on the media server */
    Available,
    Blocklisted,
    Deleted,                /* was there, removed: requestable again */
};

/* A request's own state (Seerr's MediaRequestStatus). */
enum class RequestStatus { Pending = 1, Approved, Declined, Failed, Completed };

/* The permission bits Jelly5 looks at (Seerr's server/lib/permissions.ts). */
enum Permission : uint32_t {
    kAdmin = 2,
    kManageRequests = 16,
    kRequest = 32,
    kAutoApprove = 128,
    kAutoApproveMovie = 256,
    kAutoApproveTv = 512,
    kRequestAdvanced = 8192,
    kRequestMovie = 262144,
    kRequestTv = 524288,
    kManageBlocklist = 268435456,
};

struct User {
    int id = 0;
    std::string name;                   /* Seerr's display name */
    uint32_t permissions = 0;
    /* As Seerr checks it: an administrator has every permission. */
    bool has(uint32_t p) const { return (permissions & kAdmin) || (permissions & p); }
    bool can_request(bool tv) const { return has(kRequest) || has(tv ? kRequestTv : kRequestMovie); }
    /* May choose the server, quality profile and folder of a request. */
    bool advanced() const { return has(kManageRequests) || has(kRequestAdvanced); }
    /* As Seerr's server: managing requests approves one's own as well. */
    bool auto_approved(bool tv) const
    {
        return has(kAutoApprove) || has(tv ? kAutoApproveTv : kAutoApproveMovie) || has(kManageRequests);
    }
};

/* What the server lets everyone know (/settings/public). */
struct PublicSettings {
    std::string title;                  /* the instance's name ("Seerr") */
    int media_server = 0;               /* 1 Plex, 2 Jellyfin, 3 Emby, 4 not set up */
    bool media_server_login = true, local_login = true;
    bool partial_requests = true;       /* a series' seasons can be requested one by one */
    bool special_episodes = false;      /* season 0 can be requested */
    std::string youtube_url;            /* where trailers open, when set (an Invidious, say) */
    /* The administrator's "hide" choices for Discover and search (Seerr's own
     * pages apply them as seerr_service::visible does). */
    bool hide_available = false, hide_blocklisted = false, hide_requested = false;
};

/* A film or series, as search and discover list them. */
struct Title {
    int id = 0;                         /* TMDB's */
    bool tv = false;
    std::string name, overview;
    std::string poster, backdrop;       /* TMDB paths ("/abc.jpg"): see Client::image_url */
    std::string date;                   /* release or first air date (ISO) */
    int year = 0;
    double vote = 0;                    /* TMDB's average, 0-10 */
    std::vector<int> genre_ids;
    Status status = Status::Unknown;
    std::string jellyfin_id;            /* the media server's item, when it has the title */
};

struct Season {
    int number = 0;
    std::string name, air_date;
    int episodes = 0;
    Status status = Status::Unknown;    /* on the media server */
    bool requested = false;             /* in a request that is pending or approved */
};

struct Video {
    std::string name, type, site, key, url;   /* type: Trailer, Teaser ...; url: on YouTube or Vimeo */
    int size = 0;                       /* its height (1080 ...) */
};

/* A title's page. */
struct Detail {
    Title title;
    std::string tagline;
    int runtime = 0;                    /* minutes (films) */
    std::vector<std::string> genres;
    std::vector<std::string> cast;      /* the first names billed */
    std::vector<Season> seasons;        /* series: TMDB's, with where each stands */
    std::vector<Video> videos;
    int tvdb_id = 0;                    /* series */
    bool anime = false;                 /* TMDB's "anime" keyword: Sonarr's anime defaults apply */
    /* Requests for it still waiting for approval: theirs, and who asked (a user
     * may withdraw their own: Client::cancel_request). */
    struct Waiting {
        int id = 0, user = 0;
    };
    std::vector<Waiting> waiting;
    /* The video to offer as the trailer, as Seerr's own page picks it (its
     * largest trailer; else any video it has), or null. */
    const Video *trailer() const;
    /* Its address: the YouTube URL set in Seerr (an Invidious, say) when there is one. */
    std::string trailer_url(const std::string &youtube_url) const;
};

struct Profile {
    int id = 0;
    std::string name;
};

struct RootFolder {
    int id = 0;
    std::string path;
    int64_t free_space = 0;             /* bytes */
};

/* A Radarr or Sonarr server as Seerr has it set up. */
struct Server {
    int id = 0;
    std::string name;
    bool is4k = false, is_default = false;
    int profile_id = 0;                 /* Seerr's defaults for it */
    std::string folder;
    int anime_profile_id = 0;           /* Sonarr: the defaults for anime */
    std::string anime_folder;
    std::vector<Profile> profiles;      /* filled by server_details */
    std::vector<RootFolder> folders;
};

/* A user's request limit for films or for series. limit 0: none. */
struct Quota {
    int limit = 0, days = 0, used = 0, remaining = 0;
    bool restricted = false;            /* reached */
};

/* One of the user's requests. */
struct Request {
    int id = 0;
    int tmdb_id = 0;
    bool tv = false;
    RequestStatus status = RequestStatus::Pending;
    Status media_status = Status::Unknown;
    std::string created;                /* ISO time */
};

struct RequestOptions {
    int tmdb_id = 0;
    bool tv = false;
    int tvdb_id = 0;                    /* series: sent when known */
    std::vector<int> seasons;           /* series: these; empty: every one still missing */
    /* Advanced (User::advanced): -1 / empty leaves Seerr's defaults and its
     * override rules to decide. */
    int server_id = -1, profile_id = -1;
    std::string root_folder;
};

struct RequestResult {
    enum Outcome {
        Approved,                       /* sent on to Radarr/Sonarr */
        Pending,                        /* waits for an administrator */
        NothingToRequest,               /* every season is there or asked for already */
        Duplicate,                      /* asked for already */
        QuotaReached,
        NotAllowed,
        SignedOut,                      /* the session ended: sign in again */
        Unreachable,
        Failed,
    };
    Outcome outcome = Failed;
    int id = 0;                         /* the new request's */
};

class Client {
public:
    explicit Client(const std::string &url = std::string());

    /* "192.168.1.20" -> "http://192.168.1.20:5055": a scheme and a port are kept. */
    static std::string normalize(const std::string &url);
    void set_url(const std::string &url);
    const std::string &url() const { return url_; }
    void set_timeout(int seconds) { timeout_ = seconds; }
    /* TMDB's language for names and overviews ("fr", "en" ...); empty: Seerr's.
     * Safe while requests run (the interface's language changed). */
    void set_language(const std::string &lang);

    /* The session as "name=value; ..." (Seerr's cookies), to keep between launches. */
    std::string cookies() const;
    void set_cookies(const std::string &cookies);
    bool has_session() const;
    /* Safe from any thread: requests may run in parallel. */
    std::string last_error() const;
    /* The last request got no answer at all (wrong address, server down, timeout). */
    bool last_unreachable() const;
    /* The last request's HTTP status (0: no answer). 401 or 403 on a page that
     * needs a session: it has ended (seerr_service::session_lost). */
    int last_status() const;

    /* No sign-in needed. */
    bool status(std::string *version);
    bool public_settings(PublicSettings *out);
    /* The signed-in user; false when signed out (or out of reach). */
    bool me(User *out);

    /* Quick Connect (Seerr 3.4 and later): Seerr asks Jellyfin for a code, approve
     * approves it (Jelly5: on Jellyfin, with the account in use), Seerr signs in
     * as that Jellyfin user. No password; the right Seerr account. */
    using Approve = std::function<bool(const std::string &code)>;
    bool sign_in_quick_connect(const Approve &approve, User *out);
    bool sign_in_jellyfin(const std::string &user, const std::string &password, User *out);
    bool sign_in_local(const std::string &email, const std::string &password, User *out);
    void sign_out();

    /* pages: Seerr's total (totalPages), when wanted: the end of the list, as its own pages read it. */
    std::vector<Title> search(const std::string &query, int page = 1, int *pages = nullptr);
    enum class Shelf { Trending, PopularMovies, PopularTv, UpcomingMovies, UpcomingTv };
    std::vector<Title> discover(Shelf shelf, int page = 1, int *pages = nullptr);
    /* A title's recommendations, or (similar) the titles like it: TMDB's lists, through Seerr. */
    std::vector<Title> related(int tmdb_id, bool tv, bool similar, int page = 1);
    bool movie(int tmdb_id, Detail *out);
    bool tv(int tmdb_id, Detail *out);
    /* TMDB's genre names for films or series, by id (in the language set). */
    std::map<int, std::string> genres(bool tv);

    /* Radarr (films) or Sonarr (series) servers, then one's profiles and folders. */
    std::vector<Server> servers(bool tv);
    bool server_details(bool tv, int server_id, Server *out);
    bool quota(int user_id, Quota *movie, Quota *tv);
    std::vector<Request> requests(int user_id, int take);

    /* The body request() sends (shown as is by the host test's dry run). */
    static std::string request_json(const RequestOptions &o);
    RequestResult request(const RequestOptions &o);
    /* Withdraws a request still waiting for approval (DELETE /request/{id}). */
    bool cancel_request(int request_id);

    /* Through Seerr's image cache: /imageproxy/tmdb/... is always served (the
     * "cache images" setting only changes Seerr's own web pages) and needs no
     * sign-in. size: TMDB's, "w342", "w780", "original" ... Empty path: "". */
    std::string image_url(const std::string &path, const char *size) const;

private:
    struct Reply {
        int status = 0;
        std::string body;
    };
    Reply call(const char *method, const std::string &path, const std::string &body);
    bool get(const std::string &path, std::string *body);
    bool post(const std::string &path, const std::string &json, std::string *body);
    std::string with_language(const std::string &path) const;
    bool user_from(const std::string &body, User *out);
    std::vector<Title> titles_from(const std::string &body, int *pages = nullptr);
    void set_error(std::string e);

    std::string url_, language_;
    int timeout_ = 6;
    mutable std::mutex lock_;           /* the cookies and the last calls */
    std::map<std::string, std::string> cookies_;
    /* How the last call went, per thread: screens share the client, and a status
     * read after one's own call must be that call's, not a neighbour's. */
    struct Last {
        std::string error;
        bool unreachable = false;
        int status = 0;
    };
    std::map<std::thread::id, Last> last_;
    Last &last_here();                  /* with lock_ held */
};

} // namespace seerr
