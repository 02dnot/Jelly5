/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/seerr_service.h"
#include "app/spawn.h"
#include "app/i18n.h"
#include "jf/jf_http.h"

#include "evo_boot_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
std::atomic<bool> s_sign_in_notice{false};   /* see take_sign_in_notice; set and cleared under s_lock */
std::string s_server, s_account;        /* its Jellyfin server; "server|user id" */
unsigned s_epoch = 0;                   /* bumped when the account or the settings change */
Snapshot s_snap;
std::shared_ptr<seerr::Client> s_client;
std::atomic<unsigned> s_gen{0};
bool s_seen_ready = false;              /* signed in once since the account or settings changed */
double s_last_lost = -1e9;              /* now_ms() of the last lost session (session_lost) */
double s_lost_retry_at = 0;             /* signed out by a second loss: sign in again from then (0: no) */
constexpr double kLostWait = 60000, kLostWaitMax = 30 * 60000;
double s_lost_wait = kLostWait;         /* a loss sooner than this after the last is "again"; doubles each time */
unsigned s_lost_epoch = 0;              /* the epoch that signs in again after a lost session (its cookies are gone) */
std::map<int, std::string> s_movie_genres, s_tv_genres;
std::string s_genres_lang;              /* the language they are in */

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
    if (!text)
        return;   /* out of memory: the file stays as it was */
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

/* Writes go through one background writer: a change is queued as a patch to
 * the file's tree, and the writer reads the file, patches it and writes it.
 * Nothing waits on the disk: not the render thread (the settings change from
 * there), not anyone holding s_lock (which the settings read every frame). */
std::mutex s_disk_lock;                 /* the file: the writer, or load_stored */
std::mutex s_queue_lock;                /* s_patches, s_writing */
std::vector<std::function<void(cJSON *)>> s_patches;
bool s_writing = false;

double now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void persist(std::function<void(cJSON *)> patch)
{
    std::lock_guard<std::mutex> g(s_queue_lock);
    s_patches.push_back(std::move(patch));
    if (s_writing)
        return;
    s_writing = true;
    const bool started = jelly5::spawn([] {
        for (;;) {
            std::lock_guard<std::mutex> disk(s_disk_lock);
            std::vector<std::function<void(cJSON *)>> todo;
            {
                std::lock_guard<std::mutex> q(s_queue_lock);
                if (s_patches.empty()) {
                    s_writing = false;
                    return;
                }
                todo.swap(s_patches);
            }
            const double t0 = now_ms();
            cJSON *root = load();
            for (auto &p : todo)
                p(root);
            save(root);
            cJSON_Delete(root);
            evo_bt("seerr: seerr.json saved (%zu changes) in %.0f ms", todo.size(), now_ms() - t0);
        }
    });
    if (!started)
        s_writing = false;   /* no thread: the next change tries again (the patch stays queued) */
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
    std::string cookies;                /* always the session at config.url (or none) */
    bool signed_out = false;            /* the viewer signed out: no automatic sign-in */
    std::string quick_connect_url;      /* the Seerr address the viewer approved Quick Connect for */
};

/* The file's for an account, with the changes still on their way to it (off
 * the render thread: it reads the disk). */
Stored load_stored(const std::string &server, const std::string &account)
{
    Stored s;
    std::lock_guard<std::mutex> disk(s_disk_lock);   /* not halfway through a write */
    std::vector<std::function<void(cJSON *)>> pending;
    {
        std::lock_guard<std::mutex> q(s_queue_lock);
        pending = s_patches;
    }
    cJSON *root = load();
    for (auto &p : pending)
        p(root);
    const cJSON *srv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "servers"),
                                                        server.c_str());
    s.config.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(srv, "enabled"));
    s.config.url = str(srv, "url");
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "accounts"),
                                                        account.c_str());
    const std::string auth = str(acc, "auth");
    for (int i = 0; i < (int)Auth::Count; i++)
        if (auth == kAuthNames[i])
            s.config.auth = (Auth)i;
    /* A session is Seerr's at the address it came from: never sent to another
     * (the address changed since, by this account or another on the server).
     * Kept before the address was (no "cookiesUrl"): whose is not known, dropped. */
    if (str(acc, "cookiesUrl") == s.config.url)
        s.cookies = str(acc, "cookies");
    s.signed_out = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(acc, "signedOut"));
    s.quick_connect_url = str(acc, "quickConnectUrl");
    cJSON_Delete(root);
    return s;
}

Stored s_stored;                        /* the account in use's, kept current: read every frame */

Stored read_stored() { return s_stored; }

void write_session(const std::string &cookies, bool signed_out)
{
    if (s_account.empty())
        return;
    s_stored.cookies = cookies;
    s_stored.signed_out = signed_out;
    const std::string account = s_account, url = s_stored.config.url;
    persist([account, cookies, url, signed_out](cJSON *root) {
        cJSON *acc = child(child(root, "accounts"), account.c_str());
        put_str(acc, "cookies", cookies);
        put_str(acc, "cookiesUrl", url);   /* whose session it is (load_stored) */
        put_bool(acc, "signedOut", signed_out);
    });
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

bool local_address(const std::string &url);

std::shared_ptr<seerr::Client> make_client(const Config &c, const std::string &cookies)
{
    auto cl = std::make_shared<seerr::Client>(c.url);
    cl->set_timeout(local_address(c.url) ? 5 : 10);   /* on the home network an answer comes at once */
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
            const std::string &version, const seerr::PublicSettings &ps, Why why, std::string error,
            bool lost_session = false)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (epoch != s_epoch)
        return;   /* another account or setting since: nothing of this one is said */
    if (!ok && lost_session && why != Why::SignedOut)
        s_sign_in_notice = true;   /* a saved session lost, and not renewed here: tell the viewer once */
    s_snap.version = version;
    s_snap.settings = ps;
    if (ok && !cl->has_session()) {
        /* Signed in, but no session cookie kept: every later call would answer 401
         * and sign in again (a new Quick Connect approval each time). */
        ok = false;
        why = Why::AutoFailed;
        error = "no session cookie";
        evo_bt("seerr: signed in, but Seerr's session cookie was not kept");
    }
    if (ok) {
        s_client = cl;
        s_seen_ready = true;
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
        bool lost;
        {
            std::lock_guard<std::mutex> g(s_lock);
            st = read_stored();
            jf = s_jf;
            lost = epoch == s_lost_epoch;
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
        const bool have_settings = cl->public_settings(&ps);
        seerr::User u;
        const bool had_session = cl->has_session();
        bool ok = had_session && cl->me(&u);
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
        } else if (!ok && st.config.auth == Auth::QuickConnect && jf && !jf->features().quick_connect) {
            /* No Quick Connect on this server (Emby; Seerr has it for Jellyfin only): its password */
        } else if (!ok && st.config.auth == Auth::QuickConnect && !have_settings) {
            /* Its media server not known (a 5xx, a page that is not Seerr's): asked again later */
            const std::string err = "Seerr's public settings: " + cl->last_error();
            evo_bt("seerr: %s", err.c_str());
            publish(epoch, [&](Snapshot &s) {
                s.state = State::Unreachable;
                s.error = err;
            });
            if (!wait_retry(epoch))
                return;
            continue;
        } else if (!ok && st.config.auth == Auth::QuickConnect && ps.media_server != 2) {
            /* Seerr's media server is not Jellyfin (or did not say): its Quick Connect
             * is not this account's, so no code of ours is approved for it. */
            why = Why::MethodOff;
            error = "Seerr's media server is not Jellyfin";
        } else if (!ok && st.config.auth == Auth::QuickConnect && st.quick_connect_url != st.config.url) {
            /* Approving a code hands whoever answers at this address a Jellyfin
             * session: only for an address the viewer approved themselves. */
            why = Why::NeedApproval;
            error = "Quick Connect not approved for this address";
        } else if (!ok && st.config.auth == Auth::QuickConnect && !ps.media_server_login) {
            why = Why::MethodOff;   /* Quick Connect is a Jellyfin sign-in: Seerr has those off */
            error = "Seerr's Jellyfin sign-in is off";
        } else if (!ok && st.config.auth == Auth::QuickConnect && jf) {
            evo_bt("seerr: no session, signing in by Quick Connect");
            /* Checked again right before approving: no approval after Seerr was turned
             * off, the address changed or the account went (a slow initiate). */
            ok = cl->sign_in_quick_connect(
                [jf, epoch](const std::string &code) { return current(epoch) && jf->quick_connect_authorize(code); }, &u);
            why = cl->last_status() == 403 ? Why::NotInSeerr : Why::AutoFailed;
            error = "Quick Connect: " + cl->last_error();
        }
        finish(epoch, cl, ok, u, version, ps, why, error, had_session || lost);
        return;
    }
}

/* Starts over with the settings as they are now (call with s_lock held). */
void restart_locked()
{
    s_epoch++;
    s_snap.testing = false;   /* a test of the last epoch never answers now */
    s_client.reset();
    s_snap = Snapshot();
    s_snap.state = State::Connecting;
    s_gen++;
    const unsigned epoch = s_epoch;
    if (!jelly5::spawn([epoch] { connect_worker(epoch); })) {
        s_snap.state = State::Unreachable;   /* no thread to connect on: say so, a later change retries */
        s_snap.error = "no thread";
    }
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

namespace {
std::mutex s_noted_lock;
struct Noted {
    int status;
    double at;   /* now_ms() */
};
std::map<std::string, Noted> s_noted;   /* "tv:123" -> status, for two minutes: Seerr is read again by then */
std::atomic<unsigned> s_changes{0};
void forget_noted()
{
    std::lock_guard<std::mutex> g(s_noted_lock);
    s_noted.clear();
}
std::string noted_key(int tmdb_id, bool tv) { return (tv ? "tv:" : "movie:") + std::to_string(tmdb_id); }
} // namespace

void attach(jf::Client *client)
{
    const std::string server = client->server(), account = client->server() + "|" + client->user_id();
    const Stored loaded = load_stored(server, account);   /* the disk, before taking s_lock */
    std::lock_guard<std::mutex> g(s_lock);
    s_jf = client;
    s_server = server;
    s_account = account;
    s_sign_in_notice = false;   /* the last account's */
    s_stored = loaded;
    s_seen_ready = false;
    s_last_lost = -1e9;
    s_lost_retry_at = 0;
    s_lost_wait = kLostWait;
    forget_noted();   /* another account's requests */
    const Stored st = s_stored;
    if (st.config.enabled && !st.config.url.empty()) {
        restart_locked();
    } else {
        s_epoch++;
        s_snap.testing = false;   /* a test of the last epoch never answers now */
        s_client.reset();
        s_snap = Snapshot();
        s_gen++;
    }
}

void detach()
{
    std::lock_guard<std::mutex> g(s_lock);
    s_sign_in_notice = false;
    s_epoch++;
    s_snap.testing = false;   /* a test of the last epoch never answers now */
    s_jf = nullptr;
    s_server.clear();
    s_account.clear();
    s_stored = Stored();
    s_seen_ready = false;
    s_last_lost = -1e9;
    s_lost_retry_at = 0;
    s_lost_wait = kLostWait;
    forget_noted();
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
    s_sign_in_notice = false;   /* the viewer is at it in the settings */
    if (s_account.empty())
        return;
    const Config old = s_stored.config;
    s_stored.config = c;
    s_stored.config.url = seerr::Client::normalize(c.url);
    const Config n = s_stored.config;
    const std::string server = s_server, account = s_account;
    persist([n, server, account](cJSON *root) {
        cJSON *srv = child(child(root, "servers"), server.c_str());
        put_bool(srv, "enabled", n.enabled);
        put_str(srv, "url", n.url);
        put_str(child(child(root, "accounts"), account.c_str()), "auth", kAuthNames[(int)n.auth]);
    });
    if (n.url != old.url && !s_stored.cookies.empty())
        write_session("", s_stored.signed_out);   /* the old address's session: never sent to the new one */
    if (c.enabled == old.enabled && seerr::Client::normalize(c.url) == old.url && c.auth == old.auth)
        return;
    evo_bt("seerr: %s, %s", c.enabled ? "on" : "off", seerr::Client::normalize(c.url).c_str());
    s_seen_ready = false;
    s_last_lost = -1e9;   /* the viewer's doing: a later loss is not "again" */
    s_lost_wait = kLostWait;
    if (c.enabled && !c.url.empty()) {
        restart_locked();
    } else {
        s_epoch++;
        s_snap.testing = false;   /* a test of the last epoch never answers now */
        s_client.reset();
        s_snap = Snapshot();
        s_gen++;
    }
}

void move_server(const std::string &from, const std::string &to)
{
    if (from.empty() || from == to)
        return;
    persist([from, to](cJSON *root) {
        /* Under the new key. When something is kept there already, which one is newer
         * is not known: both stay (the old one unused) and nothing is lost. */
        auto rename = [](cJSON *parent, const std::string &a, const std::string &b) {
            if (cJSON_GetObjectItemCaseSensitive(parent, b.c_str()))
                return;
            if (cJSON *o = cJSON_DetachItemFromObjectCaseSensitive(parent, a.c_str()))
                cJSON_AddItemToObject(parent, b.c_str(), o);
        };
        rename(child(root, "servers"), from, to);
        cJSON *accounts = child(root, "accounts");
        const std::string prefix = from + "|";   /* "server|user id" */
        std::vector<std::string> keys;
        for (const cJSON *a = accounts->child; a; a = a->next)
            if (a->string && std::string(a->string).compare(0, prefix.size(), prefix) == 0)
                keys.push_back(a->string);
        for (const std::string &k : keys)
            rename(accounts, k, to + "|" + k.substr(prefix.size()));
    });
    evo_bt("seerr: settings moved with the Jellyfin server to its new address");
}

void forget_account(const std::string &server, const std::string &user_id)
{
    const std::string account = server + "|" + user_id;
    persist([account](cJSON *root) { cJSON_DeleteItemFromObjectCaseSensitive(child(root, "accounts"), account.c_str()); });
}

Snapshot snapshot()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_snap;
}

unsigned generation() { return s_gen; }

bool take_sign_in_notice() { return s_sign_in_notice.exchange(false); }

bool ready()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_snap.state == State::Ready && s_client;
}

bool available()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_stored.config.enabled && s_snap.state != State::Off &&
           (s_snap.state == State::Ready || s_seen_ready);
}

void set_language()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_client)
        s_client->set_language(tmdb_language());
}

void load_genres()
{
    std::shared_ptr<seerr::Client> cl = client();
    const std::string lang = tmdb_language();
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!cl || (s_genres_lang == lang && !s_movie_genres.empty()))
            return;
    }
    std::map<int, std::string> movie = cl->genres(false), tv = cl->genres(true);
    std::lock_guard<std::mutex> g(s_lock);
    if (movie.empty() && tv.empty())
        return;
    s_movie_genres = std::move(movie);
    s_tv_genres = std::move(tv);
    s_genres_lang = lang;
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
    s_last_lost = -1e9;   /* the viewer's doing: a later loss is not "again" */
    s_lost_wait = kLostWait;
    restart_locked();
}


std::vector<seerr::Title> visible(std::vector<seerr::Title> titles)
{
    seerr::PublicSettings ps;
    seerr::User u;
    {
        std::lock_guard<std::mutex> g(s_lock);
        ps = s_snap.settings;
        u = s_snap.user;
    }
    using S = seerr::Status;
    titles.erase(std::remove_if(titles.begin(), titles.end(),
                                [&](const seerr::Title &t) {
                                    if (ps.hide_available && (t.status == S::Available || t.status == S::PartiallyAvailable))
                                        return true;
                                    /* As Seerr's page: only for those who manage the blocklist. */
                                    if (ps.hide_blocklisted && t.status == S::Blocklisted && u.has(seerr::kManageBlocklist))
                                        return true;
                                    return ps.hide_requested && t.active_request;   /* as Seerr's own lists */
                                }),
                 titles.end());
    return titles;
}

int status_of(const jf::Item &it)
{
    const int id = it.ext.tmdb_id ? it.ext.tmdb_id : it.ext.tmdb_ref;
    if (!id)
        return it.ext.status;
    std::lock_guard<std::mutex> g(s_noted_lock);
    const auto n = s_noted.find(noted_key(id, it.type == "Series"));
    return n != s_noted.end() && now_ms() - n->second.at < 120000 ? n->second.status : it.ext.status;
}

void note_status(int tmdb_id, bool tv, int status)
{
    {
        std::lock_guard<std::mutex> g(s_noted_lock);
        s_noted[noted_key(tmdb_id, tv)] = {status, now_ms()};
    }
    s_changes++;
}

unsigned changes() { return s_changes.load(); }

void approve_quick_connect()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    const std::string url = s_stored.config.url;
    s_stored.quick_connect_url = url;
    const std::string account = s_account;
    persist([account, url](cJSON *root) { put_str(child(child(root, "accounts"), account.c_str()), "quickConnectUrl", url); });
    evo_bt("seerr: Quick Connect approved for %s", url.c_str());
    s_last_lost = -1e9;   /* the viewer's doing: a later loss is not "again" */
    s_lost_wait = kLostWait;
    write_session(read_stored().cookies, false);
    restart_locked();
}

void session_lost(const seerr::Client *c)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty() || s_snap.state != State::Ready)
        return;   /* already on it */
    if (c != s_client.get())
        return;   /* a late answer to an earlier session's client: this one may be fine */
    /* A session that keeps ending must not sign in (and approve a Quick Connect
     * code) on every search: after a minute, then twice as long each time it
     * ends again that soon (to half an hour); one that lasts starts over. */
    const double t = now_ms();
    if (t - s_last_lost < s_lost_wait) {
        evo_bt("seerr: the session ended again soon; signing in again in %.0f s", s_lost_wait / 1000);
        s_lost_retry_at = s_last_lost + s_lost_wait;   /* poll() then */
        s_lost_wait = std::min(s_lost_wait * 2, kLostWaitMax);
        s_snap.state = State::SignedOut;
        s_snap.why = Why::SignedOut;   /* "Ikke pålogget – ✕": whatever the way of signing in */
        s_client.reset();
        s_gen++;
        return;
    }
    s_last_lost = t;
    s_lost_wait = kLostWait;
    evo_bt("seerr: the session ended, signing in again");
    write_session("", false);
    restart_locked();
    s_lost_epoch = s_epoch;   /* (no cookies now: finish still says the session was lost) */
}

void poll()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_lost_retry_at <= 0 || now_ms() < s_lost_retry_at)
        return;
    s_lost_retry_at = 0;
    if (s_account.empty() || s_snap.state != State::SignedOut || s_snap.why != Why::SignedOut)
        return;   /* signed in (or out on purpose) meanwhile */
    s_last_lost = now_ms();
    evo_bt("seerr: signing in again after the lost sessions");
    write_session("", false);
    restart_locked();
    s_lost_epoch = s_epoch;
}

void sign_in(const std::string &user, const std::string &password)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    const Stored st = read_stored();
    const std::string name = !user.empty() ? user : s_jf ? s_jf->user_name() : std::string();
    s_last_lost = -1e9;   /* the viewer's doing: a later loss is not "again" */
    s_lost_wait = kLostWait;
    s_epoch++;
    s_snap.testing = false;   /* a test of the last epoch never answers now */
    s_client.reset();
    s_snap.state = State::Connecting;
    s_gen++;
    const unsigned epoch = s_epoch;
    const bool started = jelly5::spawn([epoch, st, name, password] {
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
        const bool local = st.config.auth == Auth::Local;
        if (local ? !ps.local_login : !ps.media_server_login) {   /* Seerr has this way switched off */
            finish(epoch, cl, false, u, version, ps, Why::MethodOff, "sign-in method off in Seerr");
            return;
        }
        const bool ok = local ? cl->sign_in_local(name, password, &u) : cl->sign_in_jellyfin(name, password, &u);
        if (!ok && cl->last_unreachable()) {   /* gone between two requests: not a wrong password */
            const std::string err = cl->last_error();
            publish(epoch, [&](Snapshot &s) {
                s.state = State::Unreachable;
                s.error = err;
            });
            return;
        }
        /* 403: Seerr knows no such user (a Jellyfin user not imported, new sign-ins
         * off); else the name or the password. */
        finish(epoch, cl, ok, u, version, ps, cl->last_status() == 403 && !local ? Why::NotInSeerr : Why::WrongPassword,
               cl->last_error());
    });
    if (!started) {
        s_snap.state = State::Unreachable;
        s_snap.error = "no thread";
    }
}

void sign_out()
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_account.empty())
        return;
    std::shared_ptr<seerr::Client> cl = s_client;
    s_epoch++;
    s_snap.testing = false;   /* a test of the last epoch never answers now */
    s_client.reset();
    write_session("", true);
    s_lost_retry_at = 0;
    s_seen_ready = false;   /* signed out on purpose: Seerr's tab goes */
    s_snap.state = State::SignedOut;
    s_snap.user = seerr::User();
    s_snap.why = Why::SignedOut;
    s_gen++;
    evo_bt("seerr: signed out");
    if (cl)   /* ends the session on the server too */
        jelly5::spawn([cl] { cl->sign_out(); });   /* best effort: the session is dropped here anyway */
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
    const bool started = jelly5::spawn([epoch, st] {
        auto cl = make_client(st.config, st.cookies);
        std::string line, version;
        char buf[512];
        seerr::User u;
        if (!cl->status(&version)) {
            line = (cl->last_unreachable() ? T("Seerr svarer ikke på ") : T("Ingen Seerr-server på ")) + cl->url();
            if (cl->last_unreachable() && !local_address(cl->url()))   /* a public address the console may not reach */
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
    });
    if (!started) {
        std::lock_guard<std::mutex> g(s_lock);
        s_snap.testing = false;
        s_gen++;
    }
}

std::string image_url(const std::string &path, const char *size)
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_client ? s_client->image_url(path, size) : std::string();
}

jf::Item to_item(const seerr::Title &t)
{
    jf::Item it;
    it.id = std::string("seerr:") + (t.tv ? "tv:" : "movie:") + std::to_string(t.id);
    it.name = t.name;
    it.type = t.tv ? "Series" : "Movie";
    it.overview = t.overview;
    it.year = t.year;
    it.community_rating = t.vote;
    it.premiere_date = t.date;
    it.tmdb_id = std::to_string(t.id);
    {
        std::lock_guard<std::mutex> g(s_lock);
        const std::map<int, std::string> &names = t.tv ? s_tv_genres : s_movie_genres;
        for (int id : t.genre_ids) {
            const auto n = names.find(id);
            if (n != names.end())
                it.genres.push_back(n->second);
        }
    }
    it.ext.tmdb_id = t.id;
    it.ext.status = (int)t.status;
    it.ext.jellyfin_id = t.jellyfin_id;
    /* Sizes as the server's art is asked for: posters 480 wide, cards 640, backdrops the screen. */
    it.ext.poster = image_url(t.poster, "w500");
    it.ext.thumb = image_url(t.backdrop, "w780");
    it.ext.backdrop = image_url(t.backdrop, "w1280");   /* behind the rows, changing as they are browsed: light (the page loads the original) */
    return it;
}

} // namespace seerr_service
