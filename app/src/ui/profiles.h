/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "Hvem ser på?" (concept: .profiles): the saved accounts as avatars, and
 * "Legg til". Triangle twice removes an account from this console.
 */
#pragma once

#include "app/accounts.h"
#include "ui/screen.h"

#include <string>
#include <vector>

namespace ui {

class Profiles : public Screen {
public:
    struct Choice {
        bool add = false;            /* "Legg til" */
        accounts::Account account;
    };

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return 0.f; }

    bool take_choice(Choice *out);

private:
    std::vector<accounts::Account> m_list;
    int m_focus = 0;
    int m_armed = -1;                /* Triangle pressed once on this one */
    bool m_chosen = false;
    Choice m_choice;
    Lifts m_lifts;
    Drop m_drop;                        /* the focus ring */
    bool m_animating = false;
};

} // namespace ui
