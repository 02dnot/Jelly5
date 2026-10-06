/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "seerr/seerr_client.h"

#include "jf/jf_http.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace seerr {
namespace {

constexpr int kAnimeKeyword = 210024;   /* TMDB's "anime", as Seerr's own pages check it */

std::string str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : std::string();
}

double num_of(const cJSON *o, const char *key, double fallback = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

int int_of(const cJSON *o, const char *key, int fallback = 0) { return (int)num_of(o, key, fallback); }

bool bool_of(const cJSON *o, const char *key, bool fallback = false)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

std::string url_escape(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        a++;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

int year_of(const std::string &date) { return date.size() >= 4 ? std::atoi(date.substr(0, 4).c_str()) : 0; }

Status status_of(int v) { return v >= 1 && v <= 7 ? (Status)v : Status::Unknown; }

/* A title's fields from a result or a details page (Seerr names films' and
 * series' fields differently: title / name, releaseDate / firstAirDate). */
Title title_of(const cJSON *o, bool tv)
{
    Title t;
    t.id = int_of(o, "id");
    t.tv = tv;
    t.name = str_of(o, tv ? "name" : "title");
    t.overview = str_of(o, "overview");
    t.poster = str_of(o, "posterPath");
    t.backdrop = str_of(o, "backdropPath");
    t.date = str_of(o, tv ? "firstAirDate" : "releaseDate");
    t.year = year_of(t.date);
    t.vote = num_of(o, "voteAverage");
    const cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(o, "genreIds"))
        if (cJSON_IsNumber(g))
            t.genre_ids.push_back(g->valueint);
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(o, "genres"))   /* details: [{id, name}] */
        t.genre_ids.push_back(int_of(g, "id"));
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(o, "mediaInfo");
    if (cJSON_IsObject(media)) {
        t.status = status_of(int_of(media, "status", 1));
        t.jellyfin_id = str_of(media, "jellyfinMediaId");
    }
    return t;
}

/* The server defaults Seerr has for a Radarr/Sonarr server (list and details share them). */
void server_fields(const cJSON *o, Server *s)
{
    s->id = int_of(o, "id");
    s->name = str_of(o, "name");
    s->is4k = bool_of(o, "is4k");
    s->is_default = bool_of(o, "isDefault");
    s->profile_id = int_of(o, "activeProfileId");
    s->folder = str_of(o, "activeDirectory");
    s->anime_profile_id = int_of(o, "activeAnimeProfileId");
    s->anime_folder = str_of(o, "activeAnimeDirectory");
}

void quota_fields(const cJSON *o, Quota *q)
{
    if (!q)
        return;
    q->limit = int_of(o, "limit");
    q->days = int_of(o, "days");
    q->used = int_of(o, "used");
    q->remaining = int_of(o, "remaining");
    q->restricted = bool_of(o, "restricted");
}

} // namespace

const Video *Detail::trailer() const
{
    const Video *any = nullptr, *trailer = nullptr;
    for (const Video &v : videos) {
        if (v.url.empty())
            continue;
        if (v.type == "Trailer" && (!trailer || v.size >= trailer->size))
            trailer = &v;
        if (!any)
            any = &v;
    }
    return trailer ? trailer : any;
}

std::string Detail::trailer_url(const std::string &youtube_url) const
{
    const Video *v = trailer();
    if (!v)
        return std::string();
    if (v->site == "YouTube" && !youtube_url.empty() && !v->key.empty())
        return youtube_url + v->key;   /* as Seerr's page builds it */
    return v->url;
}

Client::Client(const std::string &url) { set_url(url); }

std::string Client::normalize(const std::string &typed)
{
    std::string u = trim(typed);
    while (!u.empty() && u.back() == '/')
        u.pop_back();
    if (u.empty() || u.find("://") != std::string::npos)
        return u;
    /* Typed without a scheme: plain http, and a bare host gets Seerr's port. */
    u = "http://" + u;
    if (u.find(':', 7) == std::string::npos && u.find('/', 7) == std::string::npos)
        u += ":5055";
    return u;
}

void Client::set_url(const std::string &url) { url_ = normalize(url); }

void Client::set_language(const std::string &lang)
{
    std::lock_guard<std::mutex> g(lock_);
    language_ = lang;
}

std::string Client::cookies() const
{
    std::lock_guard<std::mutex> g(lock_);
    std::string out;
    for (const auto &kv : cookies_)
        out += (out.empty() ? "" : "; ") + kv.first + "=" + kv.second;
    return out;
}

void Client::set_cookies(const std::string &cookies)
{
    std::lock_guard<std::mutex> g(lock_);
    cookies_.clear();
    for (size_t from = 0; from < cookies.size();) {
        size_t to = cookies.find(';', from);
        if (to == std::string::npos)
            to = cookies.size();
        const std::string pair = trim(cookies.substr(from, to - from));
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 1 < pair.size())
            cookies_[pair.substr(0, eq)] = pair.substr(eq + 1);
        from = to + 1;
    }
}

bool Client::has_session() const
{
    std::lock_guard<std::mutex> g(lock_);
    return cookies_.count("connect.sid") > 0;
}

std::string Client::last_error() const
{
    std::lock_guard<std::mutex> g(lock_);
    return error_;
}

bool Client::last_unreachable() const
{
    std::lock_guard<std::mutex> g(lock_);
    return unreachable_;
}

int Client::last_status() const
{
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

void Client::set_error(std::string e)
{
    std::lock_guard<std::mutex> g(lock_);
    error_ = std::move(e);
}

Client::Reply Client::call(const char *method, const std::string &path, const std::string &body)
{
    std::vector<std::string> headers{"Accept: application/json"};
    {
        std::lock_guard<std::mutex> g(lock_);
        std::string jar;
        for (const auto &kv : cookies_)
            jar += (jar.empty() ? "" : "; ") + kv.first + "=" + kv.second;
        if (!jar.empty())
            headers.push_back("Cookie: " + jar);
        /* Seerr's optional CSRF protection: the token it set, echoed on changes. */
        const auto xsrf = cookies_.find("XSRF-TOKEN");
        if (xsrf != cookies_.end() && std::strcmp(method, "GET") != 0)
            headers.push_back("X-XSRF-TOKEN: " + xsrf->second);
    }
    jf::HttpResponse r = jf::http_request(method, url_ + "/api/v1" + path, headers, body, timeout_);
    {
        std::lock_guard<std::mutex> g(lock_);
        unreachable_ = r.status == 0;
        status_ = r.status;
        for (const std::string &c : r.cookies) {
            const size_t eq = c.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            const size_t end = c.find(';', eq);
            const std::string name = trim(c.substr(0, eq));
            const std::string value = c.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
            const std::string attrs = end == std::string::npos ? std::string() : lower(c.substr(end));
            const size_t age = attrs.find("max-age=");
            const bool gone = value.empty() || attrs.find("1970") != std::string::npos ||
                              (age != std::string::npos && std::atoi(attrs.c_str() + age + 8) <= 0);
            if (gone)
                cookies_.erase(name);
            else
                cookies_[name] = value;
        }
    }
    if (!r.ok())   /* the query is left out: a secret may ride in it */
        set_error(std::string(method) + " " + path.substr(0, path.find('?')) + " -> " + std::to_string(r.status) +
                  (r.error.empty() ? "" : " " + r.error));
    return {r.status, std::move(r.body)};
}

bool Client::get(const std::string &path, std::string *body)
{
    Reply r = call("GET", path, "");
    if (r.status < 200 || r.status >= 300)
        return false;
    *body = std::move(r.body);
    return true;
}

bool Client::post(const std::string &path, const std::string &json, std::string *body)
{
    Reply r = call("POST", path, json);
    if (r.status < 200 || r.status >= 300)
        return false;
    if (body)
        *body = std::move(r.body);
    return true;
}

std::string Client::with_language(const std::string &path) const
{
    std::string lang;
    {
        std::lock_guard<std::mutex> g(lock_);
        lang = language_;
    }
    if (lang.empty())
        return path;
    return path + (path.find('?') == std::string::npos ? "?" : "&") + "language=" + url_escape(lang);
}

bool Client::status(std::string *version)
{
    std::string body;
    if (!get("/status", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const std::string v = str_of(j, "version");
    cJSON_Delete(j);
    if (v.empty()) {
        set_error("GET /status: not a Seerr server");
        return false;
    }
    if (version)
        *version = v;
    return true;
}

bool Client::public_settings(PublicSettings *out)
{
    std::string body;
    if (!get("/settings/public", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    out->title = str_of(j, "applicationTitle");
    out->media_server = int_of(j, "mediaServerType");
    out->media_server_login = bool_of(j, "mediaServerLogin", true);
    out->local_login = bool_of(j, "localLogin", true);
    out->partial_requests = bool_of(j, "partialRequestsEnabled", true);
    out->special_episodes = bool_of(j, "enableSpecialEpisodes");
    out->youtube_url = str_of(j, "youtubeUrl");
    cJSON_Delete(j);
    return true;
}

bool Client::user_from(const std::string &body, User *out)
{
    cJSON *j = cJSON_Parse(body.c_str());
    const int id = int_of(j, "id");
    if (id > 0 && out) {
        out->id = id;
        out->name = str_of(j, "displayName");
        for (const char *k : {"username", "jellyfinUsername", "email"})
            if (out->name.empty())
                out->name = str_of(j, k);
        out->permissions = (uint32_t)(int64_t)num_of(j, "permissions");
    }
    cJSON_Delete(j);
    if (id <= 0)
        set_error("Seerr sent no user");
    return id > 0;
}

bool Client::me(User *out)
{
    std::string body;
    return get("/auth/me", &body) && user_from(body, out);
}

bool Client::sign_in_quick_connect(const Approve &approve, User *out)
{
    std::string body;
    if (!post("/auth/jellyfin/quickconnect/initiate", "", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const std::string code = str_of(j, "code"), secret = str_of(j, "secret");
    cJSON_Delete(j);
    if (code.empty() || secret.empty()) {
        set_error("Quick Connect: Seerr sent no code");
        return false;
    }
    if (!approve(code)) {
        set_error("Quick Connect: the code was not approved on Jellyfin");
        return false;
    }
    /* Jellyfin knows at once; a few checks in case it takes a moment. */
    for (int i = 0; i < 10; i++) {
        bool approved = false;
        if (get("/auth/jellyfin/quickconnect/check?secret=" + url_escape(secret), &body)) {
            cJSON *c = cJSON_Parse(body.c_str());
            approved = bool_of(c, "authenticated");
            cJSON_Delete(c);
        }
        if (approved)
            break;
        usleep(300 * 1000);
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "secret", secret.c_str());
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    const bool ok = post("/auth/jellyfin/quickconnect/authenticate", text, &body);
    std::free(text);
    return ok && user_from(body, out);
}

bool Client::sign_in_jellyfin(const std::string &user, const std::string &password, User *out)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "username", user.c_str());
    cJSON_AddStringToObject(req, "password", password.c_str());
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    std::string body;
    const bool ok = post("/auth/jellyfin", text, &body);
    std::free(text);
    return ok && user_from(body, out);
}

bool Client::sign_in_local(const std::string &email, const std::string &password, User *out)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "email", email.c_str());
    cJSON_AddStringToObject(req, "password", password.c_str());
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    std::string body;
    const bool ok = post("/auth/local", text, &body);
    std::free(text);
    return ok && user_from(body, out);
}

void Client::sign_out()
{
    post("/auth/logout", "", nullptr);
    std::lock_guard<std::mutex> g(lock_);
    cookies_.clear();
}

std::vector<Title> Client::titles_from(const std::string &body)
{
    std::vector<Title> out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(j, "results")) {
        const std::string type = str_of(r, "mediaType");   /* people and collections are left out */
        if (type == "movie" || type == "tv")
            out.push_back(title_of(r, type == "tv"));
    }
    cJSON_Delete(j);
    return out;
}

std::vector<Title> Client::search(const std::string &query, int page)
{
    std::string body;
    if (!get(with_language("/search?query=" + url_escape(query) + "&page=" + std::to_string(page)), &body))
        return {};
    return titles_from(body);
}

std::vector<Title> Client::discover(Shelf shelf, int page)
{
    const char *path = "/discover/trending";
    switch (shelf) {
    case Shelf::Trending: break;
    case Shelf::PopularMovies: path = "/discover/movies"; break;
    case Shelf::PopularTv: path = "/discover/tv"; break;
    case Shelf::UpcomingMovies: path = "/discover/movies/upcoming"; break;
    case Shelf::UpcomingTv: path = "/discover/tv/upcoming"; break;
    }
    std::string body;
    if (!get(with_language(std::string(path) + "?page=" + std::to_string(page)), &body))
        return {};
    return titles_from(body);
}

/* What films' and series' pages share. */
static void detail_fields(const cJSON *j, bool tv, Detail *out)
{
    out->title = title_of(j, tv);
    out->tagline = str_of(j, "tagline");
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "genres"))
        out->genres.push_back(str_of(v, "name"));
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "credits"), "cast"))
        if (out->cast.size() < 8)
            out->cast.push_back(str_of(v, "name"));
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "relatedVideos"))
        out->videos.push_back({str_of(v, "name"), str_of(v, "type"), str_of(v, "site"), str_of(v, "key"),
                               str_of(v, "url"), int_of(v, "size")});
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "keywords"))
        if (int_of(v, "id") == kAnimeKeyword)
            out->anime = true;
}

bool Client::movie(int tmdb_id, Detail *out)
{
    std::string body;
    if (!get(with_language("/movie/" + std::to_string(tmdb_id)), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    *out = Detail();
    detail_fields(j, false, out);
    out->runtime = int_of(j, "runtime");
    cJSON_Delete(j);
    return out->title.id > 0;
}

bool Client::tv(int tmdb_id, Detail *out)
{
    std::string body;
    if (!get(with_language("/tv/" + std::to_string(tmdb_id)), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    *out = Detail();
    detail_fields(j, true, out);
    if (const cJSON *rt = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "episodeRunTime"), 0))
        out->runtime = cJSON_IsNumber(rt) ? rt->valueint : 0;
    out->tvdb_id = int_of(cJSON_GetObjectItemCaseSensitive(j, "externalIds"), "tvdbId");
    /* Where each season stands: on the server (mediaInfo.seasons), or asked for
     * in a request still pending or approved (4K requests are another matter). */
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(j, "mediaInfo");
    std::map<int, Status> on_server;
    std::map<int, bool> requested;
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(media, "seasons"))
        on_server[int_of(s, "seasonNumber")] = status_of(int_of(s, "status", 1));
    const cJSON *rq;
    cJSON_ArrayForEach(rq, cJSON_GetObjectItemCaseSensitive(media, "requests")) {
        const int st = int_of(rq, "status");
        if (bool_of(rq, "is4k") ||
            (st != (int)RequestStatus::Pending && st != (int)RequestStatus::Approved))
            continue;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(rq, "seasons"))
            requested[int_of(s, "seasonNumber")] = true;
    }
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "seasons")) {
        Season season;
        season.number = int_of(s, "seasonNumber");
        season.name = str_of(s, "name");
        season.air_date = str_of(s, "airDate");
        season.episodes = int_of(s, "episodeCount");
        const auto st = on_server.find(season.number);
        if (st != on_server.end())
            season.status = st->second;
        season.requested = requested.count(season.number) > 0;
        out->seasons.push_back(season);
    }
    cJSON_Delete(j);
    return out->title.id > 0;
}

std::map<int, std::string> Client::genres(bool tv)
{
    std::map<int, std::string> out;
    std::string body;
    if (!get(with_language(tv ? "/genres/tv" : "/genres/movie"), &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *g;
    cJSON_ArrayForEach(g, j)
        out[int_of(g, "id")] = str_of(g, "name");
    cJSON_Delete(j);
    return out;
}

std::vector<Server> Client::servers(bool tv)
{
    std::vector<Server> out;
    std::string body;
    if (!get(tv ? "/service/sonarr" : "/service/radarr", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *s;
    cJSON_ArrayForEach(s, j) {
        Server srv;
        server_fields(s, &srv);
        out.push_back(srv);
    }
    cJSON_Delete(j);
    return out;
}

bool Client::server_details(bool tv, int server_id, Server *out)
{
    std::string body;
    if (!get(std::string(tv ? "/service/sonarr/" : "/service/radarr/") + std::to_string(server_id), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    *out = Server();
    server_fields(cJSON_GetObjectItemCaseSensitive(j, "server"), out);
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "profiles"))
        out->profiles.push_back({int_of(v, "id"), str_of(v, "name")});
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "rootFolders"))
        out->folders.push_back({int_of(v, "id"), str_of(v, "path"), (int64_t)num_of(v, "freeSpace")});
    cJSON_Delete(j);
    return out->id == server_id;
}

bool Client::quota(int user_id, Quota *movie, Quota *tv)
{
    std::string body;
    if (!get("/user/" + std::to_string(user_id) + "/quota", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    quota_fields(cJSON_GetObjectItemCaseSensitive(j, "movie"), movie);
    quota_fields(cJSON_GetObjectItemCaseSensitive(j, "tv"), tv);
    cJSON_Delete(j);
    return true;
}

std::vector<Request> Client::requests(int user_id, int take)
{
    std::vector<Request> out;
    std::string body;
    if (!get("/user/" + std::to_string(user_id) + "/requests?take=" + std::to_string(take) + "&skip=0", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(j, "results")) {
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(r, "media");
        Request q;
        q.id = int_of(r, "id");
        q.tmdb_id = int_of(media, "tmdbId");
        q.tv = str_of(r, "type") == "tv" || str_of(media, "mediaType") == "tv";
        q.status = (RequestStatus)int_of(r, "status", 1);
        q.media_status = status_of(int_of(media, "status", 1));
        q.created = str_of(r, "createdAt");
        if (q.tmdb_id > 0)
            out.push_back(q);
    }
    cJSON_Delete(j);
    return out;
}

std::string Client::request_json(const RequestOptions &o)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mediaType", o.tv ? "tv" : "movie");
    cJSON_AddNumberToObject(j, "mediaId", o.tmdb_id);
    if (o.tv) {
        if (o.tvdb_id > 0)
            cJSON_AddNumberToObject(j, "tvdbId", o.tvdb_id);
        if (o.seasons.empty()) {
            cJSON_AddStringToObject(j, "seasons", "all");
        } else {
            cJSON *s = cJSON_CreateArray();
            for (int n : o.seasons)
                cJSON_AddItemToArray(s, cJSON_CreateNumber(n));
            cJSON_AddItemToObject(j, "seasons", s);
        }
    }
    if (o.server_id >= 0)
        cJSON_AddNumberToObject(j, "serverId", o.server_id);
    if (o.profile_id >= 0)
        cJSON_AddNumberToObject(j, "profileId", o.profile_id);
    if (!o.root_folder.empty())
        cJSON_AddStringToObject(j, "rootFolder", o.root_folder.c_str());
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    std::string out = text ? text : "";
    std::free(text);
    return out;
}

RequestResult Client::request(const RequestOptions &o)
{
    RequestResult res;
    Reply r = call("POST", "/request", request_json(o));
    if (r.status >= 200 && r.status < 300) {
        /* 201 with the request; 202 (nothing left to ask for) carries only a
         * message. The console sees both as 200, so the body tells them apart. */
        cJSON *j = cJSON_Parse(r.body.c_str());
        res.id = int_of(j, "id");
        const int st = int_of(j, "status");
        cJSON_Delete(j);
        if (res.id <= 0)
            res.outcome = RequestResult::NothingToRequest;
        else
            res.outcome = st == (int)RequestStatus::Pending ? RequestResult::Pending : RequestResult::Approved;
        return res;
    }
    if (r.status == 0) {
        res.outcome = RequestResult::Unreachable;
    } else if (r.status == 401) {   /* no session cookie at all (Seerr's API validator) */
        res.outcome = RequestResult::SignedOut;
    } else if (r.status == 403) {
        /* The console gets no error text: a signed-out session, a reached quota
         * and a missing permission all read 403. Ask which one it was. */
        const std::string why = last_error();
        User u;
        Quota mq, tq;
        if (!me(&u))
            res.outcome = last_unreachable() ? RequestResult::Unreachable : RequestResult::SignedOut;
        else if (quota(u.id, &mq, &tq) && (o.tv ? tq : mq).restricted)
            res.outcome = RequestResult::QuotaReached;
        else
            res.outcome = RequestResult::NotAllowed;
        set_error(why);
    } else if (r.status == 409 || r.status == 499) {   /* 409, folded into "other 4xx" on the console */
        res.outcome = RequestResult::Duplicate;
    }
    return res;
}

std::string Client::image_url(const std::string &path, const char *size) const
{
    if (path.empty() || url_.empty())
        return std::string();
    return url_ + "/imageproxy/tmdb/t/p/" + size + path;
}

} // namespace seerr
