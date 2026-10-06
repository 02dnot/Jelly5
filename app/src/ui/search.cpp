/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/search.h"
#include "app/i18n.h"
#include "app/seerr_service.h"

#include "nuvio_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
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
constexpr float kSeerrHead = 70;      /* Seerr's heading, between the library's rows and its own */
constexpr float kNoneH = 80;          /* "no results" in the library, above Seerr's */

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

/* Jellyfin ids with or without their dashes, in any case. */
std::string id_key(const std::string &id)
{
    std::string out;
    for (char c : id)
        if (c != '-')
            out += (char)std::tolower((unsigned char)c);
    return out;
}

} // namespace

int Search::Grid::row_of(int i) const { return i < library ? i / kResCols : library_rows() + (i - library) / kResCols; }

int Search::Grid::col_of(int i) const { return (i < library ? i : i - library) % kResCols; }

int Search::Grid::row_length(int row) const
{
    if (row < library_rows())
        return std::min(kResCols, library - row * kResCols);
    return std::max(0, std::min(kResCols, seerr - (row - library_rows()) * kResCols));
}

int Search::Grid::index_at(int row, int col) const
{
    const int n = row_length(row);
    if (n <= 0)
        return -1;
    col = std::min(col, n - 1);
    return row < library_rows() ? row * kResCols + col : library + (row - library_rows()) * kResCols + col;
}

Search::Search(jf::Client &client) : m_client(client) {}

void Search::activate()
{
    m_in_results = false;   /* arriving (by △ or the tab): straight onto the keyboard */
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

/* Seerr's results for the query the library's answer, without the titles the
 * library shows already (the same item, or the same TMDB film or series). */
void Search::merge(Data &d)
{
    d.seerr_shown.clear();
    if (d.for_seerr != d.for_query || d.for_query.empty())
        return;
    std::set<std::string> ids, tmdb;
    for (const jf::Item &it : d.items) {
        ids.insert(id_key(it.id));
        if (!it.tmdb_id.empty())
            tmdb.insert((it.type == "Series" ? "tv:" : "movie:") + it.tmdb_id);
    }
    for (const jf::Item &s : d.seerr) {
        if (!s.ext.jellyfin_id.empty() && ids.count(id_key(s.ext.jellyfin_id)))
            continue;
        if (tmdb.count((s.type == "Series" ? "tv:" : "movie:") + s.tmdb_id))
            continue;
        d.seerr_shown.push_back(s);
    }
}

Search::Grid Search::grid()
{
    std::lock_guard<std::mutex> g(m_data->lock);
    return {(int)m_data->items.size(), (int)m_data->seerr_shown.size()};
}

void Search::more_seerr()
{
    std::shared_ptr<Data> d = m_data;
    std::string q;
    unsigned seq;
    int page;
    {
        std::lock_guard<std::mutex> g(d->lock);
        const int library = (int)d->items.size(), shown = (int)d->seerr_shown.size();
        if (!m_in_results || m_result < library || m_result < library + shown - 8 || !d->seerr_more ||
            d->seerr_more_loading || d->seerr_pending || d->for_seerr != d->for_query || d->for_seerr.empty())
            return;
        d->seerr_more_loading = true;
        q = d->for_seerr;
        seq = d->seq;
        page = d->seerr_page + 1;
    }
    std::shared_ptr<seerr::Client> sc = seerr_service::client();
    if (!sc) {
        std::lock_guard<std::mutex> g(d->lock);
        d->seerr_more_loading = false;
        return;
    }
    std::thread([d, sc, q, seq, page] {
        const std::vector<seerr::Title> found = sc->search(q, page);
        std::lock_guard<std::mutex> g(d->lock);
        d->seerr_more_loading = false;
        if (seq != d->seq)
            return;   /* the query moved on */
        std::set<std::string> have;
        for (const jf::Item &it : d->seerr)
            have.insert(it.type + std::to_string(it.ext.tmdb_id));
        size_t added = 0;
        for (const seerr::Title &t : found) {
            jf::Item it = seerr_service::to_item(t);
            if (have.insert(it.type + std::to_string(it.ext.tmdb_id)).second) {
                d->seerr.push_back(std::move(it));
                added++;
            }
        }
        d->seerr_page = page;
        d->seerr_more = added > 0;
        merge(*d);
    }).detach();
}

void Search::start_search()
{
    std::shared_ptr<Data> d = m_data;
    jf::Client *c = &m_client;
    const std::string q = m_query;
    /* Seerr alongside the library (on its own thread: the library's results never wait for it). */
    std::shared_ptr<seerr::Client> sc = q.empty() ? nullptr : seerr_service::client();
    unsigned seq;
    {
        std::lock_guard<std::mutex> g(d->lock);
        seq = ++d->seq;
        d->seerr_pending = sc != nullptr;
        d->seerr_failed = false;
        d->seerr_page = 1;
        d->seerr_more = true;
    }
    if (sc) {
        std::thread([d, sc, q, seq] {
            std::vector<jf::Item> found;
            for (const seerr::Title &t : sc->search(q))
                found.push_back(seerr_service::to_item(t));
            const int status = sc->last_status();
            if (status == 401 || status == 403)
                seerr_service::session_lost();
            std::lock_guard<std::mutex> g(d->lock);
            if (seq != d->seq)
                return;   /* the query moved on */
            d->seerr = std::move(found);
            d->for_seerr = q;
            d->seerr_pending = false;
            d->seerr_failed = status < 200 || status >= 300;
            merge(*d);
        }).detach();
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
            std::vector<jf::Item> found = c->search(q, "Movie,Series,MusicArtist,MusicAlbum,Episode", 36);
            pt.join();
            for (const char *type : {"Movie|Series", "Person", "MusicArtist", "MusicAlbum", "Episode"}) {
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
        merge(*d);
    }).detach();
}

Action Search::input(uint32_t p)
{
    Action a;
    const Grid gr = grid();
    const int count = gr.count();
    if (p & NUVIO_BTN_SQUARE) {
        type("\b");
        return a;
    }
    if (m_in_results && count == 0)
        m_in_results = false;   /* the results went while the focus was on them */
    if (m_in_results) {
        m_result = std::min(m_result, count - 1);
        const int row = gr.row_of(m_result), col = gr.col_of(m_result);
        if (p & NUVIO_BTN_RIGHT) {
            if (col + 1 < gr.row_length(row))
                m_result++;
        } else if (p & NUVIO_BTN_LEFT) {
            if (col > 0)
                m_result--;
            else
                m_in_results = false;
        } else if (p & NUVIO_BTN_DOWN) {
            if (row + 1 < gr.rows())
                m_result = gr.index_at(row + 1, col);
        } else if (p & NUVIO_BTN_UP) {
            if (row > 0)
                m_result = gr.index_at(row - 1, col);
            else
                a.kind = Action::ToNav;
        } else if (p & NUVIO_BTN_CIRCLE) {
            m_in_results = false;
        } else if (p & NUVIO_BTN_CROSS) {
            std::lock_guard<std::mutex> g(m_data->lock);
            const int n = (int)m_data->items.size();
            if (m_result < n) {
                a.kind = Action::Open;
                a.item = m_data->items[m_result];
            } else if (m_result - n < (int)m_data->seerr_shown.size()) {
                a.kind = Action::Open;   /* the server's page when it has the title, else Seerr's */
                a.item = m_data->seerr_shown[m_result - n];
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
            m_result = std::max(0, gr.index_at(std::min(row, gr.rows() - 1), 0));
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
    more_seerr();
    std::vector<jf::Item> items, seerr;
    std::string for_query;
    bool seerr_pending, seerr_failed, seerr_asked;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        items = m_data->items;
        seerr = m_data->seerr_shown;
        for_query = m_data->for_query;
        seerr_pending = m_data->seerr_pending;
        seerr_asked = m_data->for_seerr == for_query;
        seerr_failed = m_data->seerr_failed && seerr_asked;
    }
    /* Seerr came up (signed in) after this query was typed: ask it too. */
    if (!for_query.empty() && for_query == m_query && !m_pending && !seerr_pending && !seerr_asked &&
        m_seerr_retried != for_query && seerr_service::ready()) {
        m_seerr_retried = for_query;
        start_search();
    }
    const Grid gr{(int)items.size(), (int)seerr.size()};
    m_result = std::min(m_result, std::max(0, gr.count() - 1));
    if (gr.count() == 0)
        m_in_results = false;
    auto item_at = [&](int i) -> const jf::Item & { return i < gr.library ? items[i] : seerr[i - gr.library]; };
    const jf::Item *f = m_in_results && gr.count() ? &item_at(m_result) : nullptr;
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
        gfx::text(kKbX, qy, T("Filmer, serier, personer, musikk"), {gfx::Medium, 36, 640}, kText3);
    else
        qx += gfx::text(kKbX, qy, m_query, {gfx::Bold, 52, 600}, kText);
    if (std::fmod(now, 1.0) < 0.55)
        gfx::fill({qx + 6, qy - 44, 3, 52}, 0xff00a4dcu);
    gfx::fill({kKbX, qy + 22, 7 * (kKeyW + kKeyGap) - kKeyGap, 2}, 0x33ffffffu);

    /* Keyboard (tvOS): the letters on the page itself, no panel and no key boxes;
     * the focus drop on the key, then the labels. */
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < kNumKeys; k++) {
            int r, c, s;
            key_cell(k, &r, &c, &s);
            const bool focus = m_focused && !m_in_results && k == m_key;
            const float w = s * kKeyW + (s - 1) * kKeyGap;
            const gfx::Rect rr{kKbX + c * (kKeyW + kKeyGap), kKbY + r * (kKeyH + kKeyGap), w, kKeyH};
            if (pass == 0) {
                if (focus)
                    m_drop.to(rr, k);
                if (k + 1 == kNumKeys) {
                    if (m_in_results || !m_focused)
                        m_drop.hide();
                    m_drop.draw(dt, 1.f, &anim, 12);
                }
                continue;
            }
            const char *label = k == kSpace ? T("mellomrom") : k == kDelete ? T("\xE2\x8C\xAB slett") : kKeys[k];
            gfx::text(rr.x + rr.w / 2, rr.y + rr.h / 2 + 9, label,
                      {focus ? gfx::Bold : gfx::SemiBold, s > 1 ? 22.f : 26.f}, focus ? kText : kText2, 1);
        }
    draw_pad_hints(kKbX, kKbY + 7 * (kKeyH + kKeyGap) + 22, {{PadButton::Square, T("Slett")}}, 0, 26);

    /* Results: the library's, then Seerr's under its own heading (when it is on). */
    std::string heading = T("Forslag");
    if (!for_query.empty()) {   /* each language its own quotation marks */
        char b[256];
        std::snprintf(b, sizeof b, T("Treff for \xC2\xAB%s\xC2\xBB"), for_query.c_str());
        heading = b;
    }
    gfx::text(kResX, 236, heading, {gfx::Bold, 26, 1000}, kText2);
    const seerr_service::State seerr_state = seerr_service::snapshot().state;
    const bool seerr_on = !for_query.empty() && seerr_state != seerr_service::State::Off;
    const float library_h = gr.library ? gr.library_rows() * kResPitch : for_query.empty() ? 0.f : kNoneH;
    auto row_top = [&](int row) {   /* within the results, before scrolling */
        return row < gr.library_rows() ? row * kResPitch : library_h + kSeerrHead + (row - gr.library_rows()) * kResPitch;
    };
    const int row = m_in_results ? gr.row_of(m_result) : 0;
    /* The focused row up at the top of the results, whole (Seerr's first with its heading). */
    m_scroll.to(row > 0 ? row_top(row) - (row == gr.library_rows() ? kSeerrHead : 0.f) - 14.f : 0.f);
    if (m_scroll.step(dt, 11.f))
        anim = true;
    gfx::push_scissor({kResX - 40, 256, gfx::W - kResX + 40, gfx::H - 256});
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < gr.count(); i++) {
            const bool focus = m_in_results && i == m_result;
            if ((pass == 0) == focus)
                continue;
            const float y = kResY + 26 + row_top(gr.row_of(i)) - m_scroll.value;
            if (y > gfx::H || y + kPosterH + 60 < 200)
                continue;
            const jf::Item &it = item_at(i);
            const float lift = m_lifts.step(it.id, focus, dt, &anim);
            draw_poster(m_client, it, {kResX + gr.col_of(i) * (kPosterW + kResGap), y, kPosterW, kPosterH}, lift, 1.f);
        }
    if (seerr_on) {
        const float hy = kResY + 26 + library_h - m_scroll.value;   /* the top of Seerr's block */
        gfx::text(kResX, hy + 40, T("Fra Seerr"), {gfx::Bold, 26}, kText2);
        std::string note;
        if (seerr_state == seerr_service::State::Unreachable || (seerr_failed && seerr.empty()))
            note = T("Seerr svarer ikke");
        else if (seerr_state == seerr_service::State::SignedOut)
            note = T("Ikke pålogget Seerr \xE2\x80\x93 se Innstillinger");
        else if (seerr_state == seerr_service::State::Connecting || seerr_pending)
            note = T("S\xC3\xB8ker \xE2\x80\xA6");
        else if (seerr.empty())
            note = T("Ingenting mer p\xC3\xA5 Seerr");   /* all it found is in the library */
        if (!note.empty())
            gfx::text(kResX, hy + kSeerrHead + 40, note, {gfx::Medium, 24}, kText3);
    }
    if (items.empty() && !for_query.empty())
        gfx::text(kResX, kResY + 60 - m_scroll.value, T("Ingen treff"), {gfx::Medium, 26}, kText3);
    gfx::pop_scissor();
    (void)anim;
}

} // namespace ui
