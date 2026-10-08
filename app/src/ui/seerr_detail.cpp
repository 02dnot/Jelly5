/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Laid out as the library's detail page (ui/detail.cpp): the same scrims,
 * title, meta line, glass buttons and focus drop.
 */
#include "ui/seerr_detail.h"
#include "app/spawn.h"
#include "app/i18n.h"
#include "app/seerr_service.h"

#include "gfx/art.h"
#include "nuvio_input.h"
#include "qrcodegen.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace ui {
namespace {

std::string minutes_label(int min)
{
    if (min <= 0)
        return std::string();
    char b[32];
    if (min >= 60 && min % 60 == 0)
        std::snprintf(b, sizeof b, T("%d t"), min / 60);
    else if (min >= 60)
        std::snprintf(b, sizeof b, T("%d t %d min"), min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return b;
}

} // namespace

SeerrDetail::SeerrDetail(jf::Client &client, const jf::Item &item) : m_client(client), m_item(item) {}

void SeerrDetail::activate()
{
    std::shared_ptr<Data> d = m_data;
    std::shared_ptr<seerr::Client> c = seerr_service::client();
    const int id = m_item.ext.tmdb_id;
    const bool tv = m_item.type == "Series";
    if (!c) {
        std::lock_guard<std::mutex> g(d->lock);
        d->failed = !d->loaded;
        return;
    }
    const bool started = jelly5::spawn([d, c, id, tv] {
        seerr::Detail det;
        const bool ok = tv ? c->tv(id, &det) : c->movie(id, &det);
        const int status = c->last_status();
        if (!ok && (status == 401 || status == 403))
            seerr_service::session_lost(c.get());
        {
            std::lock_guard<std::mutex> g(d->lock);
            if (ok) {
                d->detail = std::move(det);
                d->loaded = true;
                d->loads++;
                d->failed = false;
            } else if (!d->loaded) {
                d->failed = true;
            }
        }
        if (!ok)
            return;
        /* The rows under the page, once the details show (on this thread: no
         * second one to start, which can fail and abort). */
        std::vector<seerr::Title> related[2] = {seerr_service::visible(c->related(id, tv, false)),
                                                seerr_service::visible(c->related(id, tv, true))};
        std::lock_guard<std::mutex> g(d->lock);
        for (int i = 0; i < 2; i++)
            if (!related[i].empty())
                d->related[i] = std::move(related[i]);
    });
    if (!started) {   /* no thread: the page says it could not load (a later visit tries again) */
        std::lock_guard<std::mutex> g(d->lock);
        d->failed = !d->loaded;
    }
}

int SeerrDetail::status() const
{
    if (!m_loaded)
        return seerr_service::status_of(m_item);
    jf::Item it = m_item;
    it.ext.status = (int)m_detail.title.status;
    return seerr_service::status_of(it);
}

bool SeerrDetail::can_request() const
{
    const seerr_service::Snapshot s = seerr_service::snapshot();
    if (!m_loaded || s.state != seerr_service::State::Ready)
        return false;
    /* Asked for here: not again until the page is read since (Seerr would say
     * it is requested already). Asked for on another screen: a film not at all;
     * a series' page may still offer the seasons nobody asked for. */
    if (m_loads < m_requested_until)
        return false;
    const int st = status();
    if ((st == (int)seerr::Status::Pending || st == (int)seerr::Status::Processing) && m_item.type != "Series")
        return false;
    return RequestSheet::offers(m_detail, s.user, s.settings);
}

std::vector<int> SeerrDetail::my_waiting_requests() const
{
    std::vector<int> ids;
    if (!m_loaded)
        return ids;
    const seerr_service::Snapshot s = seerr_service::snapshot();
    if (s.state != seerr_service::State::Ready)
        return ids;
    for (const seerr::Detail::Waiting &w : m_detail.waiting)   /* a series asked for season by season: several */
        if (w.user == s.user.id)
            ids.push_back(w.id);
    return ids;
}

std::vector<SeerrDetail::Button> SeerrDetail::buttons() const
{
    std::vector<Button> b;
    if (m_failed && !m_loaded) {   /* the details did not come: something to press */
        b.push_back(RetryButton);
        return b;
    }
    if (can_request())
        b.push_back(RequestButton);
    if (!my_waiting_requests().empty() || m_cancelling)
        b.push_back(CancelButton);
    const std::string &jid = m_loaded && !m_detail.title.jellyfin_id.empty() ? m_detail.title.jellyfin_id
                                                                             : m_item.ext.jellyfin_id;
    if (!jid.empty() && !m_item.ext.unseen)   /* not one this account found on its server */
        b.push_back(LibraryButton);
    if (m_loaded && !m_detail.trailer_url(seerr_service::snapshot().settings.youtube_url).empty())
        b.push_back(TrailerButton);
    return b;
}

void SeerrDetail::sync_button()
{
    const std::vector<Button> b = buttons();
    if (b.empty())
        return;
    if (m_button_id >= 0) {
        const auto it = std::find(b.begin(), b.end(), (Button)m_button_id);
        m_button = it != b.end() ? (int)(it - b.begin()) : 0;
    }
    m_button = std::max(0, std::min(m_button, (int)b.size() - 1));
    m_button_id = (int)b[m_button];
    if (m_button_id != CancelButton)
        m_cancel_armed = false;   /* the button went: its prompt with it */
}

Action SeerrDetail::input(uint32_t p)
{
    Action a;
    if (m_sheet.active()) {
        m_sheet.input(p);
        return a;
    }
    if (m_qr_open) {
        if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_CROSS))
            m_qr_open = false;
        return a;
    }
    if (!(p & NUVIO_BTN_CROSS))
        m_cancel_armed = false;   /* the focus moved (or back): "Trekk tilbake" asks again */
    sync_button();
    const std::vector<Button> bs = buttons();
    m_button = std::min(m_button, std::max(0, (int)bs.size() - 1));
    auto next_row = [&](int from, int d) {   /* the next row with titles, or -1 (the buttons) */
        for (int r = from + d; r >= 0 && r < 2; r += d)
            if (!m_rows[r].empty())
                return r;
        return d > 0 ? from : -1;
    };
    if (m_row >= 0) {   /* in "Anbefalt" or "Lignende" */
        std::vector<jf::Item> &row = m_rows[m_row];
        int &col = m_cols[m_row];
        if (p & NUVIO_BTN_CIRCLE) {
            m_row = -1;   /* back to the buttons first */
        } else if (p & NUVIO_BTN_UP) {
            m_row = next_row(m_row, -1);
        } else if (p & NUVIO_BTN_DOWN) {
            m_row = next_row(m_row, 1);
        } else if (p & NUVIO_BTN_LEFT) {
            if (col > 0)
                col--;
        } else if (p & NUVIO_BTN_RIGHT) {
            if (col + 1 < (int)row.size())
                col++;
        } else if ((p & NUVIO_BTN_CROSS) && col < (int)row.size()) {
            a.kind = Action::Open;   /* its own Seerr page, or the library's when the server has it */
            a.item = row[col];
        }
        return a;
    }
    if (p & NUVIO_BTN_DOWN) {
        m_row = next_row(-1, 1);
        if (m_row < 0 || m_rows[m_row].empty())
            m_row = -1;
        return a;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        a.kind = Action::Back;
    } else if (p & NUVIO_BTN_RIGHT) {
        if (m_button + 1 < (int)bs.size())
            m_button_id = (int)bs[++m_button];
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_LEFT) {
        if (m_button > 0)
            m_button_id = (int)bs[--m_button];
        else
            m_bump = true;
    } else if ((p & NUVIO_BTN_CROSS) && !bs.empty()) {
        const seerr_service::Snapshot s = seerr_service::snapshot();
        switch (bs[m_button]) {
        case RequestButton:
            m_sheet.open(m_detail, s.user, s.settings);
            break;
        case LibraryButton: {   /* the server's own page */
            a.kind = Action::Open;
            a.item.id = !m_detail.title.jellyfin_id.empty() ? m_detail.title.jellyfin_id : m_item.ext.jellyfin_id;
            a.item.type = m_item.type;
            a.item.name = m_item.name;
            break;
        }
        case CancelButton: {   /* withdraw the viewer's requests: a second press confirms */
            if (m_cancelling)
                break;   /* on its way */
            if (!m_cancel_armed) {
                m_cancel_armed = true;   /* until the focus moves (as removing an account) */
                break;
            }
            m_cancel_armed = false;
            const std::vector<int> ids = my_waiting_requests();
            std::shared_ptr<seerr::Client> c = seerr_service::client();
            if (ids.empty() || !c)
                break;
            std::shared_ptr<Data> d = m_data;
            m_cancelling = true;
            m_note = T("Trekker tilbake \xE2\x80\xA6");
            m_note_dot = kText3;
            m_note_at = m_now;
            const bool started = jelly5::spawn([c, ids, d] {   /* d, not this: the page may close meanwhile */
                bool ok = true;
                for (int id : ids)
                    ok = c->cancel_request(id) && ok;
                std::lock_guard<std::mutex> g(d->lock);
                d->cancel_result = ok ? 1 : -1;
            });
            if (!started) {
                std::lock_guard<std::mutex> g(d->lock);
                d->cancel_result = -1;
            }
            break;
        }
        case RetryButton: {   /* the details again */
            {
                std::lock_guard<std::mutex> g(m_data->lock);
                m_data->failed = false;
            }
            m_failed = false;
            activate();
            break;
        }
        case TrailerButton: {   /* a QR code of the link: the phone plays it */
            m_qr_url = m_detail.trailer_url(s.settings.youtube_url);
            m_qr.assign(qrcodegen_BUFFER_LEN_MAX, 0);
            std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_MAX);
            m_qr_ok = qrcodegen_encodeText(m_qr_url.c_str(), tmp.data(), m_qr.data(), qrcodegen_Ecc_MEDIUM,
                                           qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
            m_qr_open = true;
            break;
        }
        }
    }
    return a;
}

void SeerrDetail::draw_qr(float dt)
{
    m_qr_a.to(m_qr_open ? 1.f : 0.f);
    if (m_qr_a.step(dt, 14.f))
        m_animating = true;
    const float a = m_qr_a.value;
    if (a <= 0.01f)
        return;
    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x99000000u, a));
    const float w = 700, side = 400, h = 56 + 40 + 40 + 36 + side + 40 + 36 + 70;
    const gfx::Rect r{(gfx::W - w) / 2, (gfx::H - h) / 2 + 24 * (1.f - a), w, h};
    glass_panel(r, 28, a);
    float y = r.y + 56 + 34;
    gfx::text(r.x + w / 2, y, T("Trailer"), {gfx::Bold, 34}, alpha(kText, a), 1);
    y += 40;
    gfx::text(r.x + w / 2, y, T("Skann med telefonen for å se den der"), {gfx::Medium, 23, w - 80}, alpha(kText2, a), 1);
    y += 36;
    const gfx::Rect panel{r.x + (w - side) / 2, y, side, side};
    gfx::fill(panel, alpha(0xffffffffu, a), 20);
    if (m_qr_ok) {
        const int n = qrcodegen_getSize(m_qr.data());
        const float m = side / (float)(n + 8);   /* 4 modules of quiet zone round it */
        for (int qy = 0; qy < n; qy++)
            for (int qx = 0; qx < n; qx++)
                if (qrcodegen_getModule(m_qr.data(), qx, qy))
                    gfx::fill({panel.x + (qx + 4) * m, panel.y + (qy + 4) * m, m + 0.4f, m + 0.4f}, alpha(0xff0b0b0fu, a));
    }
    y += side + 40;
    gfx::text(r.x + w / 2, y, m_qr_url, {gfx::Regular, 20, w - 80}, alpha(kText3, a), 1);
    draw_pad_hints(r.x + w / 2, r.y + h - 42, {{PadButton::Circle, T("Lukk")}}, 1, 26, a);
}

void SeerrDetail::draw(double now, float dt)
{
    m_now = now;
    m_animating = false;
    if (m_opened < 0)
        m_opened = now;
    m_enter.to(1.f);
    if (m_enter.step(dt, 14.f))
        m_animating = true;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_loaded = m_data->loaded;
        m_failed = m_data->failed;
        m_loads = m_data->loads;
        if (m_loaded)
            m_detail = m_data->detail;
        if (m_note_after_load && m_data->loads >= m_note_after_load && m_loaded) {
            m_note_after_load = 0;
            seerr_service::note_status(m_item.ext.tmdb_id, m_item.type == "Series", (int)m_detail.title.status);
        }
        for (int i = 0; i < 2; i++)
            if (m_rows[i].size() != m_data->related[i].size()) {
                m_rows[i].clear();
                for (const seerr::Title &t : m_data->related[i])
                    m_rows[i].push_back(seerr_service::to_item(t));
            }
    }
    /* A request went through: say how, and read the page again (its status moved). */
    seerr::RequestResult done;
    if (m_sheet.take_done(&done)) {
        m_note = request_note(done);
        m_note_dot = request_note_dot(done);
        m_note_at = now;
        m_requested_until = m_loads + 1;
        activate();
    }
    int cancelled = 0;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        std::swap(cancelled, m_data->cancel_result);
    }
    if (cancelled)
        m_cancelling = false;
    if (cancelled > 0) {   /* the title's status as Seerr has it now (others may still ask for it): after the reload */
        std::lock_guard<std::mutex> g(m_data->lock);
        m_note_after_load = m_data->loads + 1;
    }
    if (cancelled) {   /* withdrawn (or not): say so, and read the page again */
        m_note = cancelled > 0 ? T("Forespørselen er trukket tilbake") : T("Kunne ikke trekke tilbake forespørselen");
        m_note_dot = cancelled > 0 ? 0xff30d158u : 0xffff9f0au;
        m_note_at = now;
        activate();
    }
    if (!m_loaded && !m_failed)
        m_animating = true;   /* the details are on their way: show them as they come */
    const seerr::Title &t = m_loaded ? m_detail.title : seerr::Title();
    const std::string &name = m_loaded ? t.name : m_item.name;
    const bool tv = m_item.type == "Series";
    const int status = this->status();

    /* Backdrop and scrims, as the library's page. */
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    /* The page's own backdrop at full size (the screen is 4K; w1280 was three times
     * enlarged); the row's lighter one until the details come. */
    std::string backdrop = m_loaded && !t.backdrop.empty() ? seerr_service::image_url(t.backdrop, "original")
                                                           : m_item.ext.backdrop;
    if (m_loaded && (art::failed(backdrop) || !art::get(backdrop, 1920, 1080)) && !m_item.ext.backdrop.empty() &&
        art::get(m_item.ext.backdrop, 1920, 1080))
        backdrop = m_item.ext.backdrop;   /* the full one still on its way: keep showing the row's */
    if (backdrop.empty() || art::failed(backdrop))
        draw_title_card(full, "", m_item.ext.tmdb_id, 0, 0.6f);   /* its colours, at least */
    else
        art::draw(full, backdrop, "", 1920, 1080, 0, 1.f, 0);
    gfx::fill_hgradient({0, 0, 576, gfx::H}, alpha(kBg, 0.92f), alpha(kBg, 0.72f));
    gfx::fill_hgradient({576, 0, 538, gfx::H}, alpha(kBg, 0.72f), alpha(kBg, 0.2f));
    gfx::fill_hgradient({1114, 0, 326, gfx::H}, alpha(kBg, 0.2f), alpha(kBg, 0.f));
    gfx::fill_vgradient({0, 486, gfx::W, 356}, alpha(kBg, 0.f), alpha(kBg, 0.85f));
    gfx::fill_vgradient({0, 842, gfx::W, 238}, alpha(kBg, 0.85f), kBg);

    /* The text waits for the details (0.6 s at most), then fades in as one. */
    if (m_loaded || m_failed || now - m_opened > 0.6)
        m_content.to(1.f);
    if (m_content.step(dt, 12.f) || m_content.target < 1.f)
        m_animating = true;
    gfx::push_opacity(m_content.value);

    /* The page scrolls up when the rows under it have the focus. */
    if (m_page.step(dt, 10.f))
        m_animating = true;
    const float off = m_page.value;
    gfx::text(kPad, 360 - off, name, {gfx::Bold, 84, 1500}, kText);

    /* Meta: rating, year, runtime or seasons, genres; then where it stands. */
    const float my = 440 - off;
    float x = kPad;
    const gfx::TextStyle meta{gfx::Medium, 24};
    bool first = true;
    auto sep = [&] {
        if (!first) {
            gfx::fill({x + 12, my - 10, 5, 5}, kText3, 2.5f);
            x += 29;
        }
        first = false;
    };
    const double vote = m_loaded ? t.vote : m_item.community_rating;
    if (vote > 0) {
        char r[16];
        std::snprintf(r, sizeof r, "\xE2\x98\x85 %.1f", vote);
        sep();
        x += gfx::text(x, my, r, meta, 0xfff5c518u);
    }
    const int year = m_loaded ? t.year : m_item.year;
    if (year) {
        sep();
        x += gfx::text(x, my, std::to_string(year), meta, kText2);
    }
    if (m_loaded && tv) {
        int n = 0;
        for (const seerr::Season &s : m_detail.seasons)
            n += s.number > 0;
        if (n) {
            sep();
            x += gfx::text(x, my, TN(n, "%d sesong", "%d sesonger"), meta, kText2);
        }
    } else if (m_loaded && m_detail.runtime > 0) {
        sep();
        x += gfx::text(x, my, minutes_label(m_detail.runtime), meta, kText2);
    }
    if (m_loaded && !m_detail.genres.empty()) {
        sep();
        std::string g = m_detail.genres[0];
        for (size_t i = 1; i < m_detail.genres.size() && i < 3; i++)
            g += " \xC2\xB7 " + m_detail.genres[i];
        x += gfx::text(x, my, g, meta, kText2);
    }
    {   /* where it stands, always said here (also "not requested") */
        const char *label = seerr_status_label(status, true);
        const gfx::TextStyle ss{gfx::SemiBold, 22};
        const float sx = first ? x : x + 24;
        gfx::fill({sx, my - 15, 12, 12}, seerr_status_color(status), 6);
        gfx::text(sx + 22, my, label, ss, kText);
    }

    float y = my + 52;
    if (m_loaded && !m_detail.tagline.empty()) {
        gfx::text(kPad, y, m_detail.tagline, {gfx::Medium, 26, 900}, 0xe6f5f5f7u);
        y += 44;
    }
    gfx::text(kPad, y, m_loaded ? t.overview : m_item.overview, {gfx::Regular, 26, 860, 3, 37.7f}, kText2);

    /* Buttons: glass panes, the focus drop over them, then the labels. */
    const float by = 668 - off;
    sync_button();
    const std::vector<Button> bs = buttons();
    m_button = std::min(m_button, std::max(0, (int)bs.size() - 1));
    const bool sheet = m_sheet.active() || m_qr_open;
    if (bs.empty() || sheet || !m_focused || m_row >= 0)
        m_drop.hide();
    const gfx::TextStyle st{gfx::Bold, 26};
    auto label_of = [](Button b) -> std::string {
        return b == RequestButton  ? T("Be om")
               : b == LibraryButton ? T("Se i biblioteket")
               : b == CancelButton  ? T("Trekk tilbake forespørselen")
               : b == RetryButton   ? T("Prøv igjen")
                                    : T("Trailer");
    };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            m_drop.draw(dt, 1.f, &m_animating, 16);
        float bx = kPad;
        for (size_t i = 0; i < bs.size(); i++) {
            const std::string label = label_of(bs[i]);
            const bool icon = bs[i] == RequestButton;
            const float w = gfx::text_width(label, st) + 80 + (icon ? 34 : 0);
            const gfx::Rect r{bx, by, w, 76};
            bx += w + 20;
            if (pass == 0) {
                glass_panel(r, 16, 1.f, false);
                if ((int)i == m_button && !sheet && m_row < 0)
                    m_drop.to(r, (int)bs[i], 0, by);
                continue;
            }
            const float cy = r.y + r.h / 2;
            if (icon) {   /* a plus */
                gfx::fill({r.x + 40, cy - 1.75f, 20, 3.5f}, kText, 1.5f);
                gfx::fill({r.x + 48.25f, cy - 10, 3.5f, 20}, kText, 1.5f);
            }
            gfx::text(r.x + 40 + (icon ? 34 : 0), cy + 9, label,
                      (int)i == m_button && !sheet ? st : gfx::TextStyle{gfx::SemiBold, 26}, kText);
        }
    }

    /* Under the buttons: why there is no request button, the cast, the seasons. */
    float cy = by + (bs.empty() ? 30 : 76 + 46);
    const seerr_service::Snapshot snap = seerr_service::snapshot();
    if (m_cancel_armed) {   /* as removing an account: a red line says what the next press does */
        gfx::text(kPad, cy, T("Trykk \xE2\x9C\x95 igjen for å trekke tilbake forespørselen"), {gfx::Medium, 22, 1200},
                  0xffff6b6bu);
        cy += 46;
    }
    std::string why;
    if (snap.state != seerr_service::State::Ready && !m_loaded)   /* why it cannot load, first */
        why = snap.state == seerr_service::State::Unreachable ? T("Seerr svarer ikke")
                                                              : T("Ikke pålogget Seerr \xE2\x80\x93 se Innstillinger");
    else if (!m_loaded && !m_failed)
        why = T("Henter \xE2\x80\xA6");   /* the details on their way (no buttons yet) */
    else if (m_failed)
        why = T("Kunne ikke hente detaljene fra Seerr");
    else if (snap.state != seerr_service::State::Ready)
        why = snap.state == seerr_service::State::Unreachable ? T("Seerr svarer ikke")
                                                              : T("Ikke pålogget Seerr \xE2\x80\x93 se Innstillinger");
    else if (m_loaded && !snap.user.can_request(tv) &&
             (status == (int)seerr::Status::Unknown || status == (int)seerr::Status::Deleted))
        why = tv ? T("Seerr-kontoen din kan ikke be om serier") : T("Seerr-kontoen din kan ikke be om filmer");
    if (!why.empty()) {
        gfx::text(kPad, cy, why, {gfx::Medium, 22, 1200}, kText3);
        cy += 46;
    }
    if (m_loaded && !m_detail.cast.empty()) {
        std::string with;
        for (size_t i = 0; i < m_detail.cast.size() && i < 4; i++)
            with += (i ? ", " : "") + m_detail.cast[i];
        const float hw = gfx::text(kPad, cy, T("Med:"), {gfx::SemiBold, 21}, kText2);
        gfx::text(kPad + hw + 8, cy, with, {gfx::Regular, 21, 1100}, kText3);
        cy += 32;
    }
    if (m_loaded && tv && !m_detail.seasons.empty()) {
        /* Each season and where it stands: a dot of its colour (asked for: requested). */
        cy += 26;
        const gfx::TextStyle cs{gfx::SemiBold, 19};
        float sx = kPad;
        for (const seerr::Season &s : m_detail.seasons) {
            if (s.number == 0 && s.status == seerr::Status::Unknown && !s.requested)
                continue;   /* specials nobody asked for */
            char num[48];
            std::snprintf(num, sizeof num, T("Sesong %d"), s.number);
            const int st_of = s.status == seerr::Status::Unknown && s.requested ? (int)seerr::Status::Processing
                                                                                 : (int)s.status;
            /* Not by colour alone: the status in words, but for "there" and "not asked for". */
            std::string b = num;
            if (st_of != (int)seerr::Status::Available && st_of != (int)seerr::Status::Unknown)
                b += std::string(" \xC2\xB7 ") + seerr_status_label(st_of);
            const float w = 18 + 10 + 10 + gfx::text_width(b, cs) + 18;
            if (sx + w > gfx::W - kPad) {
                sx = kPad;
                cy += 48;
            }
            const gfx::Rect chip{sx, cy - 26, w, 38};
            gfx::fill(chip, 0x66101014u, 19);
            gfx::rim(chip, 19, 0.6f);
            gfx::fill({sx + 18, cy - 12, 10, 10}, seerr_status_color(st_of), 5);
            gfx::text(sx + 38, cy - 1, b, cs, kText2);
            sx += w + 12;
        }
    }
    /* "Anbefalt" and "Lignende": posters, as on Seerr's own pages. */
    {
        const float pw = 210, ph = 315, gap = 32, rowh = 470;
        float ry = cy + 90;
        const float rows_top = ry + off;   /* unscrolled */
        int shown = 0;
        for (int r = 0; r < 2; r++) {
            if (m_rows[r].empty())
                continue;
            if (m_row == r)   /* the focused row's posters under the top third of the screen */
                m_page.to(std::max(0.f, rows_top + shown * rowh - 300));
            shown++;
            gfx::text(kPad, ry, r == 0 ? T("Anbefalt") : T("Mer som dette"), {gfx::Bold, 30}, alpha(0xebffffffu, 1.f));
            const int col = std::min(m_cols[r], (int)m_rows[r].size() - 1);
            const float max_scroll = std::max(0.f, m_rows[r].size() * (pw + gap) - gap - (gfx::W - 2 * kPad));
            m_rscroll[r].to(std::min(max_scroll, std::max(0.f, (col - 1) * (pw + gap))));
            if (m_rscroll[r].step(dt, 12.f))
                m_animating = true;
            const float py = ry + 42;
            for (int pass = 0; pass < 2; pass++)   /* the focused poster last, over its neighbours */
                for (size_t i = 0; i < m_rows[r].size(); i++) {
                    const bool focus = m_focused && m_row == r && (int)i == col;
                    if (focus != (pass == 1))
                        continue;
                    const float px = kPad + i * (pw + gap) - m_rscroll[r].value;
                    if (px > gfx::W + 20 || px + pw < -60)
                        continue;
                    Anim &lift = m_lift[std::to_string(r) + "@" + std::to_string(m_rows[r][i].ext.tmdb_id)];
                    lift.to(focus ? 1.f : 0.f);
                    if (lift.step(dt, 14.f))
                        m_animating = true;
                    draw_poster(m_client, m_rows[r][i], {px, py, pw, ph}, lift.value, 1.f);
                }
            ry += rowh;
        }
        if (m_row < 0)
            m_page.to(0.f);
    }
    gfx::pop_opacity();

    /* How a request went: a note at the top for a few seconds. */
    m_note_a.to(now - m_note_at < 4.0 ? 1.f : 0.f);
    if (m_note_a.step(dt, 10.f) || m_note_a.target > 0)
        m_animating = true;
    draw_note(m_note, m_note_a.value, m_note_dot);

    m_sheet.draw(dt, &m_animating);
    draw_qr(dt);
    if (art::animating())
        m_animating = true;
}

} // namespace ui
