/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sizes and timings follow concept/style.css (.detail, .dtop, .pills,
 * .card.ep, .card.cast).
 */
#include "ui/detail.h"

#include "gfx/art.h"
#include "nuvio_input.h"
#include "ui/item_menu.h"

#include <algorithm>
#include <cstdio>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ui {
namespace {

constexpr float kTopH = 880;                    /* the info block before the sections */
constexpr float kEpW = 480, kEpH = 270, kEpGap = 32;
constexpr float kCastD = 170, kCastGap = 32;
constexpr float kSimW = 400, kSimH = 225, kSimGap = 32;
constexpr float kSeasonsH = 84, kEpisodesH = 470, kCastH = 330, kSimilarH = 340;

std::string runtime_label(int64_t ticks)
{
    const int min = (int)(ticks / jf::kTicksPerSecond / 60);
    if (min <= 0)
        return std::string();
    char b[32];
    if (min >= 60)
        std::snprintf(b, sizeof b, "%d t %d min", min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return b;
}

std::string ep_code(const jf::Item &e)
{
    char b[32];
    std::snprintf(b, sizeof b, "S%d:E%d", e.parent_index, e.index);
    return b;
}

/* 4K/HD, HDR10/HLG/Dolby Vision, Atmos/7.1/5.1, CC, from the media streams. */
std::vector<std::string> badges(const jf::Detail &d)
{
    std::vector<std::string> out;
    const jf::MediaStream *video = nullptr, *audio = nullptr;
    bool subs = false;
    for (const auto &s : d.streams) {
        if (s.type == "Video" && !video)
            video = &s;
        if (s.type == "Audio" && (!audio || s.is_default))
            audio = &s;
        if (s.type == "Subtitle")
            subs = true;
    }
    if (video) {
        if (video->width >= 3200)
            out.push_back("4K");
        else if (video->width >= 1200)
            out.push_back("HD");
        if (video->video_range_type.find("DOVI") != std::string::npos)
            out.push_back("Dolby Vision");
        else if (video->video_range == "HDR")
            out.push_back(video->video_range_type.find("HLG") != std::string::npos ? "HLG" : "HDR10");
    }
    if (audio) {
        if ((audio->display_title + audio->profile).find("Atmos") != std::string::npos)
            out.push_back("Atmos");
        else if (audio->channels >= 8)
            out.push_back("7.1");
        else if (audio->channels >= 6)
            out.push_back("5.1");
    }
    if (subs)
        out.push_back("CC");
    return out;
}

std::vector<jf::Person> cast_of(const jf::Detail &d)
{
    std::vector<jf::Person> out;
    std::set<std::string> seen;
    for (const auto &p : d.people)
        if ((p.type == "Actor" || p.type == "Director" || p.type == "GuestStar") && seen.insert(p.id).second &&
            out.size() < 24)
            out.push_back(p);
    return out;
}

/* Horizontal row scroll: the focused card at the left edge until the row ends. */
float row_target(int index, int count, float card_w, float gap)
{
    const float max_scroll = std::max(0.f, count * (card_w + gap) - gap - (gfx::W - 2 * kPad));
    return std::min(max_scroll, index * (card_w + gap));
}

} // namespace

/* Page data by item id: what was fetched (or prefetched) last, newest first. */
namespace {
std::mutex s_cache_lock;
std::list<std::string> s_cache_order;
std::map<std::string, std::shared_ptr<void>> s_cache;     /* holds Detail::Content */
std::set<std::string> s_inflight;

/* Per user: what one account has watched or liked never shows on another's page. */
std::string cache_key(const jf::Client &c, const std::string &id) { return c.user_id() + "/" + id; }
}

/* Everything the page shows, the requests in parallel. */
Detail::Content Detail::fetch(jf::Client &c, const jf::Item &base)
{
    Content out;
    out.item = base;
    jf::Detail detail;
    jf::Item item = base;
    bool got = false;
    std::vector<jf::Item> similar, seasons, resume, next, all_episodes;
    const bool series = base.type == "Series", boxset = base.type == "BoxSet";
    std::vector<std::thread> jobs;
    jobs.emplace_back([&] { got = c.item(base.id, &item, &detail); });
    if (boxset)   /* a collection's own titles take the place of "more like this" */
        jobs.emplace_back([&] { similar = c.children(base.id, "PremiereDate,ProductionYear,SortName", 200); });
    else
        jobs.emplace_back([&] { similar = c.similar(base.id, 16); });
    if (series) {
        jobs.emplace_back([&] { seasons = c.seasons(base.id); });
        jobs.emplace_back([&] { resume = c.resume(1, base.id); });
        jobs.emplace_back([&] { next = c.next_up(1, base.id); });
        jobs.emplace_back([&] { all_episodes = c.episodes(base.id, std::string()); });
    }
    for (auto &j : jobs)
        j.join();
    if (got && item.local_trailers > 0) {
        const std::vector<jf::Item> trailers = c.local_trailers(base.id);
        if (!trailers.empty()) {
            out.trailer = trailers.front();
            out.have_trailer = true;
        }
    }
    if (got) {
        out.item = item;
        out.detail = std::move(detail);
        out.have_detail = true;
    }
    out.similar = std::move(similar);
    out.seasons = std::move(seasons);
    out.all_episodes = std::move(all_episodes);
    if (boxset) {
        /* Play: the first title not yet watched, else the first. */
        for (const jf::Item &t : out.similar)
            if (!t.played) {
                out.target = t;
                out.have_target = true;
                break;
            }
        if (!out.have_target && !out.similar.empty()) {
            out.target = out.similar.front();
            out.have_target = true;
        }
    } else if (!series) {
        out.target = out.item;
        out.have_target = true;
    } else {
        /* Play continues a started episode, else the next one, else the first. */
        std::vector<jf::Item> &t = !resume.empty() ? resume : next;
        if (t.empty() && !out.all_episodes.empty())
            t.push_back(out.all_episodes.front());
        if (!t.empty()) {
            out.target = t.front();
            out.have_target = true;
        }
    }
    return out;
}

static void cache_put(const std::string &key, const Detail::Content &content);

void Detail::prefetch(jf::Client &client, const jf::Item &item)
{
    if (item.id.empty() || (item.type != "Movie" && item.type != "Series" && item.type != "BoxSet"))
        return;
    {
        std::lock_guard<std::mutex> g(s_cache_lock);
        const std::string key = cache_key(client, item.id);
        if (s_cache.count(key) || s_inflight.count(key) || s_inflight.size() >= 2)
            return;
        s_inflight.insert(key);
    }
    jf::Client *c = &client;
    const jf::Item base = item;
    std::thread([c, base] {
        Content content = fetch(*c, base);
        const std::string key = cache_key(*c, base.id);
        cache_put(key, content);
        std::lock_guard<std::mutex> g(s_cache_lock);
        s_inflight.erase(key);
    }).detach();
}

static void cache_put(const std::string &key, const Detail::Content &content)
{
    std::lock_guard<std::mutex> g(s_cache_lock);
    s_cache[key] = std::make_shared<Detail::Content>(content);
    s_cache_order.remove(key);
    s_cache_order.push_front(key);
    while (s_cache_order.size() > 40) {
        s_cache.erase(s_cache_order.back());
        s_cache_order.pop_back();
    }
}

Detail::Detail(jf::Client &client, const jf::Item &item) : m_client(client)
{
    m_data->c.item = item;
    {
        std::lock_guard<std::mutex> g(s_cache_lock);
        auto it = s_cache.find(cache_key(client, item.id));
        if (it != s_cache.end())
            m_data->c = *std::static_pointer_cast<Content>(it->second);   /* instant; refreshed below */
    }
    m_view = m_data->c;
}

void Detail::activate()
{
    std::shared_ptr<Data> d = m_data;
    jf::Client *c = &m_client;
    const jf::Item base = m_view.item;
    std::thread([d, c, base] {
        Content fresh = fetch(*c, base);
        cache_put(cache_key(*c, base.id), fresh);
        std::lock_guard<std::mutex> g(d->lock);
        d->c = std::move(fresh);
    }).detach();
}

void Detail::select_episodes()
{
    m_eps.clear();
    if (m_view.seasons.empty())
        return;
    m_season = std::max(0, std::min(m_season, (int)m_view.seasons.size() - 1));
    const jf::Item &season = m_view.seasons[m_season];
    for (const jf::Item &e : m_view.all_episodes)
        if (e.season_id == season.id || (e.season_id.empty() && e.parent_index == season.index))
            m_eps.push_back(e);
}

/* The picker moved: its season's episodes show at once, on the episode to
 * continue when it is in that season, else the first. */
void Detail::season_moved()
{
    select_episodes();
    m_episode = 0;
    for (size_t i = 0; i < m_eps.size(); i++)
        if (m_view.have_target && m_eps[i].id == m_view.target.id)
            m_episode = (int)i;
    m_scroll[Episodes].snap(row_target(m_episode, (int)m_eps.size(), kEpW, kEpGap));
}

std::vector<Detail::Zone> Detail::zones() const
{
    std::vector<Zone> z{Buttons};
    if (!m_view.seasons.empty()) {
        z.push_back(Seasons);
        if (!m_eps.empty())
            z.push_back(Episodes);
    }
    if (!cast_of(m_view.detail).empty())
        z.push_back(Cast);
    if (!m_view.similar.empty())
        z.push_back(Similar);
    return z;
}

std::vector<Detail::Button> Detail::buttons() const
{
    std::vector<Button> b;
    if (m_view.have_target)
        b.push_back(PlayButton);
    if (m_view.have_target && m_view.target.position_ticks > 0)
        b.push_back(RestartButton);
    if (m_view.have_trailer)
        b.push_back(TrailerButton);
    if (m_view.item.type != "BoxSet")
        b.push_back(WatchedButton);
    b.push_back(FavouriteButton);
    return b;
}

float Detail::zone_top(Zone z) const
{
    float y = kTopH;
    for (Zone k : zones()) {
        if (k == Buttons)
            continue;
        if (k == z)
            return y;
        y += k == Seasons ? kSeasonsH : k == Episodes ? kEpisodesH : k == Cast ? kCastH : kSimilarH;
    }
    return y;
}

/* A change shown at once on this page (and in its cache). whole: the page's own
 * title, so a series marks all its episodes. */
void Detail::apply_local(const UserDataChange &ch, bool whole)
{
    std::lock_guard<std::mutex> g(m_data->lock);
    Content &c = m_data->c;
    apply_change(c.item, ch);
    apply_change(c.target, ch);
    for (jf::Item &e : c.all_episodes) {
        apply_change(e, ch);
        if (whole && ch.played_set && c.item.type == "Series") {
            e.played = ch.played;
            e.position_ticks = 0;
            e.played_percent = 0;
        }
    }
    for (jf::Item &t : c.similar)
        apply_change(t, ch);
    m_view = c;
    select_episodes();
}

Action Detail::input(uint32_t p)
{
    Action a;
    const std::vector<Zone> zs = zones();
    const auto zi = std::find(zs.begin(), zs.end(), m_zone) - zs.begin();
    const int nb = (int)buttons().size();
    if (p & NUVIO_BTN_DOWN) {
        if (zi + 1 < (long)zs.size())
            m_zone = zs[zi + 1];
    } else if (p & NUVIO_BTN_UP) {
        if (zi > 0)
            m_zone = zs[zi - 1];
    } else if (p & NUVIO_BTN_CIRCLE) {
        if (m_zone != Buttons)
            m_zone = Buttons;
        else
            a.kind = Action::Back;
    } else if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
        const int d = (p & NUVIO_BTN_RIGHT) ? 1 : -1;
        auto move = [d](int &i, int n) { i = std::max(0, std::min(n - 1, i + d)); };
        switch (m_zone) {
        case Buttons: move(m_button, nb); break;
        case Seasons: {
            const int before = m_season;
            move(m_season, (int)m_view.seasons.size());
            m_season_picked = true;
            if (m_season != before)
                season_moved();
            break;
        }
        case Episodes:
            move(m_episode, (int)m_eps.size());
            m_season_picked = true;
            break;
        case Cast: move(m_cast, (int)cast_of(m_view.detail).size()); break;
        case Similar: move(m_similar, (int)m_view.similar.size()); break;
        default: break;
        }
    } else if ((p & NUVIO_BTN_TRIANGLE) && m_zone == Episodes && m_episode < (int)m_eps.size()) {
        const jf::Item &e = m_eps[m_episode];
        a.change.id = e.id;
        a.change.played_set = true;
        a.change.played = !e.played;
        a.kind = Action::Changed;
        a.item = e;
        apply_local(a.change, false);
    } else if (p & NUVIO_BTN_CROSS) {
        switch (m_zone) {
        case Buttons: {
            const std::vector<Button> b = buttons();
            const Button btn = b[std::min(m_button, nb - 1)];
            if (btn == FavouriteButton || btn == WatchedButton) {
                /* Shown here at once; the app writes it and refreshes the home rows. */
                UserDataChange &ch = a.change;
                ch.id = m_view.item.id;
                if (btn == FavouriteButton) {
                    ch.favorite_set = true;
                    ch.favorite = !m_view.item.favorite;
                } else {
                    ch.played_set = true;
                    ch.played = !m_view.item.played;
                }
                a.kind = Action::Changed;
                a.item = m_view.item;
                apply_local(ch, true);
            } else if (btn == TrailerButton) {
                a.kind = Action::PlayFromStart;
                a.item = m_view.trailer;
            } else if (m_view.have_target) {
                a.kind = btn == RestartButton ? Action::PlayFromStart : Action::Play;
                a.item = m_view.target;
            }
            break;
        }
        case Episodes:
            if (m_episode < (int)m_eps.size()) {
                a.kind = Action::Play;
                a.item = m_eps[m_episode];
            }
            break;
        case Cast: {
            const std::vector<jf::Person> cast = cast_of(m_view.detail);
            if (m_cast < (int)cast.size()) {
                const jf::Person &p = cast[m_cast];
                a.kind = Action::Open;
                a.item.id = p.id;
                a.item.name = p.name;
                a.item.type = "Person";
                a.item.primary_tag = p.image_tag;
                a.item.primary_blurhash = p.blurhash;
            }
            break;
        }
        case Similar:
            if (m_similar < (int)m_view.similar.size()) {
                a.kind = Action::Open;
                a.item = m_view.similar[m_similar];
            }
            break;
        default:
            break;
        }
    }
    return a;
}

void Detail::draw_top(float y0, float dt)
{
    const jf::Item &it = m_view.item;
    const bool series = it.type == "Series";

    /* Logo (fades in; nothing until then) or the title. */
    const std::string logo = m_client.image_url(it.logo_owner, "Logo", it.logo_tag, 900);
    const float title_bottom = y0 + 380;
    if (!logo.empty()) {
        if (const gfx::Texture *t = art::get(logo, 900, 300)) {
            const float iw = (float)gfx::texture_width(t), ih = (float)gfx::texture_height(t);
            const float k = std::min(800.f / iw, 210.f / ih);
            gfx::image({kPad, title_bottom - ih * k, iw * k, ih * k}, t, art::fade(logo), 0, false);
        }
    } else {
        gfx::text(kPad, title_bottom - 20, it.name, {gfx::Bold, 84, 1500}, kText);
    }

    /* Meta: rating, year, runtime or seasons, genres, age rating, tech badges. */
    const float my = y0 + 440;
    float x = kPad;
    const gfx::TextStyle meta{gfx::Medium, 24};
    bool first = true;
    auto sep = [&] {
        if (!first) {
            gfx::fill({x + 12, my - 10, 5, 5}, kText3, 2.5f);
            x += 29;
        }
        first = false;
    };
    if (it.community_rating > 0) {
        char r[16];
        std::snprintf(r, sizeof r, "\xE2\x98\x85 %.1f", it.community_rating);
        sep();
        x += gfx::text(x, my, r, meta, 0xfff5c518u);
    }
    if (it.year) {
        sep();
        x += gfx::text(x, my, std::to_string(it.year), meta, kText2);
    }
    if (series && !m_view.seasons.empty()) {
        sep();
        const size_t n = m_view.seasons.size();
        x += gfx::text(x, my, std::to_string(n) + (n == 1 ? " sesong" : " sesonger"), meta, kText2);
    } else if (!series && it.runtime_ticks > 0) {
        sep();
        x += gfx::text(x, my, runtime_label(it.runtime_ticks), meta, kText2);
    }
    if (!it.genres.empty()) {
        sep();
        std::string g = it.genres[0];
        for (size_t i = 1; i < it.genres.size() && i < 3; i++)
            g += " \xC2\xB7 " + it.genres[i];
        x += gfx::text(x, my, g, meta, kText2);
    }
    std::vector<std::string> tags;
    if (!it.official_rating.empty())
        tags.push_back(it.official_rating);
    for (const auto &b : badges(m_view.detail))
        tags.push_back(b);
    x += 16;
    for (const auto &tag : tags) {
        const gfx::TextStyle bs{gfx::Bold, 17};
        const float bw = gfx::text_width(tag, bs) + 18;
        const bool solid = tag == "4K" || tag == "Dolby Vision";
        gfx::fill({x, my - 22, bw, 30}, solid ? 0xe6ffffffu : 0x73ffffffu, 6);
        if (!solid)
            gfx::fill({x + 1.5f, my - 20.5f, bw - 3, 27}, 0xd90d0d12u, 5);
        gfx::text(x + 9, my - 1, tag, bs, solid ? 0xff000000u : kText);
        x += bw + 10;
    }

    float y = my + 52;
    if (!m_view.detail.tagline.empty()) {
        gfx::text(kPad, y, m_view.detail.tagline, {gfx::Medium, 26, 900}, 0xe6f5f5f7u);
        y += 44;
    }
    gfx::text(kPad, y, it.overview, {gfx::Regular, 26, 860, 3, 37.7f}, kText2);

    /* Buttons. */
    const float by = y0 + 668;
    float bx = kPad;
    const std::vector<Button> bs = buttons();
    for (size_t i = 0; i < bs.size(); i++) {
        const bool focus = m_zone == Buttons && (int)i == std::min(m_button, (int)bs.size() - 1);
        const float lift = m_lifts.step("btn" + std::to_string((int)bs[i]), focus, dt, &m_animating);
        const gfx::TextStyle st{gfx::Bold, 26};
        std::string label, sub;
        float pct = -1;
        if (bs[i] == PlayButton) {
            const jf::Item &t = m_view.target;
            const bool resume = m_view.have_target && t.position_ticks > 0 && t.runtime_ticks > 0;
            label = resume ? "Fortsett" : "Spill av";
            if (m_view.have_target && t.type == "Episode")
                label += "  " + ep_code(t);
            if (resume) {
                pct = (float)t.position_ticks / (float)t.runtime_ticks;
                const int left = (int)((t.runtime_ticks - t.position_ticks) / jf::kTicksPerSecond / 60);
                sub = std::to_string(std::max(1, left)) + " min igjen";
            }
        }
        float w = 76;
        if (bs[i] == WatchedButton)
            w = gfx::text_width(m_view.item.played ? "Sett" : "Merk som sett", st) + 64 + 34;
        if (bs[i] == RestartButton)
            w = gfx::text_width("Fra start", st) + 64;
        if (bs[i] == TrailerButton)
            w = gfx::text_width("Trailer", st) + 64;
        if (bs[i] == PlayButton)
            w = 40 + 30 + 14 + gfx::text_width(label, st) + (pct >= 0 ? 14 + 90 + 14 + gfx::text_width(sub, {gfx::Medium, 24}) : 0) + 40;
        const float k = 1.f + 0.08f * lift;
        const gfx::Rect r{bx - w * (k - 1) / 2, by - 76 * (k - 1) / 2, w * k, 76 * k};
        if (lift > 0.01f)
            gfx::shadow(r, 16, 24, 0.55f * lift, 14 * lift);
        gfx::fill(r, focus ? 0xfff5f5f7u : 0x24ffffffu, 16 * k);
        const uint32_t fg = focus ? 0xff0b0b0fu : kText;
        const float cy = r.y + r.h / 2;
        if (bs[i] == PlayButton) {
            for (int s = 0; s < 14; s++)   /* play glyph */
                gfx::fill({r.x + 40 * k + s * 1.5f, cy - (14 - s), 1.5f, (14 - s) * 2.f}, fg);
            float tx = r.x + (40 + 30 + 14) * k;
            tx += gfx::text(tx, cy + 9, label, st, fg);
            if (pct >= 0) {
                tx += 14;
                gfx::fill({tx, cy - 3, 90, 6}, focus ? 0x2e000000u : 0x40ffffffu, 3);
                gfx::fill({tx, cy - 3, 90 * pct, 6}, fg, 3);
                gfx::text(tx + 104, cy + 8, sub, {gfx::Medium, 24}, alpha(fg, 0.75f));
            }
        } else if (bs[i] == RestartButton) {
            gfx::text(r.x + r.w / 2, cy + 9, "Fra start", st, fg, 1);
        } else if (bs[i] == TrailerButton) {
            gfx::text(r.x + r.w / 2, cy + 9, "Trailer", st, fg, 1);
        } else if (bs[i] == WatchedButton) {
            /* A check from small squares along its two strokes. */
            const bool seen = m_view.item.played;
            const uint32_t cc = seen ? (focus ? 0xff0a8f3cu : 0xff30d158u) : alpha(fg, 0.85f);
            const float gx = r.x + 30 * k, gy = cy + 2;
            for (int s = 0; s < 6; s++)
                gfx::fill({gx + s * 1.6f, gy - 4 + s * 1.6f, 3.2f, 3.2f}, cc, 1.f);
            for (int s = 0; s < 11; s++)
                gfx::fill({gx + 8 + s * 1.6f, gy + 4 - s * 1.8f, 3.2f, 3.2f}, cc, 1.f);
            gfx::text(r.x + 30 * k + 34, cy + 9, seen ? "Sett" : "Merk som sett", st, fg);
        } else {
            /* A heart from two discs and a stack of shrinking bars (no glyph needed). */
            const bool fav = m_view.item.favorite;
            const uint32_t hc = fav ? 0xffff5a7au : alpha(fg, 0.85f);
            const float hx = r.x + r.w / 2, hy = cy - 4;
            gfx::fill({hx - 13, hy - 9, 15, 15}, hc, 7.5f);
            gfx::fill({hx - 2, hy - 9, 15, 15}, hc, 7.5f);
            for (int s = 0; s < 12; s++)
                gfx::fill({hx - 13 + s * 1.1f, hy + s * 1.15f, 26 - s * 2.2f, 1.6f}, hc);
        }
        bx += w + 20;
    }

    /* Credits. */
    std::string with, dir;
    int actors = 0;
    for (const auto &p : m_view.detail.people) {
        if (p.type == "Actor" && actors < 4)
            with += (actors++ ? ", " : "") + p.name;
        if (p.type == "Director" && dir.size() < 80)
            dir += (dir.empty() ? "" : ", ") + p.name;
    }
    float cy = by + 76 + 46;
    const gfx::TextStyle cs{gfx::Regular, 21, 1100};
    auto credit = [&](const char *head, const std::string &v) {
        if (v.empty())
            return;
        const float hw = gfx::text(kPad, cy, head, {gfx::SemiBold, 21}, kText2);
        gfx::text(kPad + hw + 8, cy, v, cs, kText3);
        cy += 32;
    };
    credit("Med:", with);
    credit("Regi:", dir);
    if (!m_view.detail.studios.empty())
        credit(series ? "Kanal:" : "Studio:", m_view.detail.studios[0]);
}

void Detail::draw_sections(float dt)
{
    const float off = m_page.value;
    const auto vis = [](float y, float h) { return y < gfx::H + 20 && y + h > -40; };

    /* Seasons: pills; the picked one is loaded after 250 ms of rest. */
    if (!m_view.seasons.empty()) {
        const float y = zone_top(Seasons) - off;
        if (vis(y, kSeasonsH)) {
            float x = kPad;
            for (size_t i = 0; i < m_view.seasons.size(); i++) {
                const jf::Item &s = m_view.seasons[i];
                const gfx::TextStyle st{gfx::SemiBold, 23};
                const float w = gfx::text_width(s.name, st) + 56;
                const bool focus = m_zone == Seasons && (int)i == m_season;
                const bool active = (int)i == m_season;
                const float k = focus ? 1.08f : 1.f;
                const gfx::Rect r{x - w * (k - 1) / 2, y - 54 * (k - 1) / 2, w * k, 54 * k};
                gfx::fill(r, focus ? 0xfff5f5f7u : active ? 0x33ffffffu : 0x14ffffffu, r.h / 2);
                gfx::text(r.x + r.w / 2, r.y + r.h / 2 + 8, s.name, st,
                          focus ? 0xff0b0b0fu : active ? kText : kText2, 1);
                x += w + 12;
            }
        }
    }

    /* Episodes: stills with title, runtime and overview. */
    if (!m_eps.empty()) {
        const float y = zone_top(Episodes) - off;
        m_scroll[Episodes].to(row_target(m_episode, (int)m_eps.size(), kEpW, kEpGap));
        if (m_scroll[Episodes].step(dt, 12.f))
            m_animating = true;
        if (vis(y, kEpisodesH)) {
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < m_eps.size(); i++) {
                    const jf::Item &e = m_eps[i];
                    const bool focus = m_zone == Episodes && (int)i == m_episode;
                    if ((pass == 0) == focus)
                        continue;
                    const float x = kPad + i * (kEpW + kEpGap) - m_scroll[Episodes].value;
                    if (x > gfx::W || x + kEpW < -40)
                        continue;
                    const float lift = m_lifts.step("ep" + e.id, focus, dt, &m_animating);
                    const float k = 1.f + 0.1f * lift;
                    const gfx::Rect r{x - kEpW * (k - 1) / 2, y - kEpH * (k - 1) / 2, kEpW * k, kEpH * k};
                    if (lift > 0.01f)
                        gfx::shadow(r, 14 * k, 30, 0.75f * lift, 22 * lift);
                    art::draw(r, landscape_url(m_client, e, 640), e.primary_blurhash, 640, 360, 14 * k);
                    if (e.played_percent > 0 && e.played_percent < 100) {
                        gfx::fill({r.x + 18, r.y + r.h - 22, r.w - 36, 6}, 0x47ffffffu, 3);
                        gfx::fill({r.x + 18, r.y + r.h - 22, (r.w - 36) * (float)(e.played_percent / 100), 6},
                                  0xffffffffu, 3);
                    }
                    if (e.played) {
                        gfx::fill({r.x + r.w - 76, r.y + 14, 62, 30}, 0xa6000000u, 15);
                        gfx::text(r.x + r.w - 45, r.y + 36, "Sett", {gfx::SemiBold, 18}, kText, 1);
                    }
                    const float ty = y + kEpH + 44;
                    char title[300];
                    std::snprintf(title, sizeof title, "%d. %s", e.index, e.name.c_str());
                    gfx::text(x, ty, title, {gfx::SemiBold, 22, kEpW}, focus ? kText : kText2);
                    gfx::text(x, ty + 30, runtime_label(e.runtime_ticks), {gfx::Medium, 19}, kText3);
                    gfx::text(x, ty + 62, e.overview, {gfx::Regular, 19, kEpW, 3, 27}, kText3);
                }
        }
    }

    /* Cast and crew: round portraits. */
    const std::vector<jf::Person> cast = cast_of(m_view.detail);
    if (!cast.empty()) {
        const float y = zone_top(Cast) - off;
        m_scroll[Cast].to(row_target(m_cast, (int)cast.size(), kCastD, kCastGap));
        if (m_scroll[Cast].step(dt, 12.f))
            m_animating = true;
        if (vis(y, kCastH)) {
            gfx::text(kPad, y + 30, "Skuespillere og crew", {gfx::Bold, 30}, 0xebffffffu);
            for (size_t i = 0; i < cast.size(); i++) {
                const jf::Person &p = cast[i];
                const float x = kPad + i * (kCastD + kCastGap) - m_scroll[Cast].value;
                if (x > gfx::W || x + kCastD < -40)
                    continue;
                const bool focus = m_zone == Cast && (int)i == m_cast;
                const float lift = m_lifts.step("cast" + p.id, focus, dt, &m_animating);
                const float k = 1.f + 0.1f * lift;
                const float d = kCastD * k;
                const gfx::Rect r{x - (d - kCastD) / 2, y + 56 - (d - kCastD) / 2, d, d};
                if (lift > 0.01f)
                    gfx::shadow(r, d / 2, 24, 0.6f * lift, 16 * lift);
                const std::string url =
                    p.image_tag.empty() ? std::string() : m_client.image_url(p.id, "Primary", p.image_tag, 340);
                if (url.empty()) {
                    gfx::fill(r, 0xff1a1a20u, d / 2);
                    gfx::text(r.x + d / 2, r.y + d / 2 + 16, p.name.substr(0, 1), {gfx::Bold, 46}, kText3, 1);
                } else {
                    art::draw(r, url, p.blurhash, 340, 340, d / 2);
                }
                gfx::text(x + kCastD / 2, y + 56 + kCastD + 40, p.name, {gfx::SemiBold, 19, kCastD + 20},
                          focus ? kText : kText2, 1);
                gfx::text(x + kCastD / 2, y + 56 + kCastD + 66, p.role.empty() ? p.type : p.role,
                          {gfx::Medium, 17, kCastD + 20}, kText3, 1);
            }
        }
    }

    /* More like this: landscape cards that open their own page. */
    if (!m_view.similar.empty()) {
        const float y = zone_top(Similar) - off;
        m_scroll[Similar].to(row_target(m_similar, (int)m_view.similar.size(), kSimW, kSimGap));
        if (m_scroll[Similar].step(dt, 12.f))
            m_animating = true;
        if (vis(y, kSimilarH)) {
            gfx::text(kPad, y + 30, m_view.item.type == "BoxSet" ? "I denne samlingen" : "Mer som dette",
                      {gfx::Bold, 30}, 0xebffffffu);
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < m_view.similar.size(); i++) {
                    const jf::Item &s = m_view.similar[i];
                    const bool focus = m_zone == Similar && (int)i == m_similar;
                    if ((pass == 0) == focus)
                        continue;
                    const float x = kPad + i * (kSimW + kSimGap) - m_scroll[Similar].value;
                    if (x > gfx::W || x + kSimW < -40)
                        continue;
                    const float lift = m_lifts.step("sim" + s.id, focus, dt, &m_animating);
                    const float k = 1.f + 0.1f * lift;
                    const gfx::Rect r{x - kSimW * (k - 1) / 2, y + 56 - kSimH * (k - 1) / 2, kSimW * k, kSimH * k};
                    if (lift > 0.01f)
                        gfx::shadow(r, 14 * k, 30, 0.75f * lift, 22 * lift);
                    art::draw(r, landscape_url(m_client, s, 640), landscape_blurhash(s), 640, 360, 14 * k);
                    if (lift > 0.01f)
                        gfx::text(r.x, r.y + r.h + 36, s.name, {gfx::SemiBold, 22, r.w}, alpha(kText, lift));
                }
        }
    }
}

void Detail::draw(double now, float dt)
{
    m_now = now;
    m_animating = false;
    if (m_opened < 0)
        m_opened = now;
    m_enter.to(1.f);
    if (m_enter.step(dt, 14.f))
        m_animating = true;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_view = m_data->c;
    }
    const jf::Item &it = m_view.item;

    /* Seasons: until the viewer moves the picker, follow the season and
     * episode to continue; episodes come from the page's own data. */
    if (!m_view.seasons.empty()) {
        if (!m_season_picked && m_view.have_target && m_view.target.id != m_followed &&
            !m_view.all_episodes.empty()) {
            /* Once per target (and again after a playback changes it). */
            for (size_t i = 0; i < m_view.seasons.size(); i++)
                if (m_view.seasons[i].id == m_view.target.season_id)
                    m_season = (int)i;
            season_moved();
            m_followed = m_view.target.id;
        } else {
            select_episodes();
        }
        m_episode = std::min(m_episode, std::max(0, (int)m_eps.size() - 1));
    }
    const std::vector<Zone> zs = zones();
    if (std::find(zs.begin(), zs.end(), m_zone) == zs.end())
        m_zone = Buttons;

    /* Page scroll: a section rises to y=160 when focused; the backdrop dims. */
    m_page.to(m_zone == Buttons ? 0.f : zone_top(m_zone) - 160);
    if (m_page.step(dt, 10.f))
        m_animating = true;

    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    art::draw(full, m_client.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920), it.backdrop_blurhash,
              1920, 1080, 0, 1.f, kBg);
    gfx::fill_hgradient({0, 0, 576, gfx::H}, alpha(kBg, 0.92f), alpha(kBg, 0.72f));
    gfx::fill_hgradient({576, 0, 538, gfx::H}, alpha(kBg, 0.72f), alpha(kBg, 0.2f));
    gfx::fill_hgradient({1114, 0, 326, gfx::H}, alpha(kBg, 0.2f), alpha(kBg, 0.f));
    gfx::fill_vgradient({0, 486, gfx::W, 356}, alpha(kBg, 0.f), alpha(kBg, 0.85f));
    gfx::fill_vgradient({0, 842, gfx::W, 238}, alpha(kBg, 0.85f), kBg);
    gfx::fill(full, alpha(kBg, std::min(0.55f, m_page.value / 700.f)));

    /* The text waits for the details (and the logo) so nothing is swapped in
     * front of the viewer; at most 0.6 s, then it fades in as one. */
    const std::string logo = m_client.image_url(it.logo_owner, "Logo", it.logo_tag, 900);
    const bool logo_ready = logo.empty() || art::get(logo, 900, 300);
    if ((m_view.have_detail && logo_ready) || now - m_opened > 0.6)
        m_content.to(1.f);
    if (m_content.step(dt, 12.f) || m_content.target < 1.f)
        m_animating = true;
    gfx::push_opacity(m_content.value);
    draw_top(-m_page.value, dt);
    draw_sections(dt);
    gfx::pop_opacity();
    if (art::animating())
        m_animating = true;
}

} // namespace ui
