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

void ItemMenu::open_actions(const jf::Item &item, bool played, bool ask_seerr)
{
    m_item = item;
    m_options.clear();
    if (played)
        m_options.push_back(Played);
    if (ask_seerr)
        m_options.push_back(AskSeerr);
    if (m_options.empty())
        return;
    m_focus = 0;
    m_asked = false;
    m_open = true;
    m_alpha.to(1.f);
}

bool ItemMenu::take_ask()
{
    const bool asked = m_asked;
    m_asked = false;
    return asked;
}

void ItemMenu::input(uint32_t p, Action *action)
{
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_OPTIONS)) {
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
    if (m_options[m_focus] == AskSeerr) {   /* the page opens its request sheet */
        m_asked = true;
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
    std::vector<const char *> labels;
    for (const Option o : m_options) {
        const char *label = "";
        switch (o) {
        case Info: label = T("Mer info"); break;
        case List: label = m_item.favorite ? T("Fjern fra Min liste") : T("Legg til i Min liste"); break;
        case Played:
            if (m_item.type == "Series")
                label = m_item.played ? T("Merk hele serien som usett") : T("Merk hele serien som sett");
            else if (m_item.type == "Season")
                label = m_item.played ? T("Merk sesongen som usett") : T("Merk sesongen som sett");
            else
                label = m_item.played ? T("Merk som usett") : T("Merk som sett");
            break;
        case Resume: label = T("Fjern fra Fortsett \xC3\xA5 se"); break;
        case AskSeerr: label = T("Be om flere sesonger"); break;
        }
        labels.push_back(label);
    }
    /* 640 wide, wider when a label needs it (in bold, as when focused). */
    float w = 640;
    for (const char *l : labels)
        w = std::max(w, gfx::text_width(l, {gfx::Bold, 26}) + 112);
    w = std::min(w, 1200.f);
    const float row_h = 68;
    const float h = 60 + 52 + (sub.empty() ? 0 : 34) + 24 + m_options.size() * (row_h + 6) + 40 + 52;
    const float rise = 24 * (1.f - a);
    const gfx::Rect r{(gfx::W - w) / 2, (gfx::H - h) / 2 + rise, w, h};
    glass_panel(r, 28, a);

    float y = r.y + 60 + 30;
    gfx::text(r.x + 48, y, title, {gfx::Bold, 34, w - 96}, alpha(kText, a));
    if (!sub.empty()) {
        y += 38;
        gfx::text(r.x + 48, y, sub, {gfx::Medium, 22, w - 96}, alpha(kText3, a));
    }
    y += 40;
    m_drop.to({r.x + 30, y + m_focus * (row_h + 6), w - 60, row_h}, m_focus, r.x, r.y);
    m_drop.draw(dt, a, animating, 14);
    for (size_t i = 0; i < m_options.size(); i++) {
        const bool focus = (int)i == m_focus;
        const gfx::Rect row{r.x + 30, y, w - 60, row_h};
        gfx::text(row.x + 26, row.y + 44, labels[i], {focus ? gfx::Bold : gfx::SemiBold, 26, w - 112},
                  alpha(focus ? kText : kText2, a));
        y += row_h + 6;
    }
    draw_pad_hints(r.x + 56, r.y + r.h - 52, {{PadButton::Cross, T("Velg")}, {PadButton::Circle, T("Lukk")}}, 0, 26,
                   a);
}

} // namespace ui
