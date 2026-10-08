/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/person.h"
#include "app/i18n.h"
#include "app/spawn.h"
#include "app/i18n_cldr.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace ui {
namespace {

constexpr float kPosterW = 220, kPosterH = 330, kGap = 32;
constexpr float kRowsTop = 640, kRowH = 470;

/* "1971-03-12T00:00:00.0000000Z" -> "12. mars 1971" (the interface's language) */
std::string born_label(const std::string &iso)
{
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
        return std::string();
    return i18n::long_date(y, m, d);
}

} // namespace

Person::Person(jf::Client &client, const jf::Item &person) : m_client(client)
{
    m_data->c.person = person;
    m_view = m_data->c;
}

void Person::activate()
{
    std::shared_ptr<Data> d = m_data;
    jf::Client *c = &m_client;
    const std::string id = m_view.person.id;
    jelly5::spawn([d, c, id] {   /* no thread: the page loads when it is opened again */
        jf::Item person;
        std::vector<jf::Item> movies, series;
        bool got = false;
        jelly5::run_all({[&] { got = c->item(id, &person); },
                         [&] { movies = c->person_items(id, "Movie", 60); },
                         [&] { series = c->person_items(id, "Series", 60); }});
        std::lock_guard<std::mutex> g(d->lock);
        if (got)
            d->c.person = person;
        d->c.movies = std::move(movies);
        d->c.series = std::move(series);
        d->c.loaded = true;
    });
}

Action Person::input(uint32_t p)
{
    Action a;
    const std::vector<jf::Item> *rows[2] = {&m_view.movies, &m_view.series};
    std::vector<int> present;
    for (int r = 0; r < 2; r++)
        if (!rows[r]->empty())
            present.push_back(r);
    if (p & NUVIO_BTN_CIRCLE) {
        a.kind = Action::Back;
        return a;
    }
    if (present.empty())
        return a;
    if (std::find(present.begin(), present.end(), m_row) == present.end())
        m_row = present[0];
    const int n = (int)rows[m_row]->size();
    if (p & NUVIO_BTN_RIGHT)
        m_cols[m_row] = std::min(n - 1, m_cols[m_row] + 1);
    else if (p & NUVIO_BTN_LEFT)
        m_cols[m_row] = std::max(0, m_cols[m_row] - 1);
    else if ((p & NUVIO_BTN_DOWN) && m_row == present.front() && present.size() > 1)
        m_row = present.back();
    else if ((p & NUVIO_BTN_UP) && m_row == present.back() && present.size() > 1)
        m_row = present.front();
    else if (p & NUVIO_BTN_CROSS) {
        a.kind = Action::Open;
        a.item = (*rows[m_row])[m_cols[m_row]];
    }
    return a;
}

void Person::draw(double now, float dt)
{
    m_animating = false;
    if (m_opened < 0)
        m_opened = now;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_view = m_data->c;
    }
    m_enter.to(1.f);
    if (m_enter.step(dt, 14.f))
        m_animating = true;
    if (m_view.loaded || now - m_opened > 0.6)
        m_content.to(1.f);
    if (m_content.step(dt, 12.f) || m_content.target < 1.f)
        m_animating = true;

    const jf::Item &p = m_view.person;
    const std::vector<jf::Item> *rows[2] = {&m_view.movies, &m_view.series};
    const char *titles[2] = {"Filmer", "Serier"};

    /* The background takes its colour from the focused title (or the first). */
    const jf::Item *focus_item = nullptr;
    if (!rows[m_row]->empty())
        focus_item = &(*rows[m_row])[std::min(m_cols[m_row], (int)rows[m_row]->size() - 1)];
    else if (!m_view.movies.empty())
        focus_item = &m_view.movies[0];
    else if (!m_view.series.empty())
        focus_item = &m_view.series[0];
    if (focus_item)
        m_ambient.set(focus_item->backdrop_blurhash.empty() ? focus_item->primary_blurhash
                                                            : focus_item->backdrop_blurhash,
                      now);
    m_ambient.draw(dt, 0.7f, &m_animating);

    /* Page: slides just enough for the focused row - the lifted card, its
     * title and year - to sit fully on screen with a margin below. */
    float row_top = kRowsTop;
    for (int r = 0; r < m_row && r < 2; r++)
        if (!rows[r]->empty())
            row_top += kRowH;
    const float row_bottom = row_top + 30 + kPosterH * 1.1f + 110;
    m_page.to(rows[m_row]->empty() ? 0.f : std::max(0.f, row_bottom - (gfx::H - 40)));
    if (m_page.step(dt, 10.f))
        m_animating = true;
    const float off = m_page.value;

    gfx::push_opacity(m_content.value);
    /* Portrait, name, birth, biography. */
    const gfx::Rect portrait{kPad, 130 - off, 280, 420};
    const std::string url = p.primary_tag.empty() ? std::string()
                                                  : m_client.image_url(p.id, "Primary", p.primary_tag, 560);
    gfx::shadow(portrait, 22, 36, 0.35f, 12);
    if (url.empty()) {
        gfx::fill_vgradient(portrait, 0xff2a2a35u, 0xff16161cu, 22);
        gfx::text(portrait.x + portrait.w / 2, portrait.y + portrait.h / 2 + 30, first_letter(p.name),
                  {gfx::Bold, 110}, kText3, 1);
    } else {
        art::draw(portrait, url, p.primary_blurhash, 560, 840, 22);
    }
    const float tx = kPad + 280 + 64;
    gfx::text(tx, 230 - off, p.name, {gfx::Bold, 72, gfx::W - tx - kPad}, kText);
    std::string meta;
    const std::string born = born_label(p.premiere_date);
    if (!born.empty())
        meta = T("Født ") + born;
    if (!p.locations.empty())
        meta += (meta.empty() ? "" : "  \xC2\xB7  ") + p.locations[0];
    const size_t total = m_view.movies.size() + m_view.series.size();
    if (m_view.loaded)
        meta += (meta.empty() ? "" : "  \xC2\xB7  ") + TN((int)total, "%d tittel her", "%d titler her");
    gfx::text(tx, 290 - off, meta, {gfx::Medium, 26, gfx::W - tx - kPad}, kText2);
    gfx::text(tx, 350 - off, p.overview.empty() ? (m_view.loaded ? T("Ingen biografi.") : "") : p.overview,
              {gfx::Regular, 25, gfx::W - tx - kPad, 6, 36}, kText2);

    /* Rows of posters. */
    float y = kRowsTop - off;
    for (int r = 0; r < 2; r++) {
        const std::vector<jf::Item> &items = *rows[r];
        if (items.empty())
            continue;
        gfx::text(kPad, y, titles[r], {gfx::Bold, 30}, 0xebffffffu);
        const int col = std::min(m_cols[r], (int)items.size() - 1);
        const float max_scroll = std::max(0.f, items.size() * (kPosterW + kGap) - kGap - (gfx::W - 2 * kPad));
        m_scroll[r].to(std::min(max_scroll, std::max(0.f, (col - 1) * (kPosterW + kGap))));
        if (m_scroll[r].step(dt, 12.f))
            m_animating = true;
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < items.size(); i++) {
                const bool f = r == m_row && (int)i == col;
                if ((pass == 0) == f)
                    continue;
                const float x = kPad + i * (kPosterW + kGap) - m_scroll[r].value;
                if (x > gfx::W || x + kPosterW < -40)
                    continue;
                const float lift = m_lifts.step(items[i].id + std::to_string(r), f, dt, &m_animating);
                draw_poster(m_client, items[i], {x, y + 30, kPosterW, kPosterH}, lift, 1.f);
                if (lift > 0.01f && items[i].year)
                    gfx::text(x - 11, y + 30 + kPosterH * 1.05f + 68, std::to_string(items[i].year), {gfx::Medium, 18},
                              alpha(kText3, lift));
            }
        y += kRowH;
    }
    if (m_view.loaded && total == 0)
        gfx::text(kPad, kRowsTop + 40 - off, T("Ingen titler med ") + p.name + T(" i biblioteket."), {gfx::Medium, 26}, kText3);
    gfx::pop_opacity();
    if (art::animating())
        m_animating = true;
}

} // namespace ui
