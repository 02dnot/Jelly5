/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings, opened from the avatar in the top bar: account (switch user,
 * sign out), playback (quality cap, audio and subtitle language, subtitle
 * mode, autoplay, automatic intro skipping), Seerr (on, its address, how it
 * signs in, the session, the network, a connection test), the server, and
 * about. A list in the Apple TV style: value on the right, Left/Right or
 * Cross changes it. Seerr's rows other than "on" show only when it is on.
 */
#pragma once

#include "ui/screen.h"

#include <string>
#include <vector>

namespace ui {

class SettingsScreen : public Screen {
public:
    enum Row {
        SwitchUser, SignOut,
        Quality, AudioLang, SubMode, SubLang, SubSize, SubBackground, Autoplay, AutoSkip, AudioDelay, NightMode, Bitstream,
        ThemeMusic,
        SeerrOn, SeerrUrl, SeerrAuth, SeerrAccount, SeerrTest,
        AppLanguage, Refresh, Updates, Together, ServerInfo, About, RowCount
    };
    explicit SettingsScreen(jf::Client &client) : m_client(client) {}
    ~SettingsScreen() override;   /* the keyboard's callback points here */

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
    void change(Row r, int dir);
    std::string value(Row r) const;
    bool shown(int r) const;
    void seerr_account();            /* Cross on the Seerr account: sign in or out */

    std::string m_seerr_email;       /* a local Seerr account: the e-mail, then the password */
    bool m_want_password = false;
    bool m_signout_armed = false;    /* Seerr's account row: the first ✕ asks, the second signs out */

    jf::Client &m_client;
    int m_row = 0;
    Anim m_scroll;
    Lifts m_lifts;
    Drop m_drop;                        /* the focus */
    bool m_animating = false;
    std::string m_server_name, m_server_version;
};

} // namespace ui
