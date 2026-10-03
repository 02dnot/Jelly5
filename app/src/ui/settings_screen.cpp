/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/settings_screen.h"

#include "app/settings.h"
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

const char *kHeaders[] = {"Konto", "Avspilling", "Server"};

int section_of(int row) { return row <= 1 ? 0 : row <= 7 ? 1 : 2; }

const char *label_of(int row)
{
    static const char *const labels[] = {"Bytt bruker",
                                         "Logg ut",
                                         "Maks kvalitet",
                                         "Foretrukket lydspråk",
                                         "Undertekster",
                                         "Undertekstspråk",
                                         "Spill neste episode automatisk",
                                         "Hopp over intro automatisk",
                                         "Server",
                                         "Om Jelly5"};
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
        return s.local.max_mbps == 0 ? "Automatisk (maks)" : std::to_string(s.local.max_mbps) + " Mbit/s";
    case AudioLang: return kLangs[index_of(kLangs, s.server.audio_language)].name;
    case SubMode: return kModes[index_of(kModes, s.server.subtitle_mode)].name;
    case SubLang: return kLangs[index_of(kLangs, s.server.subtitle_language)].name;
    case Autoplay: return s.server.autoplay_next ? "På" : "Av";
    case AutoSkip: return s.local.auto_skip_intro ? "På" : "Av";
    case ServerInfo: return m_server_name.empty() ? m_client.server() : m_server_name + "  \xC2\xB7  " + m_server_version;
    case About: return std::string("Versjon ") + JELLY5_VERSION;
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
        a.kind = Action::ToNav;
    } else if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
        change((Row)m_row, (p & NUVIO_BTN_RIGHT) ? 1 : -1);
    } else if (p & NUVIO_BTN_CROSS) {
        if (m_row == SwitchUser)
            a.kind = Action::SwitchUser;
        else if (m_row == SignOut)
            a.kind = Action::SignOut;
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

    gfx::text(left, 200 - off, "Innstillinger", {gfx::Bold, 64}, kText);
    last_section = -1;
    for (int r = 0; r < RowCount; r++) {
        const int sec = r == About ? 3 : section_of(r);
        if (sec != last_section) {
            last_section = sec;
            if (sec < 3)
                gfx::text(left + 8, ys[r] - 22 - off, kHeaders[sec], {gfx::Bold, 22}, kText3);
        }
        const bool focus = r == m_row;
        const float lift = m_lifts.step(std::to_string(r), focus, dt, &m_animating);
        const float k = 1.f + 0.02f * lift;
        const gfx::Rect rr{left - width * (k - 1) / 2, ys[r] - off - row_h * (k - 1) / 2, width * k, row_h * k};
        if (lift > 0.01f)
            gfx::shadow(rr, 16, 24, 0.5f * lift, 10 * lift);
        gfx::fill(rr, focus ? 0xfff5f5f7u : 0x0fffffffu, 16);
        const uint32_t fg = focus ? 0xff0b0b0fu : kText;
        const uint32_t fg2 = focus ? 0xb30b0b0fu : kText2;
        const float cy = rr.y + rr.h / 2 + 9;
        gfx::text(rr.x + 32, cy, label_of(r), {gfx::SemiBold, 26}, r == SignOut && !focus ? 0xffff7a7au : fg);
        const std::string v = value((Row)r);
        const bool adjustable = r >= Quality && r <= AutoSkip;
        const float vx = rr.x + rr.w - 32 - (adjustable && focus ? 30 : 0);
        gfx::text(vx, cy, v, {gfx::Medium, 24, 700}, fg2, 2);
        if (adjustable && focus) {
            gfx::text(rr.x + rr.w - 30, cy, "\xE2\x80\xBA", {gfx::Bold, 30}, fg2, 2);
            gfx::text(vx - gfx::text_width(v, {gfx::Medium, 24, 700}) - 14, cy, "\xE2\x80\xB9", {gfx::Bold, 30}, fg2, 2);
        }
    }
    gfx::text(left, y + 40 - off,
              "Lyd, undertekster og autoavspilling lagres på Jellyfin-kontoen din og gjelder i alle Jellyfin-apper.",
              {gfx::Regular, 20, width}, kText3);
    gfx::text(left, y + 72 - off, "Jelly5 er fri programvare (GPL-3.0) og bygger på EVO Player og Nuvio PS5.",
              {gfx::Regular, 20, width}, kText3);
}

} // namespace ui
