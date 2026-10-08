/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A library tab (concept: .library): a poster grid, six across, with pills
 * above and the focused title's colours as a blurred background. Pages of 60
 * load in the background as the viewer nears the end.
 *
 * Its sources are what the pills on the left choose between: the user's
 * libraries of the tab's kind, in the order they set in Jellyfin (Serier ·
 * Anime), or for music Album · Artister · Spillelister. One source: no pills,
 * the tab's title. Sort pills on the right. Opened from Biblioteker it shows
 * one library (pushed over the tab, Circle leaves it).
 */
#pragma once

#include "ui/item_menu.h"
#include "ui/screen.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class Library : public Screen {
public:
    /* view_id empty: all libraries. pushed: opened over a tab (Circle goes back). */
    /* filter: extra query (e.g. "&AlbumArtistIds=<id>": an artist's albums). */
    Library(jf::Client &client, std::string title, std::string types, std::string view_id = std::string(),
            bool pushed = false, std::string filter = std::string());
    /* Before the tab is opened (the home screen is up): its first page, then the
     * first screen of posters, so opening it shows a full grid. Cheap to call again. */
    void preload();
    /* What a library of this collection type lists, e.g. "movies" -> "Movie". */
    static std::string types_for(const std::string &collection_type);
    void set_title(std::string title) { m_title = std::move(title); }   /* the language changed */

    struct Source {
        std::string label;              /* the pill */
        std::string view, types, filter; /* parent library, item types, extra query */
    };
    /* The tab's sources (reloads when they changed); the chosen one is kept by view. */
    void set_sources(std::vector<Source> sources);
    bool has_sources() const { return !m_sources.empty(); }

    /* A change made elsewhere (marked seen, a favourite), shown at once; a title that
     * no longer passes the filters leaves the grid. */
    void apply(const UserDataChange &c);
    /* Watched since it loaded: the loaded titles are asked again, in place, when the
     * grid is next shown (refresh_if_stale, also from activate). */
    void mark_stale() { m_stale = true; }
    void refresh_if_stale();
    /* Around main's write of a change: a refresh waits until it is done. */
    static void write_started();
    static void write_done();

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return m_nav.value * (1.f - m_menu.visibility()) * (1.f - m_filter_a.value); }
    void enter_from_top() override;
    bool modal() const override { return m_filter_open || m_menu.active(); }
    bool focused_card(Card *c) const override
    {
        if (m_has_card && !m_in_pills)
            *c = m_card;
        return m_has_card && !m_in_pills;
    }

private:
    struct Data {
        std::mutex lock;
        std::vector<jf::Item> items;
        int total = -1;
        bool loading = false;
        unsigned generation = 0;    /* bumped on reload: stale pages are dropped */
        std::vector<std::string> genres;   /* this source's, for the filter */
        bool genres_loaded = false;
        unsigned genres_gen = 0;    /* bumped on a source switch: stale genres are dropped */
        int jump_to = -1;           /* an A-Å jump that has landed: the index */
        std::string jump_letter;
        unsigned list_changes = 0;  /* bumped when titles leave the list or it is swapped in place */
        std::map<int, int> letter_counts;   /* the server's count before each letter, while these match: */
        unsigned letters_gen = ~0u, letters_changes = ~0u;
    };
    /* What the viewer narrows the library to (Jellyfin's own filters). */
    struct Filters {
        bool unplayed = false, favorites = false;
        int genre = 0;              /* 0 all, else genres[genre - 1] */
        int decade = 0;             /* 0 all, else kDecades[decade] */
    };
    std::string filter_query() const;
    int active_filters() const;
    void filter_input(uint32_t p);
    void draw_filters(float dt);
    void jump_letter(int dir);
    bool by_name() const;
    void open_sheet();
    void switch_source(int i);
    double m_source_at = -1;            /* a source pill rested on: switch to it then */
    void load_more();
    void reload();
    bool refresh();   /* false: not now (a page on its way, or no thread) */
    const Source &source() const { return m_sources[std::min(m_source, (int)m_sources.size() - 1)]; }
    bool square() const;
    int pill_count() const;

    jf::Client &m_client;
    std::string m_title;
    std::vector<Source> m_sources;
    int m_source = 0;
    bool m_pushed = false;
    Card m_card;
    bool m_has_card = false;
    ItemMenu m_menu;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    bool m_warmed = false;              /* the first screen of posters asked for ahead */
    double m_preload_at = -100;         /* the last first-page request preload made */
    bool m_stale = false;               /* mark_stale: refreshed when next shown */

    int m_sort = 0;
    bool m_in_pills = false;
    int m_pill = 0;
    int m_index = 0;
    Anim m_scroll, m_nav;
    Lifts m_lifts;
    Drop m_src_drop, m_sort_drop;       /* the focus on the sources and the sorts */
    Drop m_filter_drop;                 /* ... and in the filter sheet */
    Filters m_filters;
    bool m_filter_open = false;
    int m_filter_row = 0;
    Anim m_filter_a;
    std::string m_letter;               /* the big letter after an A-Å jump */
    double m_letter_at = -10, m_now = 0;
    Ambient m_ambient;
    bool m_animating = false;
};

} // namespace ui
