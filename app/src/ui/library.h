/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Movies / Series (concept: .library): a poster grid, six across, with sort
 * pills above and the focused title's colours as a blurred background. Pages
 * of 60 load in the background as the viewer nears the end.
 */
#pragma once

#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class Library : public Screen {
public:
    Library(jf::Client &client, std::string title, std::string types);

    void set_view(const std::string &view_id);
    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return m_nav.value; }

private:
    struct Data {
        std::mutex lock;
        std::vector<jf::Item> items;
        int total = -1;
        bool loading = false;
        unsigned generation = 0;    /* bumped on reload: stale pages are dropped */
    };
    void load_more();
    void reload();

    jf::Client &m_client;
    std::string m_title, m_types, m_view;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();

    int m_sort = 0;
    bool m_in_pills = false;
    int m_pill = 0;
    int m_index = 0;
    Anim m_scroll, m_nav;
    Lifts m_lifts;
    Ambient m_ambient;
    bool m_animating = false;
};

} // namespace ui
