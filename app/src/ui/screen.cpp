/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/screen.h"

#include "gfx/art.h"
#include "ui_image.h"
#include "ui_assets.h"

#include <algorithm>

namespace ui {

uint32_t alpha(uint32_t c, float a)
{
    a = std::max(0.f, std::min(1.f, a));
    return ((uint32_t)(((c >> 24) & 0xff) * a + 0.5f) << 24) | (c & 0xffffff);
}

float Lifts::step(const std::string &key, bool focused, float dt, bool *animating)
{
    Lift &l = m_lift[key];
    l.a.to(focused ? 1.f : 0.f);
    if (l.a.step(dt, 14.f) && animating)
        *animating = true;
    return l.a.value;
}

void draw_check(float cx, float cy, float size, uint32_t color)
{
    const float u = size / 20.f, d = 3.2f * u;   /* one unit; the stroke's square */
    const float x = cx - 9 * u, y = cy;
    for (int s = 0; s < 6; s++)
        gfx::fill({x + s * 1.6f * u, y - 4 * u + s * 1.6f * u, d, d}, color, d / 3);
    for (int s = 0; s < 11; s++)
        gfx::fill({x + 8 * u + s * 1.6f * u, y + 4 * u - s * 1.8f * u, d, d}, color, d / 3);
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
    case PadButton::Touchpad: {   /* the pad's wide top: a rounded panel */
        w = d * 1.7f;
        gfx::fill({x, cy - r * 0.8f, w, d * 0.8f}, ink, d * 0.18f);
        gfx::fill({x + st, cy - r * 0.8f + st, w - 2 * st, d * 0.8f - 2 * st}, disc, d * 0.18f - st);
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
    else if (b == PadButton::Touchpad)
        w = size * 1.7f;
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

/* Apple TV's living backdrop: the picture, covering the screen, slowly zooms in
 * (6 % over 30 s) while drifting toward one corner (which one follows the
 * picture, so it is the same each time that title comes back). */
void draw_drift(const gfx::Rect &full, const gfx::Texture *t, float a, double age, const std::string &key)
{
    const float tw = (float)gfx::texture_width(t), th = (float)gfx::texture_height(t);
    if (tw <= 0 || th <= 0)
        return;
    /* The cover crop, as gfx::image(cover) makes it. */
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    const float ia = tw / th, ra = full.w / full.h;
    if (ia > ra) {
        const float k = ra / ia;
        u0 = (1.f - k) / 2;
        u1 = 1.f - u0;
    } else {
        const float k = ia / ra;
        v0 = (1.f - k) / 2;
        v1 = 1.f - v0;
    }
    /* Ease in and out over 30 s, then hold. */
    float p = (float)std::min(1.0, age / 30.0);
    p = p * p * (3.f - 2.f * p);
    const float zoom = 0.06f * p;
    unsigned h = 0;
    for (char ch : key)
        h = h * 31u + (unsigned char)ch;
    const float dx = (h & 1) ? 1.f : -1.f, dy = (h & 2) ? 0.6f : -0.6f;
    const float w = u1 - u0, hh = v1 - v0;
    const float cu = u0 + w / 2 + dx * w * zoom / 2, cv = v0 + hh / 2 + dy * hh * zoom / 2;
    const float hw = w * (1.f - zoom) / 2, hv = hh * (1.f - zoom) / 2;
    gfx::image_uv(full, t, cu - hw, cv - hv, cu + hw, cv + hv, a, 0);
}

float brand_width(float size) { return size * 880.f / 300.f; }

/* The "Jelly5" wordmark (assets/brand/wordmark.png, 880x300, built into the app):
 * tall letters from y 6 to the baseline at y 231, so drawn as tall as the type size
 * it stands where "Jelly5" set in that size would. */
static const gfx::Texture *wordmark()
{
    static const gfx::Texture *t = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const ui_asset a = ui_asset_img_wordmark();
        ui_image img;
        if (a.data && ui_image_decode(a.data, a.size, 880, 300, &img) == 0) {
            t = gfx::texture_from_image(&img);
            ui_image_free(&img);
        }
    }
    return t;
}

float draw_brand(float x, float baseline, float size, float opacity, bool glow)
{
    const gfx::Texture *t = wordmark();
    if (!t) {   /* the asset failed: plain type */
        return gfx::text(x, baseline, "Jelly5", {gfx::Bold, size}, alpha(kText, opacity));
    }
    const float h = size, w = h * 880.f / 300.f;
    const gfx::Rect r{x, baseline - h * 231.f / 300.f, w, h};
    (void)glow;   /* the wordmark carries its own soft light */
    gfx::image(r, t, opacity, 0, false);
    return w;
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
    /* Watched: a check; a series with episodes left: how many. Both on a small piece
     * of glass (tint, sheen, lit rim - no blur: there are dozens on screen). */
    auto chip = [&](const gfx::Rect &b) {
        gfx::fill(b, alpha(0x66101014u, opacity), b.h / 2);
        gfx::fill_vgradient({b.x, b.y, b.w, b.h * 0.55f}, alpha(0x2effffffu, opacity), 0x00000000u, b.h / 2);
        gfx::rim(b, b.h / 2, 0.8f * opacity);
    };
    if (it.played) {
        const gfx::Rect b{r.x + r.w - 44, r.y + 10, 34, 34};
        chip(b);
        draw_check(b.x + b.w / 2 + 1, b.y + b.h / 2 + 1, 15, alpha(0xf2ffffffu, opacity));
    } else if (it.unplayed > 0 && (it.type == "Series" || it.type == "Season")) {
        const std::string n = it.unplayed > 99 ? "99+" : std::to_string(it.unplayed);
        const gfx::TextStyle ns{gfx::SemiBold, 17};
        const float w = std::max(34.f, gfx::text_width(n, ns) + 22);
        const gfx::Rect b{r.x + r.w - 10 - w, r.y + 10, w, 34};
        chip(b);
        gfx::text(b.x + b.w / 2, b.y + 23, n, ns, alpha(0xf2ffffffu, opacity), 1);
    }
    if (it.played_percent > 0 && it.played_percent < 100) {
        gfx::fill({r.x + 14, r.y + r.h - 20, r.w - 28, 6}, alpha(0x47ffffffu, opacity), 3);
        gfx::fill({r.x + 14, r.y + r.h - 20, (r.w - 28) * (float)(it.played_percent / 100), 6},
                  alpha(0xffffffffu, opacity), 3);
    }
    /* The title under every poster (a grid is for skimming), brighter on focus; an
     * album's artist or an episode's series on a second line. */
    const uint32_t tc = lift > 0.5f ? kText : kText2;
    gfx::text(r.x, r.y + r.h + 34, it.name, {gfx::SemiBold, 20, r.w}, alpha(tc, opacity));
    const std::string &sub = it.type == "MusicAlbum" ? it.album_artist : it.type == "Episode" ? it.series_name
                                                                                              : std::string();
    if (!sub.empty())
        gfx::text(r.x, r.y + r.h + 60, sub, {gfx::Medium, 18, r.w}, alpha(kText3, opacity));
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
