/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Options on a title (the PS5's own convention): a small glass sheet of what can be done with it, as
 * Netflix's options on a card. Mer info, Legg til i / Fjern fra Min liste,
 * Merk som sett / usett, and in Fortsett å se: Fjern fra Fortsett å se.
 * The choice comes back as an Action::Changed: the screen updates its copy
 * at once and the app writes it to Jellyfin. A title with several versions
 * also offers Velg versjon: a list of them in the same sheet, and the one
 * chosen plays (Action::Play, item.media_source_id).
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
    /* A page's own menu for a part of it (a season, an episode, the series): no
     * "Mer info" (the page is it). played: offer marking it; ask_seerr: offer
     * "Be om flere sesonger" (take_ask() says it was chosen); versions: the
     * title's, offered when there are several (the list at once when nothing
     * else is offered). Nothing to offer: it stays closed. */
    void open_actions(const jf::Item &item, bool played, bool ask_seerr,
                      const std::vector<jf::Version> &versions = {});
    bool take_ask();
    bool active() const { return m_open; }
    /* 0..1 as it fades: the screen hides its top navigation under it. */
    float visibility() const { return m_alpha.value; }

    /* While active: handles the press. *action becomes Open (Mer info),
     * Changed (with its change) or Play (a version), else stays None. */
    void input(uint32_t pressed, Action *action);
    /* Over the whole screen, dimming it. */
    void draw(float dt, bool *animating);

private:
    enum Option { Info, List, Played, Resume, AskSeerr, Versions };

    void close();

    jf::Item m_item;
    std::vector<Option> m_options;
    std::vector<jf::Version> m_versions;
    bool m_picking = false;             /* the list of versions is up */
    int m_focus = 0;
    bool m_open = false;
    bool m_asked = false;
    Anim m_alpha;
    Drop m_drop;                        /* the focus */
};

} // namespace ui
