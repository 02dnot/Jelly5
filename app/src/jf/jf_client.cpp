/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_client.h"

#include "jf_http.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cJSON.h"
}

namespace jf {
namespace {

constexpr int kTimeout = 15;
constexpr const char *kVersion = "0.0.1";
constexpr const char *kFields = "Overview,Genres";            /* rows: what the UI shows */
constexpr const char *kItemFields =
    "Overview,Genres,MediaStreams,Taglines,People,Studios,ChildCount,ProductionLocations,SpecialFeatureCount";

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

bool Client::get_json(const std::string &path, std::string *body)
{
    HttpResponse r = http_request("GET", server_ + path, {auth_header(), "Accept: application/json"}, "", kTimeout);
    if (!r.ok()) {
        set_error("GET " + path + " -> " + std::to_string(r.status) + " " + r.error);
        return false;
    }
    *body = std::move(r.body);
    return true;
}

bool Client::post_json(const std::string &path, const std::string &json, std::string *body)
{
    HttpResponse r = http_request("POST", server_ + path, {auth_header(), "Accept: application/json"},
                                  json.empty() ? "{}" : json, kTimeout);
    if (!r.ok()) {
        set_error("POST " + path + " -> " + std::to_string(r.status) + " " + r.error);
        return false;
    }
    if (body)
        *body = std::move(r.body);
    return true;
}

bool Client::public_info(std::string *name, std::string *version)
{
    std::string body;
    if (!get_json("/System/Info/Public", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    if (name)
        *name = str_of(j, "ServerName");
    if (version)
        *version = str_of(j, "Version");
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

bool Client::validate()
{
    std::string body;
    if (!get_json("/Users/Me", &body))
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
    if (!get_json("/Users/Me", &body))
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
    if (!get_json("/Users/Me", &body))
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
    const bool ok = post_json("/Users/Configuration?userId=" + user_id_, text, nullptr);
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

std::vector<Item> Client::resume(int limit, const std::string &parent_id)
{
    std::string body;
    if (!get_json("/UserItems/Resume?userId=" + user_id_ + "&limit=" + std::to_string(limit) +
                      "&mediaTypes=Video&enableTotalRecordCount=false&fields=" + kFields +
                      (parent_id.empty() ? std::string() : "&parentId=" + parent_id), &body))
        return {};
    return items_of(body);
}

std::vector<Item> Client::next_up(int limit, const std::string &series_id)
{
    std::string body;
    std::string path = "/Shows/NextUp?userId=" + user_id_ + "&limit=" + std::to_string(limit) +
                       "&enableResumable=false&fields=" + kFields;
    if (!series_id.empty())
        path += "&seriesId=" + series_id;
    if (!get_json(path, &body))
        return {};
    return items_of(body);
}

std::vector<Item> Client::views()
{
    std::string body;
    if (!get_json("/UserViews?userId=" + user_id_, &body))
        return {};
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

std::vector<Item> Client::latest(const std::string &parent_id, int limit)
{
    std::string body;
    if (!get_json("/Items/Latest?userId=" + user_id_ + "&parentId=" + parent_id + "&limit=" +
                      std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
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
                      "&fields=" + kFields, &body))
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
    if (!get_json("/Items/" + id + "/SpecialFeatures?userId=" + user_id_, &body))
        return {};
    return items_of(body);   /* a bare array */
}

bool Client::item(const std::string &id, Item *out, Detail *detail)
{
    std::string body;
    if (!get_json("/Items/" + id + "?userId=" + user_id_ + "&fields=" + kItemFields, &body))
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
    HttpResponse r = http_request(favorite ? "POST" : "DELETE",
                                  server_ + "/UserFavoriteItems/" + id + "?userId=" + user_id_,
                                  {auth_header()}, "", kTimeout);
    return r.ok();
}

std::vector<Item> Client::local_trailers(const std::string &id)
{
    std::string body;
    if (!get_json("/Items/" + id + "/LocalTrailers?userId=" + user_id_, &body))
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
    HttpResponse r = http_request("POST", server_ + "/Items/" + item_id + "/RemoteSearch/Subtitles/" +
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
    std::string body;
    HttpResponse r = http_request("GET", server_ + "/Audio/" + item_id + "/Lyrics",
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
    HttpResponse r = http_request(played ? "POST" : "DELETE",
                                  server_ + "/UserPlayedItems/" + id + "?userId=" + user_id_,
                                  {auth_header()}, "", kTimeout);
    return r.ok();
}

/* Jellyfin's resume list is everything with a position: clearing it removes the title
 * (as "Remove from Continue Watching" does in Jellyfin's own clients). */
bool Client::clear_position(const std::string &id)
{
    return post_json("/UserItems/" + id + "/UserData?userId=" + user_id_, "{\"PlaybackPositionTicks\":0}", nullptr);
}

std::vector<Item> Client::favorites(int limit)
{
    std::string body;
    if (!get_json("/Items?userId=" + user_id_ + "&Filters=IsFavorite&IncludeItemTypes=Movie,Series,BoxSet"
                  "&Recursive=true&SortBy=DateCreated,SortName&SortOrder=Descending&EnableTotalRecordCount=false"
                  "&Limit=" + std::to_string(limit) + "&fields=" + kFields, &body))
        return {};
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
    if (!get_json("/Items/" + item_id + "?userId=" + user_id_ + "&fields=Chapters,Trickplay", &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return false;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "Chapters")) {
        Chapter ch;
        ch.start = num_of(c, "StartPositionTicks") / (double)kTicksPerSecond;
        ch.name = str_of(c, "Name");
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
        /* The sheets need the session: ApiKey (the legacy api_key is refused). */
        tp->url_base = server_ + "/Videos/" + item_id + "/Trickplay/" + std::to_string(best_w) + "/";
        tp->url_query = "?MediaSourceId=" + (src && src->string ? std::string(src->string) : media_source_id) +
                        "&ApiKey=" + token_;
    }
    cJSON_Delete(j);
    return true;
}

std::vector<Segment> Client::segments(const std::string &item_id)
{
    std::vector<Segment> out;
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
 * AV1 needs dav1d, which this build does not have yet: the server transcodes it.
 */
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
     "VideoCodec": "h264,hevc,vp9,mpeg2video,mpeg4,vc1,vp8,msmpeg4v3,wmv3,mpeg1video",
     "AudioCodec": "aac,ac3,eac3,truehd,dts,dca,flac,mp3,mp2,opus,vorbis,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_bluray,wmav2,wmapro"},
    {"Type": "Audio", "Container": "mp3,flac,aac,m4a,m4b,ogg,oga,opus,wav,alac,ape,wv,wma"}
  ],
  "TranscodingProfiles": [
    {"Type": "Video", "Container": "ts", "Protocol": "hls", "Context": "Streaming",
     "VideoCodec": "hevc,h264", "AudioCodec": "eac3,ac3,aac", "MaxAudioChannels": "8",
     "MinSegments": "1", "BreakOnNonKeyFrames": true},
    {"Type": "Audio", "Container": "mp3", "Protocol": "http", "Context": "Streaming", "AudioCodec": "mp3"}
  ],
  "CodecProfiles": [
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
    const int64_t cap = max_bitrate > 0 ? max_bitrate : 200000000;
    std::string req = "{\"DeviceProfile\":" + device_profile_json(max_bitrate) +
                      ",\"MaxStreamingBitrate\":" + std::to_string(cap) +
                      ",\"StartTimeTicks\":" + std::to_string(start_ticks) +
                      ",\"EnableDirectPlay\":true,\"EnableDirectStream\":true,\"EnableTranscoding\":true"
                      ",\"AllowVideoStreamCopy\":true,\"AllowAudioStreamCopy\":true,\"AutoOpenLiveStream\":true";
    if (audio_index >= 0)
        req += ",\"AudioStreamIndex\":" + std::to_string(audio_index);
    if (subtitle_index >= -1)
        req += ",\"SubtitleStreamIndex\":" + std::to_string(subtitle_index);
    req += "}";

    std::string body;
    if (!post_json("/Items/" + item_id + "/PlaybackInfo?userId=" + user_id_, req, &body))
        return false;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *ms = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "MediaSources"), 0);
    if (!ms) {
        set_error("PlaybackInfo: no media source (" + str_of(j, "ErrorCode") + ")");
        cJSON_Delete(j);
        return false;
    }
    Playback pb;
    pb.item_id = item_id;
    pb.media_source_id = str_of(ms, "Id");
    pb.play_session_id = str_of(j, "PlaySessionId");
    pb.container = str_of(ms, "Container");
    pb.default_audio = (int)num_of(ms, "DefaultAudioStreamIndex", -1);
    pb.default_subtitle = (int)num_of(ms, "DefaultSubtitleStreamIndex", -1);
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(ms, "MediaStreams"))
        pb.streams.push_back(stream_of(s, server_));

    const std::string transcoding = str_of(ms, "TranscodingUrl");
    if (bool_of(ms, "SupportsDirectPlay")) {
        pb.play_method = "DirectPlay";
        pb.url = server_ + "/Videos/" + item_id + "/stream?static=true&mediaSourceId=" + pb.media_source_id +
                 "&playSessionId=" + pb.play_session_id + "&api_key=" + token_;
    } else if (!transcoding.empty()) {
        pb.play_method = transcoding.find("/stream") != std::string::npos ? "DirectStream" : "Transcode";
        pb.url = server_ + transcoding;
        const size_t r = transcoding.find("TranscodeReasons=");
        if (r != std::string::npos)
            pb.transcode_reasons = transcoding.substr(r + 17, transcoding.find('&', r) - r - 17);
    } else {
        set_error("PlaybackInfo: the server offers no way to play this");
        cJSON_Delete(j);
        return false;
    }
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
    http_request("DELETE",
                 server_ + "/Videos/ActiveEncodings?deviceId=" + url_escape(device_id_) +
                     "&playSessionId=" + pb.play_session_id,
                 {auth_header()}, "", kTimeout);
}

} // namespace jf
