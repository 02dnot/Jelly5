/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "Hvem ser på?" (concept: .profiles): the saved accounts as avatars, and
 * "Legg til" (another user on that server). The servers are pills above
 * them, "+ Server" last. Triangle twice removes an account from this console.
 */
#pragma once

#include "app/accounts.h"
#include "ui/screen.h"

#include <map>
#include <string>
#include <vector>

namespace ui {

class Profiles : public Screen {
public:
    struct Choice {
        bool add = false;            /* "Legg til": another user on account.server */
        bool add_server = false;     /* "+ Server" */
        accounts::Account account;
    };

    /* (Again after a login was left: the same server and focus.) */
    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return 0.f; }

    bool take_choice(Choice *out);

private:
    struct Server {
        std::string key, label;      /* key: its Id, else its address */
        std::vector<accounts::Account> accounts;
    };
    void select(int server);
    std::vector<std::string> labels() const;

    std::vector<Server> m_servers;   /* in the order first saved */
    int m_server = 0;                /* the one shown */
    std::string m_shown;             /* its key, kept across activate() */
    bool m_in_pills = false;
    int m_pill = 0;                  /* focused pill; m_servers.size() is "+ Server" */
    std::map<std::string, int> m_focus_of;   /* the focus left on each server, by key */
    Drop m_pill_drop, m_add_drop;    /* the servers' bar; "+ Server" */
    Anim m_pill_scroll;              /* the pills, when there are more than fit */
    std::vector<accounts::Account> m_list;   /* the shown server's */
    int m_focus = 0;
    int m_armed = -1;                /* Triangle pressed once on this one */
    bool m_chosen = false;
    Choice m_choice;
    Lifts m_lifts;
    Drop m_drop;                        /* the focus ring */
    Anim m_scroll;                      /* the row, when there are more than fit */
    bool m_animating = false;
};

} // namespace ui
