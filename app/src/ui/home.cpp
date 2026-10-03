/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sizes, colours and timings follow concept/style.css.
 */
#include "ui/home.h"

#include "gfx/art.h"
#include "gfx/gfx.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
namespace {

constexpr float kCardW = 400, kCardH = 225, kCardGap = 32, kCardR = 14;
constexpr float kRowH = 380;
constexpr float kRowsTopFocus = 600;   /* focused row's title line, rows mode */
constexpr float kRowsTopHero = 910;    /* first row peeking under the hero */

constexpr uint32_t kStar = 0xfff5c518;

std::string runtime_label(int64_t ticks)
{
    const int min = (int)(ticks / jf::kTicksPerSecond / 60);
    if (min <= 0)
        return std::string();
    char b[32];
    if (min >= 60)
        std::snprintf(b, sizeof b, "%d t %d min", min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return b;
}

} // namespace

void Home::set_model(HomeModel model)
{
    /* Keep focus on the same title where it survived the refresh. */
    std::string focused_id;
    if (const jf::Item *f = focused_item())
        focused_id = f->id;
    m_model = std::move(model);
    m_cols.assign(m_model.rows.size(), 0);
    if (m_scroll.size() != m_model.rows.size())
        m_scroll.assign(m_model.rows.size(), Anim());
    if (m_row >= (int)m_model.rows.size())
        m_row = (int)m_model.rows.size() - 1;
    if (m_row >= 0 && !focused_id.empty()) {
        const auto &items = m_model.rows[m_row].items;
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].id == focused_id)
                m_cols[m_row] = (int)i;
    }
    if (m_model.hero.empty() && m_row < 0 && !m_model.rows.empty())
        m_row = 0;
    m_hero = 0;
    m_hero_since = m_now;
    m_hero_mode.snap(m_row < 0 ? 1.f : 0.f);
    m_rows_y.snap((float)std::max(m_row, 0));
}

const jf::Item *Home::focused_item() const
{
    if (m_row < 0)
        return m_model.hero.empty() ? nullptr : &m_model.hero[m_hero % m_model.hero.size()];
    if (m_row >= (int)m_model.rows.size())
        return nullptr;
    const auto &items = m_model.rows[m_row].items;
    const int c = m_cols[m_row];
    return c >= 0 && c < (int)items.size() ? &items[c] : nullptr;
}

void Home::activate()
{
    /* Entered from the navigation bar: start on the hero. */
    if (!m_model.hero.empty()) {
        m_row = -1;
        m_focus_changed = m_now;
    }
}

Action Home::input(uint32_t p)
{
    Action action;
    const int nrows = (int)m_model.rows.size();
    const int before_row = m_row;
    const int before_col = m_row >= 0 ? m_cols[m_row] : 0;

    if (p & NUVIO_BTN_DOWN) {
        if (m_row + 1 < nrows)
            m_row++;
    } else if (p & NUVIO_BTN_UP) {
        if (m_row > 0 || (m_row == 0 && !m_model.hero.empty()))
            m_row--;
        else if (m_row < 0 || m_model.hero.empty())
            action.kind = Action::ToNav;
    } else if (p & NUVIO_BTN_RIGHT) {
        if (m_row < 0)
            m_hero_button = 1;
        else if (m_cols[m_row] + 1 < (int)m_model.rows[m_row].items.size())
            m_cols[m_row]++;
    } else if (p & NUVIO_BTN_LEFT) {
        if (m_row < 0)
            m_hero_button = 0;
        else if (m_cols[m_row] > 0)
            m_cols[m_row]--;
    } else if (p & NUVIO_BTN_CIRCLE) {
        /* Back, as on Netflix: to the start of the row, then to the hero. */
        if (m_row >= 0 && m_cols[m_row] > 0)
            m_cols[m_row] = 0;
        else if (m_row >= 0 && !m_model.hero.empty())
            m_row = -1;
        else if (m_row > 0)
            m_row = 0;
        else
            action.kind = Action::ToNav;   /* from the hero: up to the tabs */
    } else if (p & NUVIO_BTN_CROSS) {
        if (const jf::Item *f = focused_item()) {
            const bool plays = m_row < 0 ? m_hero_button == 0 : m_model.rows[m_row].plays;
            action.kind = plays ? Action::Play : Action::Open;
            action.item = *f;
        }
    }
    if (m_row != before_row || (m_row >= 0 && m_cols[m_row] != before_col))
        m_focus_changed = m_now;
    return action;
}

std::string Home::card_url(const jf::Item &it) const
{
    if (it.type == "Episode" && !it.primary_tag.empty())
        return m_client.image_url(it.id, "Primary", it.primary_tag, 640);
    if (!it.thumb_tag.empty())
        return m_client.image_url(it.thumb_owner, "Thumb", it.thumb_tag, 640);
    if (!it.backdrop_tag.empty())
        return m_client.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 640);
    return m_client.image_url(it.id, "Primary", it.primary_tag, 640);
}

std::string Home::backdrop_url(const jf::Item &it) const
{
    return m_client.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920);
}

void Home::draw_backdrop(float dt)
{
    /* Crossfade (900 ms) to the focused title's backdrop once it has loaded. */
    const jf::Item *f = focused_item();
    const std::string want = f ? backdrop_url(*f) : std::string();
    const std::string want_hash = f ? f->backdrop_blurhash : std::string();
    if (want != m_bd_cur && want != m_bd_next && (m_now - m_focus_changed) > 0.25) {
        m_bd_next = want;
        m_bd_next_hash = want_hash;
        m_bd_mix.snap(0.f);
    }
    if (!m_bd_next.empty() && art::get(m_bd_next, 1920, 1080)) {
        m_bd_mix.to(1.f);
        if (!m_bd_mix.step(dt, 4.5f) || m_bd_cur.empty()) {
            m_bd_cur = m_bd_next;
            m_bd_cur_hash = m_bd_next_hash;
            m_bd_next.clear();
            m_bd_mix.snap(0.f);
        } else {
            m_animating = true;
        }
    }
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    if (!m_bd_cur.empty()) {
        if (const gfx::Texture *t = art::get(m_bd_cur, 1920, 1080))
            gfx::image(full, t, 1.f, 0, true);
        else if (const gfx::Texture *ph = art::blurhash(m_bd_cur_hash))
            gfx::image(full, ph, 1.f, 0, true);
    }
    if (!m_bd_next.empty() && m_bd_mix.value > 0)
        if (const gfx::Texture *t = art::get(m_bd_next, 1920, 1080))
            gfx::image(full, t, smoothstep(m_bd_mix.value), 0, true);

    /* Scrims (concept: .scrim-left, .scrim-bottom, .scrim-top). */
    gfx::fill_hgradient({0, 0, 576, gfx::H}, alpha(kBg, 0.92f), alpha(kBg, 0.72f));
    gfx::fill_hgradient({576, 0, 538, gfx::H}, alpha(kBg, 0.72f), alpha(kBg, 0.2f));
    gfx::fill_hgradient({1114, 0, 326, gfx::H}, alpha(kBg, 0.2f), alpha(kBg, 0.f));
    gfx::fill_vgradient({0, 486, gfx::W, 356}, alpha(kBg, 0.f), alpha(kBg, 0.85f));
    gfx::fill_vgradient({0, 842, gfx::W, 238}, alpha(kBg, 0.85f), kBg);
    gfx::fill_vgradient({0, 0, gfx::W, 220}, 0x8c000000u, 0x00000000u);
}

void Home::draw_info(const jf::Item &it, float bottom, bool hero, float a)
{
    if (a <= 0.f)
        return;
    const bool episode = it.type == "Episode";
    /* Measure from the bottom up so a tall logo never collides with the rows. */
    const gfx::TextStyle ov{gfx::Regular, 26, 780, hero ? 3 : 2, 37.7f};
    const gfx::TextStyle meta{gfx::Medium, 24};
    const gfx::TextStyle ep{gfx::Bold, 32};
    const float ov_lines = it.overview.empty() ? 0 : (float)ov.max_lines;
    const float buttons = hero ? 116 : 0;
    float y = bottom - buttons - ov_lines * 37.7f - 46;           /* meta baseline */
    const float meta_y = y;
    float title_bottom = meta_y - 46;
    if (episode)
        title_bottom -= 48;

    /* Logo (fading in; nothing until then) or the name in large type. */
    const std::string logo = m_client.image_url(it.logo_owner, "Logo", it.logo_tag, 800);
    const float max_lw = hero ? 640.f : 520.f, max_lh = hero ? 200.f : 150.f;
    if (!logo.empty()) {
        if (const gfx::Texture *t = art::get(logo, 800, 260)) {
            const float iw = (float)gfx::texture_width(t), ih = (float)gfx::texture_height(t);
            const float k = std::min(max_lw / iw, max_lh / ih);
            gfx::image({kPad, title_bottom - ih * k, iw * k, ih * k}, t, a * art::fade(logo), 0, false);
        }
    } else {
        const std::string name = episode ? it.series_name : it.name;
        gfx::text(kPad, title_bottom - 14, name, {gfx::Bold, hero ? 84.f : 72.f, 1500}, alpha(kText, a));
    }
    if (episode) {
        char line[256];
        std::snprintf(line, sizeof line, "S%d:E%d \xC2\xB7 %s", it.parent_index, it.index, it.name.c_str());
        gfx::text(kPad, meta_y - 52, line, ep, alpha(kText, a));
    }

    /* Meta: rating, year, runtime, genres, age rating. */
    float x = kPad;
    auto sep = [&] {
        gfx::fill({x + 12, meta_y - 10, 5, 5}, alpha(0x6bebebf5, a), 2.5f);
        x += 29;
    };
    bool first = true;
    if (it.community_rating > 0) {
        char r[16];
        std::snprintf(r, sizeof r, "\xE2\x98\x85 %.1f", it.community_rating);
        x += gfx::text(x, meta_y, r, meta, alpha(kStar, a));
        first = false;
    }
    if (it.year && !episode) {
        if (!first) sep();
        x += gfx::text(x, meta_y, std::to_string(it.year), meta, alpha(kText2, a));
        first = false;
    }
    const std::string rt = it.type == "Series" ? std::string() : runtime_label(it.runtime_ticks);
    if (!rt.empty()) {
        if (!first) sep();
        x += gfx::text(x, meta_y, rt, meta, alpha(kText2, a));
        first = false;
    }
    if (!it.genres.empty()) {
        if (!first) sep();
        std::string g = it.genres[0];
        if (it.genres.size() > 1)
            g += " \xC2\xB7 " + it.genres[1];
        x += gfx::text(x, meta_y, g, meta, alpha(kText2, a));
    }
    if (!it.official_rating.empty()) {
        x += 16;
        const gfx::TextStyle badge{gfx::Bold, 17};
        const float bw = gfx::text_width(it.official_rating, badge) + 18;
        gfx::fill({x, meta_y - 22, bw, 30}, alpha(0x73ffffff, a), 6);
        gfx::fill({x + 1.5f, meta_y - 20.5f, bw - 3, 27}, alpha(0xff0d0d12, a * 0.85f), 5);
        gfx::text(x + 9, meta_y - 1, it.official_rating, badge, alpha(kText, a));
    }

    if (!it.overview.empty())
        gfx::text(kPad, meta_y + 50, it.overview, ov, alpha(kText2, a));

    if (hero) {
        /* "Spill av" / "Fortsett" and "Mer info"; the focused one is white (.btn.focus). */
        const float by = bottom - 76;
        const bool resume = it.position_ticks > 0;
        const gfx::TextStyle bt{gfx::Bold, 26};
        float bx = kPad;
        for (int b = 0; b < 2; b++) {
            const std::string label = b == 0 ? (resume ? "Fortsett" : "Spill av") : "Mer info";
            const float bw = gfx::text_width(label, bt) + 80 + 34;
            const bool focused = m_row < 0 && m_hero_button == b;
            const float k = focused ? 1.08f : 1.f;
            const gfx::Rect r{bx - bw * (k - 1) / 2, by - 76 * (k - 1) / 2, bw * k, 76 * k};
            if (focused)
                gfx::shadow(r, 16, 24, 0.55f * a, 14);
            gfx::fill(r, alpha(focused ? 0xfff5f5f7u : 0x24ffffffu, a), 16 * k);
            const uint32_t fg = focused ? 0xff0b0b0fu : kText;
            const float px = r.x + 40 * k, py = r.y + r.h / 2;
            if (b == 0) {
                for (int i = 0; i < 14; i++)   /* play glyph */
                    gfx::fill({px + i * 1.5f, py - (14 - i) * 1.0f, 1.5f, (14 - i) * 2.0f}, alpha(fg, a));
            } else {                         /* info glyph: a ring with an "i" */
                gfx::fill({px - 2, py - 15, 30, 30}, alpha(fg, a), 15);
                gfx::fill({px + 1, py - 12, 24, 24}, alpha(focused ? 0xfff5f5f7u : 0xff2a2a30u, a), 12);
                gfx::fill({px + 11.5f, py - 8, 3, 3}, alpha(fg, a), 1.5f);
                gfx::fill({px + 11.5f, py - 3, 3, 11}, alpha(fg, a), 1.5f);
            }
            gfx::text(r.x + 72 * k, r.y + r.h / 2 + 9, label, bt, alpha(fg, a));
            bx += bw + 20;
        }
    }
}

void Home::draw_rows(float dt)
{
    /* Vertical: the focused row slides to a fixed line; rows above fade away. */
    m_rows_y.to((float)std::max(m_row, 0));
    if (m_rows_y.step(dt, 11.f))
        m_animating = true;
    m_hero_mode.to(m_row < 0 ? 1.f : 0.f);
    if (m_hero_mode.step(dt, 10.f))
        m_animating = true;
    const float top = kRowsTopFocus + (kRowsTopHero - kRowsTopFocus) * m_hero_mode.value;

    for (size_t r = 0; r < m_model.rows.size(); r++) {
        const HomeRow &row = m_model.rows[r];
        const float rel = (float)r - m_rows_y.value;
        const float ry = top + rel * kRowH;
        float a = 1.f;
        if (rel < 0)
            a = std::max(0.f, 1.f + rel * 1.6f);   /* rows above fade out */
        if (a <= 0.f || ry > gfx::H + 40)
            continue;
        gfx::text(kPad, ry + 30, row.title, {gfx::Bold, 30}, alpha(0xebffffffu, a));

        /* Horizontal: the focused card sits at the left edge, until the row ends. */
        const int col = m_cols[r];
        const float max_scroll =
            std::max(0.f, (float)row.items.size() * (kCardW + kCardGap) - kCardGap - (gfx::W - 2 * kPad));
        m_scroll[r].to(std::min(max_scroll, (float)col * (kCardW + kCardGap)));
        if (m_scroll[r].step(dt, 12.f))
            m_animating = true;

        const float cy = ry + 52;
        int focus_i = -1;
        for (size_t i = 0; i < row.items.size(); i++) {
            const float cx = kPad + (float)i * (kCardW + kCardGap) - m_scroll[r].value;
            if (cx > gfx::W + 20 || cx + kCardW < -60)
                continue;
            const jf::Item &it = row.items[i];
            const bool focused = (int)r == m_row && (int)i == col;
            if (focused) {
                focus_i = (int)i;
                continue;   /* drawn last, over its neighbours */
            }
            Anim &lift = m_lift[it.id + "@" + std::to_string(r)];
            lift.to(0.f);
            if (lift.step(dt, 14.f))
                m_animating = true;
            const float k = 1.f + 0.1f * lift.value;
            const gfx::Rect cr{cx - kCardW * (k - 1) / 2, cy - kCardH * (k - 1) / 2, kCardW * k, kCardH * k};
            art::draw(cr, card_url(it), it.thumb_blurhash.empty() ? it.backdrop_blurhash : it.thumb_blurhash,
                      640, 360, kCardR * k, a);
            if (it.played_percent > 0 && it.played_percent < 100) {
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, cr.w - 36, 6}, alpha(0x47ffffffu, a), 3);
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, (cr.w - 36) * (float)(it.played_percent / 100), 6},
                          alpha(0xffffffffu, a), 3);
            }
        }
        if (focus_i >= 0) {
            const jf::Item &it = row.items[focus_i];
            const float cx = kPad + (float)focus_i * (kCardW + kCardGap) - m_scroll[r].value;
            Anim &lift = m_lift[it.id + "@" + std::to_string(r)];
            lift.to(1.f);
            if (lift.step(dt, 14.f))
                m_animating = true;
            const float k = 1.f + 0.1f * lift.value;
            const gfx::Rect cr{cx - kCardW * (k - 1) / 2, cy - kCardH * (k - 1) / 2, kCardW * k, kCardH * k};
            gfx::shadow(cr, kCardR * k, 30, 0.75f * lift.value * a, 22 * lift.value);
            art::draw(cr, card_url(it), it.thumb_blurhash.empty() ? it.backdrop_blurhash : it.thumb_blurhash,
                      640, 360, kCardR * k, a);
            if (it.played_percent > 0 && it.played_percent < 100) {
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, cr.w - 36, 6}, alpha(0x47ffffffu, a), 3);
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, (cr.w - 36) * (float)(it.played_percent / 100), 6},
                          alpha(0xffffffffu, a), 3);
            }
            /* Label under the focused card (concept: titles only on focus). */
            const float la = a * lift.value;
            const bool ep = it.type == "Episode";
            gfx::text(cr.x, cr.y + cr.h + 38, ep ? it.series_name : it.name, {gfx::SemiBold, 22, cr.w},
                      alpha(kText, la));
            if (ep) {
                char sub[256];
                std::snprintf(sub, sizeof sub, "S%d:E%d \xC2\xB7 %s", it.parent_index, it.index, it.name.c_str());
                gfx::text(cr.x, cr.y + cr.h + 66, sub, {gfx::Medium, 19, cr.w}, alpha(kText3, la));
            }
        }
    }
}

void Home::draw(double now, float dt)
{
    m_now = now;
    m_animating = false;

    /* The hero rotates every 10 s while it has focus. */
    if (m_row < 0 && m_model.hero.size() > 1 && now - m_hero_since > 10.0) {
        m_hero = (m_hero + 1) % (int)m_model.hero.size();
        m_hero_since = now;
        m_focus_changed = now;
    }

    draw_backdrop(dt);

    /* Info panel: settle 120 ms on a title, then fade the new one in. */
    const jf::Item *f = focused_item();
    if (f && f->id != m_info_id) {
        m_info_alpha.to(0.f);
        if ((now - m_focus_changed) > 0.12 && m_info_alpha.value < 0.05f) {
            m_info_id = f->id;
            m_info_alpha.to(1.f);
        }
    } else if (f) {
        m_info_alpha.to(1.f);
    }
    if (m_info_alpha.step(dt, 16.f))
        m_animating = true;
    const jf::Item *shown = nullptr;
    if (!m_info_id.empty()) {
        if (m_row < 0) {
            for (const auto &h : m_model.hero)
                if (h.id == m_info_id)
                    shown = &h;
        } else {
            for (const auto &row : m_model.rows)
                for (const auto &it : row.items)
                    if (it.id == m_info_id)
                        shown = &it;
        }
    }
    if (shown) {
        const float hero = m_hero_mode.value;
        const float bottom = 560 + (850 - 560) * hero;
        draw_info(*shown, bottom, m_row < 0, m_info_alpha.value);
    }

    draw_rows(dt);

    if (art::animating())
        m_animating = true;
}

} // namespace ui
