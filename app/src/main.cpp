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
#include "app/accounts.h"
#include "app/settings.h"
#include "platform/ime.h"
#include "jf/jf_client.h"
#include "nuvio_input.h"
#include "nuvio_player.h"
#include "gfx/art.h"
#include "gfx/gfx.h"
#include "ui/detail.h"
#include "ui/home.h"
#include "ui/library.h"
#include "ui/nav.h"
#include "ui/login.h"
#include "ui/profiles.h"
#include "ui/search.h"
#include "ui/settings_screen.h"
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
    std::string movies_view, shows_view;
    GateRequest gate;                   /* a gate screen the main thread should open */
};
State s_state;
unsigned s_model_version = 0, s_home_version = 0;   /* model published / taken by Home */
std::atomic<unsigned> s_session{0};                 /* bumped on every account change */
std::unique_ptr<jf::Client> s_client;

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
    if (n)
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

/* Loads the home rows, all requests in parallel. With keep_hero the featured
 * titles are kept (a refresh after playback only needs the rows). */
void load_home(bool keep_hero = false)
{
    const unsigned session = s_session;
    {
        std::lock_guard<std::mutex> g(s_state.lock);
        if (s_state.phase != Phase::Home) {
            s_state.phase = Phase::Loading;
            s_state.message = "Henter biblioteket \xE2\x80\xA6";
        }
    }
    jf::Client &c = *s_client;
    std::vector<jf::Item> hero, resume, next, views;
    std::vector<std::thread> jobs;
    if (!keep_hero)
        jobs.emplace_back([&] { hero = c.featured(6); });
    jobs.emplace_back([&] { resume = c.resume(20); });
    jobs.emplace_back([&] { next = c.next_up(20); });
    jobs.emplace_back([&] { views = c.views(); });
    for (auto &j : jobs)
        j.join();
    std::vector<std::pair<std::string, std::vector<jf::Item>>> latest;
    for (const jf::Item &v : views)
        if (v.collection_type == "movies" || v.collection_type == "tvshows")
            latest.push_back({"Nylig lagt til i " + v.name, {}});
    jobs.clear();
    size_t k = 0;
    for (const jf::Item &v : views)
        if (v.collection_type == "movies" || v.collection_type == "tvshows") {
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
    if (!resume.empty()) m.rows.push_back({"Fortsett å se", std::move(resume), true});
    if (!next.empty()) m.rows.push_back({"Neste episode", std::move(next), true});
    for (auto &l : latest)
        if (!l.second.empty())
            m.rows.push_back({l.first, std::move(l.second), false});
    std::lock_guard<std::mutex> g(s_state.lock);
    if (session != s_session)
        return;   /* the account changed while this loaded */
    s_state.movies_view.clear();
    s_state.shows_view.clear();
    for (const jf::Item &v : views) {
        if (v.collection_type == "movies" && s_state.movies_view.empty())
            s_state.movies_view = v.id;
        if (v.collection_type == "tvshows" && s_state.shows_view.empty())
            s_state.shows_view = v.id;
    }
    s_state.model = std::move(m);
    s_model_version++;
    s_state.phase = Phase::Home;
    s_state.message.clear();
}

/* Signs in with a saved account (off the main thread): check the token, then
 * preferences, server info and the home rows. A rejected token asks for the
 * password again; an unreachable server is retried until the account changes. */
void use_account(accounts::Account a)
{
    const unsigned session = s_session;
    jf::Client &c = *s_client;
    c.set_server(a.server);
    c.set_session(a.token, a.user_id, a.user_name);
    c.note_image_tag(a.image_tag);
    set_phase(Phase::Connecting, "Kobler til " + (a.server_name.empty() ? a.server : a.server_name) + " \xE2\x80\xA6");
    while (session == s_session) {
        if (c.validate()) {
            accounts::set_last(a.server, a.user_id);
            a.user_name = c.user_name();
            a.image_tag = c.user_image_tag();
            accounts::remember(a);
            settings::load_server(c);
            std::string name, version;
            c.public_info(&name, &version);
            {
                std::lock_guard<std::mutex> g(s_state.lock);
                s_state.server_name = name;
                s_state.server_version = version;
            }
            evo_bt("jelly5: signed in as %s on %s %s", c.user_name().c_str(), name.c_str(), version.c_str());
            load_home();
            return;
        }
        const std::string err = c.last_error();
        evo_bt("jelly5: sign-in check failed: %s", err.c_str());
        if (err.find("-> 401") != std::string::npos) {
            request_gate(Gate::Login, a.server, a.user_name, true);
            return;
        }
        set_phase(Phase::Failed, "Får ikke kontakt med " + (a.server_name.empty() ? a.server : a.server_name) +
                                     " \xE2\x80\x93 prøver igjen \xE2\x80\xA6");
        for (int i = 0; i < 50 && session == s_session; i++)
            usleep(100 * 1000);
    }
}

void *boot(void *)
{
    settings::load_local();
    accounts::Account a;
    if (accounts::last(&a))
        use_account(a);
    else if (!accounts::load().empty())
        request_gate(Gate::Profiles);
    else
        request_gate(Gate::Login, JELLY5_SERVER, "", false);
    return nullptr;
}

/* ---- screens --------------------------------------------------------------------- */
std::unique_ptr<ui::Home> s_home;
std::unique_ptr<ui::Library> s_movies, s_shows;
std::unique_ptr<ui::Search> s_search;
std::unique_ptr<ui::SettingsScreen> s_settings;
std::unique_ptr<ui::Profiles> s_profiles;
std::unique_ptr<ui::Login> s_login;
Gate s_gate = Gate::None;
ui::Nav s_nav;
std::vector<std::unique_ptr<ui::Screen>> s_stack;   /* detail pages over the tab */
int s_tab = ui::Nav::Home;
bool s_nav_focus = false;
int s_nav_tab = ui::Nav::Home;     /* focused tab while s_nav_focus */

/* Fresh screens for a new account (nothing of the last one's library survives). */
void reset_screens()
{
    s_stack.clear();
    s_home.reset(new ui::Home(*s_client));
    s_movies.reset(new ui::Library(*s_client, "Filmer", "Movie"));
    s_shows.reset(new ui::Library(*s_client, "Serier", "Series"));
    s_search.reset(new ui::Search(*s_client));
    s_settings.reset(new ui::SettingsScreen(*s_client));
    s_tab = s_nav_tab = ui::Nav::Home;
    s_nav_focus = false;
    std::lock_guard<std::mutex> g(s_state.lock);
    s_state.model = ui::HomeModel();
    s_state.movies_view.clear();
    s_state.shows_view.clear();
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
    s_session++;
    reset_screens();
    s_gate = Gate::None;
    set_phase(Phase::Connecting, "Kobler til \xE2\x80\xA6");
    std::thread([a] { use_account(a); }).detach();
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
void shell_input(uint32_t p, jf::Item *play, bool *chose, bool *from_start)
{
    if (s_nav_focus && s_stack.empty()) {
        const int before = s_nav_tab;
        if ((p & NUVIO_BTN_LEFT) && s_nav_tab > 0)
            s_nav_tab--;
        else if ((p & NUVIO_BTN_RIGHT) && s_nav_tab + 1 < ui::Nav::Count)
            s_nav_tab++;
        else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS))
            s_nav_focus = false;
        else if ((p & NUVIO_BTN_CIRCLE) && s_tab != ui::Nav::Home)
            s_nav_tab = ui::Nav::Home;   /* back from any tab goes home, as on Netflix */
        if (s_nav_tab != before && s_nav_tab != s_tab)
            open_tab(s_nav_tab);         /* tvOS-style: focusing a tab opens it */
        return;
    }
    const ui::Action a = screen_for(s_tab)->input(p);
    switch (a.kind) {
    case ui::Action::ToNav:
        if (s_stack.empty()) {
            s_nav_focus = true;
            s_nav_tab = s_tab;
        }
        break;
    case ui::Action::Play:
    case ui::Action::PlayFromStart:
        *play = a.item;
        *chose = true;
        *from_start = a.kind == ui::Action::PlayFromStart;
        break;
    case ui::Action::Open: {
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
        s_stack.emplace_back(new ui::Detail(*s_client, target));
        s_stack.back()->activate();
        break;
    }
    case ui::Action::Back:
        if (!s_stack.empty())
            s_stack.pop_back();
        break;
    case ui::Action::SwitchUser:
        s_session++;
        open_gate({Gate::Profiles});
        set_phase(Phase::Gate);
        break;
    case ui::Action::SignOut: {
        accounts::forget(s_client->server(), s_client->user_id());
        s_client->set_session("", "", "");
        s_session++;
        const bool others = !accounts::load().empty();
        open_gate({others ? Gate::Profiles : Gate::Login, s_client->server(), "", false});
        set_phase(Phase::Gate);
        break;
    }
    default:
        break;
    }
}

/* ---- drawing (GPU, src/gfx) ------------------------------------------------------ */
void draw_wordmark(float cx, float baseline, float size)
{
    const gfx::TextStyle st{gfx::Bold, size};
    const float m = size * 0.9f, gap = size * 0.3f;
    const float x = cx - (m + gap + gfx::text_width("jelly5", st)) / 2;
    gfx::fill({x, baseline - m * 0.82f, m, m}, 0xff7f80d0u, m * 0.29f);
    gfx::text(x + m + gap, baseline, "jelly5", st, 0xfff5f5f7u);
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
        gfx::text(gfx::W / 2, 1000, hint, {gfx::Medium, 22}, 0x6bebebf5u, 1);
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
    const gfx::TextStyle st{gfx::Bold, 110};
    const float m = 104, gap = 34;
    const float x = gfx::W / 2 - (m + gap + gfx::text_width("jelly5", st)) / 2;
    const gfx::Rect mark{x, 540 - m / 2 - 10, m, m};
    gfx::shadow(mark, 30, 40, 0.25f + 0.25f * pulse, 0);   /* a soft glow under the mark */
    gfx::fill_vgradient(mark, 0xffaa5cc3u, 0xff00a4dcu, 30);
    gfx::text(x + m + gap, 540 + 38, "jelly5", st, 0xfff5f5f7u);
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
            s_movies->set_view(s_state.movies_view);
            s_shows->set_view(s_state.shows_view);
            s_nav.set_user(s_client->user_name());
        }
    }
    art::tick();
    gfx::begin_frame();
    bool animating = true;
    switch (phase) {
    case Phase::Connecting:
    case Phase::Loading:
        s_splash.snap(1.f);
        draw_splash(t, 1.f);
        break;
    case Phase::Failed:
        draw_status(message, "", t, "\xE2\x97\x8B bytt bruker eller server");
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
            draw_status("Ingenting å vise ennå", "Legg til filmer eller serier i Jellyfin.", t);
        } else {
            ui::Screen *scr = screen_for(s_tab);
            const float enter = scr->enter();
            if (enter < 1.f) {
                /* A page fading in: the screen below stays under it until it is opaque. */
                ui::Screen *below = s_stack.size() >= 2 ? s_stack[s_stack.size() - 2].get()
                                                        : (s_tab == ui::Nav::Movies   ? (ui::Screen *)s_movies.get()
                                                           : s_tab == ui::Nav::Shows  ? (ui::Screen *)s_shows.get()
                                                           : s_tab == ui::Nav::Search ? (ui::Screen *)s_search.get()
                                                                                      : (ui::Screen *)s_home.get());
                below->draw(t, dt);
                gfx::push_opacity(enter);
            }
            scr->draw(t, dt);
            if (enter < 1.f)
                gfx::pop_opacity();
            animating = scr->animating();
            const float nav = !s_stack.empty() ? 0.f : s_nav_focus ? 1.f : scr->nav_alpha();
            s_nav.draw(nav, s_tab, s_nav_focus ? s_nav_tab : -1, dt, &animating);
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

/* What a chosen item plays: a series starts at its next episode. */
bool resolve_playable(jf::Item *item)
{
    if (item->type == "Movie" || item->type == "Episode" || item->type == "Video")
        return true;
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

void play(jf::Item item, bool from_start)
{
    if (from_start)
        item.position_ticks = 0;
    if (!resolve_playable(&item)) {
        notify("Jelly5: fant ingenting å spille av her");
        return;
    }
    /* Hand over to the player without a seam: this frame is the player's own
     * loading screen (its colour, the title's backdrop at 92 %, its gradient),
     * and the art it is about to ask for is already being fetched. */
    const std::string backdrop = s_client->image_url(item.backdrop_owner, "Backdrop", item.backdrop_tag, 1920);
    const std::string logo = s_client->image_url(item.logo_owner, "Logo", item.logo_tag, 800);
    if (!backdrop.empty())
        ui_image_request(backdrop.c_str(), 1920, 1080, 0);
    if (!logo.empty()) {
        ui_image_request(logo.c_str(), 640, 230, 0);
        ui_image_request(logo.c_str(), 520, 120, 0);
    }
    gfx::begin_frame();
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, 0xff080b10u);
    if (const gfx::Texture *t = art::get(backdrop, 1920, 1080))
        gfx::image(full, t, 0.92f, 0, true);
    gfx::fill_vgradient({0, 0, gfx::W, 378}, 0x4d000000u, 0x99000000u);
    gfx::fill_vgradient({0, 378, gfx::W, 378}, 0x99000000u, 0xcc000000u);
    gfx::fill_vgradient({0, 756, gfx::W, 324}, 0xcc000000u, 0xe6000000u);
    gfx::end_frame();

    nuvio_input_close();
    std::string error;
    if (!jelly5_play(*s_client, item, &error))
        notify(("Jelly5: kunne ikke spille av\n" + error).c_str());
    nuvio_input_open(s_user);
    /* Back at once; positions and "next up" refresh behind the screen. */
    if (!s_stack.empty())
        s_stack.back()->activate();   /* a detail page reloads its progress */
    std::thread([] { load_home(true); }).detach();
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
        notify("Jelly5: skjermen kunne ikke startes");
        for (;;)
            usleep(1000 * 1000);
    }
    if (ui_text_init() != 0 || !gfx::init())
        evo_bt("jelly5: ui init failed");
    nuvio_input_open(s_user);

    char device[48];
    std::snprintf(device, sizeof device, "jelly5-ps5-%d", s_user);
    s_client.reset(new jf::Client(JELLY5_SERVER, device, "PlayStation 5"));
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

        jf::Item chosen;
        bool chose = false, from_start = false;
        if (in.pressed) {
            if (phase == Phase::Gate)
                gate_input(in.pressed);
            else if (phase == Phase::Failed && (in.pressed & NUVIO_BTN_CIRCLE)) {
                s_session++;
                open_gate({accounts::load().empty() ? Gate::Login : Gate::Profiles, s_client->server(), "", false});
                set_phase(Phase::Gate);
            } else if (phase == Phase::Home && s_home_version == s_model_version)
                shell_input(in.pressed, &chosen, &chose, &from_start);
        }
        if (phase == Phase::Gate)
            gate_poll();
        if (chose) {
            play(chosen, from_start);
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
            idle_frames = (changed || animating) ? 0 : idle_frames + 1;
            last_phase = phase;
            last_model = s_model_version;
            if ((++frames % 120) == 0)
                gfx::collect();
        } else {
            usleep(8000);
            last = now_s();
        }
    }
}
