/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adding an account: the server address (checked against the server), then
 * the user (the server's public users as avatars, or typed), the password,
 * or Quick Connect from a phone. Text entry uses the PS5 system keyboard.
 */
#pragma once

#include "jf/jf_discovery.h"

#include "app/accounts.h"
#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <vector>

namespace ui {

class Login : public Screen {
public:
    /* app_client: only its device identity is used; the login runs on its own
     * client, so the account in use is untouched until this one succeeds.
     * known_server: a server already signed in to on this PS5 (another user
     * on it): straight to Quick Connect, no address to confirm. */
    Login(const jf::Client &app_client, const std::string &server, const std::string &user, bool can_cancel,
          bool known_server = false);
    ~Login() override;

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return true; }
    float nav_alpha() const override { return 0.f; }

    /* The account signed in, once (then false again). */
    bool take_result(accounts::Account *out);

private:
    enum Step { ServerStep, UserStep, QuickConnectStep };
    struct Shared {
        std::mutex lock;
        bool busy = false;
        std::string error;
        std::string server_name, server_version, server_id;
        jf::Kind kind = jf::Kind::Jellyfin;   /* what the check found */
        std::vector<jf::PublicUser> users;
        bool checked = false;           /* the server answered: go on to the user */
        bool signed_in = false;
        accounts::Account result;
        std::string qc_code;
        bool qc_alive = false;
        std::vector<jf::FoundServer> found;   /* servers on the local network */
        std::map<std::string, bool> answers;  /* found addresses asked once: did they answer there */
        bool scanning = false;
        unsigned gen = 0;               /* bumped when the server changes: older requests' answers are dropped */
    };
    void scan(double now);
    double m_scanned_at = -100;
    bool m_found_focused = false;     /* focus moved to the first found server once */
    int m_found_col = 0;
    Anim m_found_scroll;              /* the found servers' row, when more than fit */
    Anim m_users_scroll;              /* the server's users, likewise */
    void check_server();
    void check_known_server();
    void sign_in();
    void start_quick_connect();

    /* A new client for each server checked: a request still running for the last
     * one keeps its own and never sees the address change under it. */
    std::shared_ptr<jf::Client> m_own;   /* shared with the request threads */
    jf::Client &client() const { return *m_own; }
    unsigned next_gen();
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    Step m_step = ServerStep;
    std::string m_server, m_user, m_password;
    bool m_can_cancel, m_known_server;
    bool m_checking = false;          /* a known server being checked: Quick Connect's "…" meanwhile */
    bool m_no_quick_connect = false;  /* this server has it off: ○ on the user step goes back past it */
    bool m_has_quick_connect = true;  /* the kind of server has it at all (Emby does not): its button shows */
    std::string m_known_address;      /* the known server's, to come back to */
    bool m_back_to_known = false;     /* the address step came from its Quick Connect: ○ goes back there */
    bool m_offer_moved = false;       /* it did not answer: offer it where the network finds it now */
    bool m_typed = false;             /* the address was entered here (not one this login was opened with) */
    void other_server();
    void back_to_known();
    int m_focus = 0;
    int m_user_col = 0;               /* focused public user */
    Lifts m_lifts;
    Drop m_drop;                        /* the focus on fields and buttons */
};

} // namespace ui
