/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Se sammen (SyncPlay): the groups on the server to join, a new group, or
 * leaving the one this PS5 is in. Opened from Innstillinger. Once in a group,
 * whatever anyone in it starts plays for everyone.
 */
#pragma once

#include "app/syncplay.h"
#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class SyncPlayScreen : public Screen {
public:
    explicit SyncPlayScreen(std::string user_name) : m_user(std::move(user_name)) {}

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return true; }   /* the list refreshes */
    float nav_alpha() const override { return 0.f; }

private:
    struct Data {
        std::mutex lock;
        std::vector<syncplay::Group> groups;
        bool busy = false, loaded = false;
    };
    void refresh();
    void act(int row);

    std::string m_user;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    int m_row = 0;
    double m_now = 0, m_refreshed = -10;
    Lifts m_lifts;
    Drop m_drop;                        /* the focus */
    Anim m_scroll;                      /* the list, when there are more groups than fit */
};

} // namespace ui
