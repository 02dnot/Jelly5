/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Search (concept: .search): an on-screen keyboard on the left, results as
 * posters on the right (titles, then people, albums and episodes), updated as the viewer types (300 ms debounce).
 * Suggestions fill the results while the query is empty. Square deletes.
 *
 * With Seerr on, its films and series follow under their own heading, with
 * where each stands (available, requested, pending ...): titles the library
 * already shows are left out there, and one the server has opens its page.
 */
#pragma once

#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class Search : public Screen {
public:
    explicit Search(jf::Client &client);

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return true; }   /* the caret blinks */
    float nav_alpha() const override { return 1.f; }

private:
    struct Data {
        std::mutex lock;
        std::vector<jf::Item> items;
        std::string for_query;       /* the query these results answer */
        unsigned seq = 0;
        /* Seerr: its results, and those of them shown (the library's left out). */
        std::vector<jf::Item> seerr, seerr_shown;
        std::string for_seerr;       /* the query Seerr's results answer */
        bool seerr_pending = false, seerr_failed = false;
        int seerr_page = 1;          /* Seerr's pages loaded for this query */
        bool seerr_more = true, seerr_more_loading = false;
        double seerr_more_retry_at = 0;  /* after a failed page: not before (m_now) */
    };
    /* The results as laid out: the library's rows, then Seerr's after its heading. */
    struct Grid {
        int library = 0, seerr = 0;  /* how many of each */
        int library_rows() const { return (library + 3) / 4; }
        int rows() const { return library_rows() + (seerr + 3) / 4; }
        int count() const { return library + seerr; }
        int row_of(int i) const;
        int col_of(int i) const;
        int row_length(int row) const;
        int index_at(int row, int col) const;   /* col clamped to the row */
    };
    static void merge(Data &d);      /* Seerr's shown results (with the lock held) */
    Grid grid();
    void type(const std::string &key);
    void start_search();
    void more_seerr();               /* Seerr's next page, when the focus nears the end of its results */

    jf::Client &m_client;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    std::string m_query;
    double m_changed = 0, m_now = 0;
    bool m_pending = false, m_suggested = false;
    std::string m_seerr_retried;        /* the query Seerr was asked again for, once it came up */

    bool m_in_results = false;
    int m_key = 0, m_result = 0;
    Anim m_scroll;
    Lifts m_lifts;
    Drop m_drop;                        /* the focus on the keyboard */
    Ambient m_ambient;
};

} // namespace ui
