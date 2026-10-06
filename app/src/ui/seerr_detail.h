/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title from Seerr that the library does not have (or has in part): its
 * backdrop, name, facts and where it stands, and what can be done: request
 * it (RequestSheet), see what the library has of it, or watch the trailer
 * on a phone (a QR code of the link Seerr gives: the console itself never
 * goes to YouTube). Drawn like the library's own detail pages; Circle closes
 * a sheet, then the page.
 */
#pragma once

#include "seerr/seerr_client.h"
#include "ui/screen.h"
#include "ui/seerr_request.h"

#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <vector>

namespace ui {

class SeerrDetail : public Screen {
public:
    /* item: a Seerr title (jf::Item::external()). */
    SeerrDetail(jf::Client &client, const jf::Item &item);

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return 0.f; }
    float enter() const override { return m_enter.value; }
    bool modal() const override { return m_sheet.active() || m_qr_open; }

private:
    enum Button { RequestButton, LibraryButton, TrailerButton };
    struct Data {
        std::mutex lock;
        bool loaded = false, failed = false;
        seerr::Detail detail;
        std::vector<seerr::Title> related[2];   /* recommendations, similar */
    };
    std::vector<Button> buttons() const;
    bool can_request() const;
    void draw_qr(float dt);

    jf::Client &m_client;                   /* for the posters of titles the library has */
    jf::Item m_item;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    bool m_loaded = false, m_failed = false;   /* this frame's copy */
    seerr::Detail m_detail;
    int m_button = 0;
    RequestSheet m_sheet;
    bool m_qr_open = false;
    std::string m_qr_url;
    std::vector<uint8_t> m_qr;              /* the encoded QR code */
    bool m_qr_ok = false;
    Anim m_qr_a;
    std::string m_note;                     /* how a request went, shown for a few seconds */
    double m_note_at = -100, m_now = 0, m_opened = -1;
    Anim m_enter, m_content, m_note_a;
    Drop m_drop;
    bool m_animating = false;
    /* Under the page: "Anbefalt" and "Lignende" (posters). m_row -1: the buttons. */
    std::vector<jf::Item> m_rows[2];
    int m_row = -1, m_cols[2] = {0, 0};
    Anim m_page, m_rscroll[2];
    std::map<std::string, Anim> m_lift;
};

} // namespace ui
