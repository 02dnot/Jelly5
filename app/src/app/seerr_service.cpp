/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/seerr_service.h"
#include "app/i18n.h"
#include "jf/jf_http.h"

#include "evo_boot_trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

#ifndef JELLY5_SEERR_URL
#define JELLY5_SEERR_URL ""   /* development builds: SEERR_URL from .env.local */
#endif

namespace seerr_service {
namespace {

constexpr const char *kDir = "/download0/jelly5";
constexpr const char *kFile = "/download0/jelly5/seerr.json";
constexpr const char *kAuthNames[] = {"quickconnect", "jellyfin", "local"};
constexpr int kRetrySeconds = 30;

/* Everything below; held only for moments (never across a request). */
std::mutex s_lock;
jf::Client *s_jf = nullptr;             /* the account in use (clients are never freed) */
std::string s_server, s_account;        /* its Jellyfin server; "server|user id" */
unsigned s_epoch = 0;                   /* bumped when the account or the settings change */
Snapshot s_snap;
std::shared_ptr<seerr::Client> s_client;
std::atomic<unsigned> s_gen{0};

/* ---- seerr.json ------------------------------------------------------------------ */
cJSON *load()
{
    std::string body;
    if (FILE *f = std::fopen(kFile, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            body.append(buf, n);
        std::fclose(f);
    }
    cJSON *j = cJSON_Parse(body.c_str());
    return cJSON_IsObject(j) ? j : (cJSON_Delete(j), cJSON_CreateObject());
}

void save(cJSON *root)
{
    mkdir(kDir, 0777);
    char *text = cJSON_PrintUnformatted(root);
    /* Beside, then renamed: a crash mid-write never loses the file. */
    const std::string tmp = std::string(kFile) + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        std::fputs(text, f);
        std::fclose(f);
        std::rename(tmp.c_str(), kFile);
    } else {
        evo_bt("seerr: cannot write %s", kFile);
    }
    std::free(text);
}

/* parent[key], made when missing. */
cJSON *child(cJSON *parent, const char *key)
{
    cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(o)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        o = cJSON_AddObjectToObject(parent, key);
    }
    return o;
}

std::string str(const cJSON *o, const char *k)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return v ? v : "";
}

void put_str(cJSON *o, const char *k, const std::string &v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddStringToObject(o, k, v.c_str());
}

void put_bool(cJSON *o, const char *k, bool v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddBoolToObject(o, k, v);
}

/* What is kept for the account in use (call with s_lock held). */
struct Stored {
    Config config;
    std::string cookies;
    bool signed_out = false;            /* the viewer signed out: no automatic sign-in */
};

/* The file's, for the account in use (call with s_lock held). */
Stored load_stored()
{
    Stored s;
    cJSON *root = load();
    s.config.internet = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "internet"));
    const cJSON *srv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "servers"),
                                                        s_server.c_str());
    s.config.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(srv, "enabled"));
    s.config.url = str(srv, "url");
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "accounts"),
                                                        s_account.c_str());
    const std::string auth = str(acc, "auth");
    for (int i = 0; i < (int)Auth::Count; i++)
        if (auth == kAuthNames[i])
            s.config.auth = (Auth)i;
    s.cookies = str(acc, "cookies");
    s.signed_out = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(acc, "signedOut"));
    cJSON_Delete(root);
    return s;
}

Stored s_stored;                        /* load_stored(), kept current: read every frame */

Stored read_stored() { return s_stored; }

void write_session(const std::string &cookies, bool signed_out)
{
    if (s_account.empty())
        return;
    s_stored.cookies = cookies;
    s_stored.signed_out = signed_out;
    cJSON *root = load();
    cJSON *acc = child(child(root, "accounts"), s_account.c_str());
    put_str(acc, "cookies", cookies);
    put_bool(acc, "signedOut", signed_out);
    save(root);
    cJSON_Delete(root);
}

/* ---- state ----------------------------------------------------------------------- */
std::string tmdb_language()
{
    switch (i18n::lang()) {
    case i18n::Lang::Norwegian: return "nb";
    case i18n::Lang::Spanish: return "es";
    case i18n::Lang::French: return "fr";
    case i18n::Lang::German: return "de";
    case i18n::Lang::Portuguese: return "pt";
    case i18n::Lang::Italian: return "it";
    default: return "en";
    }
}

std::shared_ptr<seerr::Client> make_client(const Config &c, const std::string &cookies)
{
    auto cl = std::make_shared<seerr::Client>(c.url);
    cl->set_timeout(c.internet ? 10 : 5);   /* on the local network an answer comes at once */
    cl->set_language(tmdb_language());
    cl->set_cookies(cookies);
    return cl;
}

bool current(unsigned epoch)
{
    std::lock_guard<std::mutex> g(s_lock);
    return epoch == s_epoch;
}

/* Changes the snapshot, if this worker's results still count. */
template <class F> void publish(unsigned epoch, F change)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (epoch != s_epoch)
        return;
    change(s_snap);
    s_gen++;
}

/* A sign-in came to an end, one way or the other. */
void finish(unsigned epoch, const std::shared_ptr<seerr::Client> &cl, bool ok, const seerr::User &u,
            const std::string &version, const seerr::PublicSettings &ps, Why why, const std::string &error)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (epoch != s_epoch)
        return;
    s_snap.version = version;
    s_snap.settings = ps;
    if (ok) {
        s_client = cl;
        s_snap.state = State::Ready;
        s_snap.user = u;
        s_snap.why = Why::None;
        s_snap.error.clear();
        write_session(cl->cookies(), false);
        evo_bt("seerr: signed in to Seerr %s as %s (user %d, permissions %u)", version.c_str(), u.name.c_str(),
               u.id, u.permissions);
    } else {
        s_client.reset();
        s_snap.state = State::SignedOut;
        s_snap.user = seerr::User();
        s_snap.why = why;
        s_snap.error = error;
        evo_bt("seerr: not signed in: %s", error.c_str());
    }
    s_gen++;
}

/* Waits out the retry delay; false when this worker is no longer wanted. */
bool wait_retry(unsigned epoch)
{
    for (int i = 0; i < kRetrySeconds * 10; i++) {
        if (!current(epoch))
            return false;
        usleep(100 * 1000);
    }
    return current(epoch);
}

/* Connects with the saved session; signs in by Quick Connect when it has none
 * (and that is the method). An address that does not answer is tried again. */
void connect_worker(unsigned epoch)
{
    while (current(epoch)) {
        Stored st;
        jf::Client *jf;
        {
            std::lock_guard<std::mutex> g(s_lock);
            st = read_stored();
            jf = s_jf;
        }
        if (!st.config.enabled || st.config.url.empty()) {
            publish(epoch, [](Snapshot &s) { s = Snapshot(); });
            return;
        }
        publish(epoch, [](Snapshot &s) {
            s.state = State::Connecting;
            s.error.clear();
        });
        auto cl = make_client(st.config, st.cookies);
        std::string version;
        if (!cl->status(&version)) {
            const std::string err = cl->last_error();
            evo_bt("seerr: %s: %s", cl->url().c_str(), err.c_str());
            publish(epoch, [&](Snapshot &s) {
                s.state = State::Unreachable;
                s.error = err;
            });
            if (!wait_retry(epoch))
                return;
            continue;
        }
        seerr::PublicSettings ps;
        cl->public_settings(&ps);
        seerr::User u;
        bool ok = cl->has_session() && cl->me(&u);
        if (ok)
            evo_bt("seerr: the saved session is still valid");
        if (!ok && cl->last_unreachable()) {   /* gone again between two requests */
            if (!wait_retry(epoch))
                return;
            continue;
        }
        Why why = Why::NeedPassword;
        std::string error = "no session; the sign-in method needs a password";
        if (!ok && st.signed_out) {
            why = Why::SignedOut;
            error = "signed out";
        } else if (!ok && st.config.auth == Auth::QuickConnect && jf) {
            evo_bt("seerr: no session, signing in by Quick Connect");
            ok = cl->sign_in_quick_connect([jf](const std::string &code) { return jf->quick_connect_authorize(code); },
                                           &u);
            why = Why::AutoFailed;
            error = "Quick Connect: " + cl->last_error();
        }
        finish(epoch, cl, ok, u, version, ps, why, error);
        return;
    }
}

/* Starts over with the settings as they are now (call with s_lock held). */
void restart_locked()
{
    s_epoch++;
    s_client.reset();
    s_snap = Snapshot();
    s_snap.state = State::Connecting;
    s_gen++;
    const unsigned epoch = s_epoch;
    std::thread([epoch] { connect_worker(epoch); }).detach();
}

/* Private addresses: what the console reaches without Internet. */
bool local_address(const std::string &url)
{
    size_t a = url.find("://");
    a = a == std::string::npos ? 0 : a + 3;
    std::string host = url.substr(a, url.find_first_of(":/", a) - a);
    if (host.empty())
        return true;
    if (host.find('.') == std::string::npos)
        return true;   /* a bare name on the local network */
    for (const char *suffix : {".local", ".lan", ".home", ".internal", ".home.arpa"}) {
        const std::string s = suffix;
        if (host.size() > s.size() && host.compare(host.size() - s.size(), s.size(), s) == 0)
            return true;
    }
    int b[4];
    if (std::sscanf(host.c_str(), "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3]) == 4)
        return b[0] == 10 || b[0] == 127 || (b[0] == 192 && b[1] == 168) || (b[0] == 172 && b[1] >= 16 && b[1] < 32) ||
               (b[0] == 169 && b[1] == 254);
    return false;
}

} // namespace

void attach(jf::Client *client)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_jf = client;
    s_server = client->server();
    s_account = client->server() + "|" + client->user_id();
    s_stored = load_stored();
    const Stored st = s_stored;
    if (st.config.enabled && !st.config.url.empty()) {
        restart_locked();
    } else {
        s_epoch++;
        s_client.reset();
        s_snap = Snapshot();
        s_gen++;
    }
}

void detach()
{
    std::lock_guard<std::mutex> g(s_lock);
    s_epoch++;
    s_jf = nullptr;
    s_server.clear();
    s_account.clear();
    s_stored = Stored();
    s_client.reset();
    s_snap = Snapshot();
    s_gen++;
}

Config config()
{
    std::lock_guard<std::mutex> g(s_lock);
    return read_stored().config;
}

void set_config(const Config &c)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    const Config old = s_stored.config;
    s_stored.config = c;
    s_stored.config.url = seerr::Client::normalize(c.url);
    cJSON *root = load();
    put_bool(root, "internet", c.internet);
    cJSON *srv = child(child(root, "servers"), s_server.c_str());
    put_bool(srv, "enabled", c.enabled);
    put_str(srv, "url", seerr::Client::normalize(c.url));
    put_str(child(child(root, "accounts"), s_account.c_str()), "auth", kAuthNames[(int)c.auth]);
    save(root);
    cJSON_Delete(root);
    if (c.enabled == old.enabled && seerr::Client::normalize(c.url) == old.url && c.auth == old.auth &&
        c.internet == old.internet)
        return;
    evo_bt("seerr: %s, %s", c.enabled ? "on" : "off", seerr::Client::normalize(c.url).c_str());
    if (c.enabled && !c.url.empty()) {
        restart_locked();
    } else {
        s_epoch++;
        s_client.reset();
        s_snap = Snapshot();
        s_gen++;
    }
}

Snapshot snapshot()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_snap;
}

unsigned generation() { return s_gen; }

bool ready()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_snap.state == State::Ready && s_client;
}

std::shared_ptr<seerr::Client> client()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_snap.state == State::Ready ? s_client : nullptr;
}

std::string suggested_url()
{
    if (*JELLY5_SEERR_URL)
        return JELLY5_SEERR_URL;
    std::lock_guard<std::mutex> g(s_lock);
    /* The Jellyfin server's host, without its port, on Seerr's. */
    size_t a = s_server.find("://");
    a = a == std::string::npos ? 0 : a + 3;
    std::string host = s_server.substr(a, s_server.find('/', a) - a);
    if (!host.empty() && host[0] == '[')
        host = host.substr(0, host.find(']') + 1);   /* IPv6 */
    else
        host = host.substr(0, host.find(':'));
    return host.empty() ? std::string() : "http://" + host + ":5055";
}

void reconnect()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    write_session(read_stored().cookies, false);   /* asked for: sign in automatically again */
    restart_locked();
}

void session_lost()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty() || s_snap.state != State::Ready)
        return;   /* already on it */
    evo_bt("seerr: the session ended, signing in again");
    write_session("", false);
    restart_locked();
}

void sign_in(const std::string &user, const std::string &password)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    const Stored st = read_stored();
    const std::string name = !user.empty() ? user : s_jf ? s_jf->user_name() : std::string();
    s_epoch++;
    s_client.reset();
    s_snap.state = State::Connecting;
    s_gen++;
    const unsigned epoch = s_epoch;
    std::thread([epoch, st, name, password] {
        auto cl = make_client(st.config, std::string());
        std::string version;
        seerr::PublicSettings ps;
        if (!cl->status(&version)) {
            const std::string err = cl->last_error();
            publish(epoch, [&](Snapshot &s) {
                s.state = State::Unreachable;
                s.error = err;
            });
            return;
        }
        cl->public_settings(&ps);
        seerr::User u;
        const bool ok = st.config.auth == Auth::Local ? cl->sign_in_local(name, password, &u)
                                                      : cl->sign_in_jellyfin(name, password, &u);
        finish(epoch, cl, ok, u, version, ps, cl->last_unreachable() ? Why::AutoFailed : Why::WrongPassword,
               cl->last_error());
    }).detach();
}

void sign_out()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    std::shared_ptr<seerr::Client> cl = s_client;
    s_epoch++;
    s_client.reset();
    write_session("", true);
    s_snap.state = State::SignedOut;
    s_snap.user = seerr::User();
    s_snap.why = Why::SignedOut;
    s_gen++;
    evo_bt("seerr: signed out");
    if (cl)   /* ends the session on the server too */
        std::thread([cl] { cl->sign_out(); }).detach();
}

void test()
{
    Stored st;
    unsigned epoch;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (s_account.empty())
            return;
        st = read_stored();
        epoch = s_epoch;
        s_snap.testing = true;
        s_gen++;
    }
    std::thread([epoch, st] {
        auto cl = make_client(st.config, st.cookies);
        std::string line, version;
        char buf[512];
        seerr::User u;
        if (!cl->status(&version)) {
            line = (cl->last_unreachable() ? T("Seerr svarer ikke på ") : T("Ingen Seerr-server på ")) + cl->url();
            if (cl->last_unreachable() && !st.config.internet && !local_address(cl->url()))
                line += T(" \xE2\x80\x93 bruk den lokale adressen");
        } else if (!cl->me(&u)) {
            std::snprintf(buf, sizeof buf, T("Seerr %s svarer, men du er ikke pålogget"), version.c_str());
            line = buf;
        } else {
            /* A poster through Seerr's image cache, as the pages will load them. */
            std::string poster;
            for (const seerr::Title &t : cl->discover(seerr::Client::Shelf::Trending))
                if (poster.empty())
                    poster = t.poster;
            const jf::HttpResponse r = jf::http_request("GET", cl->image_url(poster, "w92"), {}, "", 8);
            std::snprintf(buf, sizeof buf, T("OK \xE2\x80\x93 Seerr %s, pålogget som %s"), version.c_str(),
                          u.name.c_str());
            line = buf;
            if (poster.empty() || !r.ok() || r.body.empty())
                line += T(" \xE2\x80\x93 men bildene kommer ikke");
        }
        evo_bt("seerr: test: %s", line.c_str());
        publish(epoch, [&](Snapshot &s) {
            s.testing = false;
            s.test = line;
        });
    }).detach();
}

std::string image_url(const std::string &path, const char *size)
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_client ? s_client->image_url(path, size) : std::string();
}

} // namespace seerr_service
