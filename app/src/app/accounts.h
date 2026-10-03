/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signed-in accounts ("Hvem ser på?"), kept in the title's own storage
 * (/download0/jelly5/accounts.json): server, user and access token per
 * account, plus which one was used last.
 */
#pragma once

#include <string>
#include <vector>

namespace accounts {

struct Account {
    std::string server, server_name;
    std::string user_id, user_name, image_tag;
    std::string token;
};

std::vector<Account> load();
/* The account used last, if any. */
bool last(Account *out);
/* Adds or refreshes an account and makes it the last used. */
void remember(const Account &a);
void forget(const std::string &server, const std::string &user_id);
void set_last(const std::string &server, const std::string &user_id);

} // namespace accounts
