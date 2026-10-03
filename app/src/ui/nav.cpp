/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/nav.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "gfx/gfx.h"
#include "ui/screen.h"

#include <ctime>
#include <cstdio>

namespace ui {
namespace {
constexpr float kHintW = 36;   /* the △ beside "Søk" and its gap */
}


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
        widths[i] = gfx::text_width(tab_label(m_tabs[i]), st) + 68 + (m_tabs[i] == Search ? kHintW : 0);
        total += widths[i] + 6;
    }
    const float px = (gfx::W - total) / 2;
    glass_panel({px, cy - 37, total, 74}, 37, a, false);   /* the tab pill: frosted over the page */
    float x = px + 7;
    for (int i = 0; i < n; i++) {
        const gfx::Rect r{x, cy - 30, widths[i], 60};
        if (m_tabs[i] == focus) {
            m_focus_x.to(r.x);
            m_focus_w.to(r.w);
        } else if (m_tabs[i] == active && focus < 0) {
            gfx::fill(r, alpha(0x1fffffffu, a), 30);
        }
        x += widths[i] + 6;
    }
    if (focus >= 0 && focus != Settings) {
        if (m_focus_x.value == 0)
            m_focus_x.snap(m_focus_x.target), m_focus_w.snap(m_focus_w.target);
        const bool moving_x = m_focus_x.step(dt, 16.f);
        const bool moving_w = m_focus_w.step(dt, 16.f);
        if (moving_x || moving_w)
            *animating = true;
        gfx::fill({m_focus_x.value - 4, cy - 33, m_focus_w.value + 8, 66}, alpha(0xfff5f5f7u, a), 33);
    }
    x = px + 7;
    for (int i = 0; i < n; i++) {
        const bool f = m_tabs[i] == focus;
        const uint32_t c = f ? 0xff0b0b0fu : (m_tabs[i] == active ? kText : kText2);
        if (m_tabs[i] == Search && active != Search) {   /* △ goes straight here from elsewhere: show it */
            const float lw = gfx::text_width(tab_label(m_tabs[i]), st), lx = x + (widths[i] - lw - kHintW) / 2;
            gfx::text(lx, cy + 9, tab_label(m_tabs[i]), st, alpha(c, a));
            draw_pad_hint(lx + lw + 12, cy, PadButton::Triangle, "", 24, a);
        } else {
            gfx::text(x + widths[i] / 2, cy + 9, tab_label(m_tabs[i]), st, alpha(c, a), 1);
        }
        x += widths[i] + 6;
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
