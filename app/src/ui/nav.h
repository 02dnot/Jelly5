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
#include <vector>

namespace ui {

class Nav {
public:
    /* Settings is the avatar on the right, not a pill tab. Movies, Shows and Music
     * show only when the user has such a library. */
    enum Tab { Home = 0, Movies, Shows, Music, Search, Settings, Count };

    /* The pill tabs, in order (Home ... Search). */
    void set_tabs(std::vector<int> tabs) { m_tabs = std::move(tabs); }
    const std::vector<int> &tabs() const { return m_tabs; }

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
    std::vector<int> m_tabs{Home, Movies, Shows, Search};
    Anim m_focus_x, m_focus_w;          /* targets; the drop follows them on springs */
    float m_dx = 0, m_dvx = 0, m_dw = 0, m_dvw = 0;   /* the drop: position, width and their speeds */
    float m_pop = 0, m_vpop = 0;                       /* its swell: pressed in, then springs back */
    int m_last_focus = -2;
};

const char *tab_label(int tab);

} // namespace ui
