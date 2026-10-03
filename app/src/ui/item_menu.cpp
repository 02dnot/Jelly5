/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/item_menu.h"
#include "app/i18n.h"

#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>

namespace ui {

void apply_change(jf::Item &it, const UserDataChange &c)
{
    if (it.id != c.id)
        return;
    if (c.favorite_set)
        it.favorite = c.favorite;
    if (c.played_set) {
        it.played = c.played;
        it.position_ticks = 0;
        it.played_percent = 0;
    }
    if (c.resume_cleared) {
        it.position_ticks = 0;
        it.played_percent = 0;
    }
}

void ItemMenu::open(const jf::Item &item, bool in_resume_row)
{
    m_item = item;
    m_options.clear();
    m_options.push_back(Info);
    if (item.type == "Movie" || item.type == "Series" || item.type == "BoxSet" || item.type == "Episode" ||
        item.type == "Video")
        m_options.push_back(List);
    if (item.type != "BoxSet")
        m_options.push_back(Played);
    if (in_resume_row && item.position_ticks > 0)
        m_options.push_back(Resume);
    m_focus = 0;
    m_open = true;
    m_alpha.to(1.f);
}

void ItemMenu::input(uint32_t p, Action *action)
{
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_TRIANGLE)) {
        m_open = false;
        m_alpha.to(0.f);
        return;
    }
    if (p & NUVIO_BTN_UP)
        m_focus = std::max(0, m_focus - 1);
    else if (p & NUVIO_BTN_DOWN)
        m_focus = std::min((int)m_options.size() - 1, m_focus + 1);
    if (!(p & NUVIO_BTN_CROSS))
        return;

    m_open = false;
    m_alpha.to(0.f);
    action->item = m_item;
    if (m_options[m_focus] == Info) {
        action->kind = Action::Open;
        return;
    }
    UserDataChange &c = action->change;
    c.id = m_item.id;
    switch (m_options[m_focus]) {
    case List: c.favorite_set = true; c.favorite = !m_item.favorite; break;
    case Played: c.played_set = true; c.played = !m_item.played; break;
    case Resume: c.resume_cleared = true; break;
    default: break;
    }
    action->kind = Action::Changed;
}

void ItemMenu::draw(float dt, bool *animating)
{
    if (m_alpha.step(dt, 14.f) && animating)
        *animating = true;
    const float a = m_alpha.value;
    if (a <= 0.01f)
        return;
    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x99000000u, a));

    const bool ep = m_item.type == "Episode";
    const std::string title = ep ? m_item.series_name : m_item.name;
    std::string sub;
    if (ep) {
        char b[256];
        std::snprintf(b, sizeof b, "S%d:E%d \xC2\xB7 %s", m_item.parent_index, m_item.index, m_item.name.c_str());
        sub = b;
    }
    const float row_h = 68, w = 640;
    const float h = 60 + 52 + (sub.empty() ? 0 : 34) + 24 + m_options.size() * (row_h + 6) + 40;
    const float rise = 24 * (1.f - a);
    const gfx::Rect r{(gfx::W - w) / 2, (gfx::H - h) / 2 + rise, w, h};
    gfx::shadow(r, 28, 46, 0.7f * a, 18);
    gfx::fill(r, alpha(0xdc1c1c22u, a), 28);
    gfx::fill({r.x, r.y, r.w, 1.5f}, alpha(0x24ffffffu, a));

    float y = r.y + 60 + 30;
    gfx::text(r.x + 48, y, title, {gfx::Bold, 34, w - 96}, alpha(kText, a));
    if (!sub.empty()) {
        y += 38;
        gfx::text(r.x + 48, y, sub, {gfx::Medium, 22, w - 96}, alpha(kText3, a));
    }
    y += 40;
    for (size_t i = 0; i < m_options.size(); i++) {
        const bool focus = (int)i == m_focus;
        const gfx::Rect row{r.x + 30, y, w - 60, row_h};
        if (focus) {
            gfx::shadow(row, 14, 16, 0.4f * a, 6);
            gfx::fill(row, alpha(0xfff5f5f7u, a), 14);
        }
        const char *label = "";
        switch (m_options[i]) {
        case Info: label = T("Mer info"); break;
        case List: label = m_item.favorite ? T("Fjern fra Min liste") : T("Legg til i Min liste"); break;
        case Played: label = m_item.played ? T("Merk som usett") : T("Merk som sett"); break;
        case Resume: label = T("Fjern fra Fortsett \xC3\xA5 se"); break;
        }
        gfx::text(row.x + 26, row.y + 44, label, {gfx::SemiBold, 26}, alpha(focus ? 0xff0b0b0fu : kText2, a));
        y += row_h + 6;
    }
}

} // namespace ui
