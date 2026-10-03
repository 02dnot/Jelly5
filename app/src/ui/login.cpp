/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/login.h"

#include "qrcodegen.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"
#include "platform/ime.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>
#include <unistd.h>

namespace ui {
namespace {

constexpr float kX = 160, kW = 860;

/* Fields and buttons are glass; the focused one gets the drop (Login::m_drop),
 * which is drawn after every pane, so their text waits in s_later until then. */
Drop *s_drop = nullptr;
bool s_focused = false;
std::vector<std::function<void()>> s_later;

void focus_on(const gfx::Rect &r, bool focus)
{
    if (focus && s_drop) {
        s_drop->to(r, (int)(r.x * 7 + r.y));
        s_focused = true;
    }
}

void field(const gfx::Rect &r, const std::string &label, const std::string &value, const std::string &hint,
           bool focus, float)
{
    gfx::text(r.x, r.y - 14, label, {gfx::SemiBold, 20}, kText3);
    glass_panel(r, 16, 1.f, false);
    focus_on(r, focus);
    s_later.push_back([=] {
        gfx::text(r.x + 26, r.y + r.h / 2 + 10, value.empty() ? hint : value, {gfx::Medium, 28, r.w - 52},
                  value.empty() ? kText3 : kText);
    });
}

void button(const gfx::Rect &r, const std::string &label, bool focus, float)
{
    glass_panel(r, 16, 1.f, false);
    focus_on(r, focus);
    s_later.push_back([=] {
        gfx::text(r.x + r.w / 2, r.y + r.h / 2 + 9, label, {focus ? gfx::Bold : gfx::SemiBold, 26}, kText, 1);
    });
}

} // namespace

Login::Login(const jf::Client &app_client, const std::string &server, const std::string &user, bool can_cancel)
    : m_own(new jf::Client(server, app_client.device_id(), app_client.device_name())), m_client(*m_own),
      m_server(server), m_user(user), m_can_cancel(can_cancel)
{
}

void Login::activate()
{
    ime::init();
}

/* The keyboard's callbacks point at this screen: close it with the screen. */
Login::~Login() { ime::cancel(); }

bool Login::take_result(accounts::Account *out)
{
    std::lock_guard<std::mutex> g(m_shared->lock);
    if (!m_shared->signed_in)
        return false;
    m_shared->signed_in = false;
    *out = m_shared->result;
    return true;
}

/* Asks the network for Jellyfin servers (again every 8 s while the server step shows). */
void Login::scan(double now)
{
    std::shared_ptr<Shared> sh = m_shared;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        if (sh->scanning || now - m_scanned_at < 8.0)
            return;
        sh->scanning = true;
    }
    m_scanned_at = now;
    std::thread([sh] {
        std::vector<jf::FoundServer> f = jf::discover(1500);
        std::lock_guard<std::mutex> g(sh->lock);
        if (!f.empty() || sh->found.empty())
            sh->found = std::move(f);
        sh->scanning = false;
    }).detach();
}

void Login::check_server()
{
    m_client.set_server(m_server);
    m_server = m_client.server();
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
    }
    std::thread([sh, c] {
        std::string name, version;
        const bool ok = c->public_info(&name, &version);
        std::vector<jf::PublicUser> users = ok ? c->public_users() : std::vector<jf::PublicUser>();
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = false;
        if (!ok) {
            sh->error = T("Fant ingen Jellyfin-server på ") + c->server();
            return;
        }
        sh->server_name = name;
        sh->server_version = version;
        sh->users = std::move(users);
        sh->checked = true;
    }).detach();
}

void Login::sign_in()
{
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    const std::string user = m_user, pass = m_password;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
    }
    std::thread([sh, c, user, pass] {
        c->set_session("", "", "");
        const bool ok = c->authenticate(user, pass);
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = false;
        if (!ok) {
            sh->error = c->last_error().find("401") != std::string::npos ? T("Feil brukernavn eller passord")
                                                                        : T("Innloggingen mislyktes");
            return;
        }
        sh->result = {c->server(), sh->server_name, c->user_id(), c->user_name(), c->user_image_tag(), c->token()};
        sh->signed_in = true;
    }).detach();
}

void Login::start_quick_connect()
{
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
        sh->qc_code.clear();
        sh->qc_alive = true;
    }
    m_step = QuickConnectStep;
    m_focus = 0;
    std::thread([sh, c] {
        c->set_session("", "", "");
        jf::QuickConnect qc;
        if (!c->quick_connect_start(&qc)) {
            std::lock_guard<std::mutex> g(sh->lock);
            sh->busy = false;
            sh->error = T("Quick Connect er ikke slått på på denne serveren");
            return;
        }
        {
            std::lock_guard<std::mutex> g(sh->lock);
            sh->busy = false;
            sh->qc_code = qc.code;
        }
        for (int i = 0; i < 200; i++) {   /* a code lives about ten minutes */
            sleep(3);
            {
                std::lock_guard<std::mutex> g(sh->lock);
                if (!sh->qc_alive)
                    return;
            }
            bool approved = false;
            c->quick_connect_poll(qc, &approved);
            if (approved) {
                std::lock_guard<std::mutex> g(sh->lock);
                sh->result = {c->server(), sh->server_name, c->user_id(), c->user_name(), c->user_image_tag(),
                              c->token()};
                sh->signed_in = true;
                return;
            }
        }
    }).detach();
}

/* Focus targets per step:
 *   server: 0 address, 1 continue
 *   user:   [public users...], username, password, sign in, quick connect, other server */
Action Login::input(uint32_t p)
{
    Action a;
    if (ime::active())
        return a;
    std::vector<jf::PublicUser> users;
    bool busy;
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        users = m_shared->users;
        busy = m_shared->busy;
    }
    if (m_step == QuickConnectStep) {
        /* Quick Connect is the default; below the code: sign in with a name and password
         * instead (0), or another server (1). Circle goes back to the server. */
        if (p & NUVIO_BTN_LEFT)
            m_focus = 0;
        else if (p & NUVIO_BTN_RIGHT)
            m_focus = 1;
        else if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE)) {
            {
                std::lock_guard<std::mutex> g(m_shared->lock);
                m_shared->qc_alive = false;
                m_shared->error.clear();
            }
            m_step = (p & NUVIO_BTN_CROSS) && m_focus == 0 ? UserStep : ServerStep;
            m_focus = 0;
        }
        return a;
    }
    if (m_step == ServerStep) {
        /* The address (0), Fortsett (1), then the servers found on the network (2). */
        std::vector<jf::FoundServer> found;
        {
            std::lock_guard<std::mutex> g(m_shared->lock);
            found = m_shared->found;
        }
        const int nf = std::min(3, (int)found.size());
        if (p & NUVIO_BTN_DOWN)
            m_focus = std::min(nf > 0 ? 2 : 1, m_focus + 1);
        else if (p & NUVIO_BTN_UP)
            m_focus = std::max(0, m_focus - 1);
        else if ((p & NUVIO_BTN_LEFT) && m_focus == 2)
            m_found_col = std::max(0, m_found_col - 1);
        else if ((p & NUVIO_BTN_RIGHT) && m_focus == 2)
            m_found_col = std::min(nf - 1, m_found_col + 1);
        else if ((p & NUVIO_BTN_CROSS) && m_focus == 2 && m_found_col < nf) {
            if (!busy) {
                m_server = found[m_found_col].address;
                check_server();
            }
        } else if ((p & NUVIO_BTN_CIRCLE) && m_can_cancel)
            a.kind = Action::Back;
        else if (p & NUVIO_BTN_CROSS) {
            if (m_focus == 0)
                ime::request(ime::Kind::Url, T("Serveradresse"), m_server, [this](const std::string &t) {
                    m_server = t;
                    m_focus = 1;
                });
            else if (!busy && !m_server.empty())
                check_server();
        }
        return a;
    }
    /* User step: a row of public users (if any), then a column of fields and buttons. */
    const int nu = (int)users.size();
    const int base = nu > 0 ? 1 : 0;              /* index 0 = the users row */
    /* The buttons (Logg inn, Bruk Quick Connect, Annen server) are one row: Left and
     * Right move along it, Up goes back to the password. */
    const int buttons = base + 2, last = base + 4;
    const bool on_buttons = m_focus >= buttons;
    if (p & NUVIO_BTN_DOWN)
        m_focus = on_buttons ? m_focus : std::min(buttons, m_focus + 1);
    else if (p & NUVIO_BTN_UP)
        m_focus = on_buttons ? buttons - 1 : std::max(0, m_focus - 1);
    else if (on_buttons && (p & NUVIO_BTN_RIGHT))
        m_focus = std::min(last, m_focus + 1);
    else if (on_buttons && (p & NUVIO_BTN_LEFT))
        m_focus = std::max(buttons, m_focus - 1);
    else if (nu > 0 && m_focus == 0 && (p & NUVIO_BTN_LEFT))
        m_user_col = std::max(0, m_user_col - 1);
    else if (nu > 0 && m_focus == 0 && (p & NUVIO_BTN_RIGHT))
        m_user_col = std::min(nu - 1, m_user_col + 1);
    else if (p & NUVIO_BTN_CIRCLE) {
        start_quick_connect();   /* back to the default */
    } else if ((p & NUVIO_BTN_CROSS) && !busy) {
        const int f = m_focus - base;
        if (base && m_focus == 0) {
            m_user = users[std::min(m_user_col, nu - 1)].name;
            m_focus = base + 1;   /* straight to the password */
            if (!users[std::min(m_user_col, nu - 1)].has_password)
                sign_in();
        } else if (f == 0) {
            ime::request(ime::Kind::Text, T("Brukernavn"), m_user, [this](const std::string &t) { m_user = t; });
        } else if (f == 1) {
            ime::request(ime::Kind::Password, T("Passord"), "", [this, base](const std::string &t) {
                m_password = t;
                m_focus = base + 2;
            });
        } else if (f == 2) {
            if (!m_user.empty())
                sign_in();
        } else if (f == 3) {
            start_quick_connect();
        } else if (f == 4) {
            m_step = ServerStep;
            m_focus = 0;
        }
    }
    return a;
}

void Login::draw(double now, float dt)
{
    ime::poll();
    bool anim = false;
    std::string error, server_name, version, code;
    bool busy, checked;
    std::vector<jf::PublicUser> users;
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        error = m_shared->error;
        server_name = m_shared->server_name;
        version = m_shared->server_version;
        code = m_shared->qc_code;
        busy = m_shared->busy;
        checked = m_shared->checked;
        users = m_shared->users;
        if (checked)
            m_shared->checked = false;
    }
    if (checked)   /* the server answered: Quick Connect first */
        start_quick_connect();
    if (m_step == QuickConnectStep && code.empty() && !busy && !error.empty()) {
        m_step = UserStep;   /* no Quick Connect on this server: name and password (the message stays) */
        m_focus = 0;
    }

    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, gfx::H}, 0x40302048u, 0x00000000u);
    s_drop = &m_drop;
    s_focused = false;
    s_later.clear();
    draw_brand(kX, 190, 60);

    auto lift = [&](const std::string &k, bool f) { return m_lifts.step(k, f, dt, &anim); };
    if (m_step == ServerStep) {
        gfx::text(kX, 340, T("Koble til Jellyfin"), {gfx::Bold, 64}, kText);
        gfx::text(kX, 400, T("Skriv inn adressen til Jellyfin-serveren din, for eksempel 192.168.0.10:8096."),
                  {gfx::Medium, 28, 1200}, kText2);
        field({kX, 500, kW, 84}, "SERVER", m_server, "http://", m_focus == 0, lift("srv", m_focus == 0));
        button({kX, 640, 260, 76}, busy ? T("Kobler til \xE2\x80\xA6") : T("Fortsett"), m_focus == 1, lift("go", m_focus == 1));
        /* Servers on the network, found by asking (Jellyfin's discovery). */
        scan(now);
        std::vector<jf::FoundServer> found;
        bool scanning;
        {
            std::lock_guard<std::mutex> g(m_shared->lock);
            found = m_shared->found;
            scanning = m_shared->scanning;
        }
        const int nf = std::min(3, (int)found.size());
        if (nf > 0 && !m_found_focused) {   /* found one: offer it first */
            m_found_focused = true;
            if (m_focus == 0)
                m_focus = 2, m_found_col = 0;
        }
        m_found_col = std::min(m_found_col, std::max(0, nf - 1));
        gfx::text(kX, 800, nf > 0 ? T("FUNNET P\xC3\x85 NETTVERKET") : scanning ? T("S\xC3\x98KER P\xC3\x85 NETTVERKET \xE2\x80\xA6") : "",
                  {gfx::SemiBold, 20}, kText3);
        float fx = kX;
        for (int i = 0; i < nf; i++) {
            const std::string label = (found[i].name.empty() ? std::string("Jellyfin") : found[i].name) + "  \xC2\xB7  " +
                                      found[i].address.substr(found[i].address.find("//") == std::string::npos
                                                                  ? 0 : found[i].address.find("//") + 2);
            const float w = std::min(620.f, gfx::text_width(label, {gfx::SemiBold, 26}) + 64);
            button({fx, 822, w, 76}, label, m_focus == 2 && m_found_col == i, 0);
            fx += w + 20;
        }
    } else if (m_step == UserStep) {
        gfx::text(kX, 340, T("Logg inn"), {gfx::Bold, 64}, kText);
        gfx::text(kX, 396, server_name + "  \xC2\xB7  Jellyfin " + version + "  \xC2\xB7  " + m_server,
                  {gfx::Medium, 24, 1500}, kText3);
        float y = 470;
        const int base = users.empty() ? 0 : 1;
        if (!users.empty()) {
            float x = kX;
            for (size_t i = 0; i < users.size() && i < 8; i++) {
                const bool f = m_focus == 0 && (int)i == m_user_col;
                const float l = lift("u" + users[i].id, f);
                const float d = 120 * (1.f + 0.1f * l);
                const gfx::Rect r{x + 60 - d / 2, y + 60 - d / 2, d, d};
                if (f)   /* a ring of the focus glass round the picture */
                    glass_panel({r.x - 8, r.y - 8, d + 16, d + 16}, d / 2 + 8, 1.f, false, 1.f);
                const std::string url = users[i].image_tag.empty()
                                            ? std::string()
                                            : m_client.server() + "/Users/" + users[i].id + "/Images/Primary?tag=" +
                                                  users[i].image_tag + "&fillWidth=240";
                if (url.empty()) {
                    gfx::fill(r, 0xff6e7fd6u, d / 2);
                    gfx::text(r.x + d / 2, r.y + d / 2 + 16, users[i].name.substr(0, 1), {gfx::Bold, 48}, kText, 1);
                } else {
                    art::draw(r, url, "", 240, 240, d / 2);
                }
                gfx::text(x + 60, y + 160, users[i].name, {gfx::SemiBold, 22, 150}, f ? kText : kText2, 1);
                x += 170;
            }
            y += 220;
        }
        const int f = m_focus - base;
        field({kX, y + 30, kW, 84}, T("BRUKERNAVN"), m_user, T("Brukernavn"), f == 0, lift("user", f == 0));
        field({kX, y + 160, kW, 84}, T("PASSORD"), std::string(m_password.size(), '*'), T("Passord"), f == 1,
              lift("pass", f == 1));
        const float by = y + 290;
        button({kX, by, 240, 76}, busy ? T("Logger inn \xE2\x80\xA6") : T("Logg inn"), f == 2, lift("in", f == 2));
        button({kX + 260, by, 330, 76}, T("Bruk Quick Connect"), f == 3, lift("qc", f == 3));
        button({kX + 610, by, 250, 76}, T("Annen server"), f == 4, lift("other", f == 4));
    } else {
        gfx::text(kX, 340, "Quick Connect", {gfx::Bold, 64}, kText);
        gfx::text(kX, 410,
                  T("Åpne Jellyfin på telefonen eller PC-en, gå til Innstillinger → Quick Connect og skriv inn koden:"),
                  {gfx::Medium, 30, 1100, 2, 44}, kText2);
        const gfx::Rect box{kX, 540, 760, 220};
        glass_panel(box, 32, 1.f, false);   /* the code on glass */
        std::string spaced;
        for (char ch : code)
            (spaced += ch) += ' ';
        if (code.empty())
            gfx::text(box.x + box.w / 2, box.y + 130, "\xE2\x80\xA6", {gfx::Bold, 80}, kText3, 1);
        else
            gfx::text(box.x + box.w / 2, box.y + 158, spaced, {gfx::Bold, 120}, kText, 1);
        /* A QR code for the phone: Jellyfin's own Quick Connect page with this code
         * filled in (jellyfin-web reads ?code=); one tap on Godkjenn there signs in. */
        if (!code.empty()) {
            static std::string qr_for;
            static std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX);
            static bool qr_ok = false;
            if (qr_for != code) {
                qr_for = code;
                const std::string url = m_client.server() + "/web/#/quickconnect?code=" + code;
                std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_MAX);
                qr_ok = qrcodegen_encodeText(url.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
                                             qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
            }
            if (qr_ok) {
                const int n = qrcodegen_getSize(qr.data());
                const float side = 380, m = side / (float)(n + 8);   /* 4 modules of quiet zone round it */
                const gfx::Rect panel{gfx::W - kPad - side, 330, side, side};
                gfx::shadow(panel, 24, 30, 0.5f, 12);
                gfx::fill(panel, 0xfff5f5f7u, 24);
                for (int y = 0; y < n; y++)
                    for (int x = 0; x < n; x++)
                        if (qrcodegen_getModule(qr.data(), x, y))
                            gfx::fill({panel.x + (x + 4) * m, panel.y + (y + 4) * m, m + 0.4f, m + 0.4f}, 0xff0b0b0fu);
                gfx::text(panel.x + side / 2, panel.y + side + 52, T("Skann med telefonen"), {gfx::SemiBold, 24},
                          kText2, 1);
                gfx::text(panel.x + side / 2, panel.y + side + 86, T("og trykk Godkjenn i Jellyfin"),
                          {gfx::Medium, 20}, kText3, 1);
            }
        }
        button({kX, 800, 560, 76}, T("Logg inn med brukernavn og passord"), m_focus == 0, lift("pw", m_focus == 0));
        button({kX + 580, 800, 250, 76}, T("Annen server"), m_focus == 1, lift("other2", m_focus == 1));
    }
    if (!s_focused)
        m_drop.hide();
    m_drop.draw(dt, 1.f, &anim, 16);
    for (const auto &f : s_later)
        f();
    s_later.clear();
    if (!error.empty())
        gfx::text(kX, 1000, error, {gfx::SemiBold, 24, 1600}, 0xffff6b6bu);
    (void)now;
}

} // namespace ui
