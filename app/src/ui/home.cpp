/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sizes, colours and timings follow concept/style.css.
 */
#include "ui/home.h"
#include "app/i18n.h"

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
        std::snprintf(b, sizeof b, T("%d t %d min"), min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return b;
}

} // namespace

void Home::set_model(HomeModel model)
{
    /* Keep focus on the same title where it survived the refresh. */
    std::string focused_id, focused_row;
    if (const jf::Item *f = focused_item())
        focused_id = f->id;
    if (m_row >= 0 && m_row < (int)m_model.rows.size())
        focused_row = m_model.rows[m_row].title;
    /* Rows keep where they were left (by title: rows come and go between refreshes). */
    std::map<std::string, std::pair<int, Anim>> kept;
    for (size_t r = 0; r < m_model.rows.size() && r < m_cols.size() && r < m_scroll.size(); r++)
        kept[m_model.rows[r].title] = {m_cols[r], m_scroll[r]};
    bool hero_changed = model.hero.size() != m_model.hero.size();
    for (size_t i = 0; !hero_changed && i < model.hero.size(); i++)
        hero_changed = model.hero[i].id != m_model.hero[i].id;
    m_model = std::move(model);
    m_cols.assign(m_model.rows.size(), 0);
    m_scroll.assign(m_model.rows.size(), Anim());
    for (size_t r = 0; r < m_model.rows.size(); r++) {
        auto k = kept.find(m_model.rows[r].title);
        if (k != kept.end()) {
            m_cols[r] = std::min(k->second.first, std::max(0, (int)m_model.rows[r].items.size() - 1));
            m_scroll[r] = k->second.second;
        }
        if (!focused_row.empty() && m_model.rows[r].title == focused_row && m_row >= 0)
            m_row = (int)r;   /* the focused row moved: stay on it */
    }
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
    if (m_hero >= (int)m_model.hero.size() || hero_changed) {   /* the same titles keep their turn */
        m_hero = 0;
        m_hero_since = m_now;
    }
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
        m_hero_since = m_now;
    }
}

/* A change from the options sheet, shown at once (the rows reload behind it). */
void Home::apply(const UserDataChange &c)
{
    for (HomeRow &row : m_model.rows) {
        for (jf::Item &it : row.items)
            apply_change(it, c);
        auto gone = [&](const jf::Item &it) {
            if (it.id != c.id)
                return false;
            if (row.kind == HomeRow::Resume)
                return c.resume_cleared || c.played_set;
            if (row.kind == HomeRow::NextUp)
                return c.played_set && c.played;
            if (row.kind == HomeRow::MyList)
                return c.favorite_set && !c.favorite;
            return false;
        };
        row.items.erase(std::remove_if(row.items.begin(), row.items.end(), gone), row.items.end());
    }
    for (jf::Item &it : m_model.hero)
        apply_change(it, c);
    /* Rows left empty go; focus stays in range. */
    for (size_t r = 0; r < m_model.rows.size();)
        if (m_model.rows[r].items.empty()) {
            m_model.rows.erase(m_model.rows.begin() + r);
            m_cols.erase(m_cols.begin() + r);
            m_scroll.erase(m_scroll.begin() + r);
            if (m_row > (int)r)
                m_row--;
        } else {
            r++;
        }
    if (m_row >= (int)m_model.rows.size())
        m_row = (int)m_model.rows.size() - 1;
    if (m_row < 0 && m_model.hero.empty() && !m_model.rows.empty())
        m_row = 0;
    if (m_row >= 0)
        m_cols[m_row] = std::min(m_cols[m_row], (int)m_model.rows[m_row].items.size() - 1);
    m_focus_changed = m_now;
}

Action Home::input(uint32_t p)
{
    Action action;
    if (m_menu.active()) {
        m_menu.input(p, &action);
        if (action.kind == Action::Changed)
            apply(action.change);
        return action;
    }
    if ((p & NUVIO_BTN_OPTIONS) && !m_discover) {   /* Seerr's titles have no options of the library's */
        if (const jf::Item *f = focused_item())
            m_menu.open(*f, m_row >= 0 && m_model.rows[m_row].kind == HomeRow::Resume);
        return action;
    }
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
        else
            m_bump = true;   /* the row's end */
    } else if (p & NUVIO_BTN_LEFT) {
        if (m_row < 0)
            m_hero_button = 0;
        else if (m_cols[m_row] > 0)
            m_cols[m_row]--;
        else
            m_bump = true;
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
    /* Back on the hero: it shows the title it was left on for a full 10 s before rotating. */
    if (m_row < 0 && before_row >= 0)
        m_hero_since = m_now;
    return action;
}

std::string Home::card_url(const jf::Item &it) const
{
    if (it.external())
        return it.ext.thumb;
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
    if (it.external())
        return it.ext.backdrop;
    return m_client.image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920);
}

void Home::draw_card_art(const jf::Item &it, const gfx::Rect &r, float radius, float a) const
{
    if (m_discover) {   /* posters (the lift is in r already) */
        draw_poster(m_client, it, r, 0.f, a);
        return;
    }
    if (it.external()) {   /* Seerr's: its name on its colours, the picture over it, where it stands */
        draw_title_card(r, it.name, it.ext.tmdb_id, radius, a);
        art::draw(r, it.ext.thumb, "", 640, 360, radius, a, 0);
        draw_status_chip(r.x + 12, r.y + 12, it.ext.status, a);
        return;
    }
    art::draw(r, card_url(it), it.thumb_blurhash.empty() ? it.backdrop_blurhash : it.thumb_blurhash, 640, 360,
              radius, a);
}

void Home::draw_backdrop(float dt)
{
    /* Crossfade (900 ms) to the focused title's backdrop once it has loaded. Rows wait
     * 250 ms so fast browsing does not flicker; back on the hero it follows at once,
     * and quicker (its picture is already loaded). */
    const bool hero = m_row < 0;
    const jf::Item *f = focused_item();
    /* A title without a backdrop gets its own colours (its BlurHash) rather than
     * keeping the last one's picture: a layer keyed "colour:<id>". */
    std::string want = f ? backdrop_url(*f) : std::string();
    if (f && (want.empty() || art::failed(want)))
        want = "colour:" + f->id;
    if (!want.empty() && (m_bd.empty() || m_bd.back().url != want) && (hero || (m_now - m_focus_changed) > 0.25)) {
        /* A layer that has not started to show is replaced rather than stacked. */
        if (!m_bd.empty() && m_bd.size() > 1 && m_bd.back().mix.value < 0.02f)
            m_bd.pop_back();
        if (m_bd.empty() || m_bd.back().url != want) {
            const std::string &hash = !f->backdrop_blurhash.empty() ? f->backdrop_blurhash
                                      : !f->thumb_blurhash.empty()  ? f->thumb_blurhash
                                                                     : f->primary_blurhash;
            m_bd.push_back({want, hash, Anim(), m_now});
            m_bd.back().mix.snap(m_bd.size() == 1 ? 1.f : 0.f);
        }
        if (m_bd.size() > 4)   /* rare: a long chain of interrupted fades */
            m_bd.erase(m_bd.begin() + 1);
    }
    if (m_bd.size() > 1) {
        BackdropLayer &top = m_bd.back();
        const bool colour = top.url.compare(0, 7, "colour:") == 0;
        if (colour || art::get(top.url, 1920, 1080) || art::failed(top.url)) {
            top.mix.to(1.f);
            if (top.mix.step(dt, hero ? 8.f : 4.5f))
                m_animating = true;
            else   /* fully shown: everything under it is hidden */
                m_bd.erase(m_bd.begin(), m_bd.end() - 1);
        }
    }

    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    if (!m_bd.empty() && m_now - m_bd.back().since < 30.0)
        m_animating = true;   /* the drift moves */
    for (size_t i = 0; i < m_bd.size(); i++) {
        const float a = i == 0 ? 1.f : smoothstep(m_bd[i].mix.value);
        if (a <= 0.f)
            continue;
        const bool colour = m_bd[i].url.compare(0, 7, "colour:") == 0;
        if (const gfx::Texture *t = colour ? nullptr : art::get(m_bd[i].url, 1920, 1080))
            draw_drift(full, t, a, m_now - m_bd[i].since, m_bd[i].url);
        else if (i == 0 || colour || art::failed(m_bd[i].url))
            if (const gfx::Texture *ph = art::blurhash(m_bd[i].hash))
                gfx::image(full, ph, a, 0, true);
    }

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
        if (const gfx::Texture *t = art::get(logo, 800, 800)) {
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
    if (it.external()) {   /* where a Seerr title stands, always said (also "not requested") */
        const float sx = x > kPad ? x + 24 : x;
        gfx::fill({sx, meta_y - 15, 12, 12}, alpha(seerr_status_color(it.ext.status), a), 6);
        x = sx + 22 + gfx::text(sx + 22, meta_y, seerr_status_label(it.ext.status, true), {gfx::SemiBold, 22},
                                alpha(kText, a));
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
        /* "Spill av" / "Fortsett" and "Mer info": glass buttons, the focus drop on
         * the focused one, then their glyphs and labels. */
        const float by = bottom - 76;
        const bool resume = it.position_ticks > 0;
        const gfx::TextStyle bt{gfx::Bold, 26};
        if (m_row >= 0 || !m_focused)
            m_hero_drop.hide();
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1)
                m_hero_drop.draw(m_dt, a, &m_animating, 16);
            float bx = kPad;
            for (int b = 0; b < 2; b++) {
                const std::string label = b == 0 ? (resume ? T("Fortsett") : T("Spill av")) : T("Mer info");
                const float bw = gfx::text_width(label, bt) + 80 + 34;
                const bool focused = m_focused && m_row < 0 && m_hero_button == b;
                const gfx::Rect r{bx, by, bw, 76};
                bx += bw + 20;
                if (pass == 0) {
                    glass_panel(r, 16, a, false);
                    if (focused)
                        m_hero_drop.to(r, b, 0, by);
                    continue;
                }
                const uint32_t fg = kText;
                const float px = r.x + 40, py = r.y + r.h / 2;
                if (b == 0) {
                    for (int i = 0; i < 14; i++)   /* play glyph */
                        gfx::fill({px + i * 1.5f, py - (14 - i) * 1.0f, 1.5f, (14 - i) * 2.0f}, alpha(fg, a));
                } else {                         /* info glyph: a disc with an "i" */
                    gfx::fill({px - 2, py - 15, 30, 30}, alpha(fg, a), 15);
                    gfx::fill({px + 11.5f, py - 8, 3, 3}, alpha(0xff0b0b0fu, a), 1.5f);
                    gfx::fill({px + 11.5f, py - 3, 3, 11}, alpha(0xff0b0b0fu, a), 1.5f);
                }
                gfx::text(r.x + 72, r.y + r.h / 2 + 9, label, focused ? bt : gfx::TextStyle{gfx::SemiBold, 26}, alpha(fg, a));
            }
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

    /* Seerr's tab shows posters, as Seerr does: the backdrop is the background
     * only, never the card as well (Seerr has one of each per title). */
    const float cw = m_discover ? 236 : kCardW, ch = m_discover ? 354 : kCardH, rowh = m_discover ? 500 : kRowH;
    for (size_t r = 0; r < m_model.rows.size(); r++) {
        const HomeRow &row = m_model.rows[r];
        const float rel = (float)r - m_rows_y.value;
        const float ry = top + rel * rowh;
        float a = 1.f;
        if (rel < 0)
            a = std::max(0.f, 1.f + rel * 1.6f);   /* rows above fade out */
        if (a <= 0.f || ry > gfx::H + 40)
            continue;
        gfx::text(kPad, ry + 30, row.title, {gfx::Bold, 30}, alpha(0xebffffffu, a));

        /* Horizontal: the focused card sits at the left edge, until the row ends. */
        const int col = m_cols[r];
        const float max_scroll =
            std::max(0.f, (float)row.items.size() * (cw + kCardGap) - kCardGap - (gfx::W - 2 * kPad));
        m_scroll[r].to(std::min(max_scroll, (float)col * (cw + kCardGap)));
        if (m_scroll[r].step(dt, 12.f))
            m_animating = true;

        const float cy = ry + 52;
        int focus_i = -1;
        for (size_t i = 0; i < row.items.size(); i++) {
            const float cx = kPad + (float)i * (cw + kCardGap) - m_scroll[r].value;
            if (cx > gfx::W + 20 || cx + cw < -60)
                continue;
            const jf::Item &it = row.items[i];
            const bool focused = m_focused && (int)r == m_row && (int)i == col;
            if (focused) {
                focus_i = (int)i;
                continue;   /* drawn last, over its neighbours */
            }
            Anim &lift = m_lift[it.id + "@" + std::to_string(r)];
            lift.to(0.f);
            if (lift.step(dt, 14.f))
                m_animating = true;
            const float k = 1.f + 0.1f * lift.value;
            const gfx::Rect cr{cx - cw * (k - 1) / 2, cy - ch * (k - 1) / 2, cw * k, ch * k};
            draw_card_art(it, cr, kCardR * k, a);
            if (it.played_percent > 0 && it.played_percent < 100) {
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, cr.w - 36, 6}, alpha(0x47ffffffu, a), 3);
                gfx::fill({cr.x + 18, cr.y + cr.h - 22, (cr.w - 36) * (float)(it.played_percent / 100), 6},
                          alpha(0xffffffffu, a), 3);
            }
        }
        if (focus_i >= 0) {
            const jf::Item &it = row.items[focus_i];
            const float cx = kPad + (float)focus_i * (cw + kCardGap) - m_scroll[r].value;
            Anim &lift = m_lift[it.id + "@" + std::to_string(r)];
            lift.to(1.f);
            if (lift.step(dt, 14.f))
                m_animating = true;
            const float k = 1.f + 0.1f * lift.value;
            const gfx::Rect cr{cx - cw * (k - 1) / 2, cy - ch * (k - 1) / 2, cw * k, ch * k};
            gfx::shadow(cr, kCardR * k, 26, 0.3f * lift.value * a, 10 * lift.value);
            draw_card_art(it, cr, kCardR * k, a);
            if (m_discover)
                m_card = {cr, it.external() ? it.ext.poster : poster_url(m_client, it, 480), it.primary_blurhash,
                          kCardR * k};
            else
                m_card = {cr, card_url(it), it.thumb_blurhash.empty() ? it.backdrop_blurhash : it.thumb_blurhash,
                          kCardR * k};
            m_has_card = true;
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
    m_dt = dt;
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
    m_menu.draw(dt, &m_animating);

    if (art::animating())
        m_animating = true;
}

} // namespace ui
