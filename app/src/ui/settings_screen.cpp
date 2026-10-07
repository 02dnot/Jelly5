/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/settings_screen.h"
#include "evo_audio_out.h"

#include "app/settings.h"
#include "app/seerr_service.h"
#include "evo_agc_runtime.h"
#include "app/syncplay.h"
#include "app/i18n.h"
#include "nuvio_input.h"
#include "platform/ime.h"

#include <algorithm>

#ifndef JELLY5_VERSION
#define JELLY5_VERSION "0.0.1"
#endif

namespace ui {
namespace {

const int kQualities[] = {0, 120, 80, 60, 40, 20, 10, 8, 4};
constexpr int kNumQualities = 9;

struct Lang {
    const char *code, *name;
};
/* ISO 639-2 codes, as Jellyfin stores them. "" = no preference. */
const Lang kLangs[] = {{"", "Ingen preferanse"}, {"nor", "Norsk"},   {"eng", "Engelsk"}, {"swe", "Svensk"},
                       {"dan", "Dansk"},          {"fin", "Finsk"},   {"ger", "Tysk"},    {"fre", "Fransk"},
                       {"spa", "Spansk"},         {"ita", "Italiensk"}, {"jpn", "Japansk"}, {"kor", "Koreansk"}};
constexpr int kNumLangs = 12;

struct Mode {
    const char *code, *name;
};
const Mode kModes[] = {{"Default", "Standard"},
                       {"Smart", "Smart"},
                       {"Always", "Alltid"},
                       {"OnlyForced", "Bare tvungne"},
                       {"None", "Av"}};
constexpr int kNumModes = 5;

const char *kHeaders[] = {"Konto", "Avspilling", "Seerr", "Generelt"};

/* Its card: 0 account, 1 playback, 2 Seerr, 3 general, 4 about (no header). */
int section_of(int row)
{
    return row <= SettingsScreen::SignOut      ? 0
           : row <= SettingsScreen::ThemeMusic ? 1
           : row <= SettingsScreen::SeerrTest  ? 2
           : row == SettingsScreen::About      ? 4
                                               : 3;
}

/* Left/Right changes it (the rest act on Cross). */
bool adjustable(int r)
{
    return (r >= SettingsScreen::Quality && r <= SettingsScreen::ThemeMusic) || r == SettingsScreen::SeerrOn ||
           r == SettingsScreen::SeerrAuth ||
           (r >= SettingsScreen::AppLanguage && r <= SettingsScreen::Updates);
}

/* The sign-in Seerr gets: where the server has no Quick Connect (Emby; Seerr has
 * it for Jellyfin only), the server's password stands in for it. */
seerr_service::Auth effective_auth(seerr_service::Auth a, const jf::Client &c)
{
    return a == seerr_service::Auth::QuickConnect && !c.features().quick_connect ? seerr_service::Auth::JellyfinPassword
                                                                                 : a;
}

const char *auth_name(seerr_service::Auth a, const jf::Client &c)
{
    const bool emby = c.kind() == jf::Kind::Emby;   /* (the password is named after the server) */
    switch (effective_auth(a, c)) {
    case seerr_service::Auth::JellyfinPassword: return emby ? T("Emby-passord") : T("Jellyfin-passord");
    case seerr_service::Auth::Local: return T("Seerr-konto (e-post)");
    default: return T("Automatisk (Quick Connect)");
    }
}

const char *label_of(int row)
{
    const char *const labels[] = {T("Bytt bruker eller server"),
                                         T("Logg ut"),
                                         T("Maks kvalitet"),
                                         T("Foretrukket lydspråk"),
                                         T("Undertekster"),
                                         T("Undertekstspråk"),
                                         T("Undertekststørrelse"),
                                         T("Undertekstbakgrunn"),
                                         T("Spill neste episode automatisk"),
                                         T("Hopp over intro automatisk"),
                                         T("Lydforsinkelse"),
                                         T("Nattmodus"),
                                         T("HDMI-bitstr\xC3\xB8m"),
                                         T("Temamusikk"),
                                         "Seerr",
                                         T("Adresse"),
                                         T("P\xC3\xA5logging"),
                                         T("Seerr-konto"),
                                         T("Test tilkoblingen"),
                                         T("Språk"),
                                         T("Bildefrekvens"),
                                         T("Se etter oppdateringer"),
                                         T("Se sammen"),
                                         "Server",
                                         T("Om Jelly5")};
    return labels[row];
}

template <class T, size_t N> int index_of(const T (&list)[N], const std::string &code)
{
    for (size_t i = 0; i < N; i++)
        if (code == list[i].code)
            return (int)i;
    return 0;
}

} // namespace

SettingsScreen::~SettingsScreen() { ime::cancel(); }

void SettingsScreen::activate()
{
    m_row = 0;
    m_scroll.snap(0);
    ime::init();
}

bool SettingsScreen::shown(int r) const
{
    const jf::Features f = m_client.features();
    if (r == Together)
        return f.syncplay;
    if (r < SeerrUrl || r > SeerrTest)
        return true;
    return seerr_service::config().enabled;
}

void SettingsScreen::seerr_account()
{
    using namespace seerr_service;
    const Snapshot s = snapshot();
    if (s.state == State::Connecting)
        return;
    if (s.state == State::Ready) {   /* signing out also ends automatic sign-in: ask first */
        if (!m_signout_armed) {
            m_signout_armed = true;
            return;
        }
        m_signout_armed = false;
        sign_out();
        return;
    }
    switch (effective_auth(config().auth, m_client)) {
    case Auth::QuickConnect:   /* ✕ here approves Quick Connect for the address shown */
        if (s.why == Why::NeedApproval)
            approve_quick_connect();
        else
            reconnect();
        break;
    case Auth::JellyfinPassword:
        ime::request(ime::Kind::Password,
                     (m_client.kind() == jf::Kind::Emby ? T("Emby-passord for ") : T("Jellyfin-passord for ")) +
                         m_client.user_name(),
                     "",
                     [](const std::string &pw) { seerr_service::sign_in("", pw); });
        break;
    case Auth::Local:   /* the e-mail first; the password once the keyboard has closed */
        ime::request(ime::Kind::Text, T("E-post for Seerr-kontoen"), m_seerr_email, [this](const std::string &e) {
            m_seerr_email = e;
            m_want_password = !e.empty();
        });
        break;
    default:
        break;
    }
}

std::string SettingsScreen::value(Row r) const
{
    const settings::All s = settings::get();
    switch (r) {
    case SwitchUser: return m_client.user_name();
    case Quality:
        return s.local.max_mbps == 0 ? T("Automatisk (maks)") : std::to_string(s.local.max_mbps) + " Mbit/s";
    case AudioLang: return T(kLangs[index_of(kLangs, s.server.audio_language)].name);
    case SubMode: return T(kModes[index_of(kModes, s.server.subtitle_mode)].name);
    case SubLang: return T(kLangs[index_of(kLangs, s.server.subtitle_language)].name);
    case AppLanguage: {   /* each language in its own name */
        if (s.local.language > i18n::Auto)
            return i18n::choice_name(s.local.language);
        return std::string(T("Automatisk")) + " (" + i18n::choice_name((int)i18n::lang() + 1) + ")";
    }
    case SubSize: return std::to_string(s.local.sub_size) + " %";
    case SubBackground:
        return s.local.sub_background < 0.05f ? std::string(T("Av"))
                                              : std::to_string((int)(s.local.sub_background * 100 + 0.5f)) + " %";
    case Autoplay: return s.server.autoplay_next ? T("På") : T("Av");
    case AutoSkip: return s.local.auto_skip_intro ? T("På") : T("Av");
    case NightMode: return s.local.night_mode ? T("På") : T("Av");
    case Bitstream:   /* night mode needs the sound decoded here, so it wins */
        return !s.local.hdmi_bitstream ? T("Av") : s.local.night_mode ? T("Av med nattmodus") : T("På");
    case ThemeMusic: return s.local.theme_music ? T("På") : T("Av");
    case Updates: return s.local.check_updates ? T("På") : T("Av");
    case AudioDelay:
        return s.local.audio_delay_ms == 0 ? std::string(T("Ingen"))
                                           : (s.local.audio_delay_ms > 0 ? "+" : "") + std::to_string(s.local.audio_delay_ms) + " ms";
    case Refresh:
        if (!evo_agc_runtime_supports_120hz())
            return T("60 Hz (TV-en har ikke 120 Hz)");
        return s.local.refresh_120 ? "120 Hz" : "60 Hz";
    case Together: return syncplay::active() ? syncplay::group_name() : std::string(T("Av"));
    case ServerInfo:
        return m_server_name.empty() ? m_client.server()
                                     : m_server_name + "  \xC2\xB7  " +
                                           (m_client.kind() == jf::Kind::Emby ? "Emby " : "Jellyfin ") + m_server_version;
    case About: return std::string(T("Versjon ")) + JELLY5_VERSION;
    case SeerrOn: return seerr_service::config().enabled ? T("P\xC3\xA5") : T("Av");
    case SeerrUrl: {
        const std::string u = seerr_service::config().url;
        return u.empty() ? std::string(T("Ikke angitt")) : u;
    }
    case SeerrAuth: return auth_name(seerr_service::config().auth, m_client);
    case SeerrAccount: {
        using namespace seerr_service;
        const Snapshot sn = snapshot();
        switch (sn.state) {
        case State::Connecting: return T("Kobler til \xE2\x80\xA6");
        case State::Ready:
            return m_signout_armed ? std::string(T("Trykk \xE2\x9C\x95 igjen for å logge ut"))
                                   : sn.user.name + "  \xC2\xB7  " + T("\xE2\x9C\x95 logg ut");
        case State::Unreachable: return T("Svarer ikke \xE2\x80\x93 pr\xC3\xB8ver igjen");
        case State::SignedOut:
            if (sn.why == Why::WrongPassword)
                return T("Feil brukernavn eller passord");
            if (sn.why == Why::MethodOff)
                return T("Seerr tillater ikke denne påloggingen \xE2\x80\x93 velg en annen");
            if (sn.why == Why::NotInSeerr)
                return T("Brukeren finnes ikke i Seerr \xE2\x80\x93 be administratoren importere den");
            if (sn.why == Why::NeedApproval)
                return T("\xE2\x9C\x95 godkjenn Quick Connect for denne adressen");
            if (sn.why == Why::AutoFailed)
                return T("Automatisk p\xC3\xA5logging mislyktes \xE2\x80\x93 velg passord");
            return T("Ikke p\xC3\xA5logget \xE2\x80\x93 \xE2\x9C\x95 for \xC3\xA5 logge p\xC3\xA5");
        default: return std::string();
        }
    }
    case SeerrTest: {
        const seerr_service::Snapshot sn = seerr_service::snapshot();
        if (sn.testing)
            return T("Tester \xE2\x80\xA6");
        return sn.test.empty() ? std::string(T("\xE2\x9C\x95 for \xC3\xA5 teste")) : sn.test;
    }
    default: return std::string();
    }
}

void SettingsScreen::change(Row r, int dir)
{
    settings::All s = settings::get();
    auto cycle = [dir](int i, int n) { return ((i + dir) % n + n) % n; };
    switch (r) {
    case Quality: {
        int i = 0;
        for (int k = 0; k < kNumQualities; k++)
            if (kQualities[k] == s.local.max_mbps)
                i = k;
        s.local.max_mbps = kQualities[cycle(i, kNumQualities)];
        settings::set_local(s.local);
        break;
    }
    case AutoSkip:
        s.local.auto_skip_intro = !s.local.auto_skip_intro;
        settings::set_local(s.local);
        break;
    case NightMode:
        s.local.night_mode = !s.local.night_mode;
        settings::set_local(s.local);
        evo_audio_set_night(s.local.night_mode);
        break;
    case Bitstream:   /* from the next playback */
        s.local.hdmi_bitstream = !s.local.hdmi_bitstream;
        settings::set_local(s.local);
        break;
    case ThemeMusic:
        s.local.theme_music = !s.local.theme_music;
        settings::set_local(s.local);
        break;
    case Updates:
        s.local.check_updates = !s.local.check_updates;
        settings::set_local(s.local);
        break;
    case AudioDelay:   /* 20 ms steps: a soundbar's delay is typically 40-200 ms */
        s.local.audio_delay_ms = std::max(-500, std::min(500, s.local.audio_delay_ms + dir * 20));
        settings::set_local(s.local);
        break;
    case SubSize:
        s.local.sub_size = std::max(50, std::min(200, s.local.sub_size + dir * 10));
        settings::set_local(s.local);
        break;
    case SubBackground: {   /* Av, 25, 50, 75 % */
        const int i = (int)(s.local.sub_background * 4 + 0.5f);
        s.local.sub_background = (float)cycle(i, 4) / 4.f;
        settings::set_local(s.local);
        break;
    }
    case Refresh:
        if (!evo_agc_runtime_supports_120hz())
            break;
        s.local.refresh_120 = !s.local.refresh_120;
        settings::set_local(s.local);
        evo_agc_runtime_set_120hz(s.local.refresh_120 ? 1 : 0);
        break;
    case AppLanguage:
        s.local.language = cycle(s.local.language, i18n::ChoiceCount);   /* Automatisk, then each language */
        settings::set_local(s.local);
        i18n::set_choice(s.local.language);
        break;
    case AudioLang:
        s.server.audio_language = kLangs[cycle(index_of(kLangs, s.server.audio_language), kNumLangs)].code;
        settings::set_server(m_client, s.server);
        break;
    case SubLang:
        s.server.subtitle_language = kLangs[cycle(index_of(kLangs, s.server.subtitle_language), kNumLangs)].code;
        settings::set_server(m_client, s.server);
        break;
    case SubMode:
        s.server.subtitle_mode = kModes[cycle(index_of(kModes, s.server.subtitle_mode), kNumModes)].code;
        settings::set_server(m_client, s.server);
        break;
    case Autoplay:
        s.server.autoplay_next = !s.server.autoplay_next;
        settings::set_server(m_client, s.server);
        break;
    case SeerrOn: {   /* turned on with no address yet: the suggestion */
        seerr_service::Config c = seerr_service::config();
        c.enabled = !c.enabled;
        if (c.enabled && c.url.empty())
            c.url = seerr_service::suggested_url();
        seerr_service::set_config(c);
        break;
    }
    case SeerrAuth: {
        seerr_service::Config c = seerr_service::config();
        /* From what the row shows; past Quick Connect where the server has none. */
        c.auth = (seerr_service::Auth)cycle((int)effective_auth(c.auth, m_client), (int)seerr_service::Auth::Count);
        if (effective_auth(c.auth, m_client) != c.auth)
            c.auth = (seerr_service::Auth)cycle((int)c.auth, (int)seerr_service::Auth::Count);
        seerr_service::set_config(c);
        break;
    }
    default:
        break;
    }
}

Action SettingsScreen::input(uint32_t p)
{
    Action a;
    if (!(p & NUVIO_BTN_CROSS))
        m_signout_armed = false;   /* moved on: the account row asks again */
    if (p & NUVIO_BTN_DOWN) {
        int r = m_row + 1;
        while (r < RowCount && !shown(r))
            r++;
        if (r < RowCount)
            m_row = r;
    } else if (p & NUVIO_BTN_UP) {
        if (m_row == 0)
            a.kind = Action::ToNav;
        else
            do
                m_row--;
            while (m_row > 0 && !shown(m_row));
    } else if (p & NUVIO_BTN_CIRCLE) {
        /* Back, as elsewhere: to the top of the list first, then up to the tabs. */
        if (m_row > 0)
            m_row = 0;
        else
            a.kind = Action::ToNav;
    } else if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
        change((Row)m_row, (p & NUVIO_BTN_RIGHT) ? 1 : -1);
    } else if (p & NUVIO_BTN_CROSS) {
        if (m_row == SwitchUser)
            a.kind = Action::SwitchUser;
        else if (m_row == SignOut)
            a.kind = Action::SignOut;
        else if (m_row == Together) {   /* the groups page */
            a.kind = Action::Open;
            a.item.type = "SyncPlay";
        }
        else if (m_row == SeerrUrl) {
            const seerr_service::Config c = seerr_service::config();
            ime::request(ime::Kind::Url, T("Seerr-adresse"), c.url.empty() ? seerr_service::suggested_url() : c.url,
                         [](const std::string &t) {
                             seerr_service::Config n = seerr_service::config();
                             n.url = t;
                             seerr_service::set_config(n);
                         });
        }
        else if (m_row == SeerrAccount)
            seerr_account();
        else if (m_row == SeerrTest)
            seerr_service::test();
        else
            change((Row)m_row, 1);
    }
    return a;
}

void SettingsScreen::draw(double, float dt)
{
    m_animating = false;
    if (m_want_password && !ime::active()) {   /* a local Seerr account: its password, after the e-mail */
        m_want_password = false;
        const std::string email = m_seerr_email;
        ime::request(ime::Kind::Password, T("Passord for Seerr-kontoen"), "",
                     [email](const std::string &pw) { seerr_service::sign_in(email, pw); });
    }
    const seerr_service::State seerr_state = seerr_service::snapshot().state;
    if (seerr_state == seerr_service::State::Connecting || seerr_service::snapshot().testing)
        m_animating = true;   /* the values change on their own */
    if (!shown(m_row))
        m_row = SeerrOn;      /* turned off under the focus */
    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, 500}, 0x33302048u, 0x00000000u);

    const float row_h = 84, head_h = 70, left = 360, width = gfx::W - 2 * left;
    /* Layout: each section's header, then its rows. */
    float y = 260;
    float ys[RowCount];
    int last_section = -1;
    for (int r = 0; r < RowCount; r++) {
        ys[r] = y;
        if (!shown(r))
            continue;
        const int sec = section_of(r);
        if (sec != last_section) {
            y += last_section < 0 ? 0 : 30;
            y += head_h;
            last_section = sec;
        }
        ys[r] = y;
        y += row_h + 8;
    }
    m_scroll.to(std::max(0.f, ys[m_row] - 700));
    if (m_scroll.step(dt, 11.f))
        m_animating = true;
    const float off = m_scroll.value;

    gfx::text(left, 200 - off, T("Innstillinger"), {gfx::Bold, 64}, kText);
    /* Each section is one glass card (a grouped list); the focus is the drop. */
    for (int r0 = 0; r0 < RowCount;) {
        if (!shown(r0)) {
            r0++;
            continue;
        }
        const int sec = section_of(r0);
        int r1 = r0;   /* the section's last row shown (past rows this server hides) */
        for (int r = r0 + 1; r < RowCount && section_of(r) == sec; r++)
            if (shown(r))
                r1 = r;
        const gfx::Rect card{left - 8, ys[r0] - off - 8, width + 16, ys[r1] + row_h - ys[r0] + 16};
        if (card.y < gfx::H && card.y + card.h > 0)
            glass_panel(card, 24, 1.f, false);
        r0 = r1 + 1;
    }
    if (m_focused)
        m_drop.to({left, ys[m_row] - off, width, row_h}, m_row, 0, -off);
    else
        m_drop.hide();
    m_drop.draw(dt, 1.f, &m_animating, 16);
    last_section = -1;
    for (int r = 0; r < RowCount; r++) {
        if (!shown(r))
            continue;
        const int sec = section_of(r);
        if (sec != last_section) {
            last_section = sec;
            if (sec < 4)
                gfx::text(left + 8, ys[r] - 22 - off, sec == 2 ? kHeaders[sec] : T(kHeaders[sec]), {gfx::Bold, 22}, kText3);
        }
        const bool focus = r == m_row;
        const gfx::Rect rr{left, ys[r] - off, width, row_h};
        const uint32_t fg = kText, fg2 = focus ? kText : kText2;
        const float cy = rr.y + rr.h / 2 + 9;
        gfx::text(rr.x + 32, cy, label_of(r), {focus ? gfx::Bold : gfx::SemiBold, 26}, r == SignOut ? 0xffff7a7au : fg);
        const std::string v = value((Row)r);
        const bool adj = adjustable(r);
        const float vx = rr.x + rr.w - 32 - (adj && focus ? 30 : 0);
        gfx::text(vx, cy, v, {gfx::Medium, 24, 760}, fg2, 2);
        if (adj && focus) {
            gfx::text(rr.x + rr.w - 30, cy, "\xE2\x80\xBA", {gfx::Bold, 30}, fg2, 2);
            gfx::text(vx - gfx::text_width(v, {gfx::Medium, 24, 760}) - 14, cy, "\xE2\x80\xB9", {gfx::Bold, 30}, fg2, 2);   /* as the value's own width */
        }
    }
    gfx::text(left, y + 40 - off,
              T("Lyd, undertekster og autoavspilling lagres på kontoen din på serveren og gjelder i alle appene du bruker med den."),
              {gfx::Regular, 20, width}, kText3);
    gfx::text(left, y + 72 - off, T("Språk følger PS5-en, eller velg her."), {gfx::Regular, 20, width}, kText3);
    gfx::text(left, y + 104 - off, T("Jelly5 er fri programvare (GPL-3.0) og bygger på EVO Player og Nuvio PS5."),
              {gfx::Regular, 20, width}, kText3);
    if (seerr_service::config().enabled)   /* only where it means something */
        gfx::text(left, y + 136 - off,
                  T("Seerr henter alt fra TMDB selv: PS5-en snakker bare med serveren din og Seerr."),
                  {gfx::Regular, 20, width}, kText3);
    if (seerr_service::config().enabled && !m_client.features().quick_connect &&   /* Emby: why a password */
        effective_auth(seerr_service::config().auth, m_client) == seerr_service::Auth::JellyfinPassword)
        gfx::text(left, y + 168 - off, T("Emby har ikke Quick Connect: Seerr logger inn med Emby-passordet ditt og husker innloggingen i 30 dager."), {gfx::Regular, 20, width}, kText3);
}

} // namespace ui
