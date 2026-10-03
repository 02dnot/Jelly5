/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/syncplay_screen.h"

#include "app/i18n.h"
#include "nuvio_input.h"

#include <algorithm>
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
    std::thread([d] {
        std::vector<syncplay::Group> groups = syncplay::list();
        std::lock_guard<std::mutex> g(d->lock);
        d->groups = std::move(groups);
        d->busy = false;
        d->loaded = true;
    }).detach();
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
        std::thread([] { syncplay::leave(); }).detach();
    } else if (i-- == 0) {
        std::thread([user] { syncplay::create(user + (i18n::english() ? "'s group" : "s gruppe")); }).detach();
    } else if (i >= 0 && i < (int)groups.size()) {
        const std::string id = groups[i].id;
        std::thread([id] { syncplay::join(id); }).detach();
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
    float y = 360;
    bool anim = false;
    for (size_t r = 0; r < rows.size(); r++) {
        const bool focus = (int)r == m_row;
        const float lift = m_lifts.step("sp" + std::to_string(r) + rows[r].label, focus, dt, &anim);
        const float k = 1.f + 0.02f * lift;
        const gfx::Rect rr{left - width * (k - 1) / 2, y - 84 * (k - 1) / 2, width * k, 84 * k};
        if (lift > 0.01f)
            gfx::shadow(rr, 16, 24, 0.5f * lift, 10 * lift);
        gfx::fill(rr, focus ? 0xfff5f5f7u : 0x0fffffffu, 16);
        const uint32_t fg = focus ? 0xff0b0b0fu : rows[r].danger ? 0xffff7a7au : kText;
        gfx::text(rr.x + 32, rr.y + rr.h / 2 + 9, rows[r].label, {gfx::SemiBold, 26, width - 500}, fg);
        if (!rows[r].value.empty())
            gfx::text(rr.x + rr.w - 32, rr.y + rr.h / 2 + 9, rows[r].value, {gfx::Medium, 22, 440},
                      focus ? 0xb30b0b0fu : kText2, 2);
        y += 92;
        if (r == (in_group ? 1u : 0u) && !groups.empty())
            y += 24;   /* a gap before the groups */
    }
    if (loaded && groups.empty())
        gfx::text(left + 8, y + 40, T("Ingen andre grupper akkurat nå."), {gfx::Medium, 22}, kText3);
    draw_pad_hints(left, gfx::H - 78, {{PadButton::Circle, T("Tilbake")}}, 0, 26);
}

} // namespace ui
