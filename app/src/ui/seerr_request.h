/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Asking Seerr for a title, from its page: a glass sheet over it. A film is
 * a confirmation; a series lists its seasons to pick (every missing one, or
 * one by one when Seerr allows it). With Seerr's permission for it (advanced
 * requests), the Radarr/Sonarr server, quality profile and folder can be
 * chosen, Seerr's defaults picked to start with; left as they are, Seerr
 * decides (its override rules included). The sheet says whether the request
 * needs an administrator's approval and what is left of a quota, and when it
 * is sent, how that went: on success it closes and the page says so.
 */
#pragma once

#include "seerr/seerr_client.h"
#include "ui/anim.h"
#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class RequestSheet {
public:
    void open(const seerr::Detail &d, const seerr::User &user, const seerr::PublicSettings &ps);
    bool active() const { return m_open; }
    /* While active: handles every press. */
    void input(uint32_t pressed);
    /* Over the whole screen, dimming it. */
    void draw(float dt, bool *animating);
    /* A request went through: how, once (then false again). */
    bool take_done(seerr::RequestResult *out);

    /* Seasons the viewer may ask for: none of it there or asked for already. */
    static bool requestable(const seerr::Season &s, const seerr::PublicSettings &ps);

private:
    enum Kind { AllSeasons, OneSeason, ServerRow, ProfileRow, FolderRow, Buttons };
    struct Row {
        Kind kind;
        int season = -1;                    /* OneSeason: index in m_detail.seasons */
    };
    struct Shared {
        std::mutex lock;
        bool loaded = false;                /* servers (and their profiles, folders) and the quota */
        std::vector<seerr::Server> servers; /* this kind's, not 4K, with their details */
        seerr::Quota quota;
        bool have_quota = false;
        bool sending = false, sent = false;
        seerr::RequestResult result;
    };
    void build_rows();
    void send();
    std::string message(const seerr::RequestResult &r) const;
    const seerr::Server *server() const;

    seerr::Detail m_detail;
    seerr::User m_user;
    seerr::PublicSettings m_settings;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::vector<seerr::Server> m_servers;   /* this frame's copy */
    std::vector<Row> m_rows;
    std::vector<bool> m_picked;             /* per season */
    int m_server = 0, m_profile = 0, m_folder = 0;
    bool m_defaults_set = false, m_touched = false;   /* the advanced choices were changed */
    int m_focus = 0, m_button = 0;          /* row; on the buttons: 0 send, 1 cancel */
    std::string m_error;                    /* how the last try went, when it did not */
    bool m_open = false, m_done = false;
    seerr::RequestResult m_result;
    Anim m_alpha, m_scroll;
    Drop m_drop;
};

} // namespace ui
