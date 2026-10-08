/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/syncplay.h"

#include "app/remote.h"
#include "app/spawn.h"
#include "jf/json_num.h"

#include "evo_boot_trace.h"

#include <algorithm>
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
std::string s_entry_here;              /* the entry the player has open ("" none) */
bool s_queue_next = false;             /* the group's queue has an entry after the current one */
double s_last_update = 0;              /* the queue's LastUpdate (ms): older ones are dropped */
std::atomic<double> s_offset_ms{0};    /* server UTC - our UTC, measured */
std::atomic<unsigned> s_clock_gen{0};  /* a new one ends the last group's clock loop */
std::atomic<bool> s_ready_asked{false}; /* the group wants Ready for what plays here */
std::mutex s_wait_order;               /* SetIgnoreWait: one request at a time */
std::atomic<bool> s_ignore_want{false}, s_ignore_sent{false};

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
    if (!jelly5::spawn([c, path, json] {
            if (!c->post_json(path, json, nullptr))
                evo_bt("syncplay: %s failed: %s", path.c_str(), c->last_error().c_str());
        }))
        evo_bt("syncplay: %s not sent: no thread", path.c_str());
}

/* NTP-style: the server's clock against ours, best of a few round trips.
 * Returns the round trip (ms), or -1. */
double sync_clock(jf::Client *c)
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
    if (best_rtt >= 1e9)
        return -1;
    s_offset_ms = offset;
    evo_bt("syncplay: clock offset %.0f ms, round trip %.0f ms", offset, best_rtt);
    return best_rtt;
}

/* In a group: the clock measured again every minute (jellyfin-web's pace), and
 * the ping told to the group (it waits for its slowest member). The server takes
 * Ping only from a member, so this starts at GroupJoined; a new group, a leave
 * or another session (s_clock_gen) ends it. */
void clock_loop(jf::Client *c, unsigned gen)
{
    for (;;) {
        const double rtt = sync_clock(c);
        if (s_clock_gen != gen)
            return;
        if (rtt >= 0) {
            char b[64];
            std::snprintf(b, sizeof b, "{\"Ping\":%d}", (int)std::lround(rtt / 2));
            c->post_json("/SyncPlay/Ping", b, nullptr);
        }
        for (int i = 0; i < 120; i++) {
            usleep(500000);
            if (s_clock_gen != gen)
                return;
        }
    }
}

/* A body of string fields, escaped by cJSON (the ids come from the server). */
std::string json_of(std::initializer_list<std::pair<const char *, std::string>> fields)
{
    cJSON *j = cJSON_CreateObject();
    for (const auto &f : fields)
        cJSON_AddStringToObject(j, f.first, f.second.c_str());
    char *text = cJSON_PrintUnformatted(j);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(j);
    return out;
}

/* Whether the group waits for this PS5 (SetIgnoreWait): not while its player is
 * closed, again once the group plays here. A stop and the next start can race, so
 * one thread at a time sends the latest wish until the server has it. */
void set_ignore_wait(bool ignore)
{
    s_ignore_want = ignore;
    jf::Client *c = client();
    if (!c || s_ignore_sent == ignore)
        return;
    jelly5::spawn([c] {
        std::lock_guard<std::mutex> o(s_wait_order);
        for (int i = 0; i < 3; i++) {
            const bool want = s_ignore_want;
            if (want == s_ignore_sent)
                return;
            if (!c->post_json("/SyncPlay/SetIgnoreWait", want ? "{\"IgnoreWait\":true}" : "{\"IgnoreWait\":false}",
                              nullptr)) {
                evo_bt("syncplay: SetIgnoreWait failed: %s", c->last_error().c_str());
                return;
            }
            s_ignore_sent = want;
        }
    });
}

/* Out of a group (under s_lock). */
void forget_group()
{
    s_group_id.clear();
    s_group_name.clear();
    s_playlist_item.clear();
    s_entry_here.clear();
    s_queue_next = false;
    s_last_update = 0;
    s_clock_gen++;
    s_ignore_want = s_ignore_sent = false;
}

std::string ready_body(double position_s, bool playing)
{
    std::string entry;
    {
        std::lock_guard<std::mutex> g(s_lock);
        entry = s_playlist_item;
    }
    /* No fixed buffer: a long PlaylistItemId cut short made the JSON invalid. */
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "When", server_now_iso().c_str());
    cJSON_AddRawToObject(j, "PositionTicks", std::to_string((long long)(position_s * 10000000.0)).c_str());
    cJSON_AddBoolToObject(j, "IsPlaying", playing);
    cJSON_AddStringToObject(j, "PlaylistItemId", entry.c_str());
    char *text = cJSON_PrintUnformatted(j);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(j);
    return out;
}

} // namespace

void attach(jf::Client *c)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_client = c;
    forget_group();
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
    return c->post_json("/SyncPlay/Join", json_of({{"GroupId", group_id}}), nullptr);
}

void leave()
{
    jf::Client *c = client();
    if (c)
        c->post_json("/SyncPlay/Leave", "{}", nullptr);
    std::lock_guard<std::mutex> g(s_lock);
    forget_group();
}

bool forget_here()
{
    std::lock_guard<std::mutex> g(s_lock);
    const bool was = !s_group_id.empty();
    forget_group();
    return was;
}

void leave_async()
{
    jf::Client *c;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (s_group_id.empty())
            return;
        c = s_client;
        forget_group();   /* (the clock loop and the group's wait state, too) */
    }
    if (c && !jelly5::spawn([c] {
            if (!c->post_json("/SyncPlay/Leave", "{}", nullptr))
                evo_bt("syncplay: leave failed: %s", c->last_error().c_str());
        }))
        evo_bt("syncplay: leave not sent: no thread");
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
    if (!active())
        return;
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_entry_here = s_playlist_item;
    }
    s_ready_asked = false;
    set_ignore_wait(false);
    post_async("/SyncPlay/Ready", ready_body(position_s, playing));
}

void player_stopped()
{
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_entry_here.clear();
        if (s_group_id.empty())
            return;
    }
    s_ready_asked = false;
    set_ignore_wait(true);
}

bool ready_asked() { return s_ready_asked.exchange(false); }

void buffering(bool stalled, double position_s, bool playing)
{
    if (active())
        post_async(stalled ? "/SyncPlay/Buffering" : "/SyncPlay/Ready", ready_body(position_s, playing));
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

bool queue_has_next()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_queue_next;
}

void request_next()
{
    std::string id;
    {
        std::lock_guard<std::mutex> g(s_lock);
        id = s_playlist_item;
    }
    post_async("/SyncPlay/NextItem", json_of({{"PlaylistItemId", id}}));
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
        bool fresh;
        jf::Client *c;
        unsigned gen;
        {
            std::lock_guard<std::mutex> g(s_lock);
            fresh = group != s_group_id;   /* not a repeat for the group already joined */
            if (fresh)
                forget_group();
            s_group_id = group;
            s_group_name = str(d, "GroupName");
            c = s_client;
            gen = s_clock_gen;   /* here: a leave right after ends this loop too */
        }
        if (fresh && c && !jelly5::spawn([c, gen] { clock_loop(c, gen); }))
            evo_bt("syncplay: no thread for the clock");
    } else if (type == "GroupLeft" || type == "NotInGroup" || type == "GroupDoesNotExist") {
        std::lock_guard<std::mutex> g(s_lock);
        forget_group();
    } else if (type == "PlayQueue") {
        /* The group's queue: what plays now, and from where. */
        const std::string reason = str(d, "Reason");
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(d, "Playlist");
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(d, "PlayingItemIndex");
        const int i = cJSON_IsNumber(idx) ? jf::to_int<int>(idx->valuedouble, -1) : -1;
        const cJSON *entry = i >= 0 ? cJSON_GetArrayItem(list, i) : nullptr;
        if (!entry)
            return;
        const std::string item = str(entry, "ItemId"), entry_id = str(entry, "PlaylistItemId");
        if (!remote::is_item_id(item)) {
            evo_bt("syncplay: PlayQueue with an odd item id \"%s\", ignored", item.c_str());
            return;
        }
        const double update = parse_iso_ms(str(d, "LastUpdate"));
        bool here;
        {
            std::lock_guard<std::mutex> g(s_lock);
            if (update > 0 && update <= s_last_update)
                return;   /* not newer than the queue we have (sent again, say after a reconnect) */
            s_last_update = std::max(s_last_update, update);
            s_queue_next = i + 1 < cJSON_GetArraySize(list);
            const bool same = entry_id == s_playlist_item;
            s_playlist_item = entry_id;
            if (same && reason != "NewPlaylist" && reason != "SetCurrentItem")
                return;   /* the queue changed around what already plays */
            here = !entry_id.empty() && entry_id == s_entry_here;
        }
        if (here) {
            s_ready_asked = true;   /* already open here: Ready from where it is, no restart */
            return;
        }
        set_ignore_wait(false);   /* the group plays here again: it waits for this PS5 */
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(d, "StartPositionTicks");
        remote::Command c;
        c.kind = remote::Command::Play;
        c.item_ids.push_back(item);
        c.start_ticks = cJSON_IsNumber(start) ? jf::to_int<int64_t>(start->valuedouble) : 0;
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
    c.seek_ticks = cJSON_IsNumber(pos) ? jf::to_int<int64_t>(pos->valuedouble, -1) : -1;
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
