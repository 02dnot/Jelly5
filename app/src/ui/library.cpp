/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/library.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace ui {
namespace {

constexpr int kCols = 6;
constexpr float kPosterW = 240, kPosterH = 360, kColGap = 48, kRowPitch = 450;
constexpr float kGridTop = 300;
constexpr int kPage = 60;

struct Sort {
    const char *label, *by;
    bool desc;
};
const Sort kSorts[] = {
    {"Nylig lagt til", "DateCreated,SortName", true},
    {"A\xE2\x80\x93\xC3\x85", "SortName", false},
    {"Utgivelses\xC3\xA5r", "PremiereDate,SortName", true},
    {"Vurdering", "CommunityRating,SortName", true},
};
constexpr int kNumSorts = 4;

} // namespace

Library::Library(jf::Client &client, std::string title, std::string types, std::string view_id, bool pushed,
                 std::string filter)
    : m_client(client), m_title(std::move(title)), m_pushed(pushed)
{
    m_sources.push_back({m_title, std::move(view_id), std::move(types), std::move(filter)});
    m_nav.snap(1.f);
}

void Library::set_sources(std::vector<Source> sources)
{
    if (sources.empty())
        return;
    bool same = sources.size() == m_sources.size();
    for (size_t i = 0; same && i < sources.size(); i++)
        same = sources[i].view == m_sources[i].view && sources[i].types == m_sources[i].types &&
               sources[i].label == m_sources[i].label;
    if (same)
        return;
    /* Stay on the chosen library where it is still there. */
    const Source was = source();
    m_sources = std::move(sources);
    m_source = 0;
    for (size_t i = 0; i < m_sources.size(); i++)
        if (m_sources[i].view == was.view && m_sources[i].types == was.types)
            m_source = (int)i;
    bool used;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        used = m_data->total >= 0 || m_data->loading;
    }
    if (used) {   /* on screen before: show the new sources now */
        reload();
    } else {      /* not opened yet: activate() loads it */
        std::lock_guard<std::mutex> g(m_data->lock);
        m_data->generation++;
    }
}

bool Library::square() const
{
    const std::string &t = source().types;
    return t == "MusicAlbum" || t == "MusicArtist" || t == "Playlist";
}

int Library::pill_count() const { return (m_sources.size() > 1 ? (int)m_sources.size() : 0) + kNumSorts; }

std::string Library::types_for(const std::string &collection_type)
{
    if (collection_type == "movies") return "Movie";
    if (collection_type == "tvshows") return "Series";
    if (collection_type == "homevideos") return "Video";
    if (collection_type == "musicvideos") return "MusicVideo";
    if (collection_type == "boxsets") return "BoxSet";
    if (collection_type == "music") return "MusicAlbum";
    if (collection_type == "playlists") return "Playlist";
    return "Movie,Series,Video";   /* mixed */
}

void Library::activate()
{
    m_nav.to(1.f);
    bool need;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        need = m_data->total < 0 && !m_data->loading;
    }
    if (need)
        load_more();
}

void Library::reload()
{
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_data->items.clear();
        m_data->total = -1;
        m_data->loading = false;
        m_data->generation++;
    }
    m_index = 0;
    m_scroll.snap(0);
    load_more();
}

void Library::load_more()
{
    std::shared_ptr<Data> d = m_data;
    int start;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(d->lock);
        if (d->loading || (d->total >= 0 && (int)d->items.size() >= d->total))
            return;
        d->loading = true;
        start = (int)d->items.size();
        gen = d->generation;
    }
    const Sort s = kSorts[m_sort];
    jf::Client *c = &m_client;
    const Source src = source();
    std::thread([d, c, src, s, start, gen] {
        /* Artists are Jellyfin's album artists, as its own music tab shows them. */
        jf::Page page = src.types == "MusicArtist" ? c->album_artists(src.view, s.by, s.desc, start, kPage)
                                                   : c->library(src.view, src.types, s.by, s.desc, start, kPage, src.filter);
        std::lock_guard<std::mutex> g(d->lock);
        if (gen != d->generation)
            return;   /* the sort changed meanwhile */
        d->items.insert(d->items.end(), page.items.begin(), page.items.end());
        d->total = page.items.empty() && start == 0 ? 0 : std::max(page.total, (int)d->items.size());
        d->loading = false;
    }).detach();
}

Action Library::input(uint32_t p)
{
    Action a;
    int count;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        count = (int)m_data->items.size();
    }
    if (m_menu.active()) {
        m_menu.input(p, &a);
        if (a.kind == Action::Changed) {
            std::lock_guard<std::mutex> g(m_data->lock);
            for (jf::Item &it : m_data->items)
                apply_change(it, a.change);
        }
        return a;
    }
    if ((p & NUVIO_BTN_OPTIONS) && !m_in_pills) {
        std::lock_guard<std::mutex> g(m_data->lock);
        if (m_index < (int)m_data->items.size())
            m_menu.open(m_data->items[m_index], false);
        return a;
    }
    /* The pill row: the sources (when there is a choice), then the sorts. */
    const int ns = m_sources.size() > 1 ? (int)m_sources.size() : 0;
    if (m_in_pills) {
        if (p & NUVIO_BTN_LEFT)
            m_pill = std::max(0, m_pill - 1);
        else if (p & NUVIO_BTN_RIGHT)
            m_pill = std::min(pill_count() - 1, m_pill + 1);
        else if (p & NUVIO_BTN_CROSS) {
            if (m_pill < ns) {
                if (m_pill != m_source) {
                    m_source = m_pill;
                    reload();
                }
            } else if (m_pill - ns != m_sort) {
                m_sort = m_pill - ns;
                reload();
            }
        } else if (p & NUVIO_BTN_DOWN) {
            if (count > 0)
                m_in_pills = false;
        } else if (p & NUVIO_BTN_CIRCLE) {
            a.kind = m_pushed ? Action::Back : Action::ToNav;
        } else if ((p & NUVIO_BTN_UP) && !m_pushed) {
            a.kind = Action::ToNav;
        }
        return a;
    }
    const int row = m_index / kCols, col = m_index % kCols;
    if (p & NUVIO_BTN_RIGHT) {
        if (col + 1 < kCols && m_index + 1 < count)
            m_index++;
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_LEFT) {
        if (col > 0)
            m_index--;
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_DOWN) {
        if (m_index + kCols < count)
            m_index += kCols;
        else if (row + 1 <= (count - 1) / kCols)
            m_index = count - 1;   /* last, partial row */
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_UP) {
        if (row > 0)
            m_index -= kCols;
        else {
            m_in_pills = true;
            m_pill = ns > 0 ? m_source : m_sort;   /* to the library chosen, else the sort */
        }
    } else if (p & NUVIO_BTN_CIRCLE) {
        /* Back: to the top of the grid, then to the sort row. */
        if (m_index >= kCols)
            m_index = col;
        else {
            m_in_pills = true;
            m_pill = ns > 0 ? m_source : m_sort;
        }
    } else if (p & NUVIO_BTN_CROSS) {
        std::lock_guard<std::mutex> g(m_data->lock);
        if (m_index < (int)m_data->items.size()) {
            a.kind = Action::Open;   /* the detail page first, as everywhere */
            a.item = m_data->items[m_index];
        }
    }
    if (m_index > count - 24)
        load_more();
    return a;
}

void Library::draw(double now, float dt)
{
    m_animating = false;
    std::vector<jf::Item> items;
    int total;
    bool loading;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        items = m_data->items;
        total = m_data->total;
        loading = m_data->loading;
    }
    if (loading)
        m_animating = true;
    m_index = std::min(m_index, std::max(0, (int)items.size() - 1));
    const jf::Item *focused = (!m_in_pills && m_index < (int)items.size()) ? &items[m_index] : nullptr;
    if (focused)
        m_ambient.set(focused->backdrop_blurhash.empty() ? focused->primary_blurhash : focused->backdrop_blurhash, now);
    else if (!items.empty())
        m_ambient.set(items[0].backdrop_blurhash.empty() ? items[0].primary_blurhash : items[0].backdrop_blurhash, now);
    m_ambient.draw(dt, 0.62f, &m_animating);

    /* Grid scroll. */
    const int row = m_in_pills ? 0 : m_index / kCols;
    const bool sq = square();
    const float pitch = sq ? 360.f : kRowPitch, tile_h = sq ? kPosterW : kPosterH;
    /* The focused row comes up near the top (whole, with the next row peeking below);
     * the first row stays under the header. */
    m_scroll.to(row > 0 ? (float)row * pitch - 60.f : 0.f);
    if (m_scroll.step(dt, 11.f))
        m_animating = true;
    m_nav.to(m_scroll.target < 1.f ? 1.f : 0.f);
    if (m_nav.step(dt, 10.f))
        m_animating = true;

    /* Header: title, count and the sort pills (fade with the nav). */
    const float ha = m_nav.value;
    if (ha > 0.01f) {
        const float hy = 220 - (1.f - ha) * 40;
        const int ns = m_sources.size() > 1 ? (int)m_sources.size() : 0;
        float tw = 0;
        if (ns == 0) {
            tw = gfx::text(kPad, hy, m_title, {gfx::Bold, 64}, alpha(kText, ha));
        } else {   /* the sources as a glass bar where the title would be, the drop on the picked one */
            const gfx::TextStyle ss{gfx::Bold, 30};
            float bw = 12;
            for (int i = 0; i < ns; i++)
                bw += gfx::text_width(m_sources[i].label, ss) + 64 + 6;
            glass_panel({kPad - 6, hy - 50, bw, 76}, 38, ha, false);
            const bool here = m_in_pills && m_pill < ns;
            for (int i = 0; i < ns; i++) {
                const float w = gfx::text_width(m_sources[i].label, ss) + 64;
                if (i == (here ? m_pill : m_source))
                    m_src_drop.to({kPad + tw, hy - 44, w, 64}, i, 0, hy);
                tw += w + 6;
            }
            m_src_drop.draw(dt, ha * (here ? 1.f : 0.55f), &m_animating);
            tw = 0;
            for (int i = 0; i < ns; i++) {
                const float w = gfx::text_width(m_sources[i].label, ss) + 64;
                const bool on = i == (here ? m_pill : m_source);
                gfx::text(kPad + tw + w / 2, hy - 44 + 32 + 10, m_sources[i].label, ss, alpha(on ? kText : kText2, ha), 1);
                tw += w + 6;
            }
            tw -= 6;
        }
        if (total >= 0) {
            char cnt[32];
            std::snprintf(cnt, sizeof cnt, T("%d titler"), total);
            gfx::text(kPad + tw + 20, hy, cnt, {gfx::Medium, 24}, alpha(kText3, ha));
        }
        /* The sorts: a glass bar on the right, the drop on the focused or picked one. */
        const gfx::TextStyle st{gfx::SemiBold, 23};
        float sw[kNumSorts], bw = 12;
        for (int i = 0; i < kNumSorts; i++) {
            sw[i] = gfx::text_width(T(kSorts[i].label), st) + 56;
            bw += sw[i] + 6;
        }
        const float sx0 = gfx::W - kPad - bw + 6;
        glass_panel({sx0 - 6, hy - 44, bw, 66}, 33, ha, false);
        const bool here = m_in_pills && m_pill >= ns;
        const int on = here ? m_pill - ns : m_sort;
        float x = sx0;
        for (int i = 0; i < kNumSorts; i++) {
            if (i == on)
                m_sort_drop.to({x, hy - 38, sw[i], 54}, i, 0, hy);
            x += sw[i] + 6;
        }
        m_sort_drop.draw(dt, ha * (here ? 1.f : 0.55f), &m_animating);
        x = sx0;
        for (int i = 0; i < kNumSorts; i++) {
            gfx::text(x + sw[i] / 2, hy - 38 + 27 + 8, T(kSorts[i].label), i == on ? gfx::TextStyle{gfx::Bold, 23} : st,
                      alpha(i == on ? kText : kText2, ha), 1);
            x += sw[i] + 6;
        }
    }

    /* The grid, clipped below the header. */
    const float top = kGridTop - m_scroll.value;
    gfx::push_scissor({0, (kGridTop - 60) * ha, gfx::W, gfx::H});
    int focus_i = -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (int)items.size(); i++) {
            const int r = i / kCols, c = i % kCols;
            const float y = top + r * pitch;
            if (y > gfx::H + 20 || y + tile_h + 60 < 0)
                continue;
            const bool f = !m_in_pills && i == m_index;
            if (f)
                focus_i = i;
            if ((pass == 0) == f)
                continue;   /* focused poster last, over its neighbours */
            const float lift = m_lifts.step(items[i].id, f, dt, &m_animating);
            const gfx::Rect tile{kPad + c * (kPosterW + kColGap), y, kPosterW, tile_h};
            draw_poster(m_client, items[i], tile, lift, 1.f);
            if (f) {
                const float k = 1.f + 0.1f * lift;
                m_card = {{tile.x - tile.w * (k - 1) / 2, tile.y - tile.h * (k - 1) / 2, tile.w * k, tile.h * k},
                          poster_url(m_client, items[i], 480), items[i].primary_blurhash, 14 * k};
                m_has_card = true;
            }
        }
    }
    gfx::pop_scissor();
    (void)focus_i;

    if (items.empty())
        gfx::text(gfx::W / 2, 560, loading || total < 0 ? T("Henter \xE2\x80\xA6") : T("Ingenting her ennå"),
                  {gfx::Medium, 30}, kText2, 1);
    m_menu.draw(dt, &m_animating);
    if (art::animating())
        m_animating = true;
}

} // namespace ui
