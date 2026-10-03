/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/profiles.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>

namespace ui {

void Profiles::activate()
{
    m_list = accounts::load();
    m_focus = std::min(m_focus, (int)m_list.size());
    m_armed = -1;
}

bool Profiles::take_choice(Choice *out)
{
    if (!m_chosen)
        return false;
    m_chosen = false;
    *out = m_choice;
    return true;
}

Action Profiles::input(uint32_t p)
{
    const int n = (int)m_list.size() + 1;   /* + "Legg til" */
    if (p & NUVIO_BTN_LEFT)
        m_focus = std::max(0, m_focus - 1), m_armed = -1;
    else if (p & NUVIO_BTN_RIGHT)
        m_focus = std::min(n - 1, m_focus + 1), m_armed = -1;
    else if (p & NUVIO_BTN_CROSS) {
        m_choice = Choice();
        if (m_focus < (int)m_list.size())
            m_choice.account = m_list[m_focus];
        else
            m_choice.add = true;
        m_chosen = true;
    } else if ((p & NUVIO_BTN_TRIANGLE) && m_focus < (int)m_list.size()) {
        if (m_armed == m_focus) {
            accounts::forget(m_list[m_focus].server, m_list[m_focus].user_id);
            activate();
        } else {
            m_armed = m_focus;
        }
    }
    return Action();
}

void Profiles::draw(double, float dt)
{
    m_animating = false;
    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, gfx::H}, 0x40302048u, 0x00000000u);
    gfx::text(gfx::W / 2, 330, T("Hvem ser p\xC3\xA5?"), {gfx::Bold, 64}, kText, 1);

    const int n = (int)m_list.size() + 1;
    const float d = 220, gap = 64;
    float x = (gfx::W - (n * d + (n - 1) * gap)) / 2;
    /* The focus: the liquid glass drop as a ring round the picture, sliding between them. */
    const float fx = x + m_focus * (d + gap), ring = d * 1.12f + 20;
    m_drop.to({fx + d / 2 - ring / 2, 440 + d / 2 - ring / 2, ring, ring}, m_focus);
    m_drop.draw(dt, 1.f, &m_animating);
    for (int i = 0; i < n; i++, x += d + gap) {
        const bool focus = i == m_focus;
        const float lift = m_lifts.step(std::to_string(i), focus, dt, &m_animating);
        const float k = 1.f + 0.12f * lift, dd = d * k;
        const gfx::Rect r{x + d / 2 - dd / 2, 440 + d / 2 - dd / 2, dd, dd};
        if (lift > 0.01f)
            gfx::shadow(r, dd / 2, 30, 0.7f * lift, 18 * lift);
        std::string name, sub;
        if (i < (int)m_list.size()) {
            const accounts::Account &a = m_list[i];
            name = a.user_name;
            sub = a.server_name;
            gfx::fill_vgradient(r, 0xffaa5cc3u, 0xff00a4dcu, dd / 2);
            gfx::text(r.x + dd / 2, r.y + dd / 2 + 32, name.substr(0, 1), {gfx::Bold, 90}, kText, 1);
            if (!a.image_tag.empty())
                art::draw(r, a.server + "/Users/" + a.user_id + "/Images/Primary?tag=" + a.image_tag + "&fillWidth=440",
                          "", 440, 440, dd / 2, 1.f, 0);   /* over the initial until it loads */
        } else {
            name = T("Legg til");
            glass_panel(r, dd / 2, 1.f, false);
            gfx::fill({r.x + dd / 2 - 3, r.y + dd / 2 - 36, 6, 72}, kText2, 3);
            gfx::fill({r.x + dd / 2 - 36, r.y + dd / 2 - 3, 72, 6}, kText2, 3);
        }
        gfx::text(x + d / 2, 440 + d + 64, name, {gfx::SemiBold, 30, d + 40}, focus ? kText : kText2, 1);
        if (!sub.empty())
            gfx::text(x + d / 2, 440 + d + 100, sub, {gfx::Medium, 20, d + 40}, kText3, 1);
    }
    const bool armed = m_armed >= 0 && m_armed == m_focus;
    gfx::text(gfx::W / 2, 1000,
              armed ? T("Trykk \xE2\x96\xB3 igjen for \xC3\xA5 fjerne kontoen fra denne PS5-en") : "",
              {gfx::Medium, 22}, armed ? 0xffff6b6bu : kText3, 1);
    if (!armed)
        draw_pad_hints(gfx::W / 2, 992, {{PadButton::Cross, T("Velg")}, {PadButton::Triangle, T("Fjern konto")}}, 1);
    if (art::animating())
        m_animating = true;
}

} // namespace ui
