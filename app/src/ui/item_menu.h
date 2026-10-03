/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Triangle on a title: a small glass sheet of what can be done with it, as
 * Netflix's options on a card. Mer info, Legg til i / Fjern fra Min liste,
 * Merk som sett / usett, and in Fortsett å se: Fjern fra Fortsett å se.
 * The choice comes back as an Action::Changed: the screen updates its copy
 * at once and the app writes it to Jellyfin.
 */
#pragma once

#include "ui/anim.h"
#include "ui/screen.h"

#include <string>
#include <vector>

namespace ui {

/* Applies a change to a copy of an item. */
void apply_change(jf::Item &it, const UserDataChange &c);

class ItemMenu {
public:
    void open(const jf::Item &item, bool in_resume_row);
    bool active() const { return m_open; }

    /* While active: handles the press. *action becomes Open (Mer info) or
     * Changed (with its change), else stays None. */
    void input(uint32_t pressed, Action *action);
    /* Over the whole screen, dimming it. */
    void draw(float dt, bool *animating);

private:
    enum Option { Info, List, Played, Resume };

    jf::Item m_item;
    std::vector<Option> m_options;
    int m_focus = 0;
    bool m_open = false;
    Anim m_alpha;
};

} // namespace ui
