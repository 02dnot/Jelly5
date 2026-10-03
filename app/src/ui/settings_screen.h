/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings, opened from the avatar in the top bar: account (switch user,
 * sign out), playback (quality cap, audio and subtitle language, subtitle
 * mode, autoplay, automatic intro skipping), the server, and about.
 * A list in the Apple TV style: value on the right, Left/Right or Cross
 * changes it.
 */
#pragma once

#include "ui/screen.h"

#include <string>
#include <vector>

namespace ui {

class SettingsScreen : public Screen {
public:
    explicit SettingsScreen(jf::Client &client) : m_client(client) {}

    void set_server_info(const std::string &name, const std::string &version)
    {
        m_server_name = name;
        m_server_version = version;
    }
    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return m_scroll.value < 1.f ? 1.f : 0.f; }

private:
    enum Row {
        SwitchUser, SignOut,
        Quality, AudioLang, SubMode, SubLang, Autoplay, AutoSkip,
        ServerInfo, About, RowCount
    };
    void change(Row r, int dir);
    std::string value(Row r) const;

    jf::Client &m_client;
    int m_row = 0;
    Anim m_scroll;
    Lifts m_lifts;
    bool m_animating = false;
    std::string m_server_name, m_server_version;
};

} // namespace ui
