/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Search (concept: .search): an on-screen keyboard on the left, results as
 * posters on the right, updated as the viewer types (300 ms debounce).
 * Suggestions fill the results while the query is empty. Square deletes.
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
    };
    void type(const std::string &key);
    void start_search();

    jf::Client &m_client;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    std::string m_query;
    double m_changed = 0, m_now = 0;
    bool m_pending = false, m_suggested = false;

    bool m_in_results = false;
    int m_key = 0, m_result = 0;
    Anim m_scroll;
    Lifts m_lifts;
    Ambient m_ambient;
};

} // namespace ui
