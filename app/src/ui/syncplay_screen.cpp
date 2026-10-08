/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/syncplay_screen.h"

#include "app/i18n.h"
#include "app/spawn.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace ui {

/* Rows: in a group, "Forlat gruppe" first; then "Lag ny gruppe"; then the groups. */
void SyncPlayScreen::refresh()
{
    std::shared_ptr<Data> d = m_data;
    {
        std::lock_guard<std::mutex> g(d->lock);
        if (d->busy)
            return;
        d->busy = true;
    }
    m_refreshed = m_now;
    const bool started = jelly5::spawn([d] {
        std::vector<syncplay::Group> groups = syncplay::list();
        std::lock_guard<std::mutex> g(d->lock);
        d->groups = std::move(groups);
        d->busy = false;
        d->loaded = true;
    });
    if (!started) {   /* no thread: the next refresh tries again */
        std::lock_guard<std::mutex> g(d->lock);
        d->busy = false;
    }
}

void SyncPlayScreen::activate()
{
    m_row = 0;
    refresh();
}

void SyncPlayScreen::act(int row)
{
    const bool in_group = syncplay::active();
    std::vector<syncplay::Group> groups;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        groups = m_data->groups;
    }
    std::shared_ptr<Data> d = m_data;
    const std::string user = m_user;
    int i = row;
    if (in_group && i-- == 0) {
        jelly5::spawn([] { syncplay::leave(); });
    } else if (i-- == 0) {
        /* "Karis gruppe"; sized to the name, so a long one is never cut mid-letter */
        std::string group(std::strlen(T("%ss gruppe")) + user.size() + 1, '\0');
        group.resize((size_t)std::max(0, std::snprintf(&group[0], group.size(), T("%ss gruppe"), user.c_str())));
        jelly5::spawn([group] { syncplay::create(group); });
    } else if (i >= 0 && i < (int)groups.size()) {
        const std::string id = groups[i].id;
        jelly5::spawn([id] { syncplay::join(id); });
    }
    m_refreshed = m_now - 9;   /* look again shortly */
}

Action SyncPlayScreen::input(uint32_t p)
{
    Action a;
    int n;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        n = (int)m_data->groups.size();
    }
    n += syncplay::active() ? 2 : 1;
    if (p & NUVIO_BTN_CIRCLE)
        a.kind = Action::Back;
    else if (p & NUVIO_BTN_UP)
        m_row = std::max(0, m_row - 1);
    else if (p & NUVIO_BTN_DOWN)
        m_row = std::min(n - 1, m_row + 1);
    else if (p & NUVIO_BTN_CROSS)
        act(m_row);
    return a;
}

void SyncPlayScreen::draw(double now, float dt)
{
    m_now = now;
    if (now - m_refreshed > 3.0)
        refresh();
    std::vector<syncplay::Group> groups;
    bool loaded;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        groups = m_data->groups;
        loaded = m_data->loaded;
    }
    const bool in_group = syncplay::active();
    const std::string mine = syncplay::group_name();

    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, 500}, 0x33302048u, 0x00000000u);
    const float left = 360, width = gfx::W - 2 * left;
    gfx::text(left, 200, T("Se sammen"), {gfx::Bold, 64}, kText);
    gfx::text(left, 256,
              in_group ? T("Du er med i en gruppe. Det noen i gruppen starter, spilles for alle, i takt.")
                       : T("Se det samme samtidig som andre på denne Jellyfin-serveren, i takt. Den som starter noe, starter det for alle."),
              {gfx::Regular, 24, width, 2, 34}, kText2);

    struct Row {
        std::string label, value;
        bool danger = false;
    };
    std::vector<Row> rows;
    if (in_group)
        rows.push_back({T("Forlat gruppe"), mine, true});
    rows.push_back({T("Lag ny gruppe"), "", false});
    for (const syncplay::Group &g : groups) {
        std::string who;
        for (size_t i = 0; i < g.participants.size() && i < 4; i++)
            who += (i ? ", " : "") + g.participants[i];
        rows.push_back({g.name, who, false});
    }
    m_row = std::min(m_row, (int)rows.size() - 1);
    /* Rows' places: the actions, a gap, then the groups. */
    std::vector<float> ys;
    float y = 360;
    const size_t actions = in_group ? 2u : 1u;
    for (size_t r = 0; r < rows.size(); r++) {
        ys.push_back(y);
        y += 92;
        if (r + 1 == actions && !groups.empty())
            y += 24;   /* a gap before the groups */
    }
    /* More than fit above the hints: the list scrolls with the focus kept mid-way;
     * the cards and text are cut at its ends, never the drop. */
    const float view_top = 340, view_bottom = gfx::H - 130, content = ys.back() + 84 + 8 - view_top;
    bool anim = false;
    const bool clip = content > view_bottom - view_top;
    if (clip) {
        const float c = ys[m_row] + 42 - view_top;
        m_scroll.to(std::max(0.f, std::min(content - (view_bottom - view_top), c - (view_bottom - view_top) / 2)));
        anim = m_scroll.step(dt, 12.f);
    } else {
        m_scroll.snap(0);
    }
    const float sy = m_scroll.value;
    for (float &v : ys)
        v -= sy;
    /* Two glass cards (the actions, the groups), the focus drop, then the text. */
    if (clip)
        gfx::push_scissor({0, view_top, gfx::W, view_bottom - view_top});
    glass_panel({left - 8, ys[0] - 8, width + 16, ys[actions - 1] + 84 - ys[0] + 16}, 24, 1.f, false);
    if (rows.size() > actions)
        glass_panel({left - 8, ys[actions] - 8, width + 16, ys.back() + 84 - ys[actions] + 16}, 24, 1.f, false);
    if (clip)
        gfx::pop_scissor();
    m_drop.to({left, ys[m_row], width, 84}, m_row, 0, -sy);
    m_drop.draw(dt, 1.f, &anim, 16);
    if (clip)
        gfx::push_scissor({0, view_top, gfx::W, view_bottom - view_top});
    for (size_t r = 0; r < rows.size(); r++) {
        const bool focus = (int)r == m_row;
        const gfx::Rect rr{left, ys[r], width, 84};
        const uint32_t fg = rows[r].danger ? 0xffff7a7au : kText;
        gfx::text(rr.x + 32, rr.y + rr.h / 2 + 9, rows[r].label, {focus ? gfx::Bold : gfx::SemiBold, 26, width - 500}, fg);
        if (!rows[r].value.empty())
            gfx::text(rr.x + rr.w - 32, rr.y + rr.h / 2 + 9, rows[r].value, {gfx::Medium, 22, 440},
                      focus ? kText : kText2, 2);
    }
    if (clip)
        gfx::pop_scissor();
    if (loaded && groups.empty())
        gfx::text(left + 8, y + 40, T("Ingen andre grupper akkurat nå."), {gfx::Medium, 22}, kText3);
    draw_pad_hints(left, gfx::H - 78, {{PadButton::Circle, T("Tilbake")}}, 0, 26);
}

} // namespace ui
