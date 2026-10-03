/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/screen.h"

#include "gfx/art.h"

#include <algorithm>

namespace ui {

uint32_t alpha(uint32_t c, float a)
{
    a = std::max(0.f, std::min(1.f, a));
    return ((uint32_t)(((c >> 24) & 0xff) * a + 0.5f) << 24) | (c & 0xffffff);
}

float Lifts::step(const std::string &key, bool focused, float dt, bool *animating)
{
    Anim &a = m_lift[key];
    a.to(focused ? 1.f : 0.f);
    if (a.step(dt, 14.f) && animating)
        *animating = true;
    return a.value;
}

std::string poster_url(jf::Client &c, const jf::Item &it, int width)
{
    return c.image_url(it.id, "Primary", it.primary_tag, width);
}

std::string landscape_url(jf::Client &c, const jf::Item &it, int width)
{
    if (it.type == "Episode" && !it.primary_tag.empty())
        return c.image_url(it.id, "Primary", it.primary_tag, width);
    if (!it.thumb_tag.empty())
        return c.image_url(it.thumb_owner, "Thumb", it.thumb_tag, width);
    if (!it.backdrop_tag.empty())
        return c.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, width);
    return c.image_url(it.id, "Primary", it.primary_tag, width);
}

std::string landscape_blurhash(const jf::Item &it)
{
    return it.thumb_blurhash.empty() ? it.backdrop_blurhash : it.thumb_blurhash;
}

void draw_poster(jf::Client &c, const jf::Item &it, const gfx::Rect &base, float lift, float opacity)
{
    const float k = 1.f + 0.1f * lift;
    const gfx::Rect r{base.x - base.w * (k - 1) / 2, base.y - base.h * (k - 1) / 2, base.w * k, base.h * k};
    if (lift > 0.01f)
        gfx::shadow(r, 14 * k, 30, 0.75f * lift * opacity, 22 * lift);
    art::draw(r, poster_url(c, it, 480), it.primary_blurhash, 480, 720, 14 * k, opacity);
    if (it.played)
        gfx::fill({r.x + r.w - 46, r.y + 12, 34, 34}, alpha(0x8c000000u, opacity), 17);
    if (it.played_percent > 0 && it.played_percent < 100) {
        gfx::fill({r.x + 14, r.y + r.h - 20, r.w - 28, 6}, alpha(0x47ffffffu, opacity), 3);
        gfx::fill({r.x + 14, r.y + r.h - 20, (r.w - 28) * (float)(it.played_percent / 100), 6},
                  alpha(0xffffffffu, opacity), 3);
    }
    if (lift > 0.01f)
        gfx::text(r.x, r.y + r.h + 34, it.name, {gfx::SemiBold, 20, r.w}, alpha(kText, lift * opacity));
}

void Ambient::set(const std::string &blurhash, double now)
{
    m_now = now;
    if (blurhash.empty())
        return;
    if (m_cur.empty()) {
        m_cur = blurhash;
        return;
    }
    if (blurhash != m_pending) {
        m_pending = blurhash;
        m_pending_since = now;
    }
}

void Ambient::draw(float dt, float dim, bool *animating)
{
    /* Settled on a new colour: start (or redirect) the fade. */
    const bool waiting = !m_pending.empty() && m_pending != m_cur && m_pending != m_next;
    if (waiting && animating)
        *animating = true;
    if (waiting && m_now - m_pending_since > 0.35) {
        if (!m_next.empty() && m_mix.value > 0.5f)
            m_cur = m_next;       /* mostly there already: that becomes the base */
        m_next = m_pending;
        m_mix.snap(0.f);
        m_mix.to(1.f);
    }
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    if (const gfx::Texture *t = art::blurhash(m_cur))
        gfx::image(full, t, 1.f, 0, true);
    if (!m_next.empty()) {
        if (m_mix.step(dt, 3.2f)) {
            if (animating)
                *animating = true;
        } else {
            m_cur = m_next;
            m_next.clear();
        }
        if (const gfx::Texture *t = art::blurhash(m_next))
            gfx::image(full, t, smoothstep(m_mix.value), 0, true);
    }
    gfx::fill(full, alpha(kBg, dim));
}

} // namespace ui
