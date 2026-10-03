/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The screensaver (Apple TV's, with your library): after a while without a
 * button in the menus, the library's backdrops fill the screen one after the
 * other, each slowly zooming and drifting, crossfading every 12 s, with the
 * title and the clock small in the corners. Any button wakes the app (and is
 * only that: nothing starts by accident).
 */
#pragma once

#include "jf/jf_client.h"

#include <string>
#include <vector>

namespace ui {

class Screensaver {
public:
    struct Slide {
        std::string url, blurhash, title, line;
    };
    /* slides: what to show (shuffled here). */
    void start(std::vector<Slide> slides, double now);
    void stop() { m_on = false; }
    bool on() const { return m_on; }
    void draw(double now);

private:
    std::vector<Slide> m_slides;
    size_t m_index = 0;
    double m_shown = 0, m_started = 0;
    bool m_on = false;
};

} // namespace ui
