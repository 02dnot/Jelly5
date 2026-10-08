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
    const char *const kLabels[] = {T("Hjem"),   T("Filmer"),       T("Serier"), T("Musikk"), T("Direkte-TV"),
                                   T("Oppdag"), T("S\xC3\xB8k"), T("Innstillinger")};
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
    const int drop = focus >= 0 ? focus : active;
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
    /* The focus: the liquid glass drop (screen.h), on the focused tab - or on the
     * avatar for Settings - and its label crisp on top. */
    if (drop >= 0) {
        gfx::Rect dr{gfx::W - kPad - 65, cy - 35, 70, 70};   /* the avatar */
        if (drop != Settings)
            dr = {m_focus_x.target - 4, cy - 33, m_focus_w.target + 8, 66};
        m_drop.to(dr, drop, 0, cy);
    } else {
        m_drop.hide();
    }
    m_drop.draw(dt, a * (focus >= 0 ? 1.f : 0.35f), animating);   /* faint where it only marks the open tab */
    float lx = px + 7;
    for (int i = 0; i < n; i++) {
        if (m_tabs[i] == drop)
            gfx::text(lx + widths[i] / 2, cy + 9, tab_label(m_tabs[i]), {gfx::Bold, 25}, alpha(kText, a), 1);
        lx += widths[i] + 6;
    }

    /* Clock and the viewer's initial. */
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char clock[8];
    std::snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    /* The initial on glass (as in Hvem ser på?); the picture fades in over it.
     * Jellyfin has no BlurHash for users. */
    const gfx::Rect av{gfx::W - kPad - 60, cy - 30, 60, 60};
    glass_panel(av, 30, a, false);
    const std::string initial = m_user.empty() ? "?" : first_letter(m_user);
    gfx::text(gfx::W - kPad - 30, cy + 10, initial, {gfx::Bold, 28}, alpha(kText, a), 1);
    if (!m_avatar.empty()) {
        art::draw(av, m_avatar, "", 440, 440, 30, a, 0);
        if (art::animating() && animating) *animating = true;
    }
    gfx::text(gfx::W - kPad - 84, cy + 9, clock, {gfx::SemiBold, 26}, alpha(kText2, a), 2);
}

} // namespace ui
