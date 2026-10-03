/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/screensaver.h"

#include "gfx/art.h"
#include "ui/anim.h"
#include "ui/screen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace ui {
namespace {
constexpr double kSlide = 12.0, kFade = 1.6;
}

void Screensaver::start(std::vector<Slide> slides, double now)
{
    std::srand((unsigned)time(nullptr));
    for (size_t i = slides.size(); i > 1; i--)
        std::swap(slides[i - 1], slides[(size_t)std::rand() % i]);
    m_slides = std::move(slides);
    m_index = 0;
    m_shown = m_started = now;
    m_on = !m_slides.empty();
}

void Screensaver::draw(double now)
{
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, 0xff000000u);
    if (m_slides.empty())
        return;
    /* The next slide waits for its picture; then it crossfades in. */
    const size_t next = (m_index + 1) % m_slides.size();
    const gfx::Texture *cur = art::get(m_slides[m_index].url, 1920, 1080);
    const gfx::Texture *nxt = art::get(m_slides[next].url, 1920, 1080);   /* loads ahead */
    if (now - m_shown >= kSlide + kFade && nxt) {
        m_index = next;
        m_shown = now - kFade;
        cur = nxt;
    }
    const float wake = smoothstep(std::min(1.f, (float)(now - m_started) / 1.5f));
    if (cur)
        draw_drift(full, cur, wake, now - m_shown, m_slides[m_index].url);
    const double into_next = now - m_shown - kSlide;
    if (into_next > 0 && nxt) {
        const float f = smoothstep(std::min(1.f, (float)(into_next / kFade)));
        draw_drift(full, nxt, f * wake, into_next, m_slides[next].url);
    }

    /* Corners: the title bottom left, the clock bottom right, over soft shade. */
    gfx::fill_vgradient({0, gfx::H - 300, gfx::W, 300}, 0x00000000u, 0x8c000000u);
    const Slide &s = m_slides[m_index];
    const float ta = wake * (into_next > 0 ? 1.f - smoothstep(std::min(1.f, (float)(into_next / 0.6))) : 1.f);
    gfx::text(kPad, gfx::H - 112, s.title, {gfx::Bold, 40, 1100}, alpha(kText, ta));
    if (!s.line.empty())
        gfx::text(kPad, gfx::H - 70, s.line, {gfx::Medium, 24, 1100}, alpha(kText2, ta));
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char clock[8];
    std::snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    gfx::text(gfx::W - kPad, gfx::H - 76, clock, {gfx::SemiBold, 64}, alpha(kText, wake * 0.9f), 2);
}

} // namespace ui
