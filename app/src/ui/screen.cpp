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
    if (it.type == "Episode" && !it.series_primary_tag.empty())   /* its series' poster */
        return c.image_url(it.series_id, "Primary", it.series_primary_tag, width);
    return c.image_url(it.id, "Primary", it.primary_tag, width);
}

void glass_panel(const gfx::Rect &r, float radius, float a, bool shadow)
{
    if (a <= 0.01f)
        return;
    if (shadow)
        gfx::shadow(r, radius, 46, 0.6f * a, 18);
    const int glass = gfx::backdrop_blur(r, radius, 18.f, a);
    if (glass == 2)
        return;   /* the shader drew the whole pane */
    if (glass == 1) {   /* Liquid Glass: clear, a sheen from above, a lit rim */
        gfx::fill(r, alpha(0x4d0c0c12u, a), radius);
        gfx::fill_vgradient({r.x, r.y, r.w, r.h * 0.45f}, alpha(0x1affffffu, a), 0x00000000u, radius);
        gfx::rim(r, radius, 0.9f * a);
    } else {
        gfx::fill(r, alpha(0xdc1c1c22u, a), radius);
        gfx::fill({r.x + radius * 0.6f, r.y, r.w - radius * 1.2f, 1.5f}, alpha(0x2effffffu, a));
    }
}

float draw_pad_hint(float x, float cy, PadButton b, const std::string &label, float size, float a)
{
    const uint32_t disc = alpha(0xff3a3a42u, a), ink = alpha(0xfff5f5f7u, a);
    const float d = size, r = d / 2, st = std::max(2.f, d * 0.09f);   /* disc, radius, stroke */
    float w = d;
    switch (b) {
    case PadButton::Cross: {
        gfx::fill({x, cy - r, d, d}, disc, r);
        const int n = 10;
        const float s = d * 0.46f;
        for (int i = 0; i <= n; i++) {   /* two strokes from small squares */
            const float t = (float)i / n * s;
            gfx::fill({x + r - s / 2 + t - st / 2, cy - s / 2 + t - st / 2, st, st}, ink, st / 2);
            gfx::fill({x + r + s / 2 - t - st / 2, cy - s / 2 + t - st / 2, st, st}, ink, st / 2);
        }
        break;
    }
    case PadButton::Circle: {
        gfx::fill({x, cy - r, d, d}, disc, r);
        const float o = d * 0.52f, i = o - 2 * st;
        gfx::fill({x + r - o / 2, cy - o / 2, o, o}, ink, o / 2);
        gfx::fill({x + r - i / 2, cy - i / 2, i, i}, disc, i / 2);
        break;
    }
    case PadButton::Triangle: {
        gfx::fill({x, cy - r, d, d}, disc, r);
        auto tri = [&](float h, uint32_t c) {   /* a filled triangle from rows */
            const int n = (int)(h / 1.2f);
            for (int k = 0; k < n; k++) {
                const float row = (float)k / n, ww = h * 1.15f * row;
                gfx::fill({x + r - ww / 2, cy - h * 0.55f + row * h, std::max(1.f, ww), h / n + 0.6f}, c);
            }
        };
        tri(d * 0.5f, ink);
        tri(d * 0.5f - 2.6f * st, disc);
        break;
    }
    case PadButton::Square: {
        gfx::fill({x, cy - r, d, d}, disc, r);
        const float o = d * 0.44f, i = o - 2 * st;
        gfx::fill({x + r - o / 2, cy - o / 2, o, o}, ink, 1.5f);
        gfx::fill({x + r - i / 2, cy - i / 2, i, i}, disc, 1.f);
        break;
    }
    case PadButton::Options: {   /* a pill with three lines */
        w = d * 1.5f;
        gfx::fill({x, cy - r, w, d}, disc, r);
        const float lw = d * 0.5f;
        for (int k = -1; k <= 1; k++)
            gfx::fill({x + w / 2 - lw / 2, cy + k * d * 0.17f - st / 2, lw, st}, ink, st / 2);
        break;
    }
    case PadButton::L1:
    case PadButton::R1: {
        const char *t = b == PadButton::L1 ? "L1" : "R1";
        const gfx::TextStyle ts{gfx::Bold, d * 0.5f};
        w = gfx::text_width(t, ts) + d * 0.7f;
        gfx::fill({x, cy - r, w, d}, disc, r);
        gfx::text(x + w / 2, cy + d * 0.18f, t, ts, ink, 1);
        break;
    }
    }
    if (label.empty())
        return w;
    return w + 10 + gfx::text(x + w + 10, cy + d * 0.28f, label, {gfx::Medium, d * 0.72f}, alpha(kText2, a));
}

float pad_hint_width(PadButton b, const std::string &label, float size)
{
    float w = size;
    if (b == PadButton::Options)
        w = size * 1.5f;
    else if (b == PadButton::L1 || b == PadButton::R1)
        w = gfx::text_width("L1", {gfx::Bold, size * 0.5f}) + size * 0.7f;
    return label.empty() ? w : w + 10 + gfx::text_width(label, {gfx::Medium, size * 0.72f});
}

float draw_pad_hints(float x, float cy, const std::vector<PadHint> &hints, int align, float size, float a)
{
    const float gap = size * 0.9f;
    float total = 0;
    for (size_t i = 0; i < hints.size(); i++)
        total += pad_hint_width(hints[i].button, hints[i].label, size) + (i ? gap : 0);
    float hx = align == 1 ? x - total / 2 : align == 2 ? x - total : x;
    for (const PadHint &h : hints)
        hx += draw_pad_hint(hx, cy, h.button, h.label, size, a) + gap;
    return total;
}

float brand_width(float size)
{
    return size * 0.95f + size * 0.3f + gfx::text_width("Jelly5", {gfx::Bold, size});
}

float draw_brand(float x, float baseline, float size, float opacity, bool glow)
{
    const float m = size * 0.95f;                     /* the mark's side */
    const gfx::Rect mark{x, baseline - m * 0.84f, m, m};
    gfx::push_opacity(opacity);
    if (glow)
        gfx::shadow(mark, m * 0.29f, m * 0.4f, 0.45f, 0);
    gfx::fill_vgradient(mark, 0xffaa5cc3u, 0xff00a4dcu, m * 0.29f);
    gfx::text(mark.x + m * 0.5f, mark.y + m * 0.5f + m * 0.27f, "J", {gfx::Bold, m * 0.74f}, 0xffffffffu, 1);
    const float w = gfx::text(x + m + size * 0.3f, baseline, "Jelly5", {gfx::Bold, size}, kText);
    gfx::pop_opacity();
    return m + size * 0.3f + w;
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
