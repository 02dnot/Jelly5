/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/seerr_request.h"
#include "app/spawn.h"
#include "app/i18n.h"
#include "app/seerr_service.h"

#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace ui {
namespace {

constexpr float kW = 940, kRowH = 66, kRowGap = 6, kMaxRows = 7;

std::string season_name(const seerr::Season &s)
{
    if (!s.name.empty())
        return s.name;
    char b[48];
    std::snprintf(b, sizeof b, T("Sesong %d"), s.number);
    return b;
}

/* A box, ticked or not; dim when it cannot change. */
void checkbox(float x, float cy, bool on, bool enabled, float a)
{
    const gfx::Rect b{x, cy - 15, 30, 30};
    const uint32_t ink = enabled ? kText : kText3;
    if (on) {
        gfx::fill(b, alpha(enabled ? 0xff0a84ffu : 0x66ffffffu, a), 8);
        draw_check(b.x + 15, b.y + 15, 10, alpha(0xffffffffu, a));
    } else {
        gfx::fill(b, alpha(ink, a * 0.9f), 8);
        gfx::fill({b.x + 2.5f, b.y + 2.5f, 25, 25}, alpha(0xff1c1c22u, a), 6.5f);
    }
}

} // namespace

bool RequestSheet::requestable(const seerr::Season &s, const seerr::PublicSettings &ps)
{
    if ((s.number == 0 && !ps.special_episodes) || s.requested)
        return false;
    return s.status == seerr::Status::Unknown || s.status == seerr::Status::Deleted;
}

std::string request_note(const seerr::RequestResult &r)
{
    using R = seerr::RequestResult;
    return r.outcome == R::Approved  ? T("Forespørselen er godkjent \xE2\x80\x93 den hentes snart")
           : r.outcome == R::Pending ? T("Forespørselen er sendt \xE2\x80\x93 venter på godkjenning")
                                     : T("Ingenting å be om: alt er der eller forespurt allerede");
}

bool RequestSheet::offers(const seerr::Detail &d, const seerr::User &user, const seerr::PublicSettings &ps)
{
    const seerr::Title &t = d.title;
    if (t.id <= 0 || t.status == seerr::Status::Blocklisted || !user.can_request(t.tv))
        return false;
    if (!t.tv)
        return t.status == seerr::Status::Unknown || t.status == seerr::Status::Deleted;
    for (const seerr::Season &s : d.seasons)
        if (requestable(s, ps))
            return true;
    return false;
}

void RequestSheet::open(const seerr::Detail &d, const seerr::User &user, const seerr::PublicSettings &ps)
{
    m_detail = d;
    m_user = user;
    m_settings = ps;
    m_shared = std::make_shared<Shared>();   /* an earlier sheet's request finishes on its own */
    m_servers.clear();
    m_picked.assign(d.seasons.size(), false);
    for (size_t i = 0; i < d.seasons.size(); i++)
        m_picked[i] = requestable(d.seasons[i], ps);
    m_server = m_profile = m_folder = 0;
    m_defaults_set = m_touched = false;
    m_button = 0;
    m_error.clear();
    m_done = false;
    m_open = true;
    m_alpha.to(1.f);
    m_scroll.snap(0);
    build_rows();
    m_focus = (int)m_rows.size() - 1;   /* on "Request": a film is one press away */

    /* The servers with their profiles and folders (only for those who may pick
     * them) and the quota, behind the sheet. */
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<seerr::Client> c = seerr_service::client();
    const bool tv = d.title.tv, advanced = user.advanced();
    const int uid = user.id;
    if (!c) {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->loaded = true;
        return;
    }
    const bool started = jelly5::spawn([sh, c, tv, advanced, uid] {
        std::vector<seerr::Server> list;
        if (advanced)
            for (const seerr::Server &s : c->servers(tv)) {
                if (s.is4k)
                    continue;   /* 4K requests are not offered */
                seerr::Server full;
                if (c->server_details(tv, s.id, &full))
                    list.push_back(full);
            }
        seerr::Quota mq, tq;
        const bool have_quota = c->quota(uid, &mq, &tq);
        std::lock_guard<std::mutex> g(sh->lock);
        sh->servers = std::move(list);
        sh->quota = tv ? tq : mq;
        sh->have_quota = have_quota;
        sh->loaded = true;
    });
    if (!started) {   /* no thread: the sheet works with Seerr's defaults, without the quota */
        std::lock_guard<std::mutex> g(sh->lock);
        sh->loaded = true;
    }
}

void RequestSheet::build_rows()
{
    const Kind focused = m_focus >= 0 && m_focus < (int)m_rows.size() ? m_rows[m_focus].kind : Buttons;
    m_rows.clear();
    if (m_detail.title.tv) {
        m_rows.push_back({AllSeasons});
        if (m_settings.partial_requests)
            for (size_t i = 0; i < m_detail.seasons.size(); i++)
                if (m_detail.seasons[i].number > 0 || m_settings.special_episodes)
                    m_rows.push_back({OneSeason, (int)i});
    }
    if (m_user.advanced() && !m_servers.empty()) {
        if (m_servers.size() > 1)
            m_rows.push_back({ServerRow});
        m_rows.push_back({ProfileRow});
        m_rows.push_back({FolderRow});
    }
    m_rows.push_back({Buttons});
    m_focus = (int)m_rows.size() - 1;
    if (focused != Buttons)
        for (size_t i = 0; i < m_rows.size(); i++)
            if (m_rows[i].kind == focused) {
                m_focus = (int)i;
                break;
            }
}

const seerr::Server *RequestSheet::server() const
{
    return m_server >= 0 && m_server < (int)m_servers.size() ? &m_servers[m_server] : nullptr;
}

std::string RequestSheet::message(const seerr::RequestResult &r) const
{
    using R = seerr::RequestResult;
    switch (r.outcome) {
    case R::Duplicate: return T("Allerede forespurt");
    case R::QuotaReached: return T("Kvoten for forespørsler er nådd");
    case R::NotAllowed: return T("Du har ikke lov til å be om dette");
    case R::SignedOut: return T("Seerr-økten var utløpt \xE2\x80\x93 logger på igjen, prøv på nytt");
    case R::Unreachable: return T("Seerr svarer ikke");
    default: return T("Forespørselen mislyktes");
    }
}

void RequestSheet::send()
{
    seerr::RequestOptions o;
    o.tmdb_id = m_detail.title.id;
    o.tv = m_detail.title.tv;
    o.tvdb_id = m_detail.tvdb_id;
    if (o.tv) {
        bool all = true;
        for (size_t i = 0; i < m_detail.seasons.size(); i++) {
            if (!requestable(m_detail.seasons[i], m_settings))
                continue;
            if (m_picked[i])
                o.seasons.push_back(m_detail.seasons[i].number);
            else
                all = false;
        }
        if (o.seasons.empty()) {
            m_error = T("Velg minst én sesong");
            return;
        }
        if (all || !m_settings.partial_requests)
            o.seasons.clear();   /* every missing season: Seerr's "all" */
    }
    if (m_touched)
        if (const seerr::Server *s = server()) {   /* chosen: say so (else Seerr decides) */
            o.server_id = s->id;
            if (m_profile < (int)s->profiles.size())
                o.profile_id = s->profiles[m_profile].id;
            if (m_folder < (int)s->folders.size())
                o.root_folder = s->folders[m_folder].path;
        }
    std::shared_ptr<seerr::Client> c = seerr_service::client();
    if (!c) {
        m_error = T("Seerr svarer ikke");
        return;
    }
    m_error.clear();
    std::shared_ptr<Shared> sh = m_shared;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->sending = true;
    }
    const bool started = jelly5::spawn([sh, c, o] {
        const seerr::RequestResult r = c->request(o);
        if (r.outcome == seerr::RequestResult::SignedOut)
            seerr_service::session_lost();
        std::lock_guard<std::mutex> g(sh->lock);
        sh->sending = false;
        sh->sent = true;
        sh->result = r;
    });
    if (!started) {   /* no thread: nothing was sent; say so, the viewer can press again */
        std::lock_guard<std::mutex> g(sh->lock);
        sh->sending = false;
        m_error = T("Kunne ikke sende forespørselen");
    }
}

bool RequestSheet::take_done(seerr::RequestResult *out)
{
    if (!m_done)
        return false;
    m_done = false;
    *out = m_result;
    return true;
}

void RequestSheet::input(uint32_t p)
{
    bool sending;
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        sending = m_shared->sending;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        if (!sending) {   /* a request on its way is waited for */
            m_open = false;
            m_alpha.to(0.f);
        }
        return;
    }
    if (sending || m_rows.empty())
        return;
    auto enabled = [&](int i) {
        const Row &r = m_rows[i];
        return r.kind != OneSeason || requestable(m_detail.seasons[r.season], m_settings);
    };
    const Row row = m_rows[m_focus];
    const int dir = (p & NUVIO_BTN_RIGHT) ? 1 : (p & NUVIO_BTN_LEFT) ? -1 : 0;
    if (p & NUVIO_BTN_DOWN) {
        for (int i = m_focus + 1; i < (int)m_rows.size(); i++)
            if (enabled(i)) {
                m_focus = i;
                break;
            }
    } else if (p & NUVIO_BTN_UP) {
        for (int i = m_focus - 1; i >= 0; i--)
            if (enabled(i)) {
                m_focus = i;
                break;
            }
    } else if (dir && row.kind == Buttons) {
        m_button = dir > 0 ? 1 : 0;
    } else if ((dir || (p & NUVIO_BTN_CROSS)) && (row.kind == ServerRow || row.kind == ProfileRow || row.kind == FolderRow)) {
        const int step = dir ? dir : 1;
        auto cycle = [step](int i, int n) { return n > 0 ? ((i + step) % n + n) % n : 0; };
        const seerr::Server *s = server();
        if (row.kind == ServerRow) {
            m_server = cycle(m_server, (int)m_servers.size());
            m_defaults_set = false;   /* the new server's defaults */
        } else if (row.kind == ProfileRow && s) {
            m_profile = cycle(m_profile, (int)s->profiles.size());
        } else if (row.kind == FolderRow && s) {
            m_folder = cycle(m_folder, (int)s->folders.size());
        }
        m_touched = true;
    } else if (p & NUVIO_BTN_CROSS) {
        if (row.kind == AllSeasons) {
            bool all = true;
            for (size_t i = 0; i < m_picked.size(); i++)
                if (requestable(m_detail.seasons[i], m_settings) && !m_picked[i])
                    all = false;
            for (size_t i = 0; i < m_picked.size(); i++)
                m_picked[i] = !all && requestable(m_detail.seasons[i], m_settings);
        } else if (row.kind == OneSeason) {
            m_picked[row.season] = !m_picked[row.season];
        } else if (row.kind == Buttons) {
            if (m_button == 0) {
                send();
            } else {
                m_open = false;
                m_alpha.to(0.f);
            }
        }
    }
}

void RequestSheet::draw(float dt, bool *animating)
{
    if (m_alpha.step(dt, 14.f) && animating)
        *animating = true;
    const float a = m_alpha.value;
    if (a <= 0.01f)
        return;

    /* What arrived behind the sheet. */
    bool loaded, sending, have_quota;
    seerr::Quota quota;
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        loaded = m_shared->loaded;
        sending = m_shared->sending;
        have_quota = m_shared->have_quota;
        quota = m_shared->quota;
        if (loaded && m_servers.empty() && !m_shared->servers.empty()) {
            m_servers = m_shared->servers;
            m_server = 0;
            for (size_t i = 0; i < m_servers.size(); i++)
                if (m_servers[i].is_default)
                    m_server = (int)i;
            build_rows();
        }
        if (m_shared->sent) {
            m_shared->sent = false;
            const seerr::RequestResult &r = m_shared->result;
            using R = seerr::RequestResult;
            if (r.outcome == R::Approved || r.outcome == R::Pending || r.outcome == R::NothingToRequest) {
                m_result = r;
                m_done = true;
                if (r.outcome != R::NothingToRequest)   /* shown everywhere at once, before Seerr is read again */
                    seerr_service::note_status(m_detail.title.id, m_detail.title.tv,
                                               (int)(r.outcome == R::Approved ? seerr::Status::Processing
                                                                              : seerr::Status::Pending));
                m_open = false;
                m_alpha.to(0.f);
            } else {
                m_error = message(r);
            }
        }
    }
    if (!m_defaults_set && server()) {   /* Seerr's defaults for the server (anime's for anime) */
        const seerr::Server &s = *server();
        const int pid = m_detail.anime && s.anime_profile_id ? s.anime_profile_id : s.profile_id;
        const std::string &dir = m_detail.anime && !s.anime_folder.empty() ? s.anime_folder : s.folder;
        m_profile = m_folder = 0;
        for (size_t i = 0; i < s.profiles.size(); i++)
            if (s.profiles[i].id == pid)
                m_profile = (int)i;
        for (size_t i = 0; i < s.folders.size(); i++)
            if (s.folders[i].path == dir)
                m_folder = (int)i;
        m_defaults_set = true;
    }
    if ((sending || !loaded) && animating)
        *animating = true;   /* what is on its way shows as it comes */

    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x99000000u, a));

    /* Info lines under the rows: who approves it, the quota, how the last try went. */
    std::vector<std::pair<std::string, uint32_t>> notes;
    const bool tv = m_detail.title.tv;
    if (m_user.auto_approved(tv))
        notes.push_back({T("Godkjennes automatisk og sendes rett videre"), kText2});
    else
        notes.push_back({T("En administrator må godkjenne den"), kText2});
    if (have_quota && quota.limit > 0) {
        char b[160];
        std::snprintf(b, sizeof b, T("%d av %d forespørsler brukt (siste %d dager)"), quota.used, quota.limit,
                      quota.days);
        notes.push_back({b, quota.restricted ? 0xffff9f0au : kText2});
    }
    if (m_user.advanced() && !loaded)
        notes.push_back({T("Henter valg \xE2\x80\xA6"), kText3});
    if (sending)
        notes.push_back({T("Sender \xE2\x80\xA6"), kText});
    else if (!m_error.empty())
        notes.push_back({m_error, 0xffff9f0au});

    const int list_rows = (int)m_rows.size() - 1;   /* the buttons sit below the list */
    const float list_h = std::min((float)list_rows, kMaxRows) * (kRowH + kRowGap);
    const float h = 56 + 46 + 40 + 24 + list_h + (list_rows ? 18 : 0) + notes.size() * 34 + 24 + 76 + 48;
    const float rise = 24 * (1.f - a);
    const gfx::Rect r{(gfx::W - kW) / 2, (gfx::H - h) / 2 + rise, kW, h};
    glass_panel(r, 28, a);

    float y = r.y + 56 + 34;
    char title[256];   /* each language its own quotation marks */
    std::snprintf(title, sizeof title, T("Be om \xC2\xAB%s\xC2\xBB"), m_detail.title.name.c_str());
    gfx::text(r.x + 48, y, title, {gfx::Bold, 34, kW - 96}, alpha(kText, a));
    y += 40;
    std::string sub = tv ? T("Serie") : T("Film");
    if (m_detail.title.year)
        sub += "  \xC2\xB7  " + std::to_string(m_detail.title.year);
    gfx::text(r.x + 48, y, sub, {gfx::Medium, 22}, alpha(kText3, a));
    y += 24 + 24;

    /* The list (seasons, then the advanced choices), scrolling when long. */
    const float list_y = y;
    const int focus_row = m_rows[m_focus].kind == Buttons ? -1 : m_focus;
    if (focus_row >= 0) {
        const float fy = focus_row * (kRowH + kRowGap);
        if (fy - m_scroll.target > list_h - kRowH)
            m_scroll.to(fy - list_h + kRowH + kRowGap);
        if (fy < m_scroll.target)
            m_scroll.to(fy);
    }
    if (m_scroll.step(dt, 12.f) && animating)
        *animating = true;
    gfx::push_scissor({r.x, list_y - 4, r.w, list_h + 8});
    const gfx::Rect *drop_on = nullptr;
    gfx::Rect focus_rect{0, 0, 0, 0};
    if (focus_row >= 0) {
        focus_rect = {r.x + 30, list_y + focus_row * (kRowH + kRowGap) - m_scroll.value, kW - 60, kRowH};
        drop_on = &focus_rect;
    }
    if (drop_on) {
        m_drop.to(*drop_on, m_focus, r.x, r.y);
        m_drop.draw(dt, a, animating, 14);
    }
    for (int i = 0; i < list_rows; i++) {
        const Row &row = m_rows[i];
        const float ry = list_y + i * (kRowH + kRowGap) - m_scroll.value;
        if (ry > list_y + list_h || ry + kRowH < list_y)
            continue;
        const bool focus = i == m_focus;
        const float cy = ry + kRowH / 2;
        const gfx::TextStyle ls{focus ? gfx::Bold : gfx::SemiBold, 25};
        const uint32_t fg = focus ? kText : kText2;
        const float lx = r.x + 30 + 26, rx = r.x + kW - 30 - 26;
        if (row.kind == AllSeasons || row.kind == OneSeason) {
            bool on, en;
            std::string label, right;
            if (row.kind == AllSeasons) {
                on = true;
                int n = 0;
                for (size_t k = 0; k < m_picked.size(); k++)
                    if (requestable(m_detail.seasons[k], m_settings)) {
                        n++;
                        on = on && m_picked[k];
                    }
                en = n > 0;
                on = on && n > 0;
                label = m_settings.partial_requests ? T("Alle sesonger") : T("Alle manglende sesonger");
                char b[48];
                std::snprintf(b, sizeof b, n == 1 ? T("%d sesong") : T("%d sesonger"), n);
                right = b;
            } else {
                const seerr::Season &s = m_detail.seasons[row.season];
                en = requestable(s, m_settings);
                on = en ? (bool)m_picked[row.season] : s.status == seerr::Status::Available;
                label = season_name(s);
                if (en) {
                    char b[48];
                    std::snprintf(b, sizeof b, T("%d episoder"), s.episodes);
                    right = b;
                } else {
                    right = s.requested && s.status == seerr::Status::Unknown ? T("Forespurt")
                                                                              : seerr_status_label((int)s.status, true);
                }
            }
            checkbox(lx, cy, on, en, a);
            gfx::text(lx + 46, cy + 9, label, {ls.weight, ls.size, kW - 380}, alpha(en ? fg : kText3, a));
            gfx::text(rx, cy + 8, right, {gfx::Medium, 22}, alpha(kText3, a), 2);
        } else if (const seerr::Server *s = server()) {
            std::string label, value;
            if (row.kind == ServerRow) {
                label = tv ? "Sonarr" : "Radarr";
                value = s->name;
            } else if (row.kind == ProfileRow) {
                label = T("Kvalitetsprofil");
                value = m_profile < (int)s->profiles.size() ? s->profiles[m_profile].name : "\xE2\x80\x93";
            } else {
                label = T("Rotmappe");
                if (m_folder < (int)s->folders.size()) {
                    const seerr::RootFolder &f = s->folders[m_folder];
                    char b[64];
                    std::snprintf(b, sizeof b, T("  \xC2\xB7  %lld GB ledig"), (long long)(f.free_space >> 30));
                    value = f.path + b;
                } else {
                    value = "\xE2\x80\x93";
                }
            }
            gfx::text(lx, cy + 9, label, ls, alpha(fg, a));
            const gfx::TextStyle vs{gfx::Medium, 23, 520};
            gfx::text(rx - (focus ? 26 : 0), cy + 8, value, vs, alpha(fg, a), 2);
            if (focus) {
                gfx::text(rx, cy + 9, "\xE2\x80\xBA", {gfx::Bold, 30}, alpha(fg, a), 2);
                gfx::text(rx - 26 - gfx::text_width(value, vs) - 14, cy + 9, "\xE2\x80\xB9", {gfx::Bold, 30},
                          alpha(fg, a), 2);
            }
        }
    }
    gfx::pop_scissor();
    y = list_y + list_h + (list_rows ? 18 : 0);

    for (const auto &n : notes) {
        y += 34;
        gfx::text(r.x + 48, y - 8, n.first, {gfx::Medium, 22, kW - 96}, alpha(n.second, a));
    }
    y += 24;

    /* The buttons: Request (or Try again) and Cancel; the drop when the focus is here. */
    const bool on_buttons = m_rows[m_focus].kind == Buttons;
    const std::string send_label = m_error.empty() ? T("Be om") : T("Prøv igjen");
    const gfx::TextStyle bt{gfx::Bold, 26};
    const float bw0 = gfx::text_width(send_label, bt) + 96, bw1 = gfx::text_width(T("Avbryt"), bt) + 80;
    const gfx::Rect b0{r.x + 48, y, bw0, 76}, b1{r.x + 48 + bw0 + 20, y, bw1, 76};
    glass_panel(b0, 16, a, false);
    glass_panel(b1, 16, a, false);
    if (on_buttons) {
        m_drop.to(m_button == 0 ? b0 : b1, 100 + m_button, r.x, r.y);
        m_drop.draw(dt, a, animating, 16);
    }
    gfx::text(b0.x + b0.w / 2, b0.y + 47, sending ? T("Sender \xE2\x80\xA6") : send_label, bt, alpha(kText, a), 1);
    gfx::text(b1.x + b1.w / 2, b1.y + 47, T("Avbryt"), {on_buttons && m_button == 1 ? gfx::Bold : gfx::SemiBold, 26},
              alpha(kText, a), 1);
    draw_pad_hints(r.x + r.w - 48, b0.y + 38, {{PadButton::Cross, T("Velg")}, {PadButton::Circle, T("Lukk")}}, 2, 26, a);
}

} // namespace ui
