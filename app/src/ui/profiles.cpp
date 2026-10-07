/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/profiles.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <numeric>

namespace ui {

namespace {
constexpr float kPillMax = 420;   /* a long server name is ellipsised */
}

void Profiles::activate()
{
    if (!m_shown.empty())
        m_focus_of[m_shown] = m_focus;
    const std::vector<accounts::Account> list = accounts::load();

    /* One pill per server: accounts on the same server (the same Id or the same
     * address) and on servers linked through them, in the order first saved. */
    const int n = (int)list.size();
    std::vector<int> root(n);
    std::iota(root.begin(), root.end(), 0);
    auto find = [&](int i) {
        while (root[i] != i)
            i = root[i] = root[root[i]];
        return i;
    };
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            if (accounts::same_server(list[i], list[j]))
                root[std::max(find(i), find(j))] = std::min(find(i), find(j));
    m_servers.clear();
    std::vector<int> group_of(n, -1);
    for (int i = 0; i < n; i++) {
        const int r = find(i);
        if (group_of[r] < 0)
            group_of[r] = (int)m_servers.size(), m_servers.emplace_back();
        m_servers[group_of[r]].accounts.push_back(list[i]);
    }
    for (Server &s : m_servers) {
        for (const accounts::Account &a : s.accounts) {
            if (s.key.empty())
                s.key = a.server_id;
            if (s.label.empty())
                s.label = a.server_name;
        }
        if (s.key.empty())
            s.key = s.accounts.front().server;
        if (s.label.empty())
            s.label = host_of(s.accounts.front().server);
    }
    /* Two servers with one name: their addresses tell them apart (counted before any is renamed). */
    const std::vector<std::string> names = labels();
    for (Server &s : m_servers)
        if (std::count(names.begin(), names.end(), s.label) > 1)
            s.label = host_of(s.accounts.front().server);

    /* The server shown before (back from a login), else the last used account's. */
    int shown = 0;
    accounts::Account last;
    const bool has_last = m_shown.empty() && accounts::last(&last);
    for (int i = 0; i < (int)m_servers.size(); i++)
        if (m_servers[i].key == m_shown ||
            (has_last && std::any_of(m_servers[i].accounts.begin(), m_servers[i].accounts.end(),
                                     [&](const accounts::Account &a) { return accounts::same_server(a, last); })))
            shown = i;
    m_server = -1;   /* (the focus to leave is saved above) */
    select(shown);
    m_pill = std::min(m_pill, (int)m_servers.size());
    if (m_in_pills && m_pill < (int)m_servers.size())
        m_pill = m_server;
}

std::vector<std::string> Profiles::labels() const
{
    std::vector<std::string> out;
    for (const Server &s : m_servers)
        out.push_back(s.label);
    return out;
}

void Profiles::select(int server)
{
    if (m_server >= 0 && m_server < (int)m_servers.size() && server != m_server)
        m_focus_of[m_servers[m_server].key] = m_focus;
    m_server = server;
    const bool any = server < (int)m_servers.size();
    m_shown = any ? m_servers[server].key : std::string();
    m_list = any ? m_servers[server].accounts : std::vector<accounts::Account>();
    const auto f = any ? m_focus_of.find(m_shown) : m_focus_of.end();
    m_focus = std::min(f != m_focus_of.end() ? f->second : 0, (int)m_list.size());
    m_armed = -1;
}

bool Profiles::take_choice(Choice *out)
{
    if (!m_chosen)
        return false;
    m_chosen = false;
    *out = m_choice;
    return true;
}

Action Profiles::input(uint32_t p)
{
    const int ns = (int)m_servers.size();
    if (m_in_pills) {
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
            m_pill = (p & NUVIO_BTN_LEFT) ? std::max(0, m_pill - 1) : std::min(ns, m_pill + 1);
            if (m_pill < ns && m_pill != m_server)
                select(m_pill);   /* the users follow the focus */
        } else if ((p & NUVIO_BTN_CROSS) && m_pill == ns) {
            m_choice = Choice();
            m_choice.add_server = true;
            m_chosen = true;
        } else if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_DOWN | NUVIO_BTN_CIRCLE)) {
            m_in_pills = false;   /* back down to the users */
        }
        return Action();
    }
    const int n = (int)m_list.size() + 1;   /* + "Legg til" */
    if ((p & NUVIO_BTN_UP) && ns > 0)
        m_in_pills = true, m_pill = m_server, m_armed = -1;
    else if (p & NUVIO_BTN_LEFT)
        m_focus = std::max(0, m_focus - 1), m_armed = -1;
    else if (p & NUVIO_BTN_RIGHT)
        m_focus = std::min(n - 1, m_focus + 1), m_armed = -1;
    else if (p & NUVIO_BTN_CROSS) {
        m_choice = Choice();
        if (m_focus < (int)m_list.size()) {
            m_choice.account = m_list[m_focus];
        } else if (m_list.empty()) {
            m_choice.add_server = true;   /* nothing saved: any server */
        } else {
            /* Another user on this server, at the address its last used account had. */
            m_choice.add = true;
            m_choice.account = m_list.front();
            accounts::Account last;
            if (accounts::last(&last))
                for (const accounts::Account &a : m_list)
                    if (a.server == last.server)
                        m_choice.account = a;
        }
        m_chosen = true;
    } else if ((p & NUVIO_BTN_TRIANGLE) && m_focus < (int)m_list.size()) {
        if (m_armed == m_focus) {
            accounts::forget(m_list[m_focus].server, m_list[m_focus].user_id);
            activate();
        } else {
            m_armed = m_focus;
        }
    }
    return Action();
}

void Profiles::draw(double, float dt)
{
    m_animating = false;
    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, gfx::H}, 0x40302048u, 0x00000000u);
    const int ns = (int)m_servers.size();
    const bool pills = ns > 0;
    const float oy = pills ? 40 : 0;   /* room for the pills under the title */
    gfx::text(gfx::W / 2, pills ? 260 : 330, T("Hvem ser p\xC3\xA5?"), {gfx::Bold, 64}, kText, 1);

    if (pills) {
        /* The servers as a glass bar (as the library's sources), "+ Server" beside it;
         * the drop on the focused one, faint on the shown one while the users have focus. */
        const std::vector<std::string> names = labels(), add = {T("+ Server")};
        const float bw = pill_bar_width(names, kPillMax), aw = pill_bar_width(add), gap = 20, py = 320;
        const float row_w = bw + gap + aw, margin = 160, room = gfx::W - 2 * margin;
        const int f = m_in_pills ? m_pill : m_server;   /* more than fit: the row slides, the focused pill mid-screen */
        const float c = f == ns ? bw + gap + aw / 2 : pill_bar_center(names, f, kPillMax);
        const float x0 = row_w <= room ? (gfx::W - row_w) / 2
                                       : margin - row_scroll(m_pill_scroll, row_w, room, c, dt, &m_animating);
        const bool on_add = m_in_pills && m_pill == ns;
        pill_bar(x0, py, names, on_add ? m_server : m_in_pills ? m_pill : m_server, m_in_pills && !on_add,
                 m_pill_drop, dt, 1.f, &m_animating, x0, 0, kPillMax);
        pill_bar(x0 + bw + gap, py, add, on_add ? 0 : -1, true, m_add_drop, dt, 1.f, &m_animating, x0, 0);
    }

    const int n = (int)m_list.size() + 1;
    const float d = 220, gap = 64, top = 440 + oy;
    /* Centred; with more than fit, the row scrolls with the focus kept mid-screen. */
    const float row_w = n * (d + gap) - gap, margin = 160, room = gfx::W - 2 * margin;
    float x = row_w <= room ? (gfx::W - row_w) / 2
                            : margin - row_scroll(m_scroll, row_w, room, m_focus * (d + gap) + d / 2, dt, &m_animating);
    /* The focus: the liquid glass drop as a ring round the picture, sliding between them. */
    const float fx = x + m_focus * (d + gap), ring = d * 1.12f + 28;
    if (m_in_pills)
        m_drop.hide();
    else
        m_drop.to({fx + d / 2 - ring / 2, top + d / 2 - ring / 2, ring, ring}, m_focus, x, 0);
    m_drop.draw(dt, 1.f, &m_animating);
    for (int i = 0; i < n; i++, x += d + gap) {
        const bool focus = i == m_focus && !m_in_pills;
        const bool user = i < (int)m_list.size();
        const float lift =
            m_lifts.step(user ? m_list[i].server + "/" + m_list[i].user_id : std::string("+"), focus, dt, &m_animating);
        const float k = 1.f + 0.12f * lift, dd = d * k;
        const gfx::Rect r{x + d / 2 - dd / 2, top + d / 2 - dd / 2, dd, dd};
        std::string name;   /* (its server is the pill above; no shadow: on the dark page it read as a black halo) */
        if (user) {
            const accounts::Account &a = m_list[i];
            name = a.user_name;
            glass_panel(r, dd / 2, 1.f, false);
            gfx::text(r.x + dd / 2, r.y + dd / 2 + 32, name.substr(0, 1), {gfx::Bold, 90}, kText, 1);
            if (!a.image_tag.empty())
                art::draw(r, a.server + "/Users/" + a.user_id + "/Images/Primary?tag=" + a.image_tag + "&fillWidth=440",
                          "", 440, 440, dd / 2, 1.f, 0);   /* over the initial until it loads */
        } else {
            name = T("Legg til");
            glass_panel(r, dd / 2, 1.f, false);
            gfx::fill({r.x + dd / 2 - 3, r.y + dd / 2 - 36, 6, 72}, kText2, 3);
            gfx::fill({r.x + dd / 2 - 36, r.y + dd / 2 - 3, 72, 6}, kText2, 3);
        }
        gfx::text(x + d / 2, top + d + 64, name, {gfx::SemiBold, 30, d + 40}, focus ? kText : kText2, 1);
    }
    const bool armed = m_armed >= 0 && m_armed == m_focus && !m_in_pills;
    gfx::text(gfx::W / 2, 1000,
              armed ? T("Trykk \xE2\x96\xB3 igjen for \xC3\xA5 fjerne kontoen fra denne PS5-en") : "",
              {gfx::Medium, 22}, armed ? 0xffff6b6bu : kText3, 1);
    if (!armed && m_in_pills)
        draw_pad_hints(gfx::W / 2, 992, {{PadButton::Cross, T("Velg")}}, 1);
    else if (!armed)
        draw_pad_hints(gfx::W / 2, 992, {{PadButton::Cross, T("Velg")}, {PadButton::Triangle, T("Fjern konto")}}, 1);
    if (art::animating())
        m_animating = true;
}

} // namespace ui
