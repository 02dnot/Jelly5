/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Music behind the menus: the now-playing page and the mini player.
 *
 * Music plays on the player's own thread without a picture (headless), so
 * the menus stay up. NowPlaying is the player's music screen drawn by the
 * app as a page: the same PlayerUi, fed the published status, its commands
 * sent to the player through app/remote (as a phone's would be). Circle
 * leaves the page and the music plays on; the mini player at the bottom of
 * the menus shows it, Options opens the page again.
 */
#pragma once

#include "nuvio_now_playing.h"
#include "ui/player_ui.h"
#include "ui/screen.h"

#include <memory>

namespace ui {

class NowPlaying : public Screen {
public:
    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return true; }   /* the clock and the bar move */
    float nav_alpha() const override { return 0.f; }
    float enter() const override { return m_enter.value; }

private:
    bool refresh();                     /* the latest status; false when nothing plays */

    std::unique_ptr<NuvioRequest> m_req;   /* stable while m_ui points at it */
    NuvioStatus m_st;
    unsigned m_track = ~0u;
    PlayerUi m_ui;
    Anim m_enter;
};

/* The mini player over the menus while music plays (and the page is not up). */
void draw_mini_player(double now, float opacity);

} // namespace ui
