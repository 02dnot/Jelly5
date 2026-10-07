/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_client.h"

#include "jf_http.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cJSON.h"
}

namespace jf {
namespace {

std::atomic<int> g_unreachable{0};   /* requests in a row that got no answer at all */

/* Every request of the client goes through here, so the app can tell when the
 * server has gone away (no answer, not an HTTP error) and when it is back. */
HttpResponse tracked_request(const std::string &method, const std::string &url,
                             const std::vector<std::string> &headers, const std::string &body, int timeout_s)
{
    HttpResponse r = http_request(method, url, headers, body, timeout_s);
    if (r.status == 0)
        g_unreachable++;
    else
        g_unreachable = 0;
    return r;
}

constexpr int kTimeout = 15;
constexpr const char *kVersion = "0.0.1";
constexpr const char *kFields = "Overview,Genres";            /* rows: what the UI shows */
constexpr const char *kItemFields =
    "Overview,Genres,MediaStreams,Taglines,People,Studios,ChildCount,ProductionLocations,SpecialFeatureCount,"
    "ProviderIds";   /* a series' TMDB/TVDB ids: Seerr finds it by them */

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

bool bool_of(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

/* The first tag of an image type in ImageTags, or of an array (BackdropImageTags). */
std::string tag_of(const cJSON *o, const char *type)
{
    return str_of(cJSON_GetObjectItemCaseSensitive(o, "ImageTags"), type);
}

std::string first_of(const cJSON *o, const char *key)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    const cJSON *f = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, 0) : nullptr;
    return cJSON_IsString(f) ? f->valuestring : std::string();
}

std::string blurhash_of(const cJSON *o, const char *type, const std::string &tag)
{
    if (tag.empty())
        return std::string();
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "ImageBlurHashes");
    return str_of(cJSON_GetObjectItemCaseSensitive(b, type), tag.c_str());
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

/* Quotes for the MediaBrowser authorization header (no quotes or commas). */
std::string header_safe(const std::string &s)
{
    std::string out;
    for (char c : s)
        if (c != '"' && c != ',' && c != '\r' && c != '\n')
            out += c;
    return out;
}

Item item_of(const cJSON *o)
{
    Item it;
    it.id = str_of(o, "Id");
    it.name = str_of(o, "Name");
    it.type = str_of(o, "Type");
    it.overview = str_of(o, "Overview");
    it.official_rating = str_of(o, "OfficialRating");
    it.video3d = str_of(o, "Video3DFormat");
    it.series_id = str_of(o, "SeriesId");
    it.series_name = str_of(o, "SeriesName");
    it.season_id = str_of(o, "SeasonId");
    it.season_name = str_of(o, "SeasonName");
    it.index = (int)num_of(o, "IndexNumber", -1);
    it.parent_index = (int)num_of(o, "ParentIndexNumber", -1);
    it.year = (int)num_of(o, "ProductionYear", 0);
    it.community_rating = num_of(o, "CommunityRating", 0);
    it.runtime_ticks = (int64_t)num_of(o, "RunTimeTicks", 0);
    const cJSON *ud = cJSON_GetObjectItemCaseSensitive(o, "UserData");
    it.position_ticks = (int64_t)num_of(ud, "PlaybackPositionTicks", 0);
    it.played_percent = num_of(ud, "PlayedPercentage", 0);
    it.played = bool_of(ud, "Played");
    it.favorite = bool_of(ud, "IsFavorite");
    it.unplayed = (int)num_of(ud, "UnplayedItemCount", 0);
    it.local_trailers = (int)num_of(o, "LocalTrailerCount", 0);
    it.special_features = (int)num_of(o, "SpecialFeatureCount", 0);
    const cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(o, "Genres"))
        if (cJSON_IsString(g))
            it.genres.push_back(g->valuestring);

    it.primary_tag = tag_of(o, "Primary");
    it.primary_blurhash = blurhash_of(o, "Primary", it.primary_tag);
    it.thumb_tag = tag_of(o, "Thumb");
    it.thumb_owner = it.id;
    if (it.thumb_tag.empty()) {
        it.thumb_tag = str_of(o, "ParentThumbImageTag");
        it.thumb_owner = str_of(o, "ParentThumbItemId");
    }
    it.logo_tag = tag_of(o, "Logo");
    it.logo_owner = it.id;
    if (it.logo_tag.empty()) {
        it.logo_tag = str_of(o, "ParentLogoImageTag");
        it.logo_owner = str_of(o, "ParentLogoItemId");
    }
    it.backdrop_tag = first_of(o, "BackdropImageTags");
    it.backdrop_owner = it.id;
    if (it.backdrop_tag.empty()) {
        it.backdrop_tag = first_of(o, "ParentBackdropImageTags");
        it.backdrop_owner = str_of(o, "ParentBackdropItemId");
    }
    it.backdrop_blurhash = blurhash_of(o, "Backdrop", it.backdrop_tag);
    it.thumb_blurhash = blurhash_of(o, "Thumb", it.thumb_tag);
    if (it.type == "Episode" && !it.primary_tag.empty())
        it.thumb_blurhash = it.primary_blurhash;
    it.collection_type = str_of(o, "CollectionType");
    it.series_primary_tag = str_of(o, "SeriesPrimaryImageTag");
    it.album_id = str_of(o, "AlbumId");
    it.album = str_of(o, "Album");
    it.album_artist = str_of(o, "AlbumArtist");
    const cJSON *aa = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, "AlbumArtists"), 0);
    it.album_artist_id = str_of(aa, "Id");
    if (it.album_artist.empty())
        it.album_artist = first_of(o, "Artists");
    it.album_primary_tag = str_of(o, "AlbumPrimaryImageTag");
    it.album_blurhash = blurhash_of(o, "Primary", it.album_primary_tag);
    it.premiere_date = str_of(o, "PremiereDate");
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(o, "ProductionLocations"))
        if (cJSON_IsString(g))
            it.locations.push_back(g->valuestring);
    it.tmdb_id = str_of(cJSON_GetObjectItemCaseSensitive(o, "ProviderIds"), "Tmdb");
    it.tvdb_id = str_of(cJSON_GetObjectItemCaseSensitive(o, "ProviderIds"), "Tvdb");
    return it;
}

MediaStream stream_of(const cJSON *s, const std::string &server)
{
    MediaStream m;
    m.index = (int)num_of(s, "Index", -1);
    m.type = str_of(s, "Type");
    m.codec = str_of(s, "Codec");
    m.language = str_of(s, "Language");
    m.title = str_of(s, "Title");
    m.display_title = str_of(s, "DisplayTitle");
    m.profile = str_of(s, "Profile");
    m.video_range = str_of(s, "VideoRange");
    m.video_range_type = str_of(s, "VideoRangeType");
    if (m.type == "Video" && m.video_range_type.empty()) {
        /* Emby: VideoRange "SDR" / "HDR 10" ..., the kind in ExtendedVideoType and
         * SubType. Said the way Jellyfin does, so the badges and the Dolby Vision
         * check below read one language. */
        const std::string ext = str_of(s, "ExtendedVideoType"), sub = str_of(s, "ExtendedVideoSubType");
        if (ext == "DolbyVision")
            m.video_range_type = sub == "DoviProfile50"   ? "DOVI"   /* no compatible base layer */
                                 : sub == "DoviProfile84" ? "DOVIWithHLG"
                                 : sub == "DoviProfile82" ? "DOVIWithSDR"
                                                          : "DOVIWithHDR10";
        else if (ext == "Hdr10")
            m.video_range_type = "HDR10";
        else if (ext == "Hdr10Plus")
            m.video_range_type = "HDR10Plus";
        else if (ext == "HyperLogGamma")
            m.video_range_type = "HLG";
        else if (!ext.empty())
            m.video_range_type = "SDR";
        if (!ext.empty())
            m.video_range = m.video_range_type == "SDR" || m.video_range_type == "DOVIWithSDR" ? "SDR" : "HDR";
    }
    m.width = (int)num_of(s, "Width", 0);
    m.height = (int)num_of(s, "Height", 0);
    m.channels = (int)num_of(s, "Channels", 0);
    m.bit_depth = (int)num_of(s, "BitDepth", 0);
    m.is_default = bool_of(s, "IsDefault");
    m.is_forced = bool_of(s, "IsForced");
    m.is_external = bool_of(s, "IsExternal");
    m.is_text = bool_of(s, "IsTextSubtitleStream");
    const std::string d = str_of(s, "DeliveryUrl");
    if (!d.empty())
        m.delivery_url = d.rfind("http", 0) == 0 ? d : server + d;
    return m;
}

} // namespace

const char *kind_key(Kind k) { return k == Kind::Emby ? "emby" : "jellyfin"; }

Kind kind_of_key(const std::string &key) { return key == "emby" ? Kind::Emby : Kind::Jellyfin; }

Kind kind_of_info(const std::string &product_name, const std::string &version)
{
    std::string p = product_name;
    for (char &c : p)
        c = (char)std::tolower((unsigned char)c);
    if (p.find("jellyfin") != std::string::npos)
        return Kind::Jellyfin;
    if (p.find("emby") != std::string::npos)
        return Kind::Emby;
    /* No product name: Emby's versions are 4.x.y.z, Jellyfin's 10.x and up. */
    const int major = std::atoi(version.c_str());
    return !version.empty() && major > 0 && major < 10 ? Kind::Emby : Kind::Jellyfin;
}

Features Client::features() const
{
    Features f;
    if (emby()) {
        f.quick_connect = f.syncplay = f.lyrics = false;
        f.home_sections = false;
    }
    return f;
}

std::string Client::socket_url() const
{
    return emby() ? server_ + "/embywebsocket?api_key=" + token_ + "&deviceId=" + url_escape(device_id_)
                  : server_ + "/socket?deviceId=" + url_escape(device_id_);
}

std::string Client::me_path() const { return emby() ? "/Users/" + user_id_ : std::string("/Users/Me"); }

std::string Client::user_item_path(const std::string &id) const
{
    return emby() ? "/Users/" + user_id_ + "/Items/" + id + "?UserId=" + user_id_
                  : "/Items/" + id + "?userId=" + user_id_;
}

Client::Client(std::string server, std::string device_id, std::string device_name)
    : server_(std::move(server)), device_id_(std::move(device_id)), device_name_(std::move(device_name))
{
    while (!server_.empty() && server_.back() == '/')
        server_.pop_back();
}

void Client::set_server(std::string server)
{
    server_ = std::move(server);
    while (!server_.empty() && server_.back() == '/')
        server_.pop_back();
    if (server_.empty() || server_.find("://") != std::string::npos)
        return;
    /* Typed without a scheme: plain http, and a bare host gets Jellyfin's port. */
    server_ = "http://" + server_;
    if (server_.find(':', 7) == std::string::npos && server_.find('/', 7) == std::string::npos)
        server_ += ":8096";
}

void Client::set_session(std::string token, std::string user_id, std::string user_name)
{
    token_ = std::move(token);
    user_id_ = std::move(user_id);
    user_name_ = std::move(user_name);
}

std::string Client::auth_header() const
{
    std::string h = "Authorization: MediaBrowser Client=\"Jelly5\", Device=\"" + header_safe(device_name_) +
                    "\", DeviceId=\"" + header_safe(device_id_) + "\", Version=\"" + kVersion + "\"";
    if (!token_.empty())
        h += ", Token=\"" + token_ + "\"";
    return h;
}

bool Client::get_json(const std::string &path, std::string *body, int timeout_s)
{
    HttpResponse r = tracked_request("GET", server_ + path, {auth_header(), "Accept: application/json"}, "",
                                     timeout_s > 0 ? timeout_s : kTimeout);
    if (!r.ok()) {
        set_error("GET " + path + " -> " + std::to_string(r.status) + " " + r.error);
        return false;
    }
    *body = std::move(r.body);
    return true;
}

bool Client::post_json(const std::string &path, const std::string &json, std::string *body)
{
    HttpResponse r = tracked_request("POST", server_ + path, {auth_header(), "Accept: application/json"},
                                  json.empty() ? "{}" : json, kTimeout);
    if (!r.ok()) {
        set_error("POST " + path + " -> " + std::to_string(r.status) + " " + r.error);
        return false;
    }
    if (body)
        *body = std::move(r.body);
    return true;
}

bool Client::public_info(std::string *name, std::string *version, std::string *id, Kind *kind, int timeout_s)
{
    std::string body;
    if (!get_json("/System/Info/Public", &body, timeout_s))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    if (name)
        *name = str_of(j, "ServerName");
    if (version)
        *version = str_of(j, "Version");
    if (id)
        *id = str_of(j, "Id");
    if (kind)
        *kind = kind_of_info(str_of(j, "ProductName"), str_of(j, "Version"));
    cJSON_Delete(j);
    return true;
}

static bool take_auth(cJSON *j, Client *c)
{
    const std::string token = str_of(j, "AccessToken");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "User");
    if (token.empty() || !u)
        return false;
    c->set_session(token, str_of(u, "Id"), str_of(u, "Name"));
    c->note_image_tag(str_of(u, "PrimaryImageTag"));
    return true;
}

bool Client::authenticate(const std::string &user, const std::string &password)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "Username", user.c_str());
    cJSON_AddStringToObject(req, "Pw", password.c_str());
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    std::string body;
    const bool ok = post_json("/Users/AuthenticateByName", s, &body);
    std::free(s);
    if (!ok)
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const bool got = j && take_auth(j, this);
    cJSON_Delete(j);
    return got;
}

bool Client::quick_connect_start(QuickConnect *out)
{
    if (emby()) {
        set_error("Quick Connect: not on Emby");
        return false;
    }
    std::string body;
    if (!post_json("/QuickConnect/Initiate", "", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    out->code = str_of(j, "Code");
    out->secret = str_of(j, "Secret");
    cJSON_Delete(j);
    return !out->secret.empty();
}

bool Client::quick_connect_poll(const QuickConnect &qc, bool *approved)
{
    *approved = false;
    std::string body;
    if (!get_json("/QuickConnect/Connect?secret=" + url_escape(qc.secret), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const bool authed = j && bool_of(j, "Authenticated");
    cJSON_Delete(j);
    if (!authed)
        return true;
    std::string req = "{\"Secret\":\"" + qc.secret + "\"}";
    if (!post_json("/Users/AuthenticateWithQuickConnect", req, &body))
        return false;
    j = cJSON_Parse(body.c_str());
    *approved = j && take_auth(j, this);
    cJSON_Delete(j);
    return true;
}

bool Client::quick_connect_authorize(const std::string &code)
{
    if (emby()) {
        set_error("Quick Connect: not on Emby");
        return false;
    }
    std::string body;
    if (!post_json("/QuickConnect/Authorize?code=" + url_escape(code), "", &body))
        return false;
    if (body.find("true") == std::string::npos) {
        set_error("Quick Connect: the server did not approve the code");
        return false;
    }
    return true;
}

bool Client::validate()
{
    std::string body;
    if (!get_json(me_path(), &body))
        return false;
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        user_image_tag_ = str_of(j, "PrimaryImageTag");
        user_name_ = str_of(j, "Name");
        latest_excludes_.clear();
        const cJSON *ex;
        cJSON_ArrayForEach(ex, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "Configuration"),
                                                                "LatestItemsExcludes"))
            if (cJSON_IsString(ex))
                latest_excludes_.push_back(ex->valuestring);
        const cJSON *policy = cJSON_GetObjectItemCaseSensitive(j, "Policy");
        is_admin_ = bool_of(policy, "IsAdministrator");
        manages_subtitles_ = is_admin_ || bool_of(policy, "EnableSubtitleManagement");
        cJSON_Delete(j);
    }
    return true;
}

std::vector<PublicUser> Client::public_users()
{
    std::vector<PublicUser> out;
    std::string body;
    if (!get_json("/Users/Public", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *u;
    cJSON_ArrayForEach(u, j) {
        PublicUser p;
        p.id = str_of(u, "Id");
        p.name = str_of(u, "Name");
        p.image_tag = str_of(u, "PrimaryImageTag");
        p.has_password = bool_of(u, "HasPassword");
        out.push_back(p);
    }
    cJSON_Delete(j);
    return out;
}

bool Client::get_prefs(UserPrefs *out)
{
    std::string body;
    if (!get_json(me_path(), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(j, "Configuration");
    if (cfg) {
        out->audio_language = str_of(cfg, "AudioLanguagePreference");
        out->subtitle_language = str_of(cfg, "SubtitleLanguagePreference");
        out->subtitle_mode = str_of(cfg, "SubtitleMode");
        out->autoplay_next = bool_of(cfg, "EnableNextEpisodeAutoPlay");
    }
    cJSON_Delete(j);
    return cfg != nullptr;
}

bool Client::set_prefs(const UserPrefs &p)
{
    /* The server takes the whole configuration: read it, change ours, send it back. */
    std::string body;
    if (!get_json(me_path(), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    cJSON *cfg = cJSON_DetachItemFromObjectCaseSensitive(j, "Configuration");
    cJSON_Delete(j);
    if (!cfg)
        return false;
    auto put = [cfg](const char *k, cJSON *v) { cJSON_ReplaceItemInObjectCaseSensitive(cfg, k, v); };
    put("AudioLanguagePreference", cJSON_CreateString(p.audio_language.c_str()));
    put("SubtitleLanguagePreference", cJSON_CreateString(p.subtitle_language.c_str()));
    put("SubtitleMode", cJSON_CreateString(p.subtitle_mode.empty() ? "Default" : p.subtitle_mode.c_str()));
    put("EnableNextEpisodeAutoPlay", cJSON_CreateBool(p.autoplay_next));
    char *text = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    const bool ok = post_json(emby() ? "/Users/" + user_id_ + "/Configuration"
                                     : "/Users/Configuration?userId=" + user_id_,
                              text, nullptr);
    std::free(text);
    return ok;
}

std::vector<Item> Client::items_of(const std::string &body)
{
    std::vector<Item> out;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return out;
    const cJSON *arr = cJSON_IsArray(j) ? j : cJSON_GetObjectItemCaseSensitive(j, "Items");
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
        out.push_back(item_of(it));
    cJSON_Delete(j);
    return out;
}

std::vector<Item> Client::resume(int limit, const std::string &parent_id, std::string *raw)
{
    std::string body;
    if (!get_json((emby() ? "/Users/" + user_id_ + "/Items/Resume?UserId=" : "/UserItems/Resume?userId=") +
                      user_id_ + "&limit=" + std::to_string(limit) +
                      "&mediaTypes=Video&enableTotalRecordCount=false&fields=" + kFields +
                      (parent_id.empty() ? std::string() : "&parentId=" + parent_id), &body))
        return {};
    if (raw)
        *raw = body;
    return items_of(body);
}

std::vector<Item> Client::next_up(int limit, const std::string &series_id, std::string *raw)
{
    std::string body;
    std::string path = "/Shows/NextUp?userId=" + user_id_ + "&limit=" + std::to_string(limit) +
                       "&enableResumable=false&fields=" + kFields;
    if (!series_id.empty())
        path += "&seriesId=" + series_id;
    if (!get_json(path, &body))
        return {};
    if (raw)
        *raw = body;
    return items_of(body);
}

std::vector<Item> Client::views(std::string *raw)
{
    std::string body;
    /* Emby: without channels and Live TV, which are views there too (Jelly5 shows neither). */
    if (!get_json(emby() ? "/Users/" + user_id_ + "/Views?IncludeExternalContent=false" : "/UserViews?userId=" + user_id_,
                  &body))
        return {};
    if (raw)
        *raw = body;
    return items_of(body);
}

/* No total count: the server's count of a random recursive query doubles its time. */
std::vector<Item> Client::featured(int limit, std::string *raw)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&IncludeItemTypes=Movie,Series&Recursive=true&SortBy=Random"
                  "&ImageTypes=Logo,Backdrop&EnableTotalRecordCount=false&Limit=" + std::to_string(limit * 2) +
                  "&fields=" + kFields, &body))
        return {};
    if (raw)
        *raw = body;
    return featured_from(body, limit);
}

std::vector<Item> Client::featured_from(const std::string &body, int limit)
{
    std::vector<Item> out;
    for (Item &it : items_of(body))
        if (!it.logo_tag.empty() && !it.backdrop_tag.empty() && it.logo_owner == it.id &&
            (int)out.size() < limit)
            out.push_back(std::move(it));
    return out;
}

std::vector<Item> Client::latest(const std::string &parent_id, int limit, std::string *raw)
{
    std::string body;
    if (!get_json((emby() ? "/Users/" + user_id_ + "/Items/Latest?UserId=" : "/Items/Latest?userId=") + user_id_ +
                      "&parentId=" + parent_id + "&limit=" +
                      std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
    if (raw)
        *raw = body;
    return items_of(body);
}

std::vector<Item> Client::episodes(const std::string &series_id, const std::string &season_id)
{
    std::string body;
    std::string path = "/Shows/" + series_id + "/Episodes?userId=" + user_id_ + "&fields=Overview";
    if (!season_id.empty())
        path += "&seasonId=" + season_id;
    if (!get_json(path, &body))
        return {};
    return items_of(body);
}

Page Client::library(const std::string &parent_id, const std::string &types, const std::string &sort_by,
                     bool descending, int start, int limit, const std::string &filter)
{
    Page page;
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + (parent_id.empty() ? std::string() : "&parentId=" + parent_id) +
                      "&IncludeItemTypes=" + types +
                      "&Recursive=true&SortBy=" + sort_by + "&SortOrder=" + (descending ? "Descending" : "Ascending") +
                      "&StartIndex=" + std::to_string(start) + "&Limit=" + std::to_string(limit) +
                      "&fields=" + kFields + "&EnableTotalRecordCount=true" + filter, &body))
        return page;
    page.items = items_of(body);
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        page.total = (int)num_of(j, "TotalRecordCount", (double)page.items.size());
        cJSON_Delete(j);
    }
    return page;
}

std::vector<Item> Client::search(const std::string &term, const std::string &types, int limit)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&searchTerm=" + url_escape(term) + "&IncludeItemTypes=" + types +
                      "&Recursive=true&EnableTotalRecordCount=false&Limit=" + std::to_string(limit) +
                      "&fields=" + kFields + ",ProviderIds", &body))   /* TMDB ids: Seerr's results match them */
        return {};
    return items_of(body);
}

std::vector<Client::Recommendation> Client::recommendations(int categories, int items)
{
    std::vector<Recommendation> out;
    std::string body;
    if (!get_json("/Movies/Recommendations?userId=" + user_id_ + "&categoryLimit=" + std::to_string(categories) +
                      "&itemLimit=" + std::to_string(items) + "&fields=" + kFields, &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *c;
    cJSON_ArrayForEach(c, j) {
        Recommendation r;
        r.type = str_of(c, "RecommendationType");
        r.baseline = str_of(c, "BaselineItemName");
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(c, "Items"))
            r.items.push_back(item_of(it));
        out.push_back(std::move(r));
    }
    cJSON_Delete(j);
    return out;
}

std::vector<std::string> Client::genres()
{
    std::vector<std::string> out;
    std::string body;
    if (!get_json("/Genres?userId=" + user_id_ + "&IncludeItemTypes=Movie,Series&Recursive=true"
                  "&EnableTotalRecordCount=false", &body))
        return out;
    for (const Item &g : items_of(body))
        out.push_back(g.name);
    return out;
}

std::string Client::escape(const std::string &s) { return url_escape(s); }

int unreachable_streak() { return g_unreachable.load(); }

std::vector<std::string> Client::home_sections()
{
    std::vector<std::string> out;
    if (!features().home_sections)
        return out;   /* Emby keeps its home elsewhere: the default order */
    std::string body;
    if (!get_json("/DisplayPreferences/usersettings?userId=" + user_id_ + "&client=emby", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *prefs = cJSON_GetObjectItemCaseSensitive(j, "CustomPrefs");
    for (int i = 0; i < 10; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(prefs, ("homesection" + std::to_string(i)).c_str());
        if (!cJSON_IsString(v) || !v->valuestring)
            break;
        out.push_back(v->valuestring);
    }
    cJSON_Delete(j);
    return out;
}

bool Client::ping()
{
    return tracked_request("GET", server_ + "/System/Info/Public", {"Accept: application/json"}, "", 5).ok();
}

std::vector<std::string> Client::genres_in(const std::string &parent_id, const std::string &types)
{
    std::vector<std::string> out;
    std::string body;
    if (!get_json("/Genres?userId=" + user_id_ + (parent_id.empty() ? std::string() : "&parentId=" + parent_id) +
                      "&IncludeItemTypes=" + types + "&Recursive=true&SortBy=SortName&EnableTotalRecordCount=false",
                  &body))
        return out;
    for (const Item &g : items_of(body))
        out.push_back(g.name);
    return out;
}

int Client::count_before(const std::string &parent_id, const std::string &types, const std::string &filter,
                         const std::string &letter)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + (parent_id.empty() ? std::string() : "&parentId=" + parent_id) +
                      "&IncludeItemTypes=" + types + "&Recursive=true&Limit=0&EnableTotalRecordCount=true" +
                      "&NameLessThan=" + url_escape(letter) + filter,
                  &body))
        return -1;
    int n = -1;
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        n = (int)num_of(j, "TotalRecordCount", -1);
        cJSON_Delete(j);
    }
    return n;
}

std::vector<Item> Client::genre_items(const std::string &genre, int limit)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&Genres=" + url_escape(genre) +
                      "&IncludeItemTypes=Movie,Series&Recursive=true&SortBy=Random&EnableTotalRecordCount=false"
                      "&Limit=" + std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

std::vector<Item> Client::special_features(const std::string &id)
{
    std::string body;
    if (!get_json(emby() ? "/Users/" + user_id_ + "/Items/" + id + "/SpecialFeatures"
                         : "/Items/" + id + "/SpecialFeatures?userId=" + user_id_,
                  &body))
        return {};
    return items_of(body);   /* a bare array */
}

std::vector<Item> Client::theme_songs(const std::string &id)
{
    std::string body;
    if (!get_json("/Items/" + id + "/ThemeSongs?userId=" + user_id_ + "&inheritFromParent=true", &body))
        return {};
    return items_of(body);
}

bool Client::item(const std::string &id, Item *out, Detail *detail)
{
    std::string body;
    if (!get_json(user_item_path(id) + "&fields=" + kItemFields, &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    *out = item_of(j);
    if (detail) {
        *detail = Detail();
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "People")) {
            Person person;
            person.id = str_of(p, "Id");
            person.name = str_of(p, "Name");
            person.role = str_of(p, "Role");
            person.type = str_of(p, "Type");
            person.image_tag = str_of(p, "PrimaryImageTag");
            const cJSON *bh = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(p, "ImageBlurHashes"),
                                                               "Primary");
            person.blurhash = str_of(bh, person.image_tag.c_str());
            detail->people.push_back(person);
        }
        const cJSON *st;
        cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(j, "Studios"))
            detail->studios.push_back(str_of(st, "Name"));
        detail->tagline = first_of(j, "Taglines");
        cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(j, "MediaStreams"))
            detail->streams.push_back(stream_of(st, server_));
    }
    cJSON_Delete(j);
    return true;
}

std::vector<Item> Client::seasons(const std::string &series_id)
{
    std::string body;
    if (!get_json("/Shows/" + series_id + "/Seasons?userId=" + user_id_, &body))
        return {};
    return items_of(body);
}

std::vector<Item> Client::similar(const std::string &id, int limit)
{
    std::string body;
    if (!get_json("/Items/" + id + "/Similar?userId=" + user_id_ + "&limit=" + std::to_string(limit) +
                      "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

std::vector<Item> Client::person_items(const std::string &person_id, const std::string &types, int limit)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&PersonIds=" + person_id + "&IncludeItemTypes=" + types +
                      "&Recursive=true&SortBy=PremiereDate,ProductionYear,SortName&SortOrder=Descending"
                      "&EnableTotalRecordCount=false&Limit=" +
                      std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

bool Client::set_favorite(const std::string &id, bool favorite)
{
    HttpResponse r = tracked_request(favorite ? "POST" : "DELETE",
                                  server_ + (emby() ? "/Users/" + user_id_ + "/FavoriteItems/" + id
                                                    : "/UserFavoriteItems/" + id + "?userId=" + user_id_),
                                  {auth_header()}, "", kTimeout);
    return r.ok();
}

std::vector<Item> Client::local_trailers(const std::string &id)
{
    std::string body;
    if (!get_json(emby() ? "/Users/" + user_id_ + "/Items/" + id + "/LocalTrailers"
                         : "/Items/" + id + "/LocalTrailers?userId=" + user_id_,
                  &body))
        return {};
    /* A bare array, not {"Items": [...]}. */
    std::vector<Item> out;
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        const cJSON *t;
        cJSON_ArrayForEach(t, j)
            out.push_back(item_of(t));
        cJSON_Delete(j);
    }
    return out;
}

void Client::check_subtitle_search()
{
    subtitle_search_ = false;
    if (!manages_subtitles_)
        return;
    if (!is_admin_) {   /* the plugin list is for administrators: assume one is there */
        subtitle_search_ = true;
        return;
    }
    std::string body;
    if (!get_json("/Plugins", &body))
        return;
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        const cJSON *p;
        cJSON_ArrayForEach(p, j) {
            const std::string name = str_of(p, "Name");
            const std::string status = str_of(p, "Status");
            if ((name.find("ubtitle") != std::string::npos || name == "Subbuzz" || name == "Podnapisi" ||
                 name == "Addic7ed") &&
                (status.empty() || status == "Active"))
                subtitle_search_ = true;
        }
        cJSON_Delete(j);
    }
}

std::vector<RemoteSubtitle> Client::search_subtitles(const std::string &item_id, const std::string &language)
{
    std::vector<RemoteSubtitle> out;
    std::string body;
    if (!get_json("/Items/" + item_id + "/RemoteSearch/Subtitles/" + language + "?isPerfectMatch=false", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *r;
    cJSON_ArrayForEach(r, j) {
        RemoteSubtitle x;
        x.id = str_of(r, "Id");
        x.name = str_of(r, "Name");
        x.provider = str_of(r, "ProviderName");
        x.language = str_of(r, "ThreeLetterISOLanguageName");
        x.format = str_of(r, "Format");
        x.downloads = (int)num_of(r, "DownloadCount", 0);
        x.hash_match = bool_of(r, "IsHashMatch");
        x.forced = bool_of(r, "Forced");
        x.hearing_impaired = bool_of(r, "HearingImpaired");
        if (!x.id.empty())
            out.push_back(std::move(x));
    }
    cJSON_Delete(j);
    return out;
}

bool Client::download_subtitle(const std::string &item_id, const std::string &subtitle_id)
{
    HttpResponse r = tracked_request("POST", server_ + "/Items/" + item_id + "/RemoteSearch/Subtitles/" +
                                              url_escape(subtitle_id),
                                  {auth_header()}, "", 60);
    if (!r.ok())
        set_error("subtitle download -> " + std::to_string(r.status) + " " + r.error);
    return r.ok();
}

Page Client::album_artists(const std::string &parent_id, const std::string &sort_by, bool descending, int start,
                           int limit)
{
    Page page;
    std::string body;
    if (!get_json("/Artists/AlbumArtists?userId=" + user_id_ +
                      (parent_id.empty() ? std::string() : "&parentId=" + parent_id) + "&SortBy=" + sort_by +
                      "&SortOrder=" + (descending ? "Descending" : "Ascending") + "&StartIndex=" +
                      std::to_string(start) + "&Limit=" + std::to_string(limit) + "&fields=" + kFields +
                      "&EnableTotalRecordCount=true", &body))
        return page;
    page.items = items_of(body);
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        page.total = (int)num_of(j, "TotalRecordCount", (double)page.items.size());
        cJSON_Delete(j);
    }
    return page;
}

std::vector<Item> Client::playlist_items(const std::string &playlist_id)
{
    std::string body;
    if (!get_json("/Playlists/" + playlist_id + "/Items?userId=" + user_id_ + "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

std::vector<LyricLine> Client::lyrics(const std::string &item_id)
{
    std::vector<LyricLine> out;
    if (!features().lyrics)
        return out;
    HttpResponse r = tracked_request("GET", server_ + "/Audio/" + item_id + "/Lyrics",
                                  {auth_header(), "Accept: application/json"}, "", kTimeout);
    if (!r.ok())
        return out;   /* 404: no lyrics, not an error */
    cJSON *j = cJSON_Parse(r.body.c_str());
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(j, "Lyrics")) {
        LyricLine x;
        x.text = str_of(l, "Text");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(l, "Start");
        if (cJSON_IsNumber(st))
            x.start = st->valuedouble / (double)kTicksPerSecond;
        /* Cue positions count UTF-16 units (.NET strings); turn them into bytes. */
        auto byte_at = [&x](int units) {
            size_t b = 0;
            for (int u = 0; u < units && b < x.text.size();) {
                const unsigned char c = (unsigned char)x.text[b];
                const size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                u += len == 4 ? 2 : 1;
                b += len;
            }
            return std::min(b, x.text.size());
        };
        const cJSON *cue;
        cJSON_ArrayForEach(cue, cJSON_GetObjectItemCaseSensitive(l, "Cues")) {
            const cJSON *cs = cJSON_GetObjectItemCaseSensitive(cue, "Start");
            if (!cJSON_IsNumber(cs))
                continue;
            LyricLine::Cue c;
            c.start = cs->valuedouble / (double)kTicksPerSecond;
            c.from = byte_at(cJSON_GetObjectItemCaseSensitive(cue, "Position")
                                 ? cJSON_GetObjectItemCaseSensitive(cue, "Position")->valueint : 0);
            c.to = byte_at(cJSON_GetObjectItemCaseSensitive(cue, "EndPosition")
                               ? cJSON_GetObjectItemCaseSensitive(cue, "EndPosition")->valueint : 0);
            if (c.to > c.from)
                x.cues.push_back(c);
        }
        out.push_back(std::move(x));
    }
    cJSON_Delete(j);
    return out;
}

bool Client::post_capabilities()
{
    return post_json("/Sessions/Capabilities/Full",
                     "{\"PlayableMediaTypes\":[\"Video\",\"Audio\"],\"SupportedCommands\":[\"DisplayMessage\"],"
                     "\"SupportsMediaControl\":true,\"SupportsPersistentIdentifier\":true}",
                     nullptr);
}

std::vector<Item> Client::instant_mix(const std::string &id, int limit)
{
    std::string body;
    if (!get_json("/Items/" + id + "/InstantMix?userId=" + user_id_ + "&limit=" + std::to_string(limit) +
                      "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

bool Client::set_played(const std::string &id, bool played)
{
    HttpResponse r = tracked_request(played ? "POST" : "DELETE",
                                  server_ + (emby() ? "/Users/" + user_id_ + "/PlayedItems/" + id
                                                    : "/UserPlayedItems/" + id + "?userId=" + user_id_),
                                  {auth_header()}, "", kTimeout);
    return r.ok();
}

/* Jellyfin's resume list is everything with a position: clearing it removes the title
 * (as "Remove from Continue Watching" does in Jellyfin's own clients). */
bool Client::clear_position(const std::string &id)
{
    return post_json(emby() ? "/Users/" + user_id_ + "/Items/" + id + "/UserData"
                            : "/UserItems/" + id + "/UserData?userId=" + user_id_,
                     "{\"PlaybackPositionTicks\":0}", nullptr);
}

std::vector<Item> Client::favorites(int limit, std::string *raw)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&Filters=IsFavorite&IncludeItemTypes=Movie,Series,BoxSet"
                  "&Recursive=true&SortBy=DateCreated,SortName&SortOrder=Descending&EnableTotalRecordCount=false"
                  "&Limit=" + std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
    if (raw)
        *raw = body;
    return items_of(body);
}

std::vector<Item> Client::children(const std::string &parent_id, const std::string &sort_by, int limit)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&parentId=" + parent_id + "&SortBy=" + sort_by +
                      "&EnableTotalRecordCount=false&Limit=" + std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
    return items_of(body);
}

bool Client::media_extras(const std::string &item_id, const std::string &media_source_id,
                          std::vector<Chapter> *chapters, Trickplay *tp)
{
    std::string body;
    if (!get_json(user_item_path(item_id) + (emby() ? "&fields=Chapters" : "&fields=Chapters,Trickplay"), &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "Chapters")) {
        /* Emby marks intros and credits as chapters (MarkerType IntroStart ...): not chapters. */
        const std::string marker = str_of(c, "MarkerType");
        if (!marker.empty() && marker != "Chapter")
            continue;
        Chapter ch;
        ch.start = num_of(c, "StartPositionTicks") / (double)kTicksPerSecond;
        ch.name = str_of(c, "Name");
        ch.image_tag = str_of(c, "ImageTag");
        chapters->push_back(ch);
    }
    /* Trickplay: { media source id: { width: info } }; this version, else any, and the
     * width nearest 320 (what the scrub preview shows). */
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(j, "Trickplay");
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(sources, media_source_id.c_str());
    if (!src && cJSON_IsObject(sources))
        src = sources->child;
    const cJSON *best = nullptr;
    int best_w = 0;
    const cJSON *w;
    cJSON_ArrayForEach(w, src) {
        const int width = (int)num_of(w, "Width");
        if (width > 0 && (!best || std::abs(width - 320) < std::abs(best_w - 320))) {
            best = w;
            best_w = width;
        }
    }
    if (best) {
        tp->width = best_w;
        tp->height = (int)num_of(best, "Height");
        tp->tile_w = (int)num_of(best, "TileWidth");
        tp->tile_h = (int)num_of(best, "TileHeight");
        tp->count = (int)num_of(best, "ThumbnailCount");
        tp->interval = num_of(best, "Interval") / 1000.0;
        /* The sheets need the session: ApiKey (Jellyfin 12 refuses the legacy api_key). */
        tp->url_base = server_ + "/Videos/" + item_id + "/Trickplay/" + std::to_string(best_w) + "/";
        tp->url_query = "?MediaSourceId=" + (src && src->string ? std::string(src->string) : media_source_id) +
                        "&ApiKey=" + token_;
    }
    cJSON_Delete(j);
    if (emby())
        emby_thumbnails(item_id, tp);
    return true;
}

/* Emby's scrub previews: single thumbnails, one every few seconds (the library's
 * "thumbnail image extraction"), each an image of the item found by its time.
 * Without a token, like every image (they are cached as images are). */
void Client::emby_thumbnails(const std::string &item_id, Trickplay *tp)
{
    constexpr int kWidth = 320;
    std::string body;
    if (!get_json("/Items/" + item_id + "/ThumbnailSet?Width=" + std::to_string(kWidth), &body))
        return;   /* 404: the library makes none */
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *thumbs = cJSON_GetObjectItemCaseSensitive(j, "Thumbnails");
    const int count = cJSON_GetArraySize(thumbs);
    const double aspect = num_of(j, "AspectRatio", 16.0 / 9.0);
    const cJSON *first = cJSON_GetArrayItem(thumbs, 0), *second = cJSON_GetArrayItem(thumbs, 1);
    const int64_t step = (int64_t)(num_of(second, "PositionTicks") - num_of(first, "PositionTicks"));
    const std::string tag = str_of(first, "ImageTag");
    /* One tag for the set (as Emby gives it); thumbnails with tags of their own
     * could not be found by time alone: then none rather than wrong ones. */
    bool one_tag = true;
    const cJSON *th;
    cJSON_ArrayForEach(th, thumbs)
        one_tag = one_tag && str_of(th, "ImageTag") == tag;
    if (count >= 2 && step > 0 && aspect > 0 && !tag.empty() && one_tag) {
        tp->width = kWidth;
        tp->height = (int)(kWidth / aspect + 0.5);
        tp->tile_w = tp->tile_h = 1;
        tp->count = count;
        tp->interval = (double)step / kTicksPerSecond;
        tp->sheet_ticks = step;
        tp->first_ticks = (int64_t)num_of(first, "PositionTicks");
        tp->url_base = server_ + "/Items/" + item_id + "/Images/Thumbnail?maxWidth=" + std::to_string(kWidth) +
                       "&quality=90&tag=" + url_escape(tag) + "&PositionTicks=";
        tp->url_query.clear();
    }
    cJSON_Delete(j);
}

std::vector<Segment> Client::segments(const std::string &item_id)
{
    std::vector<Segment> out;
    if (emby())
        return emby_markers(item_id);
    std::string body;
    if (!get_json("/MediaSegments/" + item_id, &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "Items")) {
        Segment s;
        s.type = str_of(it, "Type");
        s.start = num_of(it, "StartTicks") / kTicksPerSecond;
        s.end = num_of(it, "EndTicks") / kTicksPerSecond;
        if (s.end > s.start)
            out.push_back(s);
    }
    cJSON_Delete(j);
    return out;
}

/* Emby marks intros and credits among the chapters (MarkerType IntroStart,
 * IntroEnd, CreditsStart; its intro detection is an Emby Premiere feature):
 * an intro runs from its start to its end (else the next chapter), credits to
 * the end of the title. */
std::vector<Segment> Client::emby_markers(const std::string &item_id)
{
    std::string body;
    if (!get_json(user_item_path(item_id) + "&fields=Chapters", &body))
        return {};
    return emby_markers_of(body);
}

std::vector<Segment> Client::emby_markers_of(const std::string &item_json)
{
    std::vector<Segment> out;
    cJSON *j = cJSON_Parse(item_json.c_str());
    const double runtime = num_of(j, "RunTimeTicks") / kTicksPerSecond;
    std::vector<std::pair<std::string, double>> marks;   /* marker type (or "" for a chapter), start */
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "Chapters"))
        marks.push_back({str_of(c, "MarkerType"), num_of(c, "StartPositionTicks") / kTicksPerSecond});
    cJSON_Delete(j);
    std::stable_sort(marks.begin(), marks.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (size_t i = 0; i < marks.size(); i++) {
        Segment s;
        s.start = marks[i].second;
        if (marks[i].first == "IntroStart") {
            s.type = "Intro";
            s.end = -1;
            for (size_t k = i + 1; k < marks.size() && s.end < 0; k++)   /* its end */
                if (marks[k].first == "IntroEnd")
                    s.end = marks[k].second;
            for (size_t k = i + 1; k < marks.size() && s.end < 0; k++)   /* else the next chapter after it */
                if ((marks[k].first.empty() || marks[k].first == "Chapter") && marks[k].second > s.start)
                    s.end = marks[k].second;
        } else if (marks[i].first == "CreditsStart") {
            s.type = "Outro";
            s.end = runtime;
        } else {
            continue;
        }
        if (s.end > s.start)
            out.push_back(s);
    }
    return out;
}

std::string Client::image_url(const std::string &owner, const char *type, const std::string &tag,
                              int width) const
{
    if (owner.empty() || tag.empty())
        return std::string();
    return server_ + "/Items/" + owner + "/Images/" + type + "?fillWidth=" + std::to_string(width) +
           "&quality=90&tag=" + tag;
}

/*
 * What the PS5 plays itself (EVO/Nuvio engine): sceVideodec2 decodes H.264
 * and HEVC Main/Main10 up to 3840x2176 and VP9; FFmpeg covers the rest in
 * software and every audio codec (multichannel PCM out). Dolby Vision plays
 * its HDR10 base layer, so only profiles with a compatible base are allowed.
 * AV1 needs dav1d: a build with JELLY5_AV1 (FFmpeg + libdav1d, see
 * scripts/build-av1.sh) decodes AV1 Main (4:2:0, 8/10-bit) on the CPU, 4K
 * capped at 30 fps until 4K60 is measured on the console. Without it the
 * server transcodes AV1: FFmpeg's own av1 decoder crashes on the PS5.
 */
#if defined(JELLY5_AV1)
#define JELLY5_AV1_CODEC "av1,"
#define JELLY5_AV1_PROFILES R"(
    {"Type": "Video", "Codec": "av1", "Conditions": [
      {"Condition": "LessThanEqual", "Property": "Width", "Value": "3840", "IsRequired": false},
      {"Condition": "LessThanEqual", "Property": "VideoBitDepth", "Value": "10", "IsRequired": false},
      {"Condition": "EqualsAny", "Property": "VideoProfile", "Value": "main", "IsRequired": false},
      {"Condition": "EqualsAny", "Property": "VideoRangeType",
       "Value": "SDR|HDR10|HLG|DOVIWithHDR10|DOVIWithHLG|DOVIWithSDR|HDR10Plus", "IsRequired": false}]},
    {"Type": "Video", "Codec": "av1",
     "ApplyConditions": [
      {"Condition": "GreaterThanEqual", "Property": "Width", "Value": "2560", "IsRequired": false}],
     "Conditions": [
      {"Condition": "LessThanEqual", "Property": "VideoFramerate", "Value": "31", "IsRequired": false}]},)"
#else
#define JELLY5_AV1_CODEC ""
#define JELLY5_AV1_PROFILES ""
#endif

std::string Client::device_profile_json(int64_t max_bitrate)
{
    std::string profile = R"({
  "Name": "Jelly5 PS5",
  "MaxStreamingBitrate": 200000000,
  "MaxStaticBitrate": 200000000,
  "MusicStreamingTranscodingBitrate": 384000,
  "DirectPlayProfiles": [
    {"Type": "Video",
     "Container": "mkv,webm,mp4,m4v,mov,ts,mpegts,m2ts,mts,avi,wmv,asf,flv,3gp,ogv,mpg,mpeg,vob",
     "VideoCodec": "h264,hevc,)" JELLY5_AV1_CODEC R"(vp9,mpeg2video,mpeg4,vc1,vp8,msmpeg4v3,wmv3,mpeg1video",
     "AudioCodec": "aac,ac3,eac3,truehd,dts,dca,flac,mp3,mp2,opus,vorbis,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_bluray,wmav2,wmapro"},
    {"Type": "Audio", "Container": "mp3,flac,aac,m4a,m4b,ogg,oga,opus,wav,alac,ape,wv,wma"}
  ],
  "TranscodingProfiles": [
    {"Type": "Video", "Container": "ts", "Protocol": "hls", "Context": "Streaming",
     "VideoCodec": "hevc,h264", "AudioCodec": "eac3,ac3,aac", "MaxAudioChannels": "8",
     "MinSegments": "1", "BreakOnNonKeyFrames": true},
    {"Type": "Audio", "Container": "mp3", "Protocol": "http", "Context": "Streaming", "AudioCodec": "mp3"}
  ],
  "CodecProfiles": [)" JELLY5_AV1_PROFILES R"(
    {"Type": "Video", "Codec": "h264", "Conditions": [
      {"Condition": "LessThanEqual", "Property": "Width", "Value": "3840", "IsRequired": false},
      {"Condition": "LessThanEqual", "Property": "VideoLevel", "Value": "52", "IsRequired": false}]},
    {"Type": "Video", "Codec": "hevc", "Conditions": [
      {"Condition": "LessThanEqual", "Property": "Width", "Value": "3840", "IsRequired": false},
      {"Condition": "LessThanEqual", "Property": "VideoBitDepth", "Value": "10", "IsRequired": false},
      {"Condition": "EqualsAny", "Property": "VideoProfile", "Value": "main|main 10", "IsRequired": false},
      {"Condition": "EqualsAny", "Property": "VideoRangeType",
       "Value": "SDR|HDR10|HLG|DOVIWithHDR10|DOVIWithHLG|DOVIWithSDR|HDR10Plus", "IsRequired": false}]},
    {"Type": "Video", "Codec": "vp9", "Conditions": [
      {"Condition": "LessThanEqual", "Property": "Width", "Value": "3840", "IsRequired": false}]}
  ],
  "ContainerProfiles": [],
  "ResponseProfiles": [],
  "SubtitleProfiles": [
    {"Format": "srt", "Method": "Embed"}, {"Format": "subrip", "Method": "Embed"},
    {"Format": "ass", "Method": "Embed"}, {"Format": "ssa", "Method": "Embed"},
    {"Format": "pgssub", "Method": "Embed"}, {"Format": "pgs", "Method": "Embed"},
    {"Format": "dvdsub", "Method": "Embed"}, {"Format": "dvbsub", "Method": "Embed"},
    {"Format": "vtt", "Method": "Embed"}, {"Format": "webvtt", "Method": "Embed"},
    {"Format": "mov_text", "Method": "Embed"},
    {"Format": "srt", "Method": "External"}, {"Format": "ass", "Method": "External"},
    {"Format": "ssa", "Method": "External"}, {"Format": "vtt", "Method": "External"}
  ]
})";
    if (max_bitrate > 0) {
        const std::string cap = std::to_string(max_bitrate);
        for (size_t at; (at = profile.find("200000000")) != std::string::npos;)
            profile.replace(at, 9, cap);
    }
    return profile;
}

bool Client::playback_info(const std::string &item_id, int64_t start_ticks, int audio_index,
                           int subtitle_index, Playback *out, int64_t max_bitrate)
{
    if (!playback_info_as(item_id, start_ticks, audio_index, subtitle_index, out, max_bitrate, false))
        return false;
    /* Dolby Vision profile 5 has no base layer the PS5 shows right (green and
     * purple). Jellyfin's profile keeps it from playing directly or being copied
     * (VideoRangeType); Emby's cannot say so. There a DV5 version comes last
     * (playback_info_as), and when it is still the one chosen it is asked for
     * again with the video encoded - also when the answer was HLS, which may
     * copy the video. If that is refused (no transcoding for this user), the
     * first answer stands. */
    if (!emby())
        return true;
    for (const MediaStream &s : out->streams)
        if (s.type == "Video" && s.video_range_type == "DOVI") {
            Playback encoded;
            if (!playback_info_as(item_id, start_ticks, audio_index, subtitle_index, &encoded, max_bitrate, true))
                return true;
            stop_encoding(*out);
            *out = std::move(encoded);
            return true;
        }
    return true;
}

bool Client::playback_info_as(const std::string &item_id, int64_t start_ticks, int audio_index,
                              int subtitle_index, Playback *out, int64_t max_bitrate, bool transcode)
{
    const int64_t cap = max_bitrate > 0 ? max_bitrate : 200000000;
    const char *direct = transcode ? "false" : "true";
    std::string req = "{\"DeviceProfile\":" + device_profile_json(max_bitrate) +
                      ",\"MaxStreamingBitrate\":" + std::to_string(cap) +
                      ",\"StartTimeTicks\":" + std::to_string(start_ticks) +
                      ",\"EnableDirectPlay\":" + direct + ",\"EnableDirectStream\":" + direct +
                      ",\"EnableTranscoding\":true,\"AllowVideoStreamCopy\":" + direct +
                      ",\"AllowAudioStreamCopy\":true,\"AutoOpenLiveStream\":true";
    if (audio_index >= 0)
        req += ",\"AudioStreamIndex\":" + std::to_string(audio_index);
    if (subtitle_index >= -1)
        req += ",\"SubtitleStreamIndex\":" + std::to_string(subtitle_index);
    req += "}";

    std::string body;
    if (!post_json("/Items/" + item_id + "/PlaybackInfo?userId=" + user_id_, req, &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const std::string session = str_of(j, "PlaySessionId");
    /* Every version (media source) and how it would play. The best goes first: played
     * directly over streamed over transcoded, then the most pixels, then the most bits
     * (within the quality cap the profile carries). */
    struct Candidate {
        const cJSON *ms;
        Version v;
        int rank;
        bool dv5_direct;   /* Emby: Dolby Vision 5, played as it is (see playback_info) */
    };
    std::vector<Candidate> found;
    const cJSON *ms;
    cJSON_ArrayForEach(ms, cJSON_GetObjectItemCaseSensitive(j, "MediaSources")) {
        Candidate c{ms, Version(), 0, false};
        c.v.id = str_of(ms, "Id");
        c.v.name = str_of(ms, "Name");
        c.v.bitrate = (int64_t)num_of(ms, "Bitrate", 0);
        std::string codec, range;
        bool dv5 = false;
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(ms, "MediaStreams"))
            if (str_of(s, "Type") == "Video" && c.v.height == 0) {
                c.v.height = (int)num_of(s, "Height", 0);
                codec = str_of(s, "Codec");
                range = str_of(s, "VideoRange");
                dv5 = stream_of(s, server_).video_range_type == "DOVI";
            }
        const std::string transcoding = str_of(ms, "TranscodingUrl");
        if (bool_of(ms, "SupportsDirectPlay") && !transcode) {
            c.v.play_method = "DirectPlay";
            /* Jellyfin: ApiKey (10.8 on; the legacy api_key counts only with the server's
             * legacy authorization on, which newer servers have off). Emby: api_key only. */
            c.v.url = server_ + "/Videos/" + item_id + "/stream?static=true&mediaSourceId=" + c.v.id +
                      "&playSessionId=" + session + "&" + token_param() + "=" + token_;
            c.rank = 3;
        } else if (!transcoding.empty()) {
            c.v.play_method = transcoding.find("/stream") != std::string::npos ? "DirectStream" : "Transcode";
            c.v.url = server_ + transcoding;
            c.rank = c.v.play_method == "DirectStream" ? 2 : 1;
        } else {
            continue;
        }
        if (dv5 && emby() && !transcode) {
            c.rank = 0;   /* Emby: any other version first (see playback_info) */
            c.dv5_direct = true;
        }
        for (char &ch : codec)
            ch = (char)std::toupper((unsigned char)ch);
        c.v.label = (c.v.height ? std::to_string(c.v.height) + "p" : std::string()) +
                    (codec.empty() ? "" : " \xC2\xB7 " + codec) +
                    (range.rfind("HDR", 0) == 0 ? " \xC2\xB7 HDR" : "");   /* (Emby: "HDR 10") */
        found.push_back(std::move(c));
    }
    if (found.empty()) {
        set_error("PlaybackInfo: the server offers no way to play this (" + str_of(j, "ErrorCode") + ")");
        cJSON_Delete(j);
        return false;
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        if (a.v.height != b.v.height) return a.v.height > b.v.height;
        return a.v.bitrate > b.v.bitrate;
    });
    /* Nor is such a version offered in the player's version list when another plays. */
    if (!found.front().dv5_direct)
        found.erase(std::remove_if(found.begin(), found.end(), [](const Candidate &c) { return c.dv5_direct; }),
                    found.end());
    ms = found.front().ms;
    Playback pb;
    pb.item_id = item_id;
    pb.media_source_id = str_of(ms, "Id");
    pb.play_session_id = session;
    pb.container = str_of(ms, "Container");
    pb.default_audio = (int)num_of(ms, "DefaultAudioStreamIndex", -1);
    pb.default_subtitle = (int)num_of(ms, "DefaultSubtitleStreamIndex", -1);
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(ms, "MediaStreams"))
        pb.streams.push_back(stream_of(s, server_));
    pb.play_method = found.front().v.play_method;
    pb.url = found.front().v.url;
    const std::string transcoding = str_of(ms, "TranscodingUrl");
    const size_t r = transcoding.find("TranscodeReasons=");
    if (pb.play_method != "DirectPlay" && r != std::string::npos)
        pb.transcode_reasons = transcoding.substr(r + 17, transcoding.find('&', r) - r - 17);
    for (Candidate &c : found)
        pb.versions.push_back(std::move(c.v));
    cJSON_Delete(j);
    *out = std::move(pb);
    return true;
}

static std::string report_body(const Playback &pb, int64_t position_ticks, bool paused, bool with_method)
{
    std::string b = "{\"ItemId\":\"" + pb.item_id + "\",\"MediaSourceId\":\"" + pb.media_source_id +
                    "\",\"PlaySessionId\":\"" + pb.play_session_id +
                    "\",\"PositionTicks\":" + std::to_string(position_ticks) +
                    ",\"IsPaused\":" + (paused ? "true" : "false") + ",\"CanSeek\":true";
    if (with_method)
        b += ",\"PlayMethod\":\"" + pb.play_method + "\"";
    return b + "}";
}

void Client::report_start(const Playback &pb, int64_t position_ticks)
{
    post_json("/Sessions/Playing", report_body(pb, position_ticks, false, true), nullptr);
}

void Client::report_progress(const Playback &pb, int64_t position_ticks, bool paused)
{
    post_json("/Sessions/Playing/Progress", report_body(pb, position_ticks, paused, true), nullptr);
}

void Client::report_stopped(const Playback &pb, int64_t position_ticks)
{
    post_json("/Sessions/Playing/Stopped", report_body(pb, position_ticks, false, false), nullptr);
}

void Client::stop_encoding(const Playback &pb)
{
    if (pb.play_method == "DirectPlay")
        return;
    tracked_request("DELETE",
                 server_ + "/Videos/ActiveEncodings?deviceId=" + url_escape(device_id_) +
                     "&playSessionId=" + pb.play_session_id,
                 {auth_header()}, "", kTimeout);
}

} // namespace jf
