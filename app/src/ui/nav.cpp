/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/nav.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "gfx/gfx.h"
#include "ui/screen.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>

namespace ui {

const char *tab_label(int tab)
{
    const char *const kLabels[] = {T("Hjem"), T("Filmer"), T("Serier"), T("Musikk"), T("S\xC3\xB8k"), T("Innstillinger")};
    return tab >= 0 && tab < Nav::Count ? kLabels[tab] : "";
}

void Nav::draw(float a, int active, int focus, float dt, bool *animating)
{
    if (a <= 0.01f)
        return;
    const float slide = (1.f - a) * -60.f;
    const float cy = 66 + slide;

    /* Wordmark. */
    draw_brand(kPad, cy + 12, 34, a);

    /* The tab pill, centred. */
    const gfx::TextStyle st{gfx::SemiBold, 25};
    const int n = (int)m_tabs.size();
    float widths[Count], total = 14;
    for (int i = 0; i < n; i++) {
        widths[i] = gfx::text_width(tab_label(m_tabs[i]), st) + 68;
        total += widths[i] + 6;
    }
    const float px = (gfx::W - total) / 2;
    /* The drop sits on the focused tab, or on the open one when the bar has no
     * focus (as iOS shows the selection), so a tab change by △ slides it too. */
    const int drop = focus >= 0 ? (focus != Settings ? focus : -1) : (active != Settings ? active : -1);
    glass_panel({px, cy - 37, total, 74}, 37, a, false);   /* the tab pill: frosted over the page */
    float x = px + 7;
    for (int i = 0; i < n; i++) {
        const gfx::Rect r{x, cy - 30, widths[i], 60};
        if (m_tabs[i] == drop) {
            m_focus_x.to(r.x);
            m_focus_w.to(r.w);
        }
        x += widths[i] + 6;
    }
    /* The labels first: the focus drop is a lens over them and magnifies its own. */
    x = px + 7;
    for (int i = 0; i < n; i++) {
        const bool f = m_tabs[i] == focus;
        const uint32_t c = f || m_tabs[i] == active ? kText : kText2;
        gfx::text(x + widths[i] / 2, cy + 9, tab_label(m_tabs[i]), st, alpha(c, a), 1);
        x += widths[i] + 6;
    }
    if (drop >= 0) {
        /* The focus is a drop of liquid glass (iOS 26's tab bar). It moves on an
         * underdamped spring - it overshoots and settles back - stretches along
         * its speed and thins as it does, and swells when it sets off. */
        if (m_last_focus < 0 || m_dw == 0) {   /* appearing: in place, popping in */
            m_dx = m_focus_x.target, m_dw = m_focus_w.target, m_dvx = m_dvw = 0;
            m_pop = -0.35f, m_vpop = 0;
        } else if (drop != m_last_focus) {
            m_vpop += 2.2f;   /* a nudge: it swells as it leaves */
        }
        m_last_focus = drop;
        bool moving = false;
        for (float left = std::min(dt, 0.05f); left > 0; left -= 1.f / 240) {   /* small steps: stable */
            const float h = std::min(left, 1.f / 240);
            const float ax = 260.f * (m_focus_x.target - m_dx) - 19.f * m_dvx;
            const float aw = 260.f * (m_focus_w.target - m_dw) - 19.f * m_dvw;
            const float ap = 300.f * (0.f - m_pop) - 14.f * m_vpop;
            m_dvx += ax * h, m_dx += m_dvx * h;
            m_dvw += aw * h, m_dw += m_dvw * h;
            m_vpop += ap * h, m_pop += m_vpop * h;
        }
        moving = std::fabs(m_focus_x.target - m_dx) > 0.2f || std::fabs(m_dvx) > 2.f ||
                 std::fabs(m_focus_w.target - m_dw) > 0.2f || std::fabs(m_pop) > 0.003f || std::fabs(m_vpop) > 0.05f;
        if (moving)
            *animating = true;
        else
            m_dx = m_focus_x.target, m_dw = m_focus_w.target, m_pop = m_vpop = 0;
        const float speed = std::min(1.f, std::fabs(m_dvx) / 1600.f);
        const float grow = 1.f + 0.10f * m_pop;
        const float w = (m_dw + 8) * grow * (1.f + 0.35f * speed), hh = 66 * grow * (1.f - 0.16f * speed);
        const float cx = m_dx + m_dw / 2;
        glass_panel({cx - w / 2, cy - hh / 2, w, hh}, hh / 2, a, false, 1.f);
        /* The label on the drop, crisp over the lens. */
        float lx = px + 7;
        for (int i = 0; i < n; i++) {
            if (m_tabs[i] == drop)
                gfx::text(lx + widths[i] / 2, cy + 9, tab_label(m_tabs[i]), {gfx::Bold, 25}, alpha(kText, a), 1);
            lx += widths[i] + 6;
        }
    } else {
        m_last_focus = -1;
    }

    /* Clock and the viewer's initial. */
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char clock[8];
    std::snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    /* The avatar is the Settings target: a white ring when focused or open. */
    if (focus == Settings || (focus < 0 && active == Settings))
        gfx::fill({gfx::W - kPad - 65, cy - 35, 70, 70}, alpha(focus == Settings ? 0xfff5f5f7u : 0x66ffffffu, a), 35);
    /* The initial on the brand gradient (as in Hvem ser på?); the picture fades in over it.
     * Jellyfin has no BlurHash for users. */
    const gfx::Rect av{gfx::W - kPad - 60, cy - 30, 60, 60};
    gfx::fill_vgradient(av, alpha(0xffaa5cc3u, a), alpha(0xff00a4dcu, a), 30);
    const std::string initial = m_user.empty() ? "?" : m_user.substr(0, 1);
    gfx::text(gfx::W - kPad - 30, cy + 10, initial, {gfx::Bold, 28}, alpha(kText, a), 1);
    if (!m_avatar.empty()) {
        art::draw(av, m_avatar, "", 440, 440, 30, a, 0);
        if (art::animating() && animating) *animating = true;
    }
    gfx::text(gfx::W - kPad - 84, cy + 9, clock, {gfx::SemiBold, 26}, alpha(kText2, a), 2);
}

} // namespace ui
