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
#include "app/spawn.h"
#include "jf/jf_http.h"
#include "evo_boot_trace.h"

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

/* key: the control (default: from where it is); ox: where its row is, when the row slides. */
void focus_on(const gfx::Rect &r, bool focus, int key = -1, float ox = 0)
{
    if (focus && s_drop) {
        s_drop->to(r, key >= 0 ? key : (int)(r.x * 7 + r.y), ox, 0);
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

void button(const gfx::Rect &r, const std::string &label, bool focus, float, int key = -1, float ox = 0)
{
    glass_panel(r, 16, 1.f, false);
    focus_on(r, focus, key, ox);
    s_later.push_back([=] {
        gfx::text(r.x + r.w / 2, r.y + r.h / 2 + 9, label, {focus ? gfx::Bold : gfx::SemiBold, 26, r.w - 40}, kText,
                  1);
    });
}

} // namespace

Login::Login(const jf::Client &app_client, const std::string &server, const std::string &user, bool can_cancel,
             bool known_server)
    : m_own(new jf::Client(server, app_client.device_id(), app_client.device_name())), m_server(server), m_user(user),
      m_can_cancel(can_cancel), m_known_server(known_server)
{
    if (known_server)
        m_known_address = server;
}

unsigned Login::next_gen()
{
    std::lock_guard<std::mutex> g(m_shared->lock);
    return ++m_shared->gen;
}

void Login::activate()
{
    ime::init();
    if (m_known_server && !m_server.empty() && !m_checking) {
        m_checking = true;
        m_step = QuickConnectStep;   /* its code box waits with "…"; an error goes to the address */
        m_focus = 0;
        check_server();
    }
}

/* "Annen server": the address step. Empty when the address here is a saved one
 * this login was opened with (that server is on "Hvem ser på?"); one typed
 * here stays to be corrected. */
void Login::other_server()
{
    next_gen();   /* a check or a Quick Connect still running is for the last server */
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        m_shared->qc_alive = false;
        m_shared->busy = false;
        m_shared->error.clear();
    }
    if (!m_typed && accounts::saved_at(client().server()))
        m_server.clear();
    m_back_to_known = m_known_server;
    m_known_server = false;
    m_checking = false;
    m_step = ServerStep;
    m_focus = 0;
}

/* ○ on the address step reached from the known server's Quick Connect: back to it. */
void Login::back_to_known()
{
    m_server = m_known_address;
    m_typed = false;
    m_known_server = true;
    m_back_to_known = false;
    m_checking = true;
    m_step = QuickConnectStep;
    m_focus = 1;
    check_server();
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

/* Asks the network for Jellyfin and Emby servers (again every 8 s while the server step shows). */
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
    const bool offer_moved = m_offer_moved;
    const bool started = jelly5::spawn([sh, offer_moved] {
        std::vector<jf::FoundServer> f = jf::discover(1500);
        /* Servers already saved are on "Hvem ser på?": signing in to one at another
         * address would move its accounts there. Only when it did not answer at its
         * saved address is it offered where the network finds it now. */
        const std::vector<accounts::Account> saved = accounts::load();
        f.erase(std::remove_if(f.begin(), f.end(),
                               [&](const jf::FoundServer &s) {
                                   return std::any_of(saved.begin(), saved.end(), [&](const accounts::Account &a) {
                                       return a.server == s.address ||
                                              (!offer_moved && !s.id.empty() && a.server_id == s.id);
                                   });
                               }),
                f.end());
        /* A server can name an address the PS5 cannot reach (one in Docker: its
         * container's 172.17.x.x), and signing in there only times out: offered
         * are the ones that answer where they said. Each address is asked once
         * while this sign-in shows. */
        std::vector<char> reachable(f.size(), 0), known(f.size(), 0);
        {
            std::lock_guard<std::mutex> g(sh->lock);
            for (size_t i = 0; i < f.size(); i++) {
                const auto at = sh->answers.find(f[i].address);
                known[i] = at != sh->answers.end();
                reachable[i] = known[i] && at->second;
            }
        }
        std::vector<std::function<void()>> checks;
        for (size_t i = 0; i < f.size(); i++)
            if (!known[i])
                checks.push_back([&f, &reachable, i] {
                /* Not through a client: its failed requests count as the signed-in
                 * server being gone (jf::unreachable_streak, the "no contact" note). */
                jf::Client probe("", "", "");
                probe.set_server(f[i].address);   /* (the address as typed ones are taken) */
                reachable[i] = jf::http_request("GET", probe.server() + "/System/Info/Public",
                                                {"Accept: application/json"}, "", 5).ok();
            });
        jelly5::run_all(checks);
        {
            std::lock_guard<std::mutex> g(sh->lock);
            for (size_t i = 0; i < f.size(); i++)
                sh->answers[f[i].address] = reachable[i];
        }
        for (size_t i = f.size(); i-- > 0;)
            if (!reachable[i]) {
                if (!known[i])
                    evo_bt("login: %s answered the search but not at %s", f[i].name.c_str(), f[i].address.c_str());
                f.erase(f.begin() + (long)i);
            }
        std::lock_guard<std::mutex> g(sh->lock);
        if (!f.empty() || sh->found.empty())
            sh->found = std::move(f);
        sh->scanning = false;
    });
    if (!started) {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->scanning = false;
    }
}

void Login::check_server()
{
    m_no_quick_connect = false;
    m_has_quick_connect = true;   /* until the check says what kind of server it is */
    m_own = std::make_shared<jf::Client>("", client().device_id(), client().device_name());
    m_own->set_server(m_server);
    m_server = client().server();
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    const unsigned gen = next_gen();
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
        sh->qc_alive = false;
    }
    const bool started = jelly5::spawn([sh, c, gen] {
        std::string name, version, id;
        jf::Kind kind = jf::Kind::Jellyfin;
        const bool ok = c->public_info(&name, &version, &id, &kind);
        if (ok)
            c->set_kind(kind);   /* (before its users are asked for and anyone signs in) */
        evo_bt("login: %s answered=%d %s %s", c->server().c_str(), ok, ok ? jf::kind_key(kind) : "-", version.c_str());
        std::vector<jf::PublicUser> users = ok ? c->public_users() : std::vector<jf::PublicUser>();
        std::lock_guard<std::mutex> g(sh->lock);
        if (sh->gen != gen)
            return;   /* another server since */
        sh->busy = false;
        if (!ok) {
            sh->error = T("Fant ingen Jellyfin- eller Emby-server på ") + c->server();
            return;
        }
        sh->server_name = name;
        sh->server_version = version;
        sh->server_id = id;
        sh->kind = kind;
        sh->users = std::move(users);
        sh->checked = true;
    });
    if (!started) {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = false;
        sh->error = T("Innloggingen mislyktes");
    }
}

void Login::sign_in()
{
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    const std::string user = m_user, pass = m_password;
    unsigned gen;
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
        gen = sh->gen;
    }
    const bool started = jelly5::spawn([sh, c, user, pass, gen] {
        c->set_session("", "", "");
        const bool ok = c->authenticate(user, pass);
        std::lock_guard<std::mutex> g(sh->lock);
        if (sh->gen != gen)
            return;   /* another server since */
        sh->busy = false;
        if (!ok) {
            sh->error = c->last_error().find("401") != std::string::npos ? T("Feil brukernavn eller passord")
                                                                        : T("Innloggingen mislyktes");
            return;
        }
        sh->result = {c->server(), sh->server_name, c->user_id(), c->user_name(), c->user_image_tag(), c->token(),
                      sh->server_id, jf::kind_key(c->kind())};
        sh->signed_in = true;
    });
    if (!started) {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = false;
        sh->error = T("Innloggingen mislyktes");
    }
}

void Login::start_quick_connect()
{
    std::shared_ptr<Shared> sh = m_shared;
    std::shared_ptr<jf::Client> c = m_own;
    const unsigned gen = next_gen();   /* (a code asked for before is dropped) */
    {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = true;
        sh->error.clear();
        sh->qc_code.clear();
        sh->qc_alive = true;
    }
    m_step = QuickConnectStep;
    m_focus = 0;
    const bool started = jelly5::spawn([sh, c, gen] {
        c->set_session("", "", "");
        jf::QuickConnect qc;
        if (!c->quick_connect_start(&qc)) {
            std::lock_guard<std::mutex> g(sh->lock);
            if (sh->gen != gen)
                return;
            sh->busy = false;
            sh->error = T("Quick Connect er ikke slått på på denne serveren");
            return;
        }
        {
            std::lock_guard<std::mutex> g(sh->lock);
            if (sh->gen != gen)
                return;
            sh->busy = false;
            sh->qc_code = qc.code;
        }
        for (int i = 0; i < 200; i++) {   /* a code lives about ten minutes */
            sleep(3);
            {
                std::lock_guard<std::mutex> g(sh->lock);
                if (!sh->qc_alive || sh->gen != gen)
                    return;
            }
            bool approved = false;
            c->quick_connect_poll(qc, &approved);
            if (approved) {
                std::lock_guard<std::mutex> g(sh->lock);
                if (!sh->qc_alive || sh->gen != gen)
                    return;   /* left meanwhile: not this sign-in any more */
                sh->result = {c->server(), sh->server_name, c->user_id(), c->user_name(), c->user_image_tag(),
                              c->token(), sh->server_id, jf::kind_key(c->kind())};
                sh->signed_in = true;
                return;
            }
        }
    });
    if (!started) {
        std::lock_guard<std::mutex> g(sh->lock);
        sh->busy = false;
        sh->error = T("Innloggingen mislyktes");
    }
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
         * instead (0), or another server (1). Circle goes back to the server, or to
         * "Hvem ser på?" when the server was a known one. */
        const bool back = (p & NUVIO_BTN_CIRCLE) && m_known_server && m_can_cancel;
        if (m_checking && !back) {   /* the server has not answered yet: only "Annen server" is there */
            m_focus = 1;
            if (p & NUVIO_BTN_CROSS)
                other_server();
            return a;
        }
        if (p & NUVIO_BTN_LEFT)
            m_focus = 0;
        else if (p & NUVIO_BTN_RIGHT)
            m_focus = 1;
        else if (back) {
            std::lock_guard<std::mutex> g(m_shared->lock);
            m_shared->qc_alive = false;
            a.kind = Action::Back;
        } else if ((p & NUVIO_BTN_CROSS) && m_focus == 0) {
            {
                std::lock_guard<std::mutex> g(m_shared->lock);
                m_shared->qc_alive = false;
                m_shared->error.clear();
            }
            m_step = UserStep;
            m_focus = 0;
        } else if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE)) {
            other_server();
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
        const int nf = (int)found.size();
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
                m_typed = true;
                check_server();
            }
        } else if ((p & NUVIO_BTN_CIRCLE) && m_back_to_known)
            back_to_known();
        else if ((p & NUVIO_BTN_CIRCLE) && m_can_cancel)
            a.kind = Action::Back;
        else if (p & NUVIO_BTN_CROSS) {
            if (m_focus == 0)
                ime::request(ime::Kind::Url, T("Serveradresse"), m_server, [this](const std::string &t) {
                    m_server = t;
                    m_typed = true;
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
    else if (on_buttons && (p & NUVIO_BTN_RIGHT))   /* (past Quick Connect where there is none) */
        m_focus = std::min(last, m_focus + 1 + (!m_has_quick_connect && m_focus + 1 == base + 3));
    else if (on_buttons && (p & NUVIO_BTN_LEFT))
        m_focus = std::max(buttons, m_focus - 1 - (!m_has_quick_connect && m_focus - 1 == base + 3));
    else if (nu > 0 && m_focus == 0 && (p & NUVIO_BTN_LEFT))
        m_user_col = std::max(0, m_user_col - 1);
    else if (nu > 0 && m_focus == 0 && (p & NUVIO_BTN_RIGHT))
        m_user_col = std::min(nu - 1, m_user_col + 1);
    else if (p & NUVIO_BTN_CIRCLE) {
        if (!m_no_quick_connect)
            start_quick_connect();   /* back to the default */
        else if (m_known_server && m_can_cancel)
            a.kind = Action::Back;   /* nothing before this: "Hvem ser på?" */
        else
            other_server();
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
        } else if (f == 3 && m_has_quick_connect) {
            start_quick_connect();
        } else if (f == 4) {
            other_server();
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
    jf::Kind kind;
    std::vector<jf::PublicUser> users;
    {
        std::lock_guard<std::mutex> g(m_shared->lock);
        error = m_shared->error;
        server_name = m_shared->server_name;
        version = m_shared->server_version;
        code = m_shared->qc_code;
        busy = m_shared->busy;
        checked = m_shared->checked;
        kind = m_shared->kind;
        users = m_shared->users;
        if (checked)
            m_shared->checked = false;
    }
    if (checked) {   /* the server answered: Quick Connect first, where the server has it */
        m_checking = false;
        m_has_quick_connect = client().features().quick_connect;   /* (its kind is set: checked) */
        if (m_has_quick_connect) {
            start_quick_connect();
        } else {
            m_no_quick_connect = true;
            m_step = UserStep;
            m_focus = 0;
        }
    }
    if (m_checking && !busy && !error.empty()) {   /* a known server did not answer: its address, and why */
        m_checking = false;
        m_known_server = false;   /* what is typed now is a new address: ○ goes back a step at a time */
        m_back_to_known = false;  /* (its Quick Connect never showed) */
        m_offer_moved = true;     /* it may answer elsewhere now: the network's answer is offered */
        m_scanned_at = -100;      /* ask the network at once */
        m_step = ServerStep;
        m_focus = 1;
    }
    if (m_step == QuickConnectStep && code.empty() && !busy && !error.empty()) {
        m_no_quick_connect = true;
        m_step = UserStep;   /* no Quick Connect on this server: name and password (the message stays) */
        m_focus = 0;
    }

    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, gfx::H}, 0x40302048u, 0x00000000u);
    s_drop = &m_drop;
    s_focused = false;
    s_later.clear();
    /* The brand small in the corner (as the top bar has it): the steps start high. */
    draw_brand(gfx::W - kPad - brand_width(44), 112, 44);

    auto lift = [&](const std::string &k, bool f) { return m_lifts.step(k, f, dt, &anim); };
    if (m_step == ServerStep) {
        gfx::text(kX, 230, T("Koble til Jellyfin eller Emby"), {gfx::Bold, 64}, kText);
        gfx::text(kX, 290, T("Skriv inn adressen til Jellyfin- eller Emby-serveren din, for eksempel 192.168.0.10:8096."),
                  {gfx::Medium, 28, 1200}, kText2);
        field({kX, 390, kW, 84}, "SERVER", m_server, "http://", m_focus == 0, lift("srv", m_focus == 0));
        button({kX, 530, 260, 76}, busy ? T("Kobler til \xE2\x80\xA6") : T("Fortsett"), m_focus == 1, lift("go", m_focus == 1));
        /* Servers on the network, found by asking (Jellyfin's discovery). */
        scan(now);
        std::vector<jf::FoundServer> found;
        bool scanning;
        {
            std::lock_guard<std::mutex> g(m_shared->lock);
            found = m_shared->found;
            scanning = m_shared->scanning;
        }
        const int nf = (int)found.size();
        if (nf > 0 && !m_found_focused) {   /* found one: offer it first */
            m_found_focused = true;
            if (m_focus == 0)
                m_focus = 2, m_found_col = 0;
        }
        m_found_col = std::min(m_found_col, std::max(0, nf - 1));
        gfx::text(kX, 690, nf > 0 ? T("FUNNET P\xC3\x85 NETTVERKET") : scanning ? T("S\xC3\x98KER P\xC3\x85 NETTVERKET \xE2\x80\xA6") : "",
                  {gfx::SemiBold, 20}, kText3);
        /* All of them in a row; more than fit, and it slides with the focus kept in view. */
        std::vector<std::string> labels(nf);
        std::vector<float> w(nf), at(nf);
        float row_w = 0;
        for (int i = 0; i < nf; i++) {
            labels[i] = (found[i].name.empty() ? std::string(T("Server")) : found[i].name) + "  \xC2\xB7  " +
                        host_of(found[i].address);
            w[i] = std::min(560.f, gfx::text_width(labels[i], {gfx::Bold, 26}) + 64);
            at[i] = row_w;
            row_w += w[i] + (i + 1 < nf ? 20 : 0);
        }
        const float fx = nf > 0 ? kX - row_scroll(m_found_scroll, row_w, gfx::W - kX - 160,
                                                  at[m_found_col] + w[m_found_col] / 2, dt, &anim)
                                : kX;
        for (int i = 0; i < nf; i++)
            button({fx + at[i], 712, w[i], 76}, labels[i], m_focus == 2 && m_found_col == i, 0, 1000 + i, fx - kX);
    } else if (m_step == UserStep) {
        gfx::text(kX, 230, T("Logg inn"), {gfx::Bold, 64}, kText);
        gfx::text(kX, 286, server_name + "  \xC2\xB7  " + (kind == jf::Kind::Emby ? "Emby " : "Jellyfin ") + version +
                               "  \xC2\xB7  " + m_server,
                  {gfx::Medium, 24, 1500}, kText3);
        float y = 340;
        const int base = users.empty() ? 0 : 1;
        if (!users.empty()) {
            /* All of them; more than fit, and the row slides with the focus kept in view. */
            const int nu = (int)users.size();
            float x = kX - row_scroll(m_users_scroll, nu * 170.f - 50, gfx::W - kX - 160,
                                      std::min(m_user_col, nu - 1) * 170.f + 60, dt, &anim);
            for (int i = 0; i < nu; i++) {
                const bool f = m_focus == 0 && i == m_user_col;
                const float l = lift("u" + users[i].id, f);
                const float d = 120 * (1.f + 0.1f * l);
                const gfx::Rect r{x + 60 - d / 2, y + 60 - d / 2, d, d};
                if (f)   /* a ring of the focus glass round the picture */
                    glass_panel({r.x - 8, r.y - 8, d + 16, d + 16}, d / 2 + 8, 1.f, false, 1.f);
                const std::string url = users[i].image_tag.empty()
                                            ? std::string()
                                            : client().server() + "/Users/" + users[i].id + "/Images/Primary?tag=" +
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
            y += 190;
        }
        /* (ends at 866 with users: the error line goes under it) */
        const int f = m_focus - base;
        field({kX, y + 30, kW, 84}, T("BRUKERNAVN"), m_user, T("Brukernavn"), f == 0, lift("user", f == 0));
        field({kX, y + 150, kW, 84}, T("PASSORD"), std::string(m_password.size(), '*'), T("Passord"), f == 1,
              lift("pass", f == 1));
        const float by = y + 270;
        button({kX, by, 240, 76}, busy ? T("Logger inn \xE2\x80\xA6") : T("Logg inn"), f == 2, lift("in", f == 2));
        if (m_has_quick_connect) {
            button({kX + 260, by, 330, 76}, T("Bruk Quick Connect"), f == 3, lift("qc", f == 3));
            button({kX + 610, by, 250, 76}, T("Annen server"), f == 4, lift("other", f == 4));
        } else {   /* Emby: no Quick Connect */
            button({kX + 260, by, 250, 76}, T("Annen server"), f == 4, lift("other", f == 4));
        }
    } else {
        gfx::text(kX, 230, "Quick Connect", {gfx::Bold, 64}, kText);
        gfx::text(kX, 300,
                  T("Åpne Jellyfin på telefonen eller PC-en, gå til Innstillinger → Quick Connect og skriv inn koden:"),
                  {gfx::Medium, 30, 1100, 2, 44}, kText2);
        const gfx::Rect box{kX, 430, 760, 220};
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
                const std::string url = client().server() + "/web/#/quickconnect?code=" + code;
                std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_MAX);
                qr_ok = qrcodegen_encodeText(url.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
                                             qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
            }
            if (qr_ok) {
                const int n = qrcodegen_getSize(qr.data());
                const float side = 380, m = side / (float)(n + 8);   /* 4 modules of quiet zone round it */
                const gfx::Rect panel{gfx::W - kPad - side, 220, side, side};
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
        if (m_checking) {   /* until the server answers, the way out is the only button */
            button({kX, 690, 250, 76}, T("Annen server"), true, lift("other2", true));
        } else {
            button({kX, 690, 560, 76}, T("Logg inn med brukernavn og passord"), m_focus == 0, lift("pw", m_focus == 0));
            button({kX + 580, 690, 250, 76}, T("Annen server"), m_focus == 1, lift("other2", m_focus == 1));
        }
    }
    if (!s_focused)
        m_drop.hide();
    m_drop.draw(dt, 1.f, &anim, 16);
    for (const auto &f : s_later)
        f();
    s_later.clear();
    if (!error.empty())
        gfx::text(kX, 940, error, {gfx::SemiBold, 24, 1600}, 0xffff6b6bu);   /* under every step's last row */
    (void)now;
}

} // namespace ui
