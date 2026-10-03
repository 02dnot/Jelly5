/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/syncplay.h"

#include "app/remote.h"

#include "evo_boot_trace.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace syncplay {
namespace {

std::mutex s_lock;
jf::Client *s_client = nullptr;
std::string s_group_id, s_group_name;
std::string s_playlist_item;           /* the group's current entry */
std::atomic<double> s_offset_ms{0};    /* server UTC - our UTC, measured */

double mono_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double utc_ms()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* "2026-10-03T12:00:03.2500000Z" -> ms since 1970 (UTC). */
double parse_iso_ms(const std::string &iso)
{
    int y, mo, d, h, mi;
    double sec;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    /* Days from the civil date (Howard Hinnant's algorithm). */
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const double days = era * 146097.0 + doe - 719468.0;
    return ((days * 24 + h) * 60 + mi) * 60000.0 + sec * 1000.0;
}

std::string iso_of_ms(double ms)
{
    const time_t t = (time_t)(ms / 1000.0);
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[48];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (int)std::fmod(ms, 1000.0));
    return b;
}

std::string server_now_iso() { return iso_of_ms(utc_ms() + s_offset_ms); }

std::string str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : std::string();
}

jf::Client *client()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_client;
}

/* A request in the background: the player's thread never waits on the server. */
void post_async(const std::string &path, const std::string &json)
{
    jf::Client *c = client();
    if (!c)
        return;
    std::thread([c, path, json] {
        if (!c->post_json(path, json, nullptr))
            evo_bt("syncplay: %s failed: %s", path.c_str(), c->last_error().c_str());
    }).detach();
}

/* NTP-style: the server's clock against ours, best of a few round trips; then
 * the round trip goes to the group (it waits for the slowest member). */
void sync_clock(jf::Client *c)
{
    double best_rtt = 1e9, offset = 0;
    for (int i = 0; i < 4; i++) {
        const double t0 = utc_ms();
        std::string body;
        if (!c->get_json("/GetUtcTime", &body))
            continue;
        const double t3 = utc_ms();
        cJSON *j = cJSON_Parse(body.c_str());
        const double t1 = parse_iso_ms(str(j, "RequestReceptionTime"));
        const double t2 = parse_iso_ms(str(j, "ResponseTransmissionTime"));
        cJSON_Delete(j);
        if (t1 <= 0 || t2 <= 0)
            continue;
        const double rtt = (t3 - t0) - (t2 - t1);
        if (rtt < best_rtt) {
            best_rtt = rtt;
            offset = ((t1 - t0) + (t2 - t3)) / 2.0;
        }
    }
    if (best_rtt < 1e9) {
        s_offset_ms = offset;
        char b[64];
        std::snprintf(b, sizeof b, "{\"Ping\":%d}", (int)best_rtt);
        c->post_json("/SyncPlay/Ping", b, nullptr);
        evo_bt("syncplay: clock offset %.0f ms, round trip %.0f ms", offset, best_rtt);
    }
}

std::string ready_body(double position_s, bool playing)
{
    char b[256];
    std::lock_guard<std::mutex> g(s_lock);
    std::snprintf(b, sizeof b, "{\"When\":\"%s\",\"PositionTicks\":%lld,\"IsPlaying\":%s,\"PlaylistItemId\":\"%s\"}",
                  server_now_iso().c_str(), (long long)(position_s * 10000000.0), playing ? "true" : "false",
                  s_playlist_item.c_str());
    return b;
}

} // namespace

void attach(jf::Client *c)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_client = c;
    s_group_id.clear();
    s_group_name.clear();
    s_playlist_item.clear();
}

bool active()
{
    std::lock_guard<std::mutex> g(s_lock);
    return !s_group_id.empty();
}

std::string group_name()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_group_name;
}

std::vector<Group> list()
{
    std::vector<Group> out;
    jf::Client *c = client();
    std::string body;
    if (!c || !c->get_json("/SyncPlay/List", &body))
        return out;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *g;
    cJSON_ArrayForEach(g, j) {
        Group x;
        x.id = str(g, "GroupId");
        x.name = str(g, "GroupName");
        x.state = str(g, "State");
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(g, "Participants"))
            if (cJSON_IsString(p))
                x.participants.push_back(p->valuestring);
        out.push_back(std::move(x));
    }
    cJSON_Delete(j);
    return out;
}

bool create(const std::string &name)
{
    jf::Client *c = client();
    if (!c)
        return false;
    sync_clock(c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "GroupName", name.c_str());
    char *text = cJSON_PrintUnformatted(j);
    const bool ok = c->post_json("/SyncPlay/New", text, nullptr);
    cJSON_free(text);
    cJSON_Delete(j);
    return ok;   /* the group itself arrives as GroupJoined */
}

bool join(const std::string &group_id)
{
    jf::Client *c = client();
    if (!c)
        return false;
    sync_clock(c);
    return c->post_json("/SyncPlay/Join", "{\"GroupId\":\"" + group_id + "\"}", nullptr);
}

void leave()
{
    jf::Client *c = client();
    if (c)
        c->post_json("/SyncPlay/Leave", "{}", nullptr);
    std::lock_guard<std::mutex> g(s_lock);
    s_group_id.clear();
    s_group_name.clear();
    s_playlist_item.clear();
}

bool play(const jf::Item &item)
{
    if (!active())
        return false;
    char b[256];
    std::snprintf(b, sizeof b, "{\"PlayingQueue\":[\"%s\"],\"PlayingItemPosition\":0,\"StartPositionTicks\":%lld}",
                  item.id.c_str(), (long long)item.position_ticks);
    post_async("/SyncPlay/SetNewQueue", b);
    return true;
}

void player_started(double position_s, bool playing)
{
    if (active())
        post_async("/SyncPlay/Ready", ready_body(position_s, playing));
}

void seeked(double position_s) { player_started(position_s, false); }

void request_pause(bool pause, double position_s)
{
    (void)position_s;
    post_async(pause ? "/SyncPlay/Pause" : "/SyncPlay/Unpause", "{}");
}

void request_seek(double position_s)
{
    char b[64];
    std::snprintf(b, sizeof b, "{\"PositionTicks\":%lld}", (long long)(position_s * 10000000.0));
    post_async("/SyncPlay/Seek", b);
}

void request_next()
{
    std::string id;
    {
        std::lock_guard<std::mutex> g(s_lock);
        id = s_playlist_item;
    }
    post_async("/SyncPlay/NextItem", "{\"PlaylistItemId\":\"" + id + "\"}");
}

double local_time_of(const std::string &iso)
{
    const double server_ms = parse_iso_ms(iso);
    if (server_ms <= 0)
        return mono_s();
    const double wait_ms = server_ms - (utc_ms() + s_offset_ms);
    return mono_s() + wait_ms / 1000.0;
}

void on_group_update(const cJSON *data)
{
    const std::string type = str(data, "Type");
    const std::string group = str(data, "GroupId");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(data, "Data");
    evo_bt("syncplay: group update %s", type.c_str());
    if (type == "GroupJoined") {
        std::lock_guard<std::mutex> g(s_lock);
        s_group_id = group;
        s_group_name = str(d, "GroupName");
    } else if (type == "GroupLeft" || type == "NotInGroup" || type == "GroupDoesNotExist") {
        std::lock_guard<std::mutex> g(s_lock);
        s_group_id.clear();
        s_group_name.clear();
        s_playlist_item.clear();
    } else if (type == "PlayQueue") {
        /* The group's queue: what plays now, and from where. */
        const std::string reason = str(d, "Reason");
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(d, "Playlist");
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(d, "PlayingItemIndex");
        const int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : -1;
        const cJSON *entry = i >= 0 ? cJSON_GetArrayItem(list, i) : nullptr;
        if (!entry)
            return;
        const std::string item = str(entry, "ItemId"), entry_id = str(entry, "PlaylistItemId");
        {
            std::lock_guard<std::mutex> g(s_lock);
            const bool same = entry_id == s_playlist_item;
            s_playlist_item = entry_id;
            if (same && reason != "NewPlaylist" && reason != "SetCurrentItem")
                return;   /* the queue changed around what already plays */
        }
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(d, "StartPositionTicks");
        remote::Command c;
        c.kind = remote::Command::Play;
        c.item_ids.push_back(item);
        c.start_ticks = cJSON_IsNumber(start) ? (int64_t)start->valuedouble : 0;
        c.play_command = "SyncPlay";   /* plays here, does not ask the group again */
        remote::send(c);
    }
}

void on_command(const cJSON *data)
{
    const std::string cmd = str(data, "Command");
    {
        std::lock_guard<std::mutex> g(s_lock);
        const std::string entry = str(data, "PlaylistItemId");
        if (!entry.empty() && !s_playlist_item.empty() && entry != s_playlist_item)
            return;   /* about another entry than the one playing here */
    }
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(data, "PositionTicks");
    remote::Command c;
    c.seek_ticks = cJSON_IsNumber(pos) ? (int64_t)pos->valuedouble : -1;
    c.at = local_time_of(str(data, "When"));
    c.syncplay = true;
    if (cmd == "Unpause") c.kind = remote::Command::Unpause;
    else if (cmd == "Pause") c.kind = remote::Command::Pause;
    else if (cmd == "Seek") c.kind = remote::Command::Seek;
    else if (cmd == "Stop") c.kind = remote::Command::Stop;
    else return;
    evo_bt("syncplay: %s in %.2f s at %.1f s", cmd.c_str(), c.at - mono_s(), c.seek_ticks / 1e7);
    remote::send(c);
}

} // namespace syncplay
