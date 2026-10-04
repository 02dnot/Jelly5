/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The app: accounts ("Hvem ser på?", sign-in with a password or Quick Connect),
 * the tabbed UI (Hjem, Filmer, Serier, Søk, Innstillinger) drawn on the GPU,
 * detail pages, and playback on the native player with the PS5 device profile.
 *
 * Hardware bring-up follows Nuvio PS5 (and EVO Player before it): decoder
 * probes and module loads run before anything else touches the process.
 */
#include "jelly5_playback.h"
#include "jf/jf_http.h"
#include "app/accounts.h"
#include "app/i18n.h"
#include "app/perf.h"
#include "app/remote.h"
#include "app/syncplay.h"
#include "app/settings.h"
#include "platform/ime.h"
#include "jf/jf_client.h"
#include "nuvio_input.h"
#include "nuvio_player.h"
#include "gfx/art.h"
#include "gfx/gfx.h"
#include "gfx/gfx_pool.h"
#include "ui/album.h"
#include "ui/detail.h"
#include "ui/home.h"
#include "ui/library.h"
#include "ui/nav.h"
#include "ui/now_playing.h"
#include "ui/person.h"
#include "ui/login.h"
#include "ui/profiles.h"
#include "ui/screensaver.h"
#include "ui/search.h"
#include "ui/settings_screen.h"
#include "ui/syncplay_screen.h"
#include "ui_image.h"
#include "ui_text.h"

#include "evo_adec.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_direct_mem.h"
#include "evo_hw.h"
#include "evo_vdec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "cJSON.h"
#include <libavformat/avformat.h>
#include <libavutil/log.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
int sceSystemServiceHideSplashScreen(void);
void evo_log_alloc_state(const char *when);
extern int g_ps5_user_id;
}

#ifndef JELLY5_SERVER
#define JELLY5_SERVER ""   /* set from JF_URL in .env.local at build time; else the login asks */
#endif
#ifndef JELLY5_VERSION
#define JELLY5_VERSION "0.0.1"
#endif

namespace evo {
extern int DisplayWidth;
extern int DisplayHeight;
}

namespace {

/* The player's decode surfaces (UI textures have their own pool, src/gfx). */
constexpr size_t kDirectMemPool = 128u * 1024u * 1024u;

int s_user = 0;

/* ---- app state: written by worker threads, read by the render loop ---------------- */
enum class Phase { Connecting, Gate, Loading, Home, Failed };
enum class Gate { None, Profiles, Login };

struct GateRequest {
    Gate kind = Gate::None;
    std::string server, user;
    bool can_cancel = false;
};

struct State {
    std::mutex lock;
    Phase phase = Phase::Connecting;
    std::string message;
    std::string server_name, server_version;
    ui::HomeModel model;
    std::vector<jf::Item> views;        /* the user's libraries, in their order */
    GateRequest gate;                   /* a gate screen the main thread should open */
};
State s_state;
unsigned s_model_version = 0, s_home_version = 0;   /* model published / taken by Home */
std::atomic<unsigned> s_session{0};                 /* bumped on every account change */

/* One client per session, configured before anyone uses it and never changed or
 * freed after: a request still running for the last account finishes on its own
 * client (and its result is dropped), never on the new account's half-set one. */
jf::Client *s_client = nullptr;
std::vector<std::unique_ptr<jf::Client>> s_clients;
std::string s_device;

jf::Client *new_client(const std::string &server)
{
    s_clients.emplace_back(new jf::Client(server, s_device, "PlayStation 5"));
    return s_clients.back().get();
}

jf::Client *client_for(const accounts::Account &a)
{
    jf::Client *c = new_client(a.server);
    c->set_session(a.token, a.user_id, a.user_name);
    c->note_image_tag(a.image_tag);
    return c;
}

void set_phase(Phase p, const std::string &message = std::string())
{
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.phase = p;
    s_state.message = message;
}

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void notify(const char *text)
{
    struct { char pad[45]; char msg[3075]; } n;
    std::memset(&n, 0, sizeof n);
    std::snprintf(n.msg, sizeof n.msg, "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

void av_log_to_boot_log(void *, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    int n = std::vsnprintf(line, sizeof line, fmt, vl);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    /* Every JSON request over avio ends this way (no Content-Length): not news. */
    if (n && !std::strstr(line, "Stream ends prematurely"))
        evo_bt("ffmpeg[%d]: %s", level, line);
}

void crash_handler(int sig, siginfo_t *si, void *)
{
    evo_bt("jelly5: CRASH signal=%d addr=%p", sig, si ? si->si_addr : nullptr);
    signal(sig, SIG_DFL);   /* re-fault so the kernel writes its crash report */
}

void install_crash_handler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

int init_hardware()
{
    evo_vdec_probe();
    evo_adec_native_probe();
    evo_hw_probe();

    if (evo_agc_runtime_init(evo::DisplayWidth, evo::DisplayHeight, 0) != 0) {
        evo_bt("jelly5: display init failed");
        return -1;
    }
    evo_agc_runtime_get_size(&evo::DisplayWidth, &evo::DisplayHeight);
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_present();
    evo_bt("jelly5: display %dx%d hdr=%d 120hz=%d", evo::DisplayWidth, evo::DisplayHeight,
           evo_agc_runtime_is_display_hdr(), evo_agc_runtime_supports_120hz());
    sceSystemServiceHideSplashScreen();

    av_log_set_callback(av_log_to_boot_log);
    av_log_set_level(AV_LOG_WARNING);
    avformat_network_init();
    evo_direct_mem_init(kDirectMemPool);

    sceUserServiceInitialize(nullptr);
    scePadInit();
    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    g_ps5_user_id = s_user = users[0];
    evo_bt("jelly5: user %d", users[0]);
    nuvio_player_init(users[0]);
    return 0;
}

/* ---- signing in --------------------------------------------------------------------- */
void request_gate(Gate kind, const std::string &server = std::string(), const std::string &user = std::string(),
                  bool can_cancel = false)
{
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.gate = {kind, server, user, can_cancel};
    s_state.phase = Phase::Gate;
}

/* The hero's titles are cached per user between launches: the server's random
 * pick takes over a second, so a start shows the last pick at once and fetches
 * the next one behind it. */
std::string hero_file(const jf::Client &c) { return "/download0/jelly5/hero-" + c.user_id() + ".json"; }

std::string read_file(const std::string &path)
{
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (4u << 20))
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void write_file(const std::string &path, const std::string &data)
{
    mkdir("/download0/jelly5", 0777);
    const std::string tmp = path + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (ok)
            std::rename(tmp.c_str(), path.c_str());
    }
}

void refresh_hero_cache(jf::Client &c, std::vector<jf::Item> *out)
{
    std::string raw;
    std::vector<jf::Item> fresh = c.featured(6, &raw);
    if (!fresh.empty())
        write_file(hero_file(c), raw);
    if (out)
        *out = std::move(fresh);
}

/* Loads the home rows, all requests in parallel. With keep_hero the featured
 * titles are kept (a refresh after playback only needs the rows). */
bool draw_connection(double now);   /* below: the note when the server is out of reach */

/* "00.001.001" or "v0.1.1" as one comparable number (major, minor, patch). */
long version_number(const std::string &v)
{
    int a = 0, b = 0, c = 0;
    std::sscanf(v.c_str() + (v[0] == 'v' ? 1 : 0), "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

/* Opt-in (Innstillinger: Se etter oppdateringer): once a launch, ask GitHub for
 * the latest release and say so when there is a newer one. The only request
 * Jelly5 makes that is not to the Jellyfin server, and only when turned on. */
void check_for_update()
{
    static bool asked = false;
    if (asked || !settings::get().local.check_updates)
        return;
    asked = true;
    std::thread([] {
        const jf::HttpResponse r =
            jf::http_request("GET", "https://api.github.com/repos/02dnot/Jelly5/releases/latest",
                             {"Accept: application/vnd.github+json", "User-Agent: Jelly5/" JELLY5_VERSION}, "", 10);
        if (!r.ok())
            return;
        cJSON *j = cJSON_Parse(r.body.c_str());
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(j, "tag_name");
        const std::string latest = cJSON_IsString(tag) && tag->valuestring ? tag->valuestring : "";
        cJSON_Delete(j);
        evo_bt("jelly5: latest release %s, this is %s", latest.c_str(), JELLY5_VERSION);
        if (!latest.empty() && version_number(latest) > version_number(JELLY5_VERSION)) {
            char msg[160];
            std::snprintf(msg, sizeof msg, T("Jelly5 %s er tilgjengelig – se GitHub"), latest.c_str());
            notify(msg);
        }
    }).detach();
}

void load_home(jf::Client &c, unsigned session, bool keep_hero = false)
{
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        if (session != s_session)
            return;
        if (s_state.phase != Phase::Home) {
            s_state.phase = Phase::Loading;
            s_state.message = T("Henter biblioteket \xE2\x80\xA6");
        }
    }
    std::vector<jf::Item> hero, resume, next, views, mylist;
    std::vector<std::thread> jobs;
    if (!keep_hero) {
        hero = c.featured_from(read_file(hero_file(c)), 6);
        if (hero.empty())
            jobs.emplace_back([&] { refresh_hero_cache(c, &hero); });
        else   /* the next launch's pick (the client outlives every request) */
            std::thread([&c] { refresh_hero_cache(c, nullptr); }).detach();
    }
    jobs.emplace_back([&] { resume = c.resume(20); });
    jobs.emplace_back([&] { next = c.next_up(20); });
    jobs.emplace_back([&] { views = c.views(); });
    jobs.emplace_back([&] { mylist = c.favorites(30); });
    std::vector<std::string> sections;
    jobs.emplace_back([&] { sections = c.home_sections(); });
    for (auto &j : jobs)
        j.join();
    /* Rows for video libraries; music opens from Biblioteker. Books and photos are not here. */
    auto video = [](const jf::Item &v) {
        const std::string &t = v.collection_type;
        return t == "movies" || t == "tvshows" || t == "homevideos" || t == "musicvideos" || t == "boxsets" ||
               t.empty();
    };
    /* A "Nylig lagt til" row per video library, unless the user left it out (Jellyfin:
     * Innstillinger -> Hjem). */
    const std::vector<std::string> &excluded = c.latest_excludes();
    auto has_latest = [&](const jf::Item &v) {
        return video(v) && v.collection_type != "boxsets" &&
               std::find(excluded.begin(), excluded.end(), v.id) == excluded.end();
    };
    bool has_music = false;
    for (const jf::Item &v : views)
        has_music = has_music || v.collection_type == "music";
    /* Biblioteker: what no tab covers (home videos, mixed, collections; playlists when
     * there is no Musikk tab). Books, photos and Live TV are not here. */
    auto untabbed = [&](const jf::Item &v) {
        const std::string &t = v.collection_type;
        return t == "homevideos" || t == "musicvideos" || t == "boxsets" || t.empty() ||
               (t == "playlists" && !has_music);
    };
    std::vector<std::pair<std::string, std::vector<jf::Item>>> latest;
    for (const jf::Item &v : views)
        if (has_latest(v))
            latest.push_back({T("Nylig lagt til i ") + v.name, {}});
    jobs.clear();
    size_t k = 0;
    for (const jf::Item &v : views)
        if (has_latest(v)) {
            auto *slot = &latest[k++].second;
            const std::string id = v.id;
            jobs.emplace_back([&c, slot, id] { *slot = c.latest(id, 20); });
        }
    for (auto &j : jobs)
        j.join();

    ui::HomeModel m;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        m.hero = keep_hero ? s_state.model.hero : std::move(hero);
    }
    using Row = ui::HomeRow;
    if (!resume.empty()) m.rows.push_back({T("Fortsett å se"), std::move(resume), true, Row::Resume});
    if (!next.empty()) m.rows.push_back({T("Neste episode"), std::move(next), true, Row::NextUp});
    if (!mylist.empty()) m.rows.push_back({T("Min liste"), std::move(mylist), false, Row::MyList});
    for (auto &l : latest)
        if (!l.second.empty())
            m.rows.push_back({l.first, std::move(l.second), false, Row::Latest});
    std::vector<jf::Item> libs;
    for (const jf::Item &v : views)
        if (untabbed(v))
            libs.push_back(v);
    {   /* recommendations and genres from the last time they loaded (load_extras) */
        std::lock_guard<std::mutex> g(s_state.lock);
        for (const Row &r : s_state.model.rows)
            if (r.kind == Row::Recommended || r.kind == Row::Genre)
                m.rows.push_back(r);
    }
    if (!libs.empty())
        m.rows.push_back({T("Biblioteker"), std::move(libs), false, Row::Libraries});
    /* The user's own order from Jellyfin (Settings -> Home), when they set one:
     * its sections in that order, the ones left out hidden; ours (Min liste,
     * recommendations, genres) after them; Biblioteker always, as it is the way
     * to what no tab covers. */
    if (!sections.empty()) {
        std::vector<Row> ordered;
        auto take = [&](Row::Kind kind) {
            for (auto it = m.rows.begin(); it != m.rows.end();)
                if (it->kind == kind) {
                    ordered.push_back(std::move(*it));
                    it = m.rows.erase(it);
                } else {
                    ++it;
                }
        };
        for (const std::string &sec : sections) {
            if (sec == "resume") take(Row::Resume);
            else if (sec == "nextup") take(Row::NextUp);
            else if (sec == "latestmedia") take(Row::Latest);
            else if (sec == "smalllibrarytiles" || sec == "librarybuttons") take(Row::Libraries);
        }
        for (auto it = m.rows.begin(); it != m.rows.end();)   /* Jellyfin's, left out: hidden */
            if (it->kind == Row::Resume || it->kind == Row::NextUp || it->kind == Row::Latest)
                it = m.rows.erase(it);
            else
                ++it;
        for (Row &r : m.rows)
            ordered.push_back(std::move(r));
        m.rows = std::move(ordered);
    }
    std::lock_guard<std::mutex> g(s_state.lock);
    if (session != s_session)
        return;   /* the account changed while this loaded */
    s_state.model = std::move(m);
    s_state.views = std::move(views);
    s_model_version++;
    s_state.phase = Phase::Home;
    s_state.message.clear();
}

/* Jellyfin's recommendations ("Fordi du så ...") and a few genres, under the
 * rows. The server takes seconds over recommendations, so the home screen
 * never waits for them: they are added when they arrive. Genres are picked
 * once per session (rows should not reshuffle while browsing). */
/* TMDB's genre names (what Jellyfin's metadata carries) in Norwegian. */
std::string genre_title(const std::string &g)
{
    if (i18n::english())
        return g;   /* the metadata's own (English) names */
    static const std::map<std::string, std::string> no = {
        {"Action", "Action"}, {"Adventure", "Eventyr"}, {"Action & Adventure", "Action og eventyr"},
        {"Animation", "Animasjon"}, {"Comedy", "Komedie"}, {"Crime", "Krim"}, {"Documentary", "Dokumentar"},
        {"Drama", "Drama"}, {"Family", "Familie"}, {"Fantasy", "Fantasy"}, {"History", "Historie"},
        {"Horror", "Skrekk"}, {"Kids", "Barn"}, {"Music", "Musikk"}, {"Mystery", "Mysterier"},
        {"Reality", "Reality"}, {"Romance", "Romantikk"}, {"Science Fiction", "Science fiction"},
        {"Sci-Fi & Fantasy", "Science fiction og fantasy"}, {"Talk", "Talkshow"}, {"Thriller", "Thriller"},
        {"TV Movie", "TV-film"}, {"War", "Krig"}, {"War & Politics", "Krig og politikk"}, {"Western", "Western"},
        {"Soap", "Såpe"}, {"News", "Nyheter"}};
    const auto it = no.find(g);
    return it == no.end() ? g : it->second;
}

void load_extras(jf::Client &c, unsigned session)
{
    using Row = ui::HomeRow;
    static std::mutex s_lock;   /* sign-in and a return from playback can both ask */
    std::lock_guard<std::mutex> once(s_lock);
    static std::vector<std::string> s_genres;
    static unsigned s_genres_for = ~0u;
    std::vector<jf::Client::Recommendation> recs;
    std::thread rt([&] { recs = c.recommendations(4, 16); });
    std::vector<Row> genre_rows;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        for (const Row &r : s_state.model.rows)
            if (r.kind == Row::Genre)
                genre_rows.push_back(r);
    }
    static unsigned s_genres_lang = ~0u;
    if (s_genres_for != session || s_genres_lang != i18n::generation()) {   /* new session, or new language */
        s_genres_for = session;
        s_genres_lang = i18n::generation();
        s_genres = c.genres();
        std::srand((unsigned)time(nullptr));
        for (size_t i = s_genres.size(); i > 1; i--)
            std::swap(s_genres[i - 1], s_genres[std::rand() % i]);
        if (s_genres.size() > 4)
            s_genres.resize(4);
        genre_rows.clear();
        std::vector<std::vector<jf::Item>> items(s_genres.size());
        std::vector<std::thread> jobs;
        for (size_t i = 0; i < s_genres.size(); i++)
            jobs.emplace_back([&, i] { items[i] = c.genre_items(s_genres[i], 20); });
        for (auto &j : jobs)
            j.join();
        for (size_t i = 0; i < s_genres.size(); i++)
            if (items[i].size() >= 6)
                genre_rows.push_back({genre_title(s_genres[i]), std::move(items[i]), false, Row::Genre});
    }
    rt.join();

    std::vector<Row> rows;
    for (auto &r : recs) {
        if (r.items.size() < 4 || r.baseline.empty())
            continue;
        std::string title;
        if (r.type == "SimilarToRecentlyPlayed") title = T("Fordi du så ") + r.baseline;
        else if (r.type == "SimilarToLikedItem") title = T("Fordi du likte ") + r.baseline;
        else if (r.type.find("Director") != std::string::npos) title = T("Regissert av ") + r.baseline;
        else if (r.type.find("Actor") != std::string::npos) title = T("Med ") + r.baseline;
        else continue;
        rows.push_back({title, std::move(r.items), false, Row::Recommended});
    }
    /* A recommendation, a genre, a recommendation ...: variety down the page. */
    std::vector<Row> mixed;
    for (size_t i = 0; i < std::max(rows.size(), genre_rows.size()); i++) {
        if (i < rows.size()) mixed.push_back(std::move(rows[i]));
        if (i < genre_rows.size()) mixed.push_back(genre_rows[i]);
    }

    std::lock_guard<std::mutex> g(s_state.lock);
    if (session != s_session || s_state.phase != Phase::Home)
        return;
    ui::HomeModel m = s_state.model;
    std::vector<Row> base, libraries;
    for (Row &r : m.rows) {
        if (r.kind == Row::Libraries) libraries.push_back(std::move(r));
        else if (r.kind != Row::Recommended && r.kind != Row::Genre) base.push_back(std::move(r));
    }
    m.rows = std::move(base);
    for (Row &r : mixed) m.rows.push_back(std::move(r));
    for (Row &r : libraries) m.rows.push_back(std::move(r));
    s_state.model = std::move(m);
    s_model_version++;
}

/* Signs in with a saved account (off the main thread): check the token, then
 * preferences, server info and the home rows. A rejected token asks for the
 * password again; an unreachable server is retried until the account changes. */
void use_account(jf::Client &c, unsigned session, accounts::Account a)
{
    set_phase(Phase::Connecting, T("Kobler til ") + (a.server_name.empty() ? a.server : a.server_name) + " \xE2\x80\xA6");
    while (session == s_session) {
        if (c.validate()) {
            if (session != s_session)
                return;   /* switched away meanwhile: leave "last account" and prefs alone */
            accounts::set_last(a.server, a.user_id);
            a.user_name = c.user_name();
            a.image_tag = c.user_image_tag();
            accounts::remember(a);
            settings::load_server(c);
            c.check_subtitle_search();
            std::string name, version;
            c.public_info(&name, &version);
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                s_state.server_name = name;
                s_state.server_version = version;
            }
            evo_bt("jelly5: signed in as %s on %s %s", c.user_name().c_str(), name.c_str(), version.c_str());
            load_home(c, session);
            /* Controllable from Jellyfin's apps ("Spill på PS5") while this session lasts. */
            remote::start(&c, [session] { return session == s_session; });
            syncplay::attach(&c);
            load_extras(c, session);
            check_for_update();
            return;
        }
        const std::string err = c.last_error();
        evo_bt("jelly5: sign-in check failed: %s", err.c_str());
        if (err.find("-> 401") != std::string::npos) {
            request_gate(Gate::Login, a.server, a.user_name, true);
            return;
        }
        set_phase(Phase::Failed, T("Får ikke kontakt med ") + (a.server_name.empty() ? a.server : a.server_name) +
                                     T(" \xE2\x80\x93 prøver igjen \xE2\x80\xA6"));
        for (int i = 0; i < 50 && session == s_session; i++)
            usleep(100 * 1000);
    }
}

/* The account to start with, read before the first screens are made. */
accounts::Account s_boot_account;
bool s_boot_has_account = false;
jf::Client *s_boot_client = nullptr;

void *boot(void *)
{
    if (s_boot_has_account)
        use_account(*s_boot_client, 0, s_boot_account);
    else if (!accounts::load().empty())
        request_gate(Gate::Profiles);
    else
        request_gate(Gate::Login, JELLY5_SERVER, "", false);
    return nullptr;
}

/* ---- screens --------------------------------------------------------------------- */
std::unique_ptr<ui::Home> s_home;
std::unique_ptr<ui::Library> s_movies, s_shows, s_music;
std::unique_ptr<ui::Search> s_search;
std::unique_ptr<ui::SettingsScreen> s_settings;
std::unique_ptr<ui::Profiles> s_profiles;
std::unique_ptr<ui::Login> s_login;
Gate s_gate = Gate::None;
ui::Nav s_nav;
std::vector<std::unique_ptr<ui::Screen>> s_stack;   /* detail pages over the tab */
ui::Screen *s_now_page = nullptr;                   /* the now-playing page, while it is on the stack */
/* The card a page was opened from: the page grows out of it as it comes in. */
struct Origin {
    ui::Screen::Card card;
    const ui::Screen *page = nullptr;
};
Origin s_origin;
ui::Screensaver s_saver;
constexpr double kSaverAfter = 300.0;   /* 5 minutes without a button */

/* The screensaver's slides: every backdrop on the home screen, once. */
std::vector<ui::Screensaver::Slide> saver_slides()
{
    std::vector<ui::Screensaver::Slide> out;
    std::set<std::string> seen;
    std::lock_guard<std::mutex> g(s_state.lock);
    auto add = [&](const jf::Item &it) {
        const std::string url = s_client->image_url(it.backdrop_owner, "Backdrop", it.backdrop_tag, 1920);
        if (url.empty() || !seen.insert(url).second)
            return;
        const bool ep = it.type == "Episode";
        std::string line = it.year ? std::to_string(it.year) : std::string();
        if (!it.genres.empty())
            line += (line.empty() ? "" : "  \xC2\xB7  ") + it.genres[0];
        out.push_back({url, it.backdrop_blurhash, ep ? it.series_name : it.name, line});
    };
    for (const jf::Item &it : s_state.model.hero)
        add(it);
    for (const ui::HomeRow &r : s_state.model.rows)
        if (r.kind != ui::HomeRow::Libraries)
            for (const jf::Item &it : r.items)
                add(it);
    return out;
}
std::vector<jf::Item> s_queue;                      /* what the chosen Play hands over as a queue */
size_t s_queue_start = 0;
int s_tab = ui::Nav::Home;
bool s_nav_focus = false;
int s_nav_tab = ui::Nav::Home;     /* focused tab while s_nav_focus */

/* ---- music behind the menus ------------------------------------------------------------ */
std::atomic<bool> s_music_on{false};      /* a track (and its queue) plays headless */
bool s_group_play = false;              /* this Play came from the SyncPlay group: play it here */

/* Ends the music and waits for its thread to let go of the player. */
/* ---- theme music --------------------------------------------------------------
 * On a film's or series' page its theme song plays quietly in the background
 * (Innstillinger: Temamusikk), as Jellyfin's own apps do; it stops when the page
 * closes or anything else plays. The player runs headless for it on its own
 * thread; s_theme_gen tells a starting theme whether it is still wanted. */
std::atomic<bool> s_theme_on{false};
std::atomic<unsigned> s_theme_gen{0};
std::string s_theme_for;   /* the page whose theme is wanted (main thread) */

void stop_theme(bool wait)
{
    s_theme_for.clear();
    s_theme_gen++;
    if (!s_theme_on)
        return;
    remote::Command c;
    c.kind = remote::Command::Stop;
    remote::send(c);
    for (int i = 0; wait && i < 300 && s_theme_on; i++)
        usleep(10 * 1000);
}

/* Each frame: the theme the open page wants, started or stopped to match. */
void theme_follow()
{
    std::string want;
    if (!s_stack.empty() && !s_music_on && settings::get().local.theme_music)
        if (const auto *d = dynamic_cast<const ui::Detail *>(s_stack.back().get())) {
            const std::string &t = d->item().type;
            if (t == "Movie" || t == "Series" || t == "Season" || t == "Episode")
                want = d->item().id;
        }
    if (want == s_theme_for)
        return;
    stop_theme(false);
    s_theme_for = want;
    if (want.empty())
        return;
    const unsigned gen = s_theme_gen;
    jf::Client *c = s_client;
    std::thread([c, want, gen] {
        for (int i = 0; i < 300 && s_theme_on; i++)   /* the last one winding down */
            usleep(10 * 1000);
        usleep(600 * 1000);   /* a page only passed through starts nothing */
        if (gen != s_theme_gen || s_theme_on)
            return;
        const std::vector<jf::Item> songs = c->theme_songs(want);
        if (songs.empty() || gen != s_theme_gen || s_music_on)
            return;
        s_theme_on = true;
        nuvio_player_set_headless(1);
        jelly5_play_theme(*c, songs.front());
        nuvio_player_set_headless(0);
        s_theme_on = false;
    }).detach();
}

void stop_music()
{
    if (!s_music_on)
        return;
    remote::Command c;
    c.kind = remote::Command::Stop;
    remote::send(c);
    for (int i = 0; i < 300 && s_music_on; i++)
        usleep(10 * 1000);
    if (s_music_on)
        evo_bt("jelly5: music did not stop in 3 s");
}

void open_now_playing()
{
    if (s_now_page && !s_stack.empty() && s_stack.back().get() == s_now_page)
        return;
    s_stack.emplace_back(new ui::NowPlaying());
    s_now_page = s_stack.back().get();
    s_now_page->activate();
}

/* Music plays on the player's thread without a picture; the menus stay up. */
void start_music(const jf::Item &item, bool shuffle, const std::vector<jf::Item> *queue, size_t start)
{
    stop_theme(true);
    stop_music();
    jf::Client *c = s_client;
    const std::vector<jf::Item> q = queue ? *queue : std::vector<jf::Item>();
    s_music_on = true;
    std::thread([c, item, shuffle, q, start] {
        nuvio_player_set_headless(1);
        std::string error;
        const bool ok = q.size() > 1 ? jelly5_play_queue(*c, q, start, &error) : jelly5_play(*c, item, &error, shuffle);
        nuvio_player_set_headless(0);
        if (!ok)
            notify((T("Jelly5: kunne ikke spille av\n") + error).c_str());
        s_music_on = false;
    }).detach();
    open_now_playing();
}

/* Fresh screens for a new account (nothing of the last one's library survives). */
void reset_screens()
{
    s_stack.clear();
    s_now_page = nullptr;
    s_home.reset(new ui::Home(*s_client));
    s_movies.reset(new ui::Library(*s_client, T("Filmer"), "Movie"));
    s_shows.reset(new ui::Library(*s_client, T("Serier"), "Series"));
    s_music.reset(new ui::Library(*s_client, T("Musikk"), "MusicAlbum"));
    s_search.reset(new ui::Search(*s_client));
    s_settings.reset(new ui::SettingsScreen(*s_client));
    s_tab = s_nav_tab = ui::Nav::Home;
    s_nav_focus = false;
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.model = ui::HomeModel();
    s_state.views.clear();
    s_home_version = ~0u;
}

/* Opens the gate screen a worker asked for (main thread). */
void open_gate(const GateRequest &r)
{
    s_gate = r.kind;
    if (r.kind == Gate::Profiles) {
        s_profiles.reset(new ui::Profiles());
        s_profiles->activate();
    } else if (r.kind == Gate::Login) {
        s_login.reset(new ui::Login(*s_client, r.server.empty() ? std::string(JELLY5_SERVER) : r.server, r.user,
                                    r.can_cancel));
        s_login->activate();
    }
}

void switch_to(const accounts::Account &a)
{
    stop_music();
    if (syncplay::active())
        syncplay::leave();
    const unsigned session = ++s_session;
    jf::Client *c = client_for(a);
    s_client = c;
    reset_screens();
    s_gate = Gate::None;
    set_phase(Phase::Connecting, T("Kobler til \xE2\x80\xA6"));
    std::thread([c, session, a] { use_account(*c, session, a); }).detach();
}

void gate_input(uint32_t p)
{
    if (s_gate == Gate::Profiles && s_profiles) {
        s_profiles->input(p);
        ui::Profiles::Choice ch;
        if (s_profiles->take_choice(&ch)) {
            if (ch.add)
                open_gate({Gate::Login, s_client->server(), "", true});
            else
                switch_to(ch.account);
        }
    } else if (s_gate == Gate::Login && s_login) {
        const ui::Action a = s_login->input(p);
        if (a.kind == ui::Action::Back)
            open_gate({accounts::load().empty() ? Gate::Login : Gate::Profiles, s_client->server(), "", false});
    }
}

/* Called every frame while a gate is up: a finished sign-in moves on. */
void gate_poll()
{
    accounts::Account a;
    if (s_gate == Gate::Login && s_login && s_login->take_result(&a)) {
        accounts::remember(a);
        switch_to(a);
    }
}

ui::Screen *screen_for(int tab)
{
    if (!s_stack.empty())
        return s_stack.back().get();
    switch (tab) {
    case ui::Nav::Movies: return s_movies.get();
    case ui::Nav::Shows: return s_shows.get();
    case ui::Nav::Music: return s_music.get();
    case ui::Nav::Search: return s_search.get();
    case ui::Nav::Settings: return s_settings.get();
    default: return s_home.get();
    }
}

void open_tab(int tab)
{
    s_tab = tab;
    if (tab == ui::Nav::Settings) {
        std::lock_guard<std::mutex> g(s_state.lock);
        s_settings->set_server_info(s_state.server_name, s_state.server_version);
    }
    screen_for(tab)->activate();
}

/* Top-level input once signed in: the tab bar, or the active screen. */
void shell_input(uint32_t p, jf::Item *play, bool *chose, bool *from_start, bool *shuffle)
{
    if ((p & NUVIO_BTN_TOUCHPAD) && s_music_on) {   /* the mini player: the now-playing page */
        s_nav_focus = false;
        open_now_playing();
        return;
    }
    if ((p & NUVIO_BTN_TRIANGLE) && s_stack.empty() && s_tab != ui::Nav::Search) {
        s_nav_focus = false;   /* △ from any tab: straight to search, as in YouTube on PS5 */
        s_nav_tab = ui::Nav::Search;
        open_tab(ui::Nav::Search);
        return;
    }
    /* L1/R1: the previous / next tab, as across the PS5's own menus. */
    if ((p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) && s_stack.empty() && !screen_for(s_tab)->modal()) {
        std::vector<int> order = s_nav.tabs();
        order.push_back(ui::Nav::Settings);
        const int at = (int)(std::find(order.begin(), order.end(), s_tab) - order.begin());
        const int to = at + ((p & NUVIO_BTN_R1) ? 1 : -1);
        if (at < (int)order.size() && to >= 0 && to < (int)order.size()) {
            s_nav_tab = order[to];
            open_tab(order[to]);
            if (!s_nav_focus)
                screen_for(order[to])->enter_from_top();
        } else {
            nuvio_input_pulse(70, 45);   /* the end: a soft bump */
        }
        return;
    }
    if (s_nav_focus && s_stack.empty()) {
        const int before = s_nav_tab;
        std::vector<int> order = s_nav.tabs();
        order.push_back(ui::Nav::Settings);   /* the avatar, last */
        const int at = (int)(std::find(order.begin(), order.end(), s_nav_tab) - order.begin());
        if ((p & NUVIO_BTN_LEFT) && at > 0 && at < (int)order.size())
            s_nav_tab = order[at - 1];
        else if ((p & NUVIO_BTN_RIGHT) && at + 1 < (int)order.size())
            s_nav_tab = order[at + 1];
        else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) {
            s_nav_focus = false;
            screen_for(s_tab)->enter_from_top();
        }
        else if ((p & NUVIO_BTN_CIRCLE) && s_tab != ui::Nav::Home)
            s_nav_tab = ui::Nav::Home;   /* back from any tab goes home, as on Netflix */
        if (s_nav_tab != before && s_nav_tab != s_tab)
            open_tab(s_nav_tab);         /* tvOS-style: focusing a tab opens it */
        return;
    }
    ui::Screen *in_screen = screen_for(s_tab);
    const ui::Action a = in_screen->input(p);
    if (in_screen->take_bump())
        nuvio_input_pulse(70, 45);   /* a soft bump at the edge */
    if (a.kind == ui::Action::Play || a.kind == ui::Action::PlayFromStart || a.kind == ui::Action::PlayShuffled)
        nuvio_input_pulse(150, 70);  /* a firmer one to start */
    switch (a.kind) {
    case ui::Action::ToNav:
        if (s_stack.empty()) {
            s_nav_focus = true;
            s_nav_tab = s_tab;
        }
        break;
    case ui::Action::Play:
    case ui::Action::PlayFromStart:
    case ui::Action::PlayShuffled:
        *play = a.item;
        *chose = true;
        *from_start = a.kind == ui::Action::PlayFromStart;
        *shuffle = a.kind == ui::Action::PlayShuffled;
        s_queue = a.queue;
        s_queue_start = a.queue_start;
        break;
    case ui::Action::PlayMix: {   /* Jellyfin's Instant Mix from an album (or song, artist) */
        std::vector<jf::Item> mix = s_client->instant_mix(a.item.id, 60);
        if (mix.empty()) {
            notify(T("Jelly5: Jellyfin fant ingen miks her"));
            break;
        }
        *play = mix.front();
        *chose = true;
        s_queue = std::move(mix);
        s_queue_start = 0;
        break;
    }
    case ui::Action::Open: {
        ui::Screen::Card from;
        const bool have_from = screen_for(s_tab)->focused_card(&from);
        if (s_stack.size() >= 8)
            s_stack.erase(s_stack.begin());   /* "more like this" chains stay bounded */
        /* Seasons and episodes open their series' page. */
        jf::Item target = a.item;
        if ((target.type == "Season" || target.type == "Episode") && !target.series_id.empty()) {
            target = jf::Item();
            target.id = a.item.series_id;
            target.type = "Series";
            target.name = a.item.series_name;
            /* An episode carries its series' backdrop and logo: shown at once. */
            target.backdrop_owner = a.item.backdrop_owner;
            target.backdrop_tag = a.item.backdrop_tag;
            target.backdrop_blurhash = a.item.backdrop_blurhash;
            target.logo_owner = a.item.logo_owner;
            target.logo_tag = a.item.logo_tag;
        }
        if (target.type == "SyncPlay")
            s_stack.emplace_back(new ui::SyncPlayScreen(s_client->user_name()));
        else if (target.type == "Person")
            s_stack.emplace_back(new ui::Person(*s_client, target));
        else if (target.type == "MusicAlbum" || target.type == "Playlist")
            s_stack.emplace_back(new ui::Album(*s_client, target));
        else if (target.type == "MusicArtist")   /* an artist: their albums */
            s_stack.emplace_back(new ui::Library(*s_client, target.name, "MusicAlbum", "", true,
                                                 "&AlbumArtistIds=" + target.id));
        else if (target.type == "CollectionFolder" || target.type == "UserView")
            s_stack.emplace_back(new ui::Library(*s_client, target.name,
                                                 ui::Library::types_for(target.collection_type), target.id, true));
        else
            s_stack.emplace_back(new ui::Detail(*s_client, target));
        s_stack.back()->activate();
        s_origin.page = have_from ? s_stack.back().get() : nullptr;
        if (have_from)
            s_origin.card = from;
        break;
    }
    case ui::Action::Back:
        if (!s_stack.empty()) {
            if (s_stack.back().get() == s_now_page)
                s_now_page = nullptr;
            s_stack.pop_back();
        }
        break;
    case ui::Action::Changed: {   /* written, then Min liste, Fortsett å se and the rest follow */
        jf::Client *c = s_client;
        const unsigned session = s_session;
        const ui::UserDataChange ch = a.change;
        if (s_tab != ui::Nav::Home || !s_stack.empty())
            s_home->apply(ch);   /* the home rows were not the screen it was made on */
        std::thread([c, session, ch] {
            if (ch.favorite_set) c->set_favorite(ch.id, ch.favorite);
            if (ch.played_set) c->set_played(ch.id, ch.played);
            if (ch.resume_cleared) c->clear_position(ch.id);
            load_home(*c, session, true);
        }).detach();
        break;
    }
    case ui::Action::SwitchUser:
        stop_music();
        s_session++;
        open_gate({Gate::Profiles});
        set_phase(Phase::Gate);
        break;
    case ui::Action::SignOut: {
        stop_music();
        accounts::forget(s_client->server(), s_client->user_id());
        s_session++;
        s_client = new_client(s_client->server());   /* signed out; screens are remade on the next sign-in */
        const bool others = !accounts::load().empty();
        open_gate({others ? Gate::Profiles : Gate::Login, s_client->server(), "", false});
        set_phase(Phase::Gate);
        break;
    }
    default:
        break;
    }
}

/* The tabs and their pills from the user's libraries (in the order they keep in
 * Jellyfin): Filmer, Serier and Musikk show when there is such a library; a tab
 * with several offers them as pills (Serier · Anime). Music: Album · Artister ·
 * Spillelister. */
void apply_views(const std::vector<jf::Item> &views)
{
    using Source = ui::Library::Source;
    std::vector<Source> movies, shows, music;
    std::string playlists;
    std::vector<const jf::Item *> music_libs;
    for (const jf::Item &v : views) {
        if (v.collection_type == "movies") movies.push_back({v.name, v.id, "Movie", ""});
        if (v.collection_type == "tvshows") shows.push_back({v.name, v.id, "Series", ""});
        if (v.collection_type == "music") music_libs.push_back(&v);
        if (v.collection_type == "playlists") playlists = v.id;
    }
    for (const jf::Item *v : music_libs)
        music.push_back({music_libs.size() > 1 ? v->name : std::string(T("Album")), v->id, "MusicAlbum", ""});
    if (!music_libs.empty()) {
        music.push_back({T("Artister"), music_libs.size() == 1 ? music_libs[0]->id : "", "MusicArtist", ""});
        if (!playlists.empty())
            music.push_back({T("Spillelister"), playlists, "Playlist", ""});
    }
    std::vector<int> tabs{ui::Nav::Home};
    if (!movies.empty()) { tabs.push_back(ui::Nav::Movies); s_movies->set_sources(movies); }
    if (!shows.empty()) { tabs.push_back(ui::Nav::Shows); s_shows->set_sources(shows); }
    if (!music.empty()) { tabs.push_back(ui::Nav::Music); s_music->set_sources(music); }
    tabs.push_back(ui::Nav::Search);
    s_nav.set_tabs(tabs);
    /* A tab that went away (another account, a library removed): back home. */
    auto shown = [&](int t) { return t == ui::Nav::Settings || std::find(tabs.begin(), tabs.end(), t) != tabs.end(); };
    if (!shown(s_tab))
        s_tab = ui::Nav::Home;
    if (!shown(s_nav_tab))
        s_nav_tab = s_tab;
}

/* ---- drawing (GPU, src/gfx) ------------------------------------------------------ */
void draw_wordmark(float cx, float baseline, float size)
{
    ui::draw_brand(cx - ui::brand_width(size) / 2, baseline, size);
}

void draw_status(const std::string &title, const std::string &line, double t, const std::string &hint = "")
{
    gfx::fill({0, 0, gfx::W, gfx::H}, 0xff07070au);
    draw_wordmark(gfx::W / 2, 470, 96);
    gfx::text(gfx::W / 2, 580, title, {gfx::SemiBold, 38, 1400}, 0xfff5f5f7u, 1);
    if (!line.empty())
        gfx::text(gfx::W / 2, 636, line, {gfx::Medium, 28, 1400}, 0xadebebf5u, 1);
    for (int i = 0; i < 3; i++) {   /* three dots pulsing in turn */
        const float a = 0.3f + 0.7f * (0.5f + 0.5f * std::sin((float)t * 5.f - i * 0.9f));
        gfx::fill({gfx::W / 2 - 40 + i * 32, 720, 14, 14}, ((uint32_t)(a * 255) << 24) | 0xf5f5f7u, 7);
    }
    if (!hint.empty())
        ui::draw_pad_hints(gfx::W / 2, 992, {{ui::PadButton::Circle, hint}}, 1);
}

/* The start-up splash: the mark on a deep field in the app's colours,
 * breathing gently. No text: the app is simply getting ready. */
void draw_splash(double t, float a)
{
    if (a <= 0.f)
        return;
    gfx::push_opacity(a);
    gfx::fill({0, 0, gfx::W, gfx::H}, 0xff07070au);
    gfx::fill_vgradient({0, 0, gfx::W, gfx::H}, 0x26402a5cu, 0x14003c55u);
    const float pulse = 0.5f + 0.5f * std::sin((float)t * 2.2f);
    gfx::push_opacity(0.85f + 0.15f * pulse);
    ui::draw_brand(gfx::W / 2 - ui::brand_width(110) / 2, 578, 110, 1.f, true);
    gfx::pop_opacity();
    gfx::pop_opacity();
}

ui::Anim s_splash;                 /* 1 while starting, fades to 0 over the home screen */

/* Returns true when it drew something that will keep changing. */
bool draw_frame(double t, float dt)
{
    Phase phase;
    std::string message;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        phase = s_state.phase;
        message = s_state.message;
        if (phase == Phase::Home && s_model_version != s_home_version) {
            s_home_version = s_model_version;
            s_home->set_model(s_state.model);
            apply_views(s_state.views);
            const std::string &tag = s_client->user_image_tag();
            s_nav.set_user(s_client->user_name(),
                           tag.empty() ? "" : s_client->server() + "/Users/" + s_client->user_id() +
                                                  "/Images/Primary?tag=" + tag + "&fillWidth=440");
        }
    }
    art::tick();
    gfx::begin_frame();
    bool animating = true;
    if (s_saver.on() && phase == Phase::Home) {
        s_saver.draw(t);
        gfx::end_frame();
        return true;
    }
    switch (phase) {
    case Phase::Connecting:
    case Phase::Loading:
        s_splash.snap(1.f);
        draw_splash(t, 1.f);
        break;
    case Phase::Failed:
        draw_status(message, "", t, T("Bytt bruker eller server"));
        break;
    case Phase::Gate:
        s_splash.snap(0.f);
        if (s_gate == Gate::Profiles && s_profiles) {
            s_profiles->draw(t, dt);
            animating = s_profiles->animating();
        } else if (s_gate == Gate::Login && s_login) {
            s_login->draw(t, dt);
        }
        break;
    case Phase::Home:
        if (s_tab == ui::Nav::Home && s_stack.empty() && s_home->empty()) {
            draw_status(T("Ingenting å vise ennå"), T("Legg til filmer eller serier i Jellyfin."), t);
        } else {
            ui::Screen *scr = screen_for(s_tab);
            const float enter = scr->enter();
            if (enter < 1.f) {
                /* A page fading in: the screen below stays under it until it is opaque. */
                ui::Screen *below = s_stack.size() >= 2 ? s_stack[s_stack.size() - 2].get()
                                                        : (s_tab == ui::Nav::Movies   ? (ui::Screen *)s_movies.get()
                                                           : s_tab == ui::Nav::Shows  ? (ui::Screen *)s_shows.get()
                                                           : s_tab == ui::Nav::Music  ? (ui::Screen *)s_music.get()
                                                           : s_tab == ui::Nav::Search ? (ui::Screen *)s_search.get()
                                                                                      : (ui::Screen *)s_home.get());
                below->draw(t, dt);
                /* The card it was opened from lifts a little toward the viewer and fades as
                 * the page comes in: a hint of where the page came from, easy on the eye. */
                if (s_origin.page == scr) {
                    const ui::Screen::Card &c = s_origin.card;
                    const float e = ui::smoothstep(std::min(1.f, enter * 1.4f));
                    const float k = 1.f + 0.12f * e;
                    const gfx::Rect r{c.rect.x - c.rect.w * (k - 1) / 2, c.rect.y - c.rect.h * (k - 1) / 2, c.rect.w * k,
                                      c.rect.h * k};
                    const float fade = 1.f - e;
                    if (const gfx::Texture *tex = art::get(c.url, 640, 720))
                        gfx::image(r, tex, fade, c.radius * k, true);
                    else if (const gfx::Texture *bh = art::blurhash(c.blurhash))
                        gfx::image(r, bh, fade, c.radius * k, true);
                }
                gfx::push_opacity(enter);
            }
            scr->set_focused(!s_nav_focus || !s_stack.empty());
            scr->draw(t, dt);
            if (enter < 1.f)
                gfx::pop_opacity();
            animating = scr->animating();
            const float nav = !s_stack.empty() ? 0.f : s_nav_focus ? 1.f : scr->nav_alpha();
            s_nav.draw(nav, s_tab, s_nav_focus ? s_nav_tab : -1, dt, &animating);
            /* The music page closes when the music ends; elsewhere the mini player shows it. */
            if (s_now_page && !s_music_on && !s_stack.empty() && s_stack.back().get() == s_now_page) {
                s_stack.pop_back();
                s_now_page = nullptr;
            }
            if (s_music_on && (s_stack.empty() || s_stack.back().get() != s_now_page)) {
                ui::draw_mini_player(t, 1.f);
                animating = true;
            }
            if (draw_connection(t))
                animating = true;
            theme_follow();
        }
        /* The splash fades away over the first frames of the home screen. */
        s_splash.to(0.f);
        if (s_splash.step(dt, 6.f))
            animating = true;
        draw_splash(t, s_splash.value);
        break;
    }
    gfx::end_frame();
    return animating;
}

/* The server out of reach: requests get no answer at all (jf::unreachable_streak).
 * A note at the top says so while the app asks the server every 4 s; when it
 * answers, the note says so briefly and the home rows reload. Returns true while
 * it shows (frames are wanted). */
bool draw_connection(double now)
{
    static bool s_down = false, s_pinging = false;
    static double s_last_ping = 0, s_back_at = -10;
    static std::mutex s_ping_lock;
    const bool down = jf::unreachable_streak() >= 2;
    if (down) {
        s_down = true;
        std::lock_guard<std::mutex> g(s_ping_lock);
        if (!s_pinging && now - s_last_ping > 4.0) {
            s_pinging = true;
            s_last_ping = now;
            jf::Client *c = s_client;
            std::thread([c] {
                c->ping();
                std::lock_guard<std::mutex> g2(s_ping_lock);
                s_pinging = false;
            }).detach();
        }
    } else if (s_down) {
        s_down = false;
        s_back_at = now;
        jf::Client *c = s_client;
        const unsigned session = s_session;
        std::thread([c, session] { load_home(*c, session, true); }).detach();
    }
    const bool back = now - s_back_at < 2.5;
    if (!s_down && !back)
        return false;
    const std::string text = s_down ? T("Ingen kontakt med Jellyfin-serveren \xE2\x80\x93 pr\xC3\xB8ver igjen \xE2\x80\xA6")
                                    : T("Tilkoblet igjen");
    const gfx::TextStyle ts{gfx::SemiBold, 24};
    const float w = gfx::text_width(text, ts) + 72;
    const gfx::Rect r{gfx::W / 2 - w / 2, 136, w, 60};
    ui::glass_panel(r, 30, 1.f, true);
    gfx::fill({r.x + 26, r.y + 25, 10, 10}, s_down ? 0xffff9f0au : 0xff30d158u, 5);   /* amber: away, green: back */
    gfx::text(r.x + 48, r.y + 39, text, ts, ui::kText);
    return true;
}

/* What a chosen item plays: a series starts at its next episode. */
bool resolve_playable(jf::Item *item)
{
    if (item->type == "Movie" || item->type == "Episode" || item->type == "Video" || item->type == "Trailer" ||
        item->type == "MusicVideo" || item->type == "Audio")
        return true;
    if (item->type == "MusicAlbum") {   /* from Min liste or search: its first track */
        for (const jf::Item &t : s_client->children(item->id, "ParentIndexNumber,IndexNumber,SortName", 1))
            if (t.type == "Audio") {
                *item = t;
                return true;
            }
        return false;
    }
    if (item->type == "Season") {
        /* A season (the "recently added" rows group episodes by season):
         * its first unwatched episode, else its first. */
        const std::vector<jf::Item> eps = s_client->episodes(item->series_id, item->id);
        for (const jf::Item &e : eps)
            if (!e.played) {
                *item = e;
                return true;
            }
        if (eps.empty())
            return false;
        *item = eps.front();
        return true;
    }
    if (item->type != "Series")
        return false;
    std::vector<jf::Item> next = s_client->next_up(1, item->id);
    if (next.empty())
        next = s_client->episodes(item->id, std::string());
    if (next.empty()) {
        evo_bt("jelly5: nothing to play in %s: %s", item->name.c_str(), s_client->last_error().c_str());
        return false;
    }
    *item = next.front();
    return true;
}

void play(jf::Item item, bool from_start, bool shuffle = false, const std::vector<jf::Item> *queue = nullptr,
          size_t start = 0)
{
    stop_theme(true);   /* the page's theme song makes way */
    if (queue && !queue->empty())
        item = (*queue)[std::min(start, queue->size() - 1)];
    if (from_start)
        item.position_ticks = 0;
    /* Side-by-side and top-and-bottom 3D: the PS5 has no 3D output, and the two
     * squeezed pictures are no way to watch. MVC (3D Blu-ray) plays its 2D view. */
    if (item.video3d.find("SideBySide") != std::string::npos || item.video3d.find("TopAndBottom") != std::string::npos) {
        notify(T("Jelly5: 3D-filer st\xC3\xB8ttes ikke p\xC3\xA5 PS5"));
        return;
    }
    if (!resolve_playable(&item)) {
        notify(T("Jelly5: fant ingenting å spille av her"));
        return;
    }
    if (syncplay::active() && !s_group_play) {   /* in a group: everyone plays it */
        syncplay::play(item);
        notify(T("Jelly5: startes for hele gruppen"));
        return;
    }
    s_group_play = false;
    if (item.type == "Audio") {   /* music: behind the menus */
        start_music(item, shuffle, queue, start);
        return;
    }
    stop_music();   /* a picture to show: the music ends first */
    nuvio_player_set_headless(0);
    /* Hand over to the player without a seam: this frame is the player's own
     * loading screen (its colour, the title's backdrop at 92 %, its gradient),
     * and the art it is about to ask for is already being fetched. */
    const std::string backdrop = s_client->image_url(item.backdrop_owner, "Backdrop", item.backdrop_tag, 1920);
    if (!backdrop.empty())
        ui_image_request(backdrop.c_str(), 1920, 1080, 0);
    gfx::begin_frame();
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    if (item.type == "Audio") {   /* the music screen's ground: its colour and the cover's hues */
        gfx::fill(full, ui::kBg);
        const std::string &hash = !item.album_blurhash.empty() ? item.album_blurhash : item.primary_blurhash;
        if (const gfx::Texture *bh = art::blurhash(hash))
            gfx::image(full, bh, 0.55f, 0, true);
        gfx::fill_hgradient(full, 0x8c07070au, 0xd907070au);
    } else {
        gfx::fill(full, 0xff080b10u);
        if (const gfx::Texture *t = art::get(backdrop, 1920, 1080))
            gfx::image(full, t, 0.92f, 0, true);
        gfx::fill_vgradient({0, 0, gfx::W, 378}, 0x4d000000u, 0x99000000u);
        gfx::fill_vgradient({0, 378, gfx::W, 378}, 0x99000000u, 0xcc000000u);
        gfx::fill_vgradient({0, 756, gfx::W, 324}, 0xcc000000u, 0xe6000000u);
    }
    gfx::end_frame();

    nuvio_input_close();
    std::string error;
    const bool ok = queue && queue->size() > 1 ? jelly5_play_queue(*s_client, *queue, start, &error)
                                               : jelly5_play(*s_client, item, &error, shuffle);
    if (!ok)
        notify((T("Jelly5: kunne ikke spille av\n") + error).c_str());
    nuvio_input_open(s_user);
    /* Back at once; positions and "next up" refresh behind the screen. */
    if (!s_stack.empty())
        s_stack.back()->activate();   /* a detail page reloads its progress */
    jf::Client *c = s_client;
    const unsigned session = s_session;
    std::thread([c, session] {
        jelly5_wait_reports(4000);   /* the position just reported, before reading it back */
        load_home(*c, session, true);
        load_extras(*c, session);   /* what was just watched shapes "Fordi du så" */
    }).detach();
}

/* A command from a phone while the menus are up: play what it sent, or show
 * its message. (During playback the player takes them itself.) */
void remote_idle(const remote::Command &rc)
{
    if (rc.kind == remote::Command::Message) {
        notify((rc.header.empty() ? rc.text : rc.header + "\n" + rc.text).c_str());
        return;
    }
    if (rc.kind != remote::Command::Play)
        return;
    std::vector<jf::Item> q;
    s_group_play = rc.play_command == "SyncPlay";
    if (rc.play_command == "PlayInstantMix") {
        q = s_client->instant_mix(rc.item_ids.front(), 60);
    } else {
        for (const std::string &id : rc.item_ids) {
            jf::Item it;
            if (s_client->item(id, &it))
                q.push_back(it);
        }
    }
    if (q.empty()) {
        notify(T("Jelly5: fant ikke det som ble sendt"));
        return;
    }
    size_t start = std::min((size_t)std::max(0, rc.start_index), q.size() - 1);
    if (rc.play_command == "PlayShuffle") {
        std::srand((unsigned)time(nullptr));
        for (size_t i = q.size(); i > 1; i--)
            std::swap(q[i - 1], q[std::rand() % i]);
        start = 0;
    }
    q[start].position_ticks = rc.start_ticks;   /* the phone says where to start */
    evo_bt("jelly5: remote play %s (%zu in queue)", q[start].name.c_str(), q.size());
    s_stack.clear();   /* back from playback on the home screen */
    s_tab = s_nav_tab = ui::Nav::Home;
    s_nav_focus = false;
    play(q[start], false, false, &q, start);
}

} // namespace

/* Imports the console left NULL (see scripts/build.sh). */
extern "C" __attribute__((weak)) int nuvio_import_count(void) { return 0; }
extern "C" __attribute__((weak)) const char *nuvio_import_null(int) { return nullptr; }

int main()
{
    install_crash_handler();
    evo_bt("jelly5: app start " JELLY5_VERSION);
    for (int i = 0; i < nuvio_import_count(); i++)
        if (const char *name = nuvio_import_null(i))
            evo_bt("jelly5: import %s is NULL on this console", name);

    if (init_hardware() != 0) {
        notify(T("Jelly5: skjermen kunne ikke startes"));
        for (;;)
            usleep(1000 * 1000);
    }
    if (ui_text_init() != 0 || !gfx::init())
        evo_bt("jelly5: ui init failed");
    nuvio_input_open(s_user);

    char device[48];
    std::snprintf(device, sizeof device, "jelly5-ps5-%d", s_user);
    s_device = device;
    settings::load_local();
    accounts::set_ps5_user(s_user);   /* each PS5 user keeps their own Jellyfin account */
    i18n::set_choice(settings::get().local.language);
    /* 120 Hz where the display has it: smoother menus, and 24p film without 3:2 judder. */
    if (settings::get().local.refresh_120 && evo_agc_runtime_supports_120hz())
        evo_agc_runtime_set_120hz(1);
    s_boot_has_account = accounts::last(&s_boot_account);
    s_client = s_boot_client = s_boot_has_account ? client_for(s_boot_account) : new_client(JELLY5_SERVER);
    reset_screens();

    pthread_t w;
    pthread_create(&w, nullptr, boot, nullptr);
    pthread_detach(w);

    /* No self-exit: exit() teardown faults in an app process (SIGSYS, seen on
     * hardware and in EVO). The app is closed with the PS button. */
    const double t0 = now_s();
    double last = t0;
    bool animating = true;
    Phase last_phase = Phase::Connecting;
    unsigned last_model = ~0u;
    int idle_frames = 0;
    unsigned frames = 0;
    unsigned lang_gen = i18n::generation();
    for (;;) {
        nuvio_input_state in;
        nuvio_input_poll(&in);
        ime::poll();
        if (ime::active())
            in.pressed = 0;   /* the system keyboard has the controller */

        Phase phase;
        GateRequest gate;
        {
            std::lock_guard<std::mutex> g(s_state.lock);
            phase = s_state.phase;
            if (s_state.gate.kind != Gate::None) {
                gate = s_state.gate;
                s_state.gate = GateRequest();
            }
        }
        if (gate.kind != Gate::None)
            open_gate(gate);

        /* The screensaver: on after a while without a button; any button wakes the
         * app and is only that. */
        static double last_input = now_s();
        if (in.pressed) {
            last_input = now_s();
            if (s_saver.on()) {
                s_saver.stop();
                in.pressed = 0;
                animating = true;
            }
        } else if (!s_saver.on() && phase == Phase::Home && !ime::active() && now_s() - last_input > kSaverAfter) {
            s_saver.start(saver_slides(), now_s() - t0);
            animating = true;
        }

        jf::Item chosen;
        bool chose = false, from_start = false, shuffle = false;
        if (in.pressed) {
            if (phase == Phase::Gate)
                gate_input(in.pressed);
            else if (phase == Phase::Failed && (in.pressed & NUVIO_BTN_CIRCLE)) {
                s_session++;
                open_gate({accounts::load().empty() ? Gate::Login : Gate::Profiles, s_client->server(), "", false});
                set_phase(Phase::Gate);
            } else if (phase == Phase::Home && s_home_version == s_model_version)
                shell_input(in.pressed, &chosen, &chose, &from_start, &shuffle);
        }
        if (phase == Phase::Gate)
            gate_poll();
        if (lang_gen != i18n::generation()) {   /* Innstillinger -> Språk: rebuild the text that was built */
            lang_gen = i18n::generation();
            s_movies->set_title(T("Filmer"));
            s_shows->set_title(T("Serier"));
            s_music->set_title(T("Musikk"));
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                apply_views(s_state.views);   /* the pills' labels */
            }
            if (phase == Phase::Home) {
                jf::Client *c = s_client;
                const unsigned session = s_session;
                std::thread([c, session] {
                    load_home(*c, session, true);
                    load_extras(*c, session);
                }).detach();
            }
        }
        if (!chose && !s_music_on && phase == Phase::Home && s_home_version == s_model_version) {
            remote::Command rc;
            if (remote::take(&rc)) {
                remote_idle(rc);
                last = now_s();
                continue;
            }
        }
        if (chose) {
            const std::vector<jf::Item> queue = std::move(s_queue);
            s_queue.clear();
            play(chosen, from_start, shuffle, queue.empty() ? nullptr : &queue, s_queue_start);
            last = now_s();
            continue;
        }
        /* Frames only while something moves; idle, the last frame stays up. */
        const bool changed = in.pressed || phase != last_phase || s_model_version != last_model ||
                             gate.kind != Gate::None;
        if (changed || animating || idle_frames < 2) {
            const double now = now_s();
            const float dt = (float)std::min(0.1, now - last);
            last = now;
            animating = draw_frame(now - t0, dt);
#ifdef JELLY5_LOG_HOST   /* development builds: frame timing in the log */
            static perf::Frames ui_perf("ui");
            const double drawn = now_s();
            ui_perf.note((drawn - now) * 1000.0, drawn);
#endif
            idle_frames = (changed || animating) ? 0 : idle_frames + 1;
            last_phase = phase;
            last_model = s_model_version;
            if ((++frames % 120) == 0)
                gfx::collect();
        } else {
            usleep(8000);
            last = now_s();
        }
        if (s_saver.on())
            usleep(25000);   /* the screensaver drifts slowly: ~30 frames a second is plenty */
        /* Once a minute, what the app holds (idle too): anything that only grows
         * shows here over a long session. Development builds only. */
#ifdef JELLY5_LOG_HOST
        {
            static double last_health = 0;
            const double hnow = now_s();
            if (hnow - last_health >= 60.0) {
                last_health = hnow;
                size_t art_bytes, art_n, hashes;
                art::stats(&art_bytes, &art_n, &hashes);
                evo_bt("health: up %.0f min, art %zu MB in %zu, blurhash %zu, text %zu, pool %zu/%zu MB, clients %zu",
                       (hnow - t0) / 60.0, art_bytes >> 20, art_n, hashes, gfx::text_cache_size(),
                       gfx::pool_used() >> 20, gfx::pool_size() >> 20, s_clients.size());
                evo_log_alloc_state("ui");
            }
        }
#endif
    }
}
