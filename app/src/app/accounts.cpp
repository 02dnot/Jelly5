/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/accounts.h"

#include <map>

#include "evo_boot_trace.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

extern "C" {
#include "cJSON.h"
}

namespace accounts {
namespace {

constexpr const char *kDir = "/download0/jelly5";
constexpr const char *kFile = "/download0/jelly5/accounts.json";
constexpr const char *kLegacy = "/download0/jelly5/session.json";   /* phase 1's single session */

std::string read_file(const char *path)
{
    std::string body;
    if (FILE *f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            body.append(buf, n);
        std::fclose(f);
    }
    return body;
}

std::string str(const cJSON *o, const char *k)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return v ? v : "";
}

Account account_of(const cJSON *o)
{
    Account a;
    a.server = str(o, "server");
    a.server_name = str(o, "serverName");
    a.user_id = str(o, "userId");
    a.user_name = str(o, "userName");
    a.image_tag = str(o, "imageTag");
    a.token = str(o, "token");
    return a;
}

struct Store {
    std::vector<Account> list;
    std::string last_server, last_user;
    std::map<std::string, std::pair<std::string, std::string>> by_ps5;   /* PS5 user -> server, user */
};

int s_ps5_user = -1;
std::string ps5_key() { return std::to_string(s_ps5_user); }

Store read_store()
{
    Store s;
    cJSON *j = cJSON_Parse(read_file(kFile).c_str());
    if (j) {
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "accounts"))
            s.list.push_back(account_of(it));
        s.last_server = str(j, "lastServer");
        s.last_user = str(j, "lastUser");
        const cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(j, "lastByPs5User"))
            if (m->string)
                s.by_ps5[m->string] = {str(m, "server"), str(m, "user")};
        cJSON_Delete(j);
        return s;
    }
    /* First run after phase 1: take over its session. */
    if (cJSON *old = cJSON_Parse(read_file(kLegacy).c_str())) {
        Account a = account_of(old);
        if (!a.token.empty()) {
            s.list.push_back(a);
            s.last_server = a.server;
            s.last_user = a.user_id;
        }
        cJSON_Delete(old);
    }
    return s;
}

void write_store(const Store &s)
{
    mkdir(kDir, 0777);
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (const Account &a : s.list) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "server", a.server.c_str());
        cJSON_AddStringToObject(o, "serverName", a.server_name.c_str());
        cJSON_AddStringToObject(o, "userId", a.user_id.c_str());
        cJSON_AddStringToObject(o, "userName", a.user_name.c_str());
        cJSON_AddStringToObject(o, "imageTag", a.image_tag.c_str());
        cJSON_AddStringToObject(o, "token", a.token.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(j, "accounts", arr);
    cJSON_AddStringToObject(j, "lastServer", s.last_server.c_str());
    cJSON_AddStringToObject(j, "lastUser", s.last_user.c_str());
    cJSON *by = cJSON_CreateObject();
    for (const auto &kv : s.by_ps5) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "server", kv.second.first.c_str());
        cJSON_AddStringToObject(o, "user", kv.second.second.c_str());
        cJSON_AddItemToObject(by, kv.first.c_str(), o);
    }
    cJSON_AddItemToObject(j, "lastByPs5User", by);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    /* Write beside, then rename: a crash mid-write never loses the accounts. */
    const std::string tmp = std::string(kFile) + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        std::fputs(text, f);
        std::fclose(f);
        std::rename(tmp.c_str(), kFile);
    } else {
        evo_bt("accounts: cannot write %s", kFile);
    }
    std::free(text);
}

} // namespace

std::vector<Account> load() { return read_store().list; }

void set_ps5_user(int ps5_user_id) { s_ps5_user = ps5_user_id; }

bool last(Account *out)
{
    const Store s = read_store();
    std::string server = s.last_server, user = s.last_user;
    const auto mine = s.by_ps5.find(ps5_key());
    if (mine != s.by_ps5.end()) {   /* this PS5 user's own */
        server = mine->second.first;
        user = mine->second.second;
    }
    for (const Account &a : s.list)
        if (a.server == server && a.user_id == user && !a.token.empty()) {
            *out = a;
            return true;
        }
    return false;
}

void remember(const Account &a)
{
    Store s = read_store();
    bool found = false;
    for (Account &x : s.list)
        if (x.server == a.server && x.user_id == a.user_id) {
            x = a;
            found = true;
        }
    if (!found)
        s.list.push_back(a);
    s.last_server = a.server;
    s.last_user = a.user_id;
    if (s_ps5_user >= 0)
        s.by_ps5[ps5_key()] = {a.server, a.user_id};
    write_store(s);
}

void forget(const std::string &server, const std::string &user_id)
{
    Store s = read_store();
    std::vector<Account> keep;
    for (const Account &x : s.list)
        if (!(x.server == server && x.user_id == user_id))
            keep.push_back(x);
    s.list = keep;
    if (s.last_server == server && s.last_user == user_id)
        s.last_server.clear(), s.last_user.clear();
    for (auto it = s.by_ps5.begin(); it != s.by_ps5.end();)
        it = (it->second.first == server && it->second.second == user_id) ? s.by_ps5.erase(it) : std::next(it);
    write_store(s);
}

void set_last(const std::string &server, const std::string &user_id)
{
    Store s = read_store();
    s.last_server = server;
    s.last_user = user_id;
    if (s_ps5_user >= 0)
        s.by_ps5[ps5_key()] = {server, user_id};
    write_store(s);
}

} // namespace accounts
