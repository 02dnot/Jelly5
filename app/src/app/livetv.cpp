/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/livetv.h"

#include "app/i18n.h"
#include "app/i18n_cldr.h"
#include "app/spawn.h"
#include "evo_boot_trace.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <sys/stat.h>

extern "C" {
#include "cJSON.h"
}

namespace livetv {
namespace {

constexpr int64_t kHour = 3600;
constexpr int64_t kAhead = 12 * kHour;       /* loaded ahead at once: the evening from the afternoon */
constexpr int64_t kStale = 10 * 60;          /* a guide older than this is loaded again when shown */
constexpr int64_t kHorizon = 7 * 24 * kHour; /* how far ahead the guide goes (the servers' guide days) */
constexpr int64_t kKeep = 48 * kHour;        /* more than this held: what lies far from view goes */
constexpr int kMaxChannels = 2000;           /* the most a guide holds (IPTV lists can be huge) */

std::mutex s_lock;
jf::Client *s_client = nullptr;
unsigned s_gen = 0;                          /* the session: answers for an older one are dropped */
GuideRef s_guide = std::make_shared<Guide>();
std::atomic<unsigned> s_version{1};
bool s_checked = false, s_available = false;
bool s_loading = false, s_extending = false;
int64_t s_loaded_at = 0, s_failed_at = 0;
std::map<std::string, std::pair<bool, int64_t>> s_favorite_set;   /* set here: the value and when */
std::string s_last, s_previous, s_history_for;

void publish_locked(std::shared_ptr<Guide> g)
{
    s_guide = std::move(g);
    s_version++;
}

void load_history_locked()
{
    if (!s_client || s_history_for == s_client->user_id())
        return;
    s_history_for = s_client->user_id();
    s_last.clear();
    s_previous.clear();
    std::string body;
    if (FILE *f = std::fopen(history_file(s_history_for).c_str(), "rb")) {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && body.size() < 8192)
            body.append(buf, n);
        std::fclose(f);
    }
    if (cJSON *j = cJSON_Parse(body.c_str())) {
        const char *l = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "last"));
        const char *p = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "previous"));
        s_last = l ? l : "";
        s_previous = p ? p : "";
        cJSON_Delete(j);
    }
}

/* Programmes by channel, by start, without repeats (two loads can overlap). Only
 * the channels touched get new lists; the others stay shared with older snapshots. */
void add_programs(Guide &g, std::vector<jf::Item> items)
{
    std::map<std::string, std::vector<jf::Item>> touched;
    for (jf::Item &p : items) {
        if (p.channel_id.empty() || p.end_utc <= p.start_utc)
            continue;
        auto t = touched.find(p.channel_id);
        if (t == touched.end()) {
            const auto had = g.programs.find(p.channel_id);
            t = touched.emplace(p.channel_id, had != g.programs.end() && had->second ? *had->second
                                                                                     : std::vector<jf::Item>()).first;
        }
        std::vector<jf::Item> &list = t->second;
        bool have = false;
        for (jf::Item &q : list)
            if (q.id == p.id) {
                q = p;   /* the newer answer (a timer set since) */
                have = true;
                break;
            }
        if (!have)
            list.push_back(std::move(p));
    }
    for (auto &kv : touched) {
        std::vector<jf::Item> &list = kv.second;
        std::stable_sort(list.begin(), list.end(),
                         [](const jf::Item &a, const jf::Item &b) { return a.start_utc < b.start_utc; });
        /* A guide can carry programmes that overlap (a listing changed after it was
         * fetched): the later start wins the time they share. */
        for (size_t i = 0; i + 1 < list.size();) {
            if (list[i + 1].start_utc <= list[i].start_utc) {
                list.erase(list.begin() + i);
                continue;
            }
            if (list[i + 1].start_utc < list[i].end_utc)
                list[i].end_utc = list[i + 1].start_utc;
            i++;
        }
        g.programs[kv.first] = std::make_shared<const std::vector<jf::Item>>(std::move(list));
    }
}

std::vector<std::string> ids_of(const Guide &g)
{
    std::vector<std::string> ids;
    for (const jf::Item &c : g.channels)
        ids.push_back(c.id);
    return ids;
}

/* The programmes of these channels in [from, to): a big list (IPTV: hundreds of
 * channels) in a few requests side by side. false when one of them failed. */
bool fetch_programs(jf::Client *c, const std::vector<std::string> &ids, int64_t from, int64_t to,
                    std::vector<jf::Item> *out)
{
    constexpr size_t kPart = 200;
    const size_t parts = (ids.size() + kPart - 1) / kPart;
    std::vector<std::vector<jf::Item>> got(parts);
    std::vector<char> ok(parts, 0);
    for (size_t first = 0; first < parts; first += 4) {   /* (four at a time) */
        std::vector<std::function<void()>> jobs;
        for (size_t i = first; i < parts && i < first + 4; i++)
            jobs.emplace_back([&, i] {
                const std::vector<std::string> part(ids.begin() + i * kPart,
                                                    ids.begin() + std::min(ids.size(), (i + 1) * kPart));
                ok[i] = c->programs(part, from, to, &got[i]);
            });
        jelly5::run_all(jobs);
    }
    bool complete = true;
    for (size_t i = 0; i < parts; i++) {
        complete = complete && ok[i];
        for (jf::Item &it : got[i])
            out->push_back(std::move(it));
    }
    return complete;
}

/* Adds [a, b) to the spans a guide has loaded (sorted, joined where they touch). */
void cover(Guide &g, int64_t a, int64_t b)
{
    if (b <= a)
        return;
    g.covered.push_back({a, b});
    std::sort(g.covered.begin(), g.covered.end());
    std::vector<std::pair<int64_t, int64_t>> joined;
    for (const auto &r : g.covered)
        if (!joined.empty() && r.first <= joined.back().second)
            joined.back().second = std::max(joined.back().second, r.second);
        else
            joined.push_back(r);
    g.covered = std::move(joined);
}

/* Keeps only what lies in the given spans (a guide that jumped days ahead lets go
 * of the days between; memory stays bounded however far it is taken). */
void trim(Guide &g, const std::vector<std::pair<int64_t, int64_t>> &keep)
{
    auto kept = [&](int64_t a, int64_t b) {
        for (const auto &k : keep)
            if (a < k.second && b > k.first)
                return true;
        return false;
    };
    for (auto &kv : g.programs) {
        if (!kv.second)
            continue;
        bool all = true;
        for (const jf::Item &p : *kv.second)
            all = all && kept(p.start_utc, p.end_utc);
        if (all)
            continue;
        std::vector<jf::Item> left;
        for (const jf::Item &p : *kv.second)
            if (kept(p.start_utc, p.end_utc))
                left.push_back(p);
        kv.second = std::make_shared<const std::vector<jf::Item>>(std::move(left));
    }
    std::vector<std::pair<int64_t, int64_t>> covered;
    for (const auto &r : g.covered)
        for (const auto &k : keep) {
            const int64_t a = std::max(r.first, k.first), b = std::min(r.second, k.second);
            if (a < b)
                covered.push_back({a, b});
        }
    g.covered.clear();
    for (const auto &r : covered)
        cover(g, r.first, r.second);
}

/* A change to the guide, made on a worker (never on a frame): the new snapshot is
 * built outside the lock and published when nothing else was published meanwhile
 * (else it is built again on top of that one). */
void update(unsigned gen, const std::function<void(Guide &)> &change)
{
    for (int tries = 0; tries < 8; tries++) {
        GuideRef cur;
        {
            std::lock_guard<std::mutex> g(s_lock);
            if (gen != s_gen)
                return;
            cur = s_guide;
        }
        auto next = std::make_shared<Guide>(*cur);   /* (the programme lists are shared, not copied) */
        change(*next);
        std::lock_guard<std::mutex> g(s_lock);
        if (gen != s_gen)
            return;
        if (s_guide == cur) {
            publish_locked(next);
            return;
        }
    }
}

/* The channels and [from, to) of the guide, on a worker. */
/* c: the client the caller took with gen (clients are never freed; s_client may
 * be another one, or none, by the time this runs). */
void load(jf::Client *c, unsigned gen, int64_t from, int64_t to)
{
    const int64_t started = now();
    auto fresh = std::make_shared<Guide>();
    jf::Page page;
    for (int start = 0; start < kMaxChannels;) {   /* every channel, a page at a time */
        jf::Page p = c->channels(start, 500, false);   /* (what airs comes with the programmes) */
        if (!p.ok) {
            page.ok = false;
            break;
        }
        page.ok = true;
        for (jf::Item &it : p.items)
            page.items.push_back(std::move(it));
        start += 500;
        if (p.items.empty() || start >= p.total)
            break;
    }
    bool complete = page.ok;
    if (page.ok) {
        fresh->channels = std::move(page.items);
        fresh->from = from;
        fresh->horizon = from + kHorizon;
        fresh->loaded = true;
        std::vector<jf::Item> progs;
        complete = fetch_programs(c, ids_of(*fresh), from, to, &progs);
        add_programs(*fresh, std::move(progs));
        if (complete)   /* (part of it only: not marked as had, so it is asked again) */
            cover(*fresh, from, to);
        jelly5::run_all({[&] { fresh->recordings = c->recordings(60); },
                         [&] { fresh->timers = c->timers(); }});
    }
    if (!page.ok) {
        evo_bt("livetv: the guide did not load: %s", c->last_error().c_str());
        update(gen, [](Guide &g) { g.failed = true; });
    } else {
        /* What was loaded further on (the guide scrolled ahead) stays, past the new end. */
        update(gen, [&](Guide &g) {
            Guide merged = *fresh;
            /* Favourites set here while it loaded: the list it fetched is older. */
            std::map<std::string, bool> since;
            {
                std::lock_guard<std::mutex> l(s_lock);
                for (const auto &kv : s_favorite_set)
                    if (kv.second.second >= started)
                        since[kv.first] = kv.second.first;
            }
            if (!since.empty()) {
                for (jf::Item &ch : merged.channels) {
                    const auto f = since.find(ch.id);
                    if (f != since.end())
                        ch.favorite = f->second;
                }
                std::stable_sort(merged.channels.begin(), merged.channels.end(),
                                 [](const jf::Item &a, const jf::Item &b) { return a.favorite && !b.favorite; });
            }
            /* What was loaded further on (the guide scrolled or jumped ahead) stays. */
            std::vector<jf::Item> later;
            for (const auto &kv : g.programs)
                if (kv.second)
                    for (const jf::Item &p : *kv.second)
                        if (p.start_utc >= to)
                            later.push_back(p);
            add_programs(merged, std::move(later));
            for (const auto &r : g.covered)
                if (r.second > to)
                    cover(merged, std::max(r.first, to), r.second);
            g = std::move(merged);
        });
        evo_bt("livetv: %zu channels, guide from %s%s", fresh->channels.size(), clock(from).c_str(),
               complete ? "" : " (some programmes missing)");
    }
    std::lock_guard<std::mutex> g(s_lock);
    s_loading = false;
    if (gen != s_gen)
        return;
    /* A load that failed, or brought only part of the guide, is tried again soon
     * (not at once: a server answering with an error is not asked in a loop). */
    s_loaded_at = complete ? now() : 0;
    s_failed_at = complete ? 0 : now();
}

/* The programme as the server has it now (its timers), into the guide at once;
 * the rest (a series' other episodes, the list of recordings) loads with the
 * guide, which is marked old for it. */
void reload_program(jf::Client *c, unsigned gen, const std::string &id)
{
    jf::Item p;
    if (c->program(id, &p))
        update(gen, [&](Guide &g) { add_programs(g, {p}); });
    std::lock_guard<std::mutex> g(s_lock);
    if (gen == s_gen)
        s_loaded_at = 0;
}

} // namespace

const jf::Item *Guide::channel(const std::string &id) const
{
    for (const jf::Item &c : channels)
        if (c.id == id)
            return &c;
    return nullptr;
}

bool Guide::covers(int64_t t) const
{
    for (const auto &r : covered)
        if (r.first <= t && t < r.second)
            return true;
    return false;
}

const std::vector<jf::Item> *Guide::programs_of(const std::string &channel_id) const
{
    const auto it = programs.find(channel_id);
    return it == programs.end() || !it->second ? nullptr : it->second.get();
}

const jf::Item *Guide::on_at(const std::string &channel_id, int64_t t) const
{
    const std::vector<jf::Item> *list = programs_of(channel_id);
    if (!list)
        return nullptr;
    for (const jf::Item &p : *list)
        if (p.start_utc <= t && t < p.end_utc)
            return &p;
    return nullptr;
}

const jf::Item *Guide::after(const std::string &channel_id, int64_t t) const
{
    const std::vector<jf::Item> *list = programs_of(channel_id);
    if (!list)
        return nullptr;
    for (const jf::Item &p : *list)
        if (p.start_utc > t)
            return &p;
    return nullptr;
}

int64_t now() { return (int64_t)time(nullptr); }

int64_t half_hour(int64_t t) { return t - ((t % 1800) + 1800) % 1800; }

void attach(jf::Client *c)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_client = c;
    s_gen++;
    s_checked = s_available = false;
    s_loading = s_extending = false;
    s_loaded_at = s_failed_at = 0;
    s_favorite_set.clear();
    s_history_for.clear();
    publish_locked(std::make_shared<Guide>());
}

bool check()
{
    jf::Client *c;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (s_checked || !s_client)
            return s_available;
        c = s_client;
        gen = s_gen;
    }
    bool answered = false;
    const bool yes = c->live_tv_available(&answered);
    std::lock_guard<std::mutex> g(s_lock);
    if (gen == s_gen && answered) {   /* (no answer: asked again with the next home load) */
        s_checked = true;
        s_available = yes;
        load_history_locked();
        evo_bt("livetv: %s", yes ? "available" : "not available");
    }
    return yes;
}

bool available()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_available;
}

GuideRef guide()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_guide;
}

unsigned version() { return s_version; }

bool loading()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_loading;
}

void refresh(bool force)
{
    jf::Client *c;
    unsigned gen;
    int64_t from, to;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_client || !s_available || s_loading)
            return;
        const int64_t t = now();
        /* Fresh: loaded a moment ago, and it still starts in this half hour. A load
         * that failed is tried again after half a minute, not on the next frame. */
        if (!force && s_loaded_at > 0 && t - s_loaded_at < kStale && s_guide->from == half_hour(t))
            return;
        if (!force && s_failed_at > 0 && t - s_failed_at < 30)
            return;
        s_loading = true;
        gen = s_gen;
        c = s_client;
        from = half_hour(t);
        to = from + kAhead;
    }
    if (!jelly5::spawn([c, gen, from, to] { load(c, gen, from, to); })) {
        std::lock_guard<std::mutex> g(s_lock);
        s_loading = false;
    }
}

void ensure(int64_t from, int64_t to)
{
    unsigned gen;
    std::vector<std::string> ids;
    jf::Client *c;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_client || !s_available || s_extending || s_loading || !s_guide->loaded)
            return;
        from = std::max(from, s_guide->from);
        to = std::min(to, s_guide->horizon);
        /* Only what is not there yet (from the first gap to the last). */
        for (const auto &r : s_guide->covered) {
            if (r.first <= from && r.second > from)
                from = r.second;
            if (r.first < to && r.second >= to)
                to = r.first;
        }
        if (to - from < 60)
            return;
        s_extending = true;
        gen = s_gen;
        ids = ids_of(*s_guide);
        c = s_client;
    }
    const bool started = jelly5::spawn([gen, from, to, ids, c] {
        std::vector<jf::Item> more;
        const bool ok = fetch_programs(c, ids, from, to, &more);
        update(gen, [&](Guide &g) {
            /* Far from what is had (a jump days ahead): what lies between goes. */
            int64_t had = 0;
            for (const auto &r : g.covered)
                had += r.second - r.first;
            if (had + (to - from) > kKeep)
                trim(g, {{g.from, g.from + kAhead}, {from - kAhead, to + kAhead}});
            add_programs(g, more);
            if (ok)   /* (part of it only: not marked as had, so it is asked again) */
                cover(g, from, to);
        });
        std::lock_guard<std::mutex> g(s_lock);
        s_extending = false;
    });
    if (!started) {
        std::lock_guard<std::mutex> g(s_lock);
        s_extending = false;
    }
}

void set_favorite(const std::string &channel_id, bool on)
{
    jf::Client *c;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_client)
            return;
        c = s_client;
        gen = s_gen;
        s_favorite_set[channel_id] = {on, now()};
    }
    /* Shown as soon as the worker has it (a moment), written to the server after. */
    jelly5::spawn([c, gen, channel_id, on] {
        update(gen, [&](Guide &g) {
            for (jf::Item &ch : g.channels)
                if (ch.id == channel_id)
                    ch.favorite = on;
            /* Favourites first, as the server sorts them (stable: the server's order within). */
            std::stable_sort(g.channels.begin(), g.channels.end(),
                             [](const jf::Item &a, const jf::Item &b) { return a.favorite && !b.favorite; });
        });
        if (!c->set_favorite(channel_id, on))
            evo_bt("livetv: favourite not saved: %s", c->last_error().c_str());
    });
}

void record(const jf::Item &program, bool series, std::function<void(bool)> done)
{
    jf::Client *c;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        c = s_client;
        gen = s_gen;
    }
    if (!c)
        return;
    const std::string id = program.id;
    if (!jelly5::spawn([c, gen, id, series, done] {
            const bool ok = c->record(id, series);
            if (!ok)
                evo_bt("livetv: recording not set: %s", c->last_error().c_str());
            reload_program(c, gen, id);
            if (done)
                done(ok);
        }) && done)
        done(false);
}

void cancel(const jf::Item &program, bool series, std::function<void(bool)> done)
{
    jf::Client *c;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        c = s_client;
        gen = s_gen;
    }
    if (!c)
        return;
    const std::string id = program.id, timer = series ? program.series_timer_id : program.timer_id;
    if (!jelly5::spawn([c, gen, id, timer, series, done] {
            const bool ok = series ? c->cancel_series_timer(timer) : c->cancel_timer(timer);
            if (!ok)
                evo_bt("livetv: recording not cancelled: %s", c->last_error().c_str());
            reload_program(c, gen, id);
            if (done)
                done(ok);
        }) && done)
        done(false);
}

std::string history_file(const std::string &user_id) { return "/download0/jelly5/livetv-" + user_id + ".json"; }

void watched(const std::string &channel_id)
{
    std::string path, text;
    {
        std::lock_guard<std::mutex> g(s_lock);
        load_history_locked();
        if (channel_id.empty() || channel_id == s_last || s_history_for.empty())
            return;
        s_previous = s_last;
        s_last = channel_id;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "last", s_last.c_str());
        cJSON_AddStringToObject(j, "previous", s_previous.c_str());
        if (char *s = cJSON_PrintUnformatted(j)) {
            text = s;
            cJSON_free(s);
        }
        cJSON_Delete(j);
        path = history_file(s_history_for);
    }
    mkdir("/download0/jelly5", 0777);
    const std::string tmp = path + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (ok)
            std::rename(tmp.c_str(), path.c_str());
    }
}

std::string last_channel()
{
    std::lock_guard<std::mutex> g(s_lock);
    load_history_locked();
    return s_last;
}

std::string previous_channel()
{
    std::lock_guard<std::mutex> g(s_lock);
    load_history_locked();
    return s_previous;
}

std::string logo_url(const jf::Item &ch, int width)
{
    jf::Client *c;
    {
        std::lock_guard<std::mutex> g(s_lock);
        c = s_client;
    }
    return c && !ch.primary_tag.empty() ? c->image_url(ch.id, "Primary", ch.primary_tag, width) : std::string();
}

std::string picture_url(const jf::Item &p, int width)
{
    jf::Client *c;
    {
        std::lock_guard<std::mutex> g(s_lock);
        c = s_client;
    }
    if (!c)
        return std::string();
    if (!p.primary_tag.empty())
        return c->image_url(p.id, "Primary", p.primary_tag, width);
    if (!p.thumb_tag.empty())
        return c->image_url(p.thumb_owner, "Thumb", p.thumb_tag, width);
    if (!p.backdrop_tag.empty())
        return c->image_url(p.backdrop_owner, "Backdrop", p.backdrop_tag, width);
    return std::string();
}

std::string clock(int64_t utc)
{
    const time_t t = (time_t)utc;
    struct tm tm;
    localtime_r(&t, &tm);
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

std::string day_label(int64_t utc)
{
    const time_t t = (time_t)utc, n = time(nullptr);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&n, &b);
    /* Days apart, by the calendar (not 24 h steps; over New Year too). */
    auto day = [](const struct tm &x) {
        return jf::utc_of(std::to_string(x.tm_year + 1900) + "-" + (x.tm_mon < 9 ? "0" : "") + std::to_string(x.tm_mon + 1) +
                          "-" + (x.tm_mday < 10 ? "0" : "") + std::to_string(x.tm_mday)) / 86400;
    };
    const int d = (int)(day(a) - day(b));
    if (d == 0)
        return T("I dag");
    if (d == 1)
        return T("I morgen");
    if (d == -1)
        return T("I går");
    static const char *const days[] = {"Søndag", "Mandag", "Tirsdag", "Onsdag", "Torsdag", "Fredag", "Lørdag"};
    if (d > -7 && d < 7)   /* within the week the guide covers, the day's name says it */
        return T(days[a.tm_wday]);
    return std::string(T(days[a.tm_wday])) + " " + i18n::long_date(a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
}

std::string kind_label(const jf::Item &p)
{
    if (p.is_movie)
        return T("Film");
    if (p.is_sports)
        return T("Sport");
    if (p.is_news)
        return T("Nyheter");
    if (p.is_kids)
        return T("Barn");
    if (p.is_series)
        return T("Serie");
    return std::string();
}

uint32_t kind_color(const jf::Item &p)
{
    if (p.is_movie)
        return 0xffaa5cc3u;   /* the brand's violet */
    if (p.is_sports)
        return 0xff30d158u;
    if (p.is_news)
        return 0xff00a4dcu;   /* the brand's blue */
    if (p.is_kids)
        return 0xffff9f0au;
    return 0;
}

} // namespace livetv
