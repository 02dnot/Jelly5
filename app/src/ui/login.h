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
#include <vector>

namespace ui {

class Login : public Screen {
public:
    /* app_client: only its device identity is used; the login runs on its own
     * client, so the account in use is untouched until this one succeeds. */
    Login(const jf::Client &app_client, const std::string &server, const std::string &user, bool can_cancel);
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
        std::string server_name, server_version;
        std::vector<jf::PublicUser> users;
        bool checked = false;           /* the server answered: go on to the user */
        bool signed_in = false;
        accounts::Account result;
        std::string qc_code;
        bool qc_alive = false;
        std::vector<jf::FoundServer> found;   /* servers on the local network */
        bool scanning = false;
    };
    void scan(double now);
    double m_scanned_at = -100;
    bool m_found_focused = false;     /* focus moved to the first found server once */
    int m_found_col = 0;
    void check_server();
    void sign_in();
    void start_quick_connect();

    std::shared_ptr<jf::Client> m_own;   /* shared with the request threads */
    jf::Client &m_client;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    Step m_step = ServerStep;
    std::string m_server, m_user, m_password;
    bool m_can_cancel;
    int m_focus = 0;
    int m_user_col = 0;               /* focused public user */
    Lifts m_lifts;
    Drop m_drop;                        /* the focus on fields and buttons */
};

} // namespace ui
