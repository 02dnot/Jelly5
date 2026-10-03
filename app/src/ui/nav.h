/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The top navigation (concept: #topnav): wordmark, the tab pill
 * (Hjem · Filmer · Serier · Søk), clock and the viewer's avatar.
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

    /* avatar: the user's Primary image URL, empty when they have none. */
    void set_user(const std::string &name, const std::string &avatar)
    {
        m_user = name;
        m_avatar = avatar;
    }
    /* opacity: how visible (the screen decides); focus: which tab is focused, -1 none. */
    void draw(float opacity, int active, int focus, float dt, bool *animating);

private:
    std::string m_user, m_avatar;
    Anim m_focus_x, m_focus_w;
};

const char *tab_label(int tab);

} // namespace ui
