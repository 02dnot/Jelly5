/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An album or a playlist (Apple Music on tvOS): the cover on the left over its
 * colours, the title, artist and year; Spill av, Bland, Miks (Jellyfin's
 * Instant Mix) and the artist's page; the track list. Cross on a track plays
 * on from there; Circle goes back to the buttons, then off the page.
 */
#pragma once

#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class Album : public Screen {
public:
    Album(jf::Client &client, const jf::Item &album);

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return 0.f; }
    float enter() const override { return m_enter.value; }

private:
    struct Data {
        std::mutex lock;
        jf::Item album;
        std::vector<jf::Item> tracks;
        bool loaded = false;
    };

    jf::Client &m_client;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    jf::Item m_album;
    std::vector<jf::Item> m_tracks;     /* this frame's copy */
    bool m_loaded = false;

    enum Button { PlayAll, Shuffle, Mix, Artist };
    std::vector<Button> buttons() const;
    Action play_from(size_t i, bool shuffled) const;

    bool m_playlist = false;
    bool m_in_tracks = false;
    int m_button = 0;
    int m_track = 0;
    Anim m_scroll, m_enter, m_content;
    Lifts m_lifts;
    Drop m_btn_drop, m_track_drop;      /* the focus on the buttons and the tracks */
    bool m_animating = false;
};

} // namespace ui
