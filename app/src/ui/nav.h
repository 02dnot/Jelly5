/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The top navigation (concept: #topnav): wordmark, the tab pill
 * (Hjem · Filmer · Serier · Søk), clock and the viewer's initial.
 */
#pragma once

#include "ui/anim.h"

#include <string>

namespace ui {

class Nav {
public:
    /* Settings is the avatar on the right, not a pill tab. */
    enum Tab { Home = 0, Movies, Shows, Search, Settings, Count };
    static constexpr int kPillTabs = 4;

    void set_user(const std::string &name) { m_user = name; }
    /* opacity: how visible (the screen decides); focus: which tab is focused, -1 none. */
    void draw(float opacity, int active, int focus, float dt, bool *animating);

private:
    std::string m_user;
    Anim m_focus_x, m_focus_w;
};

const char *tab_label(int tab);

} // namespace ui
