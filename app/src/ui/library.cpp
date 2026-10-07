/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/library.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cctype>
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
constexpr int kSortByName = 1;
/* The decades the filter offers (index 0: all). */
const int kDecades[] = {0, 2020, 2010, 2000, 1990, 1980, 1970, 1960, 1950};
constexpr int kNumDecades = 9;
enum FilterRow { FUnplayed, FFavorites, FGenre, FDecade, FReset, FRowCount };

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

int Library::pill_count() const { return (m_sources.size() > 1 ? (int)m_sources.size() : 0) + 1; }   /* + the sort & filter button */

bool Library::by_name() const { return m_sort == kSortByName && source().types != "MusicArtist"; }

std::string Library::filter_query() const
{
    std::string q, f;
    if (m_filters.unplayed)
        f = "IsUnplayed";
    if (m_filters.favorites)
        f += std::string(f.empty() ? "" : ",") + "IsFavorite";
    if (!f.empty())
        q += "&Filters=" + f;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        if (m_filters.genre > 0 && m_filters.genre <= (int)m_data->genres.size())
            q += "&Genres=" + jf::Client::escape(m_data->genres[m_filters.genre - 1]);
    }
    if (m_filters.decade > 0 && m_filters.decade < kNumDecades) {
        q += "&Years=";
        for (int y = 0; y < 10; y++)
            q += std::to_string(kDecades[m_filters.decade] + y) + (y < 9 ? "," : "");
    }
    return q;
}

int Library::active_filters() const
{
    return (int)m_filters.unplayed + (int)m_filters.favorites + (m_filters.genre > 0) + (m_filters.decade > 0);
}

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

void Library::preload()
{
    size_t have;
    bool need;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        need = m_data->total < 0 && !m_data->loading;
        have = m_data->items.size();
    }
    if (need) {
        load_more();
        return;
    }
    if (m_warmed || have == 0 || square())
        return;
    m_warmed = true;
    std::vector<jf::Item> first;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        first.assign(m_data->items.begin(), m_data->items.begin() + std::min<size_t>(m_data->items.size(), kCols * 3));
    }
    for (const jf::Item &it : first)   /* as draw_poster asks for them */
        art::prefetch(it.external() ? it.ext.poster : poster_url(m_client, it, 480), 480, 720);
}

void Library::reload()
{
    m_warmed = false;   /* a new first page (sort, filter): its posters are asked for again */
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
    const std::string fq = filter_query();
    std::thread([d, c, src, s, start, gen, fq] {
        /* Artists are Jellyfin's album artists, as its own music tab shows them. */
        jf::Page page = src.types == "MusicArtist" ? c->album_artists(src.view, s.by, s.desc, start, kPage)
                                                   : c->library(src.view, src.types, s.by, s.desc, start, kPage, src.filter + fq);
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
    bool more;   /* pages still to come: the last row loaded is not the end */
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        count = (int)m_data->items.size();
        more = m_data->total < 0 ? m_data->loading : count < m_data->total;
    }
    if (m_filter_open) {
        filter_input(p);
        return a;
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
    if ((p & NUVIO_BTN_SQUARE) && !m_menu.active()) {   /* □: sort & filter, from anywhere here */
        open_sheet();
        return a;
    }
    if (m_in_pills) {
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
            m_pill = (p & NUVIO_BTN_LEFT) ? std::max(0, m_pill - 1) : std::min(pill_count() - 1, m_pill + 1);
            m_source_at = m_pill < ns && m_pill != m_source ? m_now + 0.35 : -1;   /* the library follows the focus */
        }
        else if (p & NUVIO_BTN_CROSS) {
            if (m_pill == ns) {   /* the sort & filter button: the sheet, on the current sort */
                open_sheet();
            } else if (m_pill < ns) {
                switch_source(m_pill);
            }
        } else if (p & NUVIO_BTN_DOWN) {
            if (m_source_at >= 0)
                switch_source(m_pill);   /* going down: the library under the focus, now */
            if (count > 0 || m_source_at < 0) {
                m_in_pills = false;
                m_index = 0;             /* the first title, leftmost */
            }
        } else if (p & NUVIO_BTN_CIRCLE) {
            a.kind = m_pushed ? Action::Back : Action::ToNav;
        } else if ((p & NUVIO_BTN_UP) && !m_pushed) {
            a.kind = Action::ToNav;
        }
        return a;
    }
    const int row = m_index / kCols, col = m_index % kCols;
    if ((p & (NUVIO_BTN_L2 | NUVIO_BTN_R2)) && by_name()) {   /* A-Å: to the previous / next letter */
        jump_letter((p & NUVIO_BTN_R2) ? 1 : -1);
        return a;
    }
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
        else if (!more)
            m_bump = true;         /* the library's end (not while its next page comes) */
    } else if (p & NUVIO_BTN_UP) {
        if (row > 0)
            m_index -= kCols;
        else {   /* up to the nearest pills: the left half to the libraries, the right to the sorts */
            m_in_pills = true;
            m_pill = ns > 0 && col < kCols / 2 ? m_source : ns;
        }
    } else if (p & NUVIO_BTN_CIRCLE) {
        /* Back: to the top of the grid, then to the sort row. */
        if (m_index >= kCols)
            m_index = col;
        else {
            m_in_pills = true;
            m_pill = ns > 0 ? m_source : ns;
        }
    } else if (p & NUVIO_BTN_CROSS) {
        std::lock_guard<std::mutex> g(m_data->lock);
        if (m_index < (int)m_data->items.size()) {
            a.kind = Action::Open;   /* the detail page first, as everywhere */
            a.item = m_data->items[m_index];
        }
    }
    if (m_index > count - kPage)   /* a page ahead: the next one is there before the grid ends */
        load_more();
    return a;
}

void Library::enter_from_top()
{
    const int ns = m_sources.size() > 1 ? (int)m_sources.size() : 0;
    m_in_pills = true;   /* the header row is always on the way down */
    m_pill = ns > 0 ? m_source : ns;
    m_source_at = -1;
}

void Library::switch_source(int i)
{
    m_source_at = -1;
    if (i == m_source || i < 0 || i >= (int)m_sources.size())
        return;
    m_source = i;
    m_filters.genre = 0;   /* the genres are the library's own */
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_data->genres.clear();
        m_data->genres_loaded = false;
    }
    reload();
}

void Library::open_sheet()
{
    m_filter_open = true;
    m_filter_row = m_sort;
    m_filter_a.to(1.f);
    std::shared_ptr<Data> d = m_data;
    bool need;
    {
        std::lock_guard<std::mutex> g(d->lock);
        need = !d->genres_loaded;
        d->genres_loaded = true;
    }
    if (need) {
        jf::Client *c = &m_client;
        const Source src = source();
        std::thread([d, c, src] {
            std::vector<std::string> gs = c->genres_in(src.view, src.types);
            std::lock_guard<std::mutex> g(d->lock);
            d->genres = std::move(gs);
        }).detach();
    }
}

/* The filter sheet: Usett and Favoritter toggle, Sjanger and Tiår step with
 * left/right, Nullstill clears. Every change reloads at once. */
void Library::filter_input(uint32_t p)
{
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_OPTIONS)) {
        m_filter_open = false;
        m_filter_a.to(0.f);
        return;
    }
    if (p & NUVIO_BTN_UP) {
        m_filter_row = std::max(0, m_filter_row - 1);
        return;
    }
    if (p & NUVIO_BTN_DOWN) {
        m_filter_row = std::min(kNumSorts + (int)FRowCount - 1, m_filter_row + 1);
        return;
    }
    if (m_filter_row < kNumSorts) {   /* Sorter etter: Cross picks */
        if ((p & NUVIO_BTN_CROSS) && m_filter_row != m_sort) {
            m_sort = m_filter_row;
            reload();
        }
        return;
    }
    int ngenres;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        ngenres = (int)m_data->genres.size();
    }
    const int dir = (p & NUVIO_BTN_LEFT) ? -1 : (p & (NUVIO_BTN_RIGHT | NUVIO_BTN_CROSS)) ? 1 : 0;
    if (!dir)
        return;
    const Filters was = m_filters;
    auto cycle = [dir](int i, int n) { return n > 0 ? ((i + dir) % n + n) % n : 0; };
    switch (m_filter_row - kNumSorts) {
    case FUnplayed: m_filters.unplayed = !m_filters.unplayed; break;
    case FFavorites: m_filters.favorites = !m_filters.favorites; break;
    case FGenre: m_filters.genre = cycle(m_filters.genre, ngenres + 1); break;
    case FDecade: m_filters.decade = cycle(m_filters.decade, kNumDecades); break;
    case FReset:
        if (p & NUVIO_BTN_CROSS)
            m_filters = Filters();
        break;
    }
    if (m_filters.unplayed != was.unplayed || m_filters.favorites != was.favorites || m_filters.genre != was.genre ||
        m_filters.decade != was.decade)
        reload();
}

void Library::draw_filters(float dt)
{
    if (m_filter_a.step(dt, 14.f))
        m_animating = true;
    const float a = m_filter_a.value;
    if (a <= 0.01f)
        return;
    std::vector<std::string> genres;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        genres = m_data->genres;
    }
    /* One sheet: Sorter etter (pick one, a check on it), then Filter. */
    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x99000000u, a));
    const float w = 760, row_h = 62, gap = 4, head = 56;
    const int rows = kNumSorts + FRowCount;
    const float h = 40 + 2 * head + rows * (row_h + gap) + 90;
    const gfx::Rect r{(gfx::W - w) / 2, (gfx::H - h) / 2 + 24 * (1.f - a), w, h};
    glass_panel(r, 28, a);
    auto row_y = [&](int i) { return r.y + 40 + head + i * (row_h + gap) + (i >= kNumSorts ? head : 0); };
    gfx::text(r.x + 60, r.y + 40 + head - 16, T("Sorter etter"), {gfx::Bold, 22}, alpha(kText3, a));
    gfx::text(r.x + 60, row_y(kNumSorts) - 16, T("Filter"), {gfx::Bold, 22}, alpha(kText3, a));
    m_filter_drop.to({r.x + 30, row_y(m_filter_row), w - 60, row_h}, m_filter_row, r.x, r.y);
    m_filter_drop.draw(dt, a, &m_animating, 16);
    for (int i = 0; i < rows; i++) {
        const bool focus = i == m_filter_row;
        const float cy = row_y(i) + row_h / 2 + 9;
        std::string label, value;
        bool check = false;
        if (i < kNumSorts) {
            label = T(kSorts[i].label);
            check = i == m_sort;
        } else {
            switch (i - kNumSorts) {
            case FUnplayed: label = T("Bare usette"); value = m_filters.unplayed ? T("På") : T("Av"); break;
            case FFavorites: label = T("Bare favoritter"); value = m_filters.favorites ? T("På") : T("Av"); break;
            case FGenre:
                label = T("Sjanger");
                value = m_filters.genre > 0 && m_filters.genre <= (int)genres.size() ? genres[m_filters.genre - 1]
                                                                                      : std::string(T("Alle"));
                break;
            case FDecade: {
                label = T("Tiår");
                char b[32];
                std::snprintf(b, sizeof b, T("%d-tallet"), kDecades[m_filters.decade]);
                value = m_filters.decade > 0 ? std::string(b) : std::string(T("Alle"));
                break;
            }
            case FReset: label = T("Nullstill filtre"); break;
            }
        }
        gfx::text(r.x + 60, cy, label, {focus || check ? gfx::Bold : gfx::SemiBold, 25},
                  alpha(focus || check ? kText : kText2, a));
        if (check)
            draw_check(r.x + w - 76, cy - 9, 22, alpha(kText, a));
        if (!value.empty()) {
            const int fr = i - kNumSorts;
            const bool on = value != T("Av") && value != T("Alle");
            gfx::text(r.x + w - 60 - (focus && fr >= FGenre ? 30 : 0), cy, value, {gfx::Medium, 23, 330},
                      alpha(on ? kText : kText3, a), 2);
            if (focus && (fr == FGenre || fr == FDecade))
                gfx::text(r.x + w - 56, cy, "\xE2\x80\xBA", {gfx::Bold, 30}, alpha(kText2, a), 2);
        }
    }
    draw_pad_hints(r.x + 60, r.y + r.h - 42, {{PadButton::Cross, T("Velg")}, {PadButton::Circle, T("Ferdig")}}, 0, 26,
                   a);
}

/* A-Å (sorted by name): L1/R1 to the start of the previous / next letter. Jellyfin
 * counts what sorts before a letter (NameLessThan, case-blind); that count is the
 * first title of the letter. The pages up to it load in one request. */
void Library::jump_letter(int dir)
{
    std::shared_ptr<Data> d = m_data;
    std::string name;
    int total;
    {
        std::lock_guard<std::mutex> g(d->lock);
        if (m_index >= (int)d->items.size() || d->loading)
            return;
        name = d->items[m_index].name;
        total = d->total;
    }
    size_t k = 0;
    while (k < name.size() && !std::isalnum((unsigned char)name[k]))
        k++;   /* past quotes and the like, as SortName does */
    const char c0 = k < name.size() ? (char)std::toupper((unsigned char)name[k]) : '#';
    const int cur = c0 >= 'A' && c0 <= 'Z' ? c0 - 'A' : -1;   /* -1: a digit or other, before A */
    const int at = m_index;
    jf::Client *c = &m_client;
    const Source src = source();
    const Sort s = kSorts[m_sort];
    const std::string fq = src.filter + filter_query();
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(d->lock);
        gen = d->generation;
        d->loading = true;
    }
    std::thread([d, c, src, s, fq, dir, cur, at, total, gen] {
        auto letter = [](int i) { return std::string(1, (char)('A' + i)); };
        int target = -1;
        std::string shown;
        if (dir > 0) {
            for (int i = cur + 1; i < 26 && target < 0; i++) {
                const int n = c->count_before(src.view, src.types, fq, letter(i));
                if (n < 0)
                    break;
                if (n > at && n < total)
                    target = n, shown = letter(i);
            }
        } else {
            const int here = cur >= 0 ? c->count_before(src.view, src.types, fq, letter(cur)) : 0;
            if (here >= 0 && here < at)
                target = here, shown = cur >= 0 ? letter(cur) : "#";
            for (int i = cur - 1; i >= 0 && target < 0; i--) {
                const int n = c->count_before(src.view, src.types, fq, letter(i));
                if (n < 0)
                    break;
                if (n < at)
                    target = n, shown = letter(i);
            }
            if (target < 0 && at > 0)
                target = 0, shown = "#";
        }
        int have;
        {
            std::lock_guard<std::mutex> g(d->lock);
            have = (int)d->items.size();
        }
        jf::Page page;
        if (target >= 0 && have < target + kPage)
            page = c->library(src.view, src.types, s.by, s.desc, have, target + kPage - have, fq);
        std::lock_guard<std::mutex> g(d->lock);
        d->loading = false;
        if (gen != d->generation)
            return;
        d->items.insert(d->items.end(), page.items.begin(), page.items.end());
        if (target >= 0 && target < (int)d->items.size()) {
            d->jump_to = target;
            d->jump_letter = shown;
        }
    }).detach();
}

void Library::draw(double now, float dt)
{
    m_animating = false;
    std::vector<jf::Item> items;
    int total;
    bool loading;
    m_now = now;
    if (m_source_at >= 0) {   /* a library pill rested on long enough: show that library */
        m_animating = true;
        if (now >= m_source_at && m_in_pills)
            switch_source(m_pill);
    }
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        if (m_data->jump_to >= 0) {   /* an A-Å jump landed */
            m_index = m_data->jump_to;
            m_in_pills = false;
            m_letter = m_data->jump_letter;
            m_letter_at = now;
            m_data->jump_to = -1;
        }
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
            std::vector<std::string> labels;
            for (int i = 0; i < ns; i++)
                labels.push_back(m_sources[i].label);
            const bool here = m_focused && m_in_pills && m_pill < ns;
            /* The pane reaches 6 px past the pills on each side, so they start on the title's column;
             * tw: the pills' own width, as the title's. */
            tw = pill_bar(kPad - 6, hy - 50, labels, here ? m_pill : m_source, here, m_src_drop, dt, ha, &m_animating,
                          0, hy) - 2 * 6;
        }
        if (total >= 0) {
            char cnt[32];
            std::snprintf(cnt, sizeof cnt, T("%d titler"), total);
            gfx::text(kPad + tw + 20, hy, cnt, {gfx::Medium, 24}, alpha(kText3, ha));
        }
        /* Sorting and filters behind one round glass button on the right (an icon:
         * tapering lines), what is in force written small beside it. */
        const int nf = active_filters();
        const float bd = 66, bx = gfx::W - kPad - bd, by = hy - 44;
        const gfx::Rect btn{bx, by, bd, bd};
        glass_panel(btn, bd / 2, ha, false);
        const bool here = m_focused && m_in_pills && m_pill == ns;
        if (here)
            m_sort_drop.to(btn, 0, 0, hy);
        else
            m_sort_drop.hide();
        m_sort_drop.draw(dt, ha, &m_animating);
        const float icx = bx + bd / 2, icy = by + bd / 2;
        for (int k = 0; k < 3; k++) {   /* three bars, each shorter: sort & filter */
            const float lw = 30 - k * 9;
            gfx::fill({icx - lw / 2, icy - 10 + k * 9, lw, 3.5f}, alpha(kText, ha), 1.75f);
        }
        if (nf > 0) {   /* how many filters are on */
            const gfx::Rect badge{bx + bd - 22, by - 4, 26, 26};
            gfx::fill(badge, alpha(0xff0a84ffu, ha), 13);
            gfx::text(badge.x + 13, badge.y + 19, std::to_string(nf), {gfx::Bold, 17}, alpha(kText, ha), 1);
        }
        std::string state = T(kSorts[m_sort].label);
        if (nf > 0)
            state += std::string("  \xC2\xB7  ") + (nf == 1 ? T("1 filter") : std::to_string(nf) + T(" filtre"));
        gfx::text(bx - 18, hy, state, {gfx::Medium, 22, 520}, alpha(here ? kText2 : kText3, ha), 2);
        const float sw = gfx::text_width(state, {gfx::Medium, 22, 520});
        if (by_name() && !m_in_pills) {   /* A-Å: the letter jump, shown where it works */
            draw_pad_hints(bx - 18 - sw, hy + 44, {{PadButton::L2, ""}, {PadButton::R2, T("Hopp til bokstav")}}, 0, 22, ha);
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
            const bool f = m_focused && !m_in_pills && i == m_index;
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
        gfx::text(gfx::W / 2, 560, loading || total < 0 ? T("Henter \xE2\x80\xA6")
                  : active_filters() > 0 ? T("Ingen titler passer filteret") : T("Ingenting her ennå"),
                  {gfx::Medium, 30}, kText2, 1);
    m_menu.draw(dt, &m_animating);
    if (art::animating())
        m_animating = true;

    /* After an A-Å jump: the letter, large on glass, for a moment. */
    const double since = now - m_letter_at;
    if (since < 1.1 && !m_letter.empty()) {
        const float la = since < 0.15 ? (float)(since / 0.15) : since > 0.8 ? (float)((1.1 - since) / 0.3) : 1.f;
        const gfx::Rect lr{gfx::W / 2 - 110, gfx::H / 2 - 110, 220, 220};
        glass_panel(lr, 44, la);
        gfx::text(gfx::W / 2, gfx::H / 2 + 46, m_letter, {gfx::Bold, 130}, alpha(kText, la), 1);
        m_animating = true;
    }
    draw_filters(dt);
}

} // namespace ui
