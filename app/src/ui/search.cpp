/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/search.h"

#include "nuvio_input.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace ui {
namespace {

/* a-z, æ ø å, 0-9 (UTF-8), then the two wide keys. */
const char *const kKeys[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
                             "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "\xC3\xA6",
                             "\xC3\xB8", "\xC3\xA5", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
constexpr int kLetters = 39;
constexpr int kSpace = 39, kDelete = 40, kNumKeys = 41;
constexpr int kKbCols = 7;
constexpr float kKbX = 120, kKbY = 300, kKeyW = 72, kKeyH = 66, kKeyGap = 10;
constexpr int kResCols = 4;
constexpr float kResX = 790, kResY = 260, kPosterW = 240, kPosterH = 360, kResGap = 40, kResPitch = 440;

/* Grid position of a key: the wide keys take three columns each. */
void key_cell(int k, int *row, int *col, int *span)
{
    if (k < kLetters) {
        *row = k / kKbCols;
        *col = k % kKbCols;
        *span = 1;
    } else if (k == kSpace) {                            /* after "0", to the row's end */
        *row = kLetters / kKbCols;
        *col = kLetters % kKbCols;
        *span = kKbCols - *col;
    } else {                                             /* delete: a row of its own */
        *row = kLetters / kKbCols + 1;
        *col = 0;
        *span = 3;
    }
}

int key_at(int row, int col)
{
    for (int k = 0; k < kNumKeys; k++) {
        int r, c, s;
        key_cell(k, &r, &c, &s);
        if (r == row && col >= c && col < c + s)
            return k;
    }
    return -1;
}

} // namespace

Search::Search(jf::Client &client) : m_client(client) {}

void Search::activate()
{
    if (!m_suggested) {
        m_suggested = true;
        start_search();
    }
}

void Search::type(const std::string &key)
{
    if (key == "\b") {
        if (m_query.empty())
            return;
        /* Drop one UTF-8 character. */
        size_t n = m_query.size() - 1;
        while (n > 0 && ((unsigned char)m_query[n] & 0xC0) == 0x80)
            n--;
        m_query.erase(n);
    } else {
        if (m_query.size() > 60)
            return;
        m_query += key;
    }
    m_changed = m_now;
    m_pending = true;
}

void Search::start_search()
{
    std::shared_ptr<Data> d = m_data;
    jf::Client *c = &m_client;
    const std::string q = m_query;
    unsigned seq;
    {
        std::lock_guard<std::mutex> g(d->lock);
        seq = ++d->seq;
    }
    std::thread([d, c, q, seq] {
        std::vector<jf::Item> r;
        if (q.empty()) {
            r = c->library("", "Movie,Series", "Random", false, 0, 16).items;
        } else {
            /* Titles and people side by side (people take the server longer); shown
             * titles first, then people, albums and episodes. */
            std::vector<jf::Item> people;
            std::thread pt([&] { people = c->search(q, "Person", 12); });
            std::vector<jf::Item> found = c->search(q, "Movie,Series,MusicAlbum,Episode", 36);
            pt.join();
            for (const char *type : {"Movie|Series", "Person", "MusicAlbum", "Episode"}) {
                const std::string t = type;
                for (const jf::Item &it : t == "Person" ? people : found)
                    if (t.find(it.type) != std::string::npos)
                        r.push_back(it);
            }
        }
        std::lock_guard<std::mutex> g(d->lock);
        if (seq != d->seq)
            return;   /* the query moved on */
        d->items = std::move(r);
        d->for_query = q;
    }).detach();
}

Action Search::input(uint32_t p)
{
    Action a;
    int count;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        count = (int)m_data->items.size();
    }
    if (p & NUVIO_BTN_SQUARE) {
        type("\b");
        return a;
    }
    if (m_in_results) {
        const int row = m_result / kResCols, col = m_result % kResCols;
        if (p & NUVIO_BTN_RIGHT) {
            if (col + 1 < kResCols && m_result + 1 < count)
                m_result++;
        } else if (p & NUVIO_BTN_LEFT) {
            if (col > 0)
                m_result--;
            else
                m_in_results = false;
        } else if (p & NUVIO_BTN_DOWN) {
            if (m_result + kResCols < count)
                m_result += kResCols;
        } else if (p & NUVIO_BTN_UP) {
            if (row > 0)
                m_result -= kResCols;
            else
                a.kind = Action::ToNav;
        } else if (p & NUVIO_BTN_CIRCLE) {
            m_in_results = false;
        } else if (p & NUVIO_BTN_CROSS) {
            std::lock_guard<std::mutex> g(m_data->lock);
            if (m_result < (int)m_data->items.size()) {
                a.kind = Action::Open;
                a.item = m_data->items[m_result];
            }
        }
        return a;
    }
    int row, col, span;
    key_cell(m_key, &row, &col, &span);
    if (p & NUVIO_BTN_RIGHT) {
        const int k = key_at(row, col + span);
        if (k >= 0)
            m_key = k;
        else if (count > 0) {
            m_in_results = true;
            m_result = std::min(count - 1, std::max(0, std::min(row, (count - 1) / kResCols)) * kResCols);
        }
    } else if (p & NUVIO_BTN_LEFT) {
        const int k = key_at(row, col - 1);
        if (k >= 0)
            m_key = k;
    } else if (p & NUVIO_BTN_DOWN) {
        int k = key_at(row + 1, col);
        if (k < 0)
            k = key_at(row + 1, 0);
        if (k >= 0)
            m_key = k;
    } else if (p & NUVIO_BTN_UP) {
        const int k = key_at(row - 1, col);
        if (k >= 0)
            m_key = k;
        else
            a.kind = Action::ToNav;
    } else if (p & NUVIO_BTN_CIRCLE) {
        a.kind = Action::ToNav;
    } else if (p & NUVIO_BTN_CROSS) {
        type(m_key == kSpace ? " " : m_key == kDelete ? "\b" : kKeys[m_key]);
    }
    return a;
}

void Search::draw(double now, float dt)
{
    m_now = now;
    if (m_pending && now - m_changed > 0.3) {
        m_pending = false;
        start_search();
    }
    std::vector<jf::Item> items;
    std::string for_query;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        items = m_data->items;
        for_query = m_data->for_query;
    }
    m_result = std::min(m_result, std::max(0, (int)items.size() - 1));
    if (items.empty())
        m_in_results = false;
    const jf::Item *f = m_in_results && !items.empty() ? &items[m_result] : nullptr;
    if (f)
        m_ambient.set(f->backdrop_blurhash.empty() ? f->primary_blurhash : f->backdrop_blurhash, now);
    else if (!items.empty())
        m_ambient.set(items[0].backdrop_blurhash.empty() ? items[0].primary_blurhash : items[0].backdrop_blurhash, now);
    bool anim = false;
    m_ambient.draw(dt, 0.7f, &anim);

    /* The query line with a blinking caret. */
    const float qy = 236;
    float qx = kKbX;
    if (m_query.empty())
        gfx::text(kKbX, qy, "Filmer, serier, personer, musikk", {gfx::Medium, 36, 640}, kText3);
    else
        qx += gfx::text(kKbX, qy, m_query, {gfx::Bold, 52, 600}, kText);
    if (std::fmod(now, 1.0) < 0.55)
        gfx::fill({qx + 6, qy - 44, 3, 52}, 0xff00a4dcu);
    gfx::fill({kKbX, qy + 22, 7 * (kKeyW + kKeyGap) - kKeyGap, 2}, 0x33ffffffu);

    /* Keyboard. */
    for (int k = 0; k < kNumKeys; k++) {
        int r, c, s;
        key_cell(k, &r, &c, &s);
        const bool focus = !m_in_results && k == m_key;
        const float w = s * kKeyW + (s - 1) * kKeyGap;
        const float lift = m_lifts.step("key" + std::to_string(k), focus, dt, &anim);
        const float kk = 1.f + 0.1f * lift;
        const gfx::Rect base{kKbX + c * (kKeyW + kKeyGap), kKbY + r * (kKeyH + kKeyGap), w, kKeyH};
        const gfx::Rect rr{base.x - w * (kk - 1) / 2, base.y - kKeyH * (kk - 1) / 2, w * kk, kKeyH * kk};
        gfx::fill(rr, focus ? 0xfff5f5f7u : 0x0fffffffu, 12);
        const char *label = k == kSpace ? "mellomrom" : k == kDelete ? "\xE2\x8C\xAB slett" : kKeys[k];
        gfx::text(rr.x + rr.w / 2, rr.y + rr.h / 2 + 9, label, {gfx::SemiBold, s > 1 ? 22.f : 26.f},
                  focus ? 0xff0b0b0fu : kText2, 1);
    }
    gfx::text(kKbX, kKbY + 7 * (kKeyH + kKeyGap) + 30, "\xE2\x96\xA2 sletter", {gfx::Medium, 20}, kText3);

    /* Results. */
    const std::string heading = for_query.empty() ? "Forslag" : "Treff for \xC2\xAB" + for_query + "\xC2\xBB";
    gfx::text(kResX, 236, heading, {gfx::Bold, 26, 1000}, kText2);
    const int row = m_in_results ? m_result / kResCols : 0;
    m_scroll.to(std::max(0.f, (float)(row - 1) * kResPitch));
    if (m_scroll.step(dt, 11.f))
        anim = true;
    gfx::push_scissor({kResX - 40, 256, gfx::W - kResX + 40, gfx::H - 256});
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < (int)items.size(); i++) {
            const bool focus = m_in_results && i == m_result;
            if ((pass == 0) == focus)
                continue;
            const float y = kResY + 26 + (i / kResCols) * kResPitch - m_scroll.value;
            if (y > gfx::H || y + kPosterH + 60 < 200)
                continue;
            const float lift = m_lifts.step(items[i].id, focus, dt, &anim);
            draw_poster(m_client, items[i], {kResX + (i % kResCols) * (kPosterW + kResGap), y, kPosterW, kPosterH},
                        lift, 1.f);
        }
    gfx::pop_scissor();
    if (items.empty() && !for_query.empty())
        gfx::text(kResX, 320, "Ingen treff", {gfx::Medium, 26}, kText3);
    (void)anim;
}

} // namespace ui
