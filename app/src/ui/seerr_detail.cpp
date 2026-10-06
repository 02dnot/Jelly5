/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Laid out as the library's detail page (ui/detail.cpp): the same scrims,
 * title, meta line, glass buttons and focus drop.
 */
#include "ui/seerr_detail.h"
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
    if (min >= 60)
        std::snprintf(b, sizeof b, T("%d t %d min"), min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", min);
    return b;
}

} // namespace

SeerrDetail::SeerrDetail(const jf::Item &item) : m_item(item) {}

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
    std::thread([d, c, id, tv] {
        seerr::Detail det;
        const bool ok = tv ? c->tv(id, &det) : c->movie(id, &det);
        const int status = c->last_status();
        if (!ok && (status == 401 || status == 403))
            seerr_service::session_lost();
        std::lock_guard<std::mutex> g(d->lock);
        if (ok) {
            d->detail = std::move(det);
            d->loaded = true;
            d->failed = false;
        } else if (!d->loaded) {
            d->failed = true;
        }
    }).detach();
}

bool SeerrDetail::can_request() const
{
    const seerr_service::Snapshot s = seerr_service::snapshot();
    return m_loaded && s.state == seerr_service::State::Ready && RequestSheet::offers(m_detail, s.user, s.settings);
}

std::vector<SeerrDetail::Button> SeerrDetail::buttons() const
{
    std::vector<Button> b;
    if (can_request())
        b.push_back(RequestButton);
    const std::string &jid = m_loaded && !m_detail.title.jellyfin_id.empty() ? m_detail.title.jellyfin_id
                                                                             : m_item.ext.jellyfin_id;
    if (!jid.empty())
        b.push_back(LibraryButton);
    if (m_loaded && !m_detail.trailer_url(seerr_service::snapshot().settings.youtube_url).empty())
        b.push_back(TrailerButton);
    return b;
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
    const std::vector<Button> bs = buttons();
    m_button = std::min(m_button, std::max(0, (int)bs.size() - 1));
    if (p & NUVIO_BTN_CIRCLE) {
        a.kind = Action::Back;
    } else if (p & NUVIO_BTN_RIGHT) {
        if (m_button + 1 < (int)bs.size())
            m_button++;
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_LEFT) {
        if (m_button > 0)
            m_button--;
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
        if (m_loaded)
            m_detail = m_data->detail;
    }
    /* A request went through: say how, and read the page again (its status moved). */
    seerr::RequestResult done;
    if (m_sheet.take_done(&done)) {
        m_note = request_note(done);
        m_note_at = now;
        activate();
    }
    if (!m_loaded && !m_failed)
        m_animating = true;   /* the details are on their way: show them as they come */
    const seerr::Title &t = m_loaded ? m_detail.title : seerr::Title();
    const std::string &name = m_loaded ? t.name : m_item.name;
    const bool tv = m_item.type == "Series";
    const int status = m_loaded ? (int)t.status : m_item.ext.status;

    /* Backdrop and scrims, as the library's page. */
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    std::string backdrop = m_item.ext.backdrop;
    if (backdrop.empty() && m_loaded)
        backdrop = seerr_service::image_url(t.backdrop, "original");   /* the screen is 4K: w1280 was three times enlarged */
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

    gfx::text(kPad, 360, name, {gfx::Bold, 84, 1500}, kText);

    /* Meta: rating, year, runtime or seasons, genres; then where it stands. */
    const float my = 440;
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
            x += gfx::text(x, my, std::to_string(n) + (n == 1 ? T(" sesong") : T(" sesonger")), meta, kText2);
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
    const float by = 668;
    const std::vector<Button> bs = buttons();
    m_button = std::min(m_button, std::max(0, (int)bs.size() - 1));
    const bool sheet = m_sheet.active() || m_qr_open;
    if (bs.empty() || sheet || !m_focused)
        m_drop.hide();
    const gfx::TextStyle st{gfx::Bold, 26};
    auto label_of = [](Button b) -> std::string {
        return b == RequestButton ? T("Be om") : b == LibraryButton ? T("Se i biblioteket") : T("Trailer");
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
                if ((int)i == m_button && !sheet)
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
    std::string why;
    if (m_failed)
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
            char b[48];
            std::snprintf(b, sizeof b, T("Sesong %d"), s.number);
            const int st_of = s.status == seerr::Status::Unknown && s.requested ? (int)seerr::Status::Processing
                                                                                 : (int)s.status;
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
    gfx::pop_opacity();

    /* How a request went: a note at the top for a few seconds. */
    m_note_a.to(now - m_note_at < 4.0 ? 1.f : 0.f);
    if (m_note_a.step(dt, 10.f) || m_note_a.target > 0)
        m_animating = true;
    draw_note(m_note, m_note_a.value);

    m_sheet.draw(dt, &m_animating);
    draw_qr(dt);
    if (art::animating())
        m_animating = true;
}

} // namespace ui
