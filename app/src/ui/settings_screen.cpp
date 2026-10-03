/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/settings_screen.h"

#include "app/settings.h"
#include "evo_agc_runtime.h"
#include "app/syncplay.h"
#include "app/i18n.h"
#include "nuvio_input.h"

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

const char *kHeaders[] = {"Konto", "Avspilling", "Generelt"};

int section_of(int row) { return row <= SettingsScreen::SignOut ? 0 : row <= SettingsScreen::AudioDelay ? 1 : 2; }

const char *label_of(int row)
{
    const char *const labels[] = {T("Bytt bruker"),
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
                                         T("Språk"),
                                         T("Bildefrekvens"),
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

void SettingsScreen::activate()
{
    m_row = 0;
    m_scroll.snap(0);
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
        if (s.local.language == i18n::Norwegian) return "Norsk";
        if (s.local.language == i18n::English) return "English";
        return std::string(T("Automatisk")) + " (" + (i18n::english() ? "English" : "Norsk") + ")";
    }
    case SubSize: return std::to_string(s.local.sub_size) + " %";
    case SubBackground:
        return s.local.sub_background < 0.05f ? std::string(T("Av"))
                                              : std::to_string((int)(s.local.sub_background * 100 + 0.5f)) + " %";
    case Autoplay: return s.server.autoplay_next ? T("På") : T("Av");
    case AutoSkip: return s.local.auto_skip_intro ? T("På") : T("Av");
    case AudioDelay:
        return s.local.audio_delay_ms == 0 ? std::string(T("Ingen"))
                                           : (s.local.audio_delay_ms > 0 ? "+" : "") + std::to_string(s.local.audio_delay_ms) + " ms";
    case Refresh:
        if (!evo_agc_runtime_supports_120hz())
            return T("60 Hz (TV-en har ikke 120 Hz)");
        return s.local.refresh_120 ? "120 Hz" : "60 Hz";
    case Together: return syncplay::active() ? syncplay::group_name() : std::string(T("Av"));
    case ServerInfo: return m_server_name.empty() ? m_client.server() : m_server_name + "  \xC2\xB7  " + m_server_version;
    case About: return std::string(T("Versjon ")) + JELLY5_VERSION;
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
        s.local.language = cycle(s.local.language, 3);   /* Automatisk, Norsk, English */
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
    default:
        break;
    }
}

Action SettingsScreen::input(uint32_t p)
{
    Action a;
    if (p & NUVIO_BTN_DOWN)
        m_row = std::min((int)RowCount - 1, m_row + 1);
    else if (p & NUVIO_BTN_UP) {
        if (m_row == 0)
            a.kind = Action::ToNav;
        else
            m_row--;
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
        else
            change((Row)m_row, 1);
    }
    return a;
}

void SettingsScreen::draw(double, float dt)
{
    m_animating = false;
    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    gfx::fill_vgradient({0, 0, gfx::W, 500}, 0x33302048u, 0x00000000u);

    const float row_h = 84, head_h = 70, left = 360, width = gfx::W - 2 * left;
    /* Layout: each section's header, then its rows. */
    float y = 260;
    float ys[RowCount];
    int last_section = -1;
    for (int r = 0; r < RowCount; r++) {
        const int sec = r == About ? 3 : section_of(r);
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
        const int sec = r0 == About ? 3 : section_of(r0);
        int r1 = r0;
        while (r1 + 1 < RowCount && (r1 + 1 == About ? 3 : section_of(r1 + 1)) == sec)
            r1++;
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
        const int sec = r == About ? 3 : section_of(r);
        if (sec != last_section) {
            last_section = sec;
            if (sec < 3)
                gfx::text(left + 8, ys[r] - 22 - off, T(kHeaders[sec]), {gfx::Bold, 22}, kText3);
        }
        const bool focus = r == m_row;
        const gfx::Rect rr{left, ys[r] - off, width, row_h};
        const uint32_t fg = kText, fg2 = focus ? kText : kText2;
        const float cy = rr.y + rr.h / 2 + 9;
        gfx::text(rr.x + 32, cy, label_of(r), {focus ? gfx::Bold : gfx::SemiBold, 26}, r == SignOut ? 0xffff7a7au : fg);
        const std::string v = value((Row)r);
        const bool adjustable = r >= Quality && r <= Refresh;
        const float vx = rr.x + rr.w - 32 - (adjustable && focus ? 30 : 0);
        gfx::text(vx, cy, v, {gfx::Medium, 24, 700}, fg2, 2);
        if (adjustable && focus) {
            gfx::text(rr.x + rr.w - 30, cy, "\xE2\x80\xBA", {gfx::Bold, 30}, fg2, 2);
            gfx::text(vx - gfx::text_width(v, {gfx::Medium, 24, 700}) - 14, cy, "\xE2\x80\xB9", {gfx::Bold, 30}, fg2, 2);
        }
    }
    gfx::text(left, y + 40 - off,
              T("Lyd, undertekster og autoavspilling lagres på Jellyfin-kontoen din og gjelder i alle Jellyfin-apper."),
              {gfx::Regular, 20, width}, kText3);
    gfx::text(left, y + 72 - off, T("Språk følger PS5-en, eller velg her."), {gfx::Regular, 20, width}, kText3);
    gfx::text(left, y + 104 - off, T("Jelly5 er fri programvare (GPL-3.0) og bygger på EVO Player og Nuvio PS5."),
              {gfx::Regular, 20, width}, kText3);
}

} // namespace ui
