/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sizes and timings follow concept/style.css (.player ...).
 */
#include "ui/player_ui.h"

#include "gfx/art.h"
#include "gfx/gfx.h"
#include "nuvio_subs.h"
#include "ui/screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ui {
namespace {

constexpr float W = gfx::W, H = gfx::H;
constexpr float kBarY = H - 120, kBarH = 10;
constexpr uint32_t kGlass = 0xdc1c1c22u;
constexpr uint32_t kAccent = 0xff00a4dcu;

std::string fmt_time(double s)
{
    const int t = (int)std::max(0.0, s);
    char b[32];
    if (t >= 3600)
        std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else
        std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string clock_at(double seconds_from_now)
{
    const time_t t = time(nullptr) + (time_t)std::max(0.0, seconds_from_now);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

/* Language names in Norwegian; the player's own names otherwise. */
std::string language_name(const std::string &code)
{
    struct L {
        const char *codes, *name;
    };
    static const L names[] = {{"nor nob no nb", "Norsk"},   {"nno nn", "Nynorsk"},  {"eng en", "Engelsk"},
                              {"swe sv", "Svensk"},        {"dan da", "Dansk"},     {"fin fi", "Finsk"},
                              {"ger deu de", "Tysk"},      {"fre fra fr", "Fransk"}, {"spa es", "Spansk"},
                              {"ita it", "Italiensk"},     {"jpn ja", "Japansk"},   {"kor ko", "Koreansk"},
                              {"chi zho zh", "Kinesisk"},  {"por pt", "Portugisisk"}, {"rus ru", "Russisk"},
                              {"dut nld nl", "Nederlandsk"}, {"pol pl", "Polsk"},   {"ice isl is", "Islandsk"}};
    if (code.empty() || code == "und")
        return "Ukjent språk";
    std::string lc = code;
    for (char &c : lc)
        c = (char)std::tolower((unsigned char)c);
    for (const L &l : names) {
        const std::string list = std::string(" ") + l.codes + " ";
        if (list.find(" " + lc + " ") != std::string::npos)
            return l.name;
    }
    const std::string n = nuvio_language_name(code);
    return n.empty() ? code : n;
}

void play_glyph(float x, float cy, float size, uint32_t c)
{
    const int n = (int)(size / 1.5f);
    for (int i = 0; i < n; i++) {
        const float h = size * 1.15f * (1.f - (float)i / n);
        gfx::fill({x + i * 1.5f, cy - h / 2, 1.6f, h}, c);
    }
}

void pause_glyph(float cx, float cy, float size, uint32_t c)
{
    gfx::fill({cx - size * 0.42f, cy - size / 2, size * 0.3f, size}, c, 3);
    gfx::fill({cx + size * 0.12f, cy - size / 2, size * 0.3f, size}, c, 3);
}

} // namespace

void PlayerUi::begin(const NuvioRequest *req, double now)
{
    *this = PlayerUi();
    m_req = req;
    m_now = m_last = m_load_since = now;
    a_loading.snap(1.f);
    m_dirty = true;
}

void PlayerUi::show_controls(double now)
{
    m_controls = true;
    m_hide_at = now + 4.0;
    m_dirty = true;
}

void PlayerUi::toast(const std::string &text, double now)
{
    m_toast = text;
    m_toast_until = now + 3.0;
    m_dirty = true;
}

float PlayerUi::subtitle_lift() const { return a_controls.value * 150.f; }

int PlayerUi::current_skip(const NuvioStatus &st) const
{
    if (!m_req || !st.started)
        return -1;
    for (size_t i = 0; i < m_req->skips.size() && i < 16; i++) {
        const NuvioSkip &k = m_req->skips[i];
        if (k.type == "outro" || k.type == "credits")
            continue;   /* the next-episode card covers the end */
        if (st.position >= k.start && st.position < k.end - 1.0 && !m_skip_done[i])
            return (int)i;
    }
    return -1;
}

bool PlayerUi::next_card(const NuvioStatus &st) const
{
    if (!m_req || !m_req->has_next || m_card_dismissed || !st.started || st.duration <= 0)
        return false;
    for (const NuvioSkip &k : m_req->skips)
        if ((k.type == "outro" || k.type == "credits") && st.position >= k.start && st.position < k.end)
            return true;
    const NuvioPrefs &p = m_req->prefs;
    const double left = st.duration - st.position;
    return p.next_by_minutes ? left <= p.next_minutes * 60.0
                             : st.position / st.duration * 100.0 >= std::min(99.0, p.next_percent) ||
                                   left <= 45.0;
}

std::vector<PlayerUi::Tab> PlayerUi::tabs() const
{
    std::vector<Tab> t{TabInfo, TabAudio, TabSubs};
    if (m_req && m_req->episodes.size() > 1)
        t.push_back(TabEpisodes);
    return t;
}

void PlayerUi::seek_step(int dir, const NuvioStatus &st, double now)
{
    if (!m_seeking) {
        m_seeking = true;
        m_seek_target = st.position;
        m_seek_step = 10;
    } else if (now - m_seek_last_step < 0.35) {
        m_seek_step = std::min(120.f, m_seek_step * 1.35f);   /* held or tapped fast: faster */
    } else {
        m_seek_step = 10;
    }
    m_seek_last_step = now;
    m_seek_target = std::max(0.0, std::min(st.duration > 0 ? st.duration - 1 : 1e9, m_seek_target + dir * m_seek_step));
    m_seek_commit_at = now + 0.75;
    show_controls(now);
}

void PlayerUi::playback_ended(const NuvioStatus &, std::vector<OsdCommand> &out)
{
    if (m_req && m_req->has_next && m_req->prefs.autoplay_next)
        out.push_back({OsdCmd::PlayNext});
    else
        out.push_back({OsdCmd::Stop});
}

void PlayerUi::tick(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    m_now = st.now;
    if (!m_req)
        return;
    if (st.started && !m_shown_once) {
        m_shown_once = true;
        show_controls(st.now);   /* the controls show for a moment as the picture appears */
        m_hide_at = st.now + 2.5;
    }

    /* Automatic intro skipping (Innstillinger). */
    if (m_req->prefs.auto_skip && !m_seeking) {
        const int k = current_skip(st);
        if (k >= 0) {
            m_skip_done[k] = true;
            out.push_back({OsdCmd::SeekTo, m_req->skips[k].end});
        }
    }
    if (m_seeking && st.now >= m_seek_commit_at) {
        out.push_back({OsdCmd::SeekTo, m_seek_target});
        m_seeking = false;
        m_hide_at = st.now + 2.0;
    }
    if (m_controls && !st.paused && !m_panel && !m_seeking && st.now >= m_hide_at) {
        m_controls = false;
        m_dirty = true;
    }

    /* The next-episode countdown (10 s) when autoplay is on. */
    if (next_card(st)) {
        if (m_card_since < 0)
            m_card_since = st.now;
        if (m_req->prefs.autoplay_next && st.now - m_card_since >= 10.0 && !st.paused) {
            m_card_dismissed = true;
            out.push_back({OsdCmd::PlayNext});
        }
    } else {
        m_card_since = -1;
    }
}

void PlayerUi::panel_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const std::vector<Tab> ts = tabs();
    m_tab = std::min(m_tab, (int)ts.size() - 1);
    const Tab tab = ts[m_tab];
    int rows = 0;
    if (tab == TabAudio)
        rows = (int)st.audio.size();
    else if (tab == TabSubs)
        rows = 1 + nuvio_subs_count() + 1 + (m_advanced ? 5 : 0);   /* off, tracks, Avansert, style */
    else if (tab == TabEpisodes)
        rows = (int)m_req->episodes.size();
    m_dirty = true;

    if (m_row < 0) {
        if (p & NUVIO_BTN_LEFT)
            m_tab = std::max(0, m_tab - 1), m_advanced = false;
        else if (p & NUVIO_BTN_RIGHT)
            m_tab = std::min((int)ts.size() - 1, m_tab + 1), m_advanced = false;
        else if ((p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) && rows > 0)
            m_row = 0, m_scroll = 0;
        else if (p & (NUVIO_BTN_UP | NUVIO_BTN_CIRCLE))
            m_panel = false;
        return;
    }
    if (p & NUVIO_BTN_UP) {
        m_row--;
        return;
    }
    if (p & NUVIO_BTN_DOWN) {
        m_row = std::min(rows - 1, m_row + 1);
        return;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        if (m_advanced && tab == TabSubs && m_row > nuvio_subs_count() + 1) {
            m_advanced = false;
            m_row = nuvio_subs_count() + 1;
        } else {
            m_row = -1;
        }
        return;
    }
    const int dir = (p & NUVIO_BTN_RIGHT) ? 1 : (p & NUVIO_BTN_LEFT) ? -1 : 0;
    const bool cross = (p & NUVIO_BTN_CROSS) != 0;
    if (tab == TabAudio && cross && m_row < (int)st.audio.size()) {
        out.push_back({OsdCmd::SelectAudio, 0, m_row});
    } else if (tab == TabEpisodes && cross && m_row < (int)m_req->episodes.size()) {
        const NuvioEpisode &e = m_req->episodes[m_row];
        OsdCommand c{OsdCmd::PlayEpisode};
        c.season = e.season;
        c.episode = e.episode;
        out.push_back(c);
    } else if (tab == TabSubs) {
        const int n = nuvio_subs_count();
        if (m_row == 0 && cross) {
            out.push_back({OsdCmd::SelectSubtitle, 0, -1});
        } else if (m_row <= n && cross) {
            out.push_back({OsdCmd::SelectSubtitle, 0, m_row - 1});
        } else if (m_row == n + 1 && cross) {
            m_advanced = !m_advanced;
        } else if (m_row > n + 1 && (dir || cross)) {
            const int d = dir ? dir : 1;
            nuvio_sub_style s;
            nuvio_subs_get_style(&s);
            switch (m_row - n - 2) {
            case 0: {
                const int ms = std::max(-30000, std::min(30000, nuvio_subs_delay_ms() + d * 100));
                out.push_back({OsdCmd::SubtitleDelay, (double)ms});
                return;
            }
            case 1: s.size_pct = std::max(50, std::min(200, s.size_pct + d * 10)); break;
            case 2: s.offset_pct = std::max(0.f, std::min(40.f, s.offset_pct + d * 2.f)); break;
            case 3: {
                const float steps[] = {0.f, 0.25f, 0.5f, 0.75f};
                int i = 0;
                for (int k = 0; k < 4; k++)
                    if (std::fabs(s.background - steps[k]) < 0.05f)
                        i = k;
                s.background = steps[(i + d + 4) % 4];
                break;
            }
            case 4: s.outline = !s.outline; break;
            }
            nuvio_subs_set_style(&s);
            out.push_back({OsdCmd::SubtitleStyle});
        }
    }
}

void PlayerUi::input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const uint32_t p = in.pressed;
    if (!p || !m_req)
        return;
    m_dirty = true;
    const double now = st.now;

    if (!st.error.empty()) {
        if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE))
            out.push_back({OsdCmd::Stop});
        return;
    }
    if (m_panel) {
        panel_input(p, st, out);
        show_controls(now);
        return;
    }
    const int skip = current_skip(st);
    if (p & NUVIO_BTN_CROSS) {
        if (m_seeking) {
            out.push_back({OsdCmd::SeekTo, m_seek_target});
            m_seeking = false;
        } else if (skip >= 0) {
            m_skip_done[skip] = true;
            out.push_back({OsdCmd::SeekTo, m_req->skips[skip].end});
        } else if (next_card(st)) {
            m_card_dismissed = true;
            out.push_back({OsdCmd::PlayNext});
        } else {
            out.push_back({OsdCmd::TogglePause});
            m_flash_icon = st.paused ? "play" : "pause";
            a_flash.snap(1.f);
            show_controls(now);
        }
        return;
    }
    if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
        seek_step((p & NUVIO_BTN_RIGHT) ? 1 : -1, st, now);
        return;
    }
    if (p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) {   /* quick jumps: -10 s / +30 s */
        const double to = st.position + ((p & NUVIO_BTN_R1) ? 30.0 : -10.0);
        out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(st.duration - 1, to))});
        show_controls(now);
        return;
    }
    if (p & NUVIO_BTN_DOWN) {
        m_panel = true;
        m_row = -1;
        m_advanced = false;
        show_controls(now);
        return;
    }
    if (p & NUVIO_BTN_SQUARE) {   /* straight to the subtitles */
        m_panel = true;
        const std::vector<Tab> ts = tabs();
        m_tab = (int)(std::find(ts.begin(), ts.end(), TabSubs) - ts.begin());
        m_row = 0;
        show_controls(now);
        return;
    }
    if (p & (NUVIO_BTN_UP | NUVIO_BTN_OPTIONS | NUVIO_BTN_TOUCHPAD)) {
        show_controls(now);
        return;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        if (m_seeking) {
            m_seeking = false;                 /* cancel the scrub */
        } else if (next_card(st)) {
            m_card_dismissed = true;           /* watch the credits */
        } else if (m_controls && !st.paused) {
            m_controls = false;                /* first Back hides, as on Apple TV */
        } else {
            out.push_back({OsdCmd::Stop});
        }
    }
}

bool PlayerUi::wants_frame(const NuvioStatus &st)
{
    if (m_dirty)
        return true;
    const bool moving = a_controls.value != a_controls.target || a_loading.value != a_loading.target ||
                        a_panel.value != a_panel.target || a_skip.value != a_skip.target ||
                        a_next.value != a_next.target || a_spinner.value != a_spinner.target ||
                        a_toast.value != a_toast.target || a_error.value != a_error.target ||
                        a_flash.value > 0.f;
    /* Paused with the controls up, the clock and countdowns still tick. */
    static double last_second = 0;
    const bool second = std::floor(st.now) != std::floor(last_second);
    last_second = st.now;
    return moving || m_seeking || (second && (m_controls || m_card_since >= 0)) || a_loading.value > 0.f ||
           st.buffering;
}

/* ---- drawing ---------------------------------------------------------------------- */

void PlayerUi::draw_loading(const NuvioStatus &st)
{
    const float a = smoothstep(a_loading.value);
    if (a <= 0.f)
        return;
    gfx::push_opacity(a);
    const gfx::Rect full{0, 0, W, H};
    gfx::fill(full, 0xff080b10u);
    if (const gfx::Texture *t = art::get(m_req->backdrop, 1920, 1080))
        gfx::image(full, t, 0.92f, 0, true);
    gfx::fill_vgradient({0, 0, W, 378}, 0x4d000000u, 0x99000000u);
    gfx::fill_vgradient({0, 378, W, 378}, 0x99000000u, 0xcc000000u);
    gfx::fill_vgradient({0, 756, W, 324}, 0xcc000000u, 0xe6000000u);

    /* The logo fades in after 400 ms, breathes, then fills as the stream opens. */
    const float t = (float)(st.now - m_load_since);
    float ia = t < 0.4f ? 0.f : std::min(1.f, (t - 0.4f) / 0.7f);
    const float progress = std::max(0.f, std::min(1.f, st.open_progress));
    if (progress <= 0.f)
        ia *= 0.75f + 0.25f * (0.5f + 0.5f * std::cos((t - 0.4f) * 3.14159f));
    if (const gfx::Texture *logo = art::get(m_req->logo, 800, 300)) {
        const float iw = (float)gfx::texture_width(logo), ih = (float)gfx::texture_height(logo);
        const float k = std::min(640.f / iw, 230.f / ih);
        const gfx::Rect r{W / 2 - iw * k / 2, H / 2 - 150 + (230 - ih * k) / 2, iw * k, ih * k};
        if (progress > 0.f) {
            gfx::image(r, logo, ia * 0.25f, 0, false);
            gfx::push_scissor({r.x, r.y, r.w * progress, r.h});
            gfx::image(r, logo, ia, 0, false);
            gfx::pop_scissor();
        } else {
            gfx::image(r, logo, ia, 0, false);
        }
    } else if (m_req->logo.empty()) {
        gfx::text(W / 2, H / 2 - 40, m_req->header_title(), {gfx::Bold, 48, 1500}, alpha(kText, ia), 1);
        gfx::text(W / 2, H / 2 + 14, m_req->header_subtitle(), {gfx::Medium, 30, 1500}, alpha(kText2, ia), 1);
    }
    gfx::pop_opacity();
}

void PlayerUi::draw_bar(const NuvioStatus &st, float a)
{
    const float x0 = kPad, w = W - 2 * kPad;
    const double d = st.duration > 0 ? st.duration : 1;
    const double pos = m_seeking ? m_seek_target : st.position;
    const float h = m_seeking ? 14.f : kBarH;
    const float y = kBarY - h / 2;
    gfx::fill({x0, y, w, h}, alpha(0x38ffffffu, a), h / 2);
    gfx::fill({x0, y, w * (float)std::min(1.0, st.buffered / d), h}, alpha(0x47ffffffu, a), h / 2);
    for (const NuvioSkip &k : m_req->skips) {   /* intro, recap, credits */
        const float sx = x0 + w * (float)(k.start / d), ex = x0 + w * (float)(std::min(k.end, d) / d);
        gfx::fill({sx, y, std::max(2.f, ex - sx), h}, alpha(0x8c00a4dcu, a), 0);
    }
    const float px = x0 + w * (float)std::min(1.0, pos / d);
    gfx::fill({x0, y, px - x0, h}, alpha(0xffffffffu, a), h / 2);
    const float hd = m_seeking ? 34.f : 26.f;
    gfx::shadow({px - hd / 2, kBarY - hd / 2, hd, hd}, hd / 2, 10, 0.5f * a, 2);
    gfx::fill({px - hd / 2, kBarY - hd / 2, hd, hd}, alpha(0xffffffffu, a), hd / 2);

    /* Elapsed · ends at · remaining (concept .times). */
    const gfx::TextStyle ts{gfx::SemiBold, 24};
    gfx::text(x0, H - 62, fmt_time(pos), ts, alpha(kText2, a));
    gfx::text(x0 + w, H - 62, "\xE2\x88\x92" + fmt_time(d - pos), ts, alpha(kText2, a), 2);
    if (st.duration > 0)
        gfx::text(W / 2, H - 62, "Slutter kl. " + clock_at(d - pos), {gfx::Medium, 24}, alpha(kText3, a), 1);

    /* The scrub bubble: time (and a trickplay frame when the server has them). */
    if (m_seeking) {
        const float bx = std::max(x0 + 80, std::min(x0 + w - 80, px));
        const std::string t = fmt_time(pos);
        const gfx::TextStyle bs{gfx::Bold, 28};
        const float tw = gfx::text_width(t, bs) + 32;
        const gfx::Rect r{bx - tw / 2, kBarY - 80, tw, 50};
        gfx::fill(r, alpha(0xd9000000u, a), 12);
        gfx::text(bx, r.y + 36, t, bs, alpha(kText, a), 1);
    }
}

void PlayerUi::draw_controls(const NuvioStatus &st)
{
    const float a = smoothstep(a_controls.value);
    if (a <= 0.f)
        return;
    gfx::fill_vgradient({0, 0, W, 260}, alpha(0xb3000000u, a), 0x00000000u);
    gfx::fill_vgradient({0, H - 340, W, 340}, 0x00000000u, alpha(0xd9000000u, a));

    /* Top: the logo (or the title) and the episode line; the clock. */
    float sub_y = 160;
    if (const gfx::Texture *logo = m_req->logo.empty() ? nullptr : art::get(m_req->logo, 800, 300)) {
        const float iw = (float)gfx::texture_width(logo), ih = (float)gfx::texture_height(logo);
        const float k = std::min(420.f / iw, 110.f / ih);
        gfx::image({kPad, 70 + (110 - ih * k), iw * k, ih * k}, logo, a, 0, false);
        sub_y = 222;
    } else {
        gfx::text(kPad, 118, m_req->header_title(), {gfx::Bold, 44, 1100}, alpha(kText, a));
    }
    std::string sub;
    if (m_req->season > 0 && m_req->episode > 0) {
        char b[64];
        std::snprintf(b, sizeof b, "S%d:E%d", m_req->season, m_req->episode);
        sub = b;
        if (!m_req->episode_title.empty())
            sub += " \xC2\xB7 " + m_req->episode_title;
    } else {
        sub = m_req->year;
        if (!m_req->runtime.empty())
            sub += (sub.empty() ? "" : " \xC2\xB7 ") + m_req->runtime;
    }
    gfx::text(kPad, sub_y, sub, {gfx::Medium, 25, 1100}, alpha(kText2, a));
    if (m_req->prefs.show_clock)
        gfx::text(W - kPad, 108, clock_at(0), {gfx::SemiBold, 28}, alpha(kText2, a), 2);

    /* Play state, left of the bar's start; hints on the right. */
    if (st.paused)
        play_glyph(kPad, kBarY - 58, 22, alpha(kText, a));
    gfx::text(W - kPad, kBarY - 44, "\xE2\x86\x93 Info, lyd og undertekster", {gfx::Medium, 20}, alpha(kText3, a), 2);
    draw_bar(st, a);
}

void PlayerUi::draw_skip_next(const NuvioStatus &st)
{
    /* Skip intro / recap: a white pill, Cross acts (concept .skip.focus). */
    const int k = current_skip(st);
    a_skip.to(k >= 0 ? 1.f : 0.f);
    if (a_skip.value > 0.01f) {
        static std::string label = "Hopp over intro";
        if (k >= 0)
            label = m_req->skips[k].type == "recap" ? "Hopp over oppsummering"
                    : m_req->skips[k].type == "preview" ? "Hopp over forhåndsvisning" : "Hopp over intro";
        const gfx::TextStyle st2{gfx::Bold, 26};
        const float w = gfx::text_width(label, st2) + 72;
        const float y = H - 200 - (1.f - a_skip.value) * 20 - (m_controls ? 30 : 0);
        const gfx::Rect r{W - kPad - w, y, w, 72};
        gfx::push_opacity(a_skip.value);
        gfx::shadow(r, 14, 24, 0.5f, 10);
        gfx::fill(r, 0xfff5f5f7u, 14);
        gfx::text(r.x + r.w / 2, r.y + 46, label, st2, 0xff0b0b0fu, 1);
        gfx::pop_opacity();
    }

    /* Next episode: a glass card with the still, the title and the countdown. */
    const bool card = next_card(st);
    a_next.to(card ? 1.f : 0.f);
    if (a_next.value > 0.01f && m_req->has_next) {
        const NuvioEpisode &n = m_req->next;
        const gfx::Rect r{W - kPad - 560, H - 230 - (1.f - a_next.value) * 20 - (m_controls ? 40 : 0), 560, 156};
        gfx::push_opacity(a_next.value);
        gfx::shadow(r, 20, 30, 0.6f, 12);
        gfx::fill(r, kGlass, 20);
        art::draw({r.x + 18, r.y + 18, 213, 120}, n.thumbnail, "", 480, 270, 10);
        const float tx = r.x + 250;
        gfx::text(tx, r.y + 44, "NESTE EPISODE", {gfx::Bold, 17}, kText3);
        char title[256];
        std::snprintf(title, sizeof title, "S%d:E%d \xC2\xB7 %s", n.season, n.episode, n.title.c_str());
        gfx::text(tx, r.y + 80, title, {gfx::Bold, 24, r.w - 270}, kText);
        if (m_req->prefs.autoplay_next && m_card_since >= 0) {
            const double left = std::max(0.0, 10.0 - (st.now - m_card_since));
            char c[48];
            std::snprintf(c, sizeof c, "Spilles om %d s  \xC2\xB7  \xE2\x9C\x95 n\xC3\xA5", (int)std::ceil(left));
            gfx::text(tx, r.y + 116, c, {gfx::Medium, 20}, kText2);
            gfx::fill({tx, r.y + 132, r.w - 270, 4}, 0x33ffffffu, 2);
            gfx::fill({tx, r.y + 132, (r.w - 270) * (float)(1.0 - left / 10.0), 4}, kAccent, 2);
        } else {
            gfx::text(tx, r.y + 116, "\xE2\x9C\x95 spill av  \xC2\xB7  \xE2\x97\x8B se rulletekst", {gfx::Medium, 20}, kText2);
        }
        gfx::pop_opacity();
    }
}

void PlayerUi::draw_panel(const NuvioStatus &st)
{
    a_panel.to(m_panel ? 1.f : 0.f);
    const float a = smoothstep(a_panel.value);
    if (a <= 0.01f)
        return;
    const std::vector<Tab> ts = tabs();
    const int tab_i = std::min(m_tab, (int)ts.size() - 1);
    const Tab tab = ts[tab_i];
    const float pw = 1100, ph = 560;
    const gfx::Rect r{W / 2 - pw / 2, 60 - (1.f - a) * (ph + 80), pw, ph};
    gfx::push_opacity(a);
    gfx::fill({0, 0, W, H}, 0x59000000u);   /* the picture dims behind the panel */
    gfx::shadow(r, 28, 40, 0.7f, 16);
    gfx::fill(r, kGlass, 28);

    /* Tabs. */
    static const char *const names[] = {"Info", "Lyd", "Undertekster", "Episoder"};
    float x = r.x + 40;
    for (size_t i = 0; i < ts.size(); i++) {
        const gfx::TextStyle st2{gfx::SemiBold, 24};
        const float w = gfx::text_width(names[ts[i]], st2) + 52;
        const bool active = (int)i == tab_i, focus = active && m_row < 0;
        gfx::fill({x, r.y + 34, w, 54}, focus ? 0xfff5f5f7u : active ? 0x33ffffffu : 0x14ffffffu, 27);
        gfx::text(x + w / 2, r.y + 69, names[ts[i]], st2, focus ? 0xff0b0b0fu : active ? kText : kText2, 1);
        x += w + 10;
    }

    const float lx = r.x + 40, lw = pw - 80, ly = r.y + 120, row_h = 58;
    const int visible = 7;
    if (m_row >= 0) {
        if (m_row < m_scroll)
            m_scroll = m_row;
        if (m_row >= m_scroll + visible)
            m_scroll = m_row - visible + 1;
    } else {
        m_scroll = 0;
    }
    auto row = [&](int i, const std::string &label, const std::string &right, bool selected, bool dim = false) {
        if (i < m_scroll || i >= m_scroll + visible)
            return;
        const float y = ly + (i - m_scroll) * (row_h + 4);
        const bool focus = i == m_row;
        if (focus)
            gfx::fill({lx, y, lw, row_h}, 0xfff5f5f7u, 12);
        const uint32_t fg = focus ? 0xff0b0b0fu : dim ? kText3 : kText2;
        gfx::text(lx + 22, y + 38, label, {selected ? gfx::Bold : gfx::Medium, 24, lw - 260}, fg);
        if (!right.empty())
            gfx::text(lx + lw - 22 - (selected ? 34 : 0), y + 38, right, {gfx::Medium, 21}, focus ? 0x990b0b0fu : kText3, 2);
        if (selected)
            gfx::text(lx + lw - 22, y + 38, "\xE2\x9C\x93", {gfx::Bold, 24}, focus ? 0xff0b0b0fu : kText, 2);
    };

    if (tab == TabInfo) {
        const std::string title = m_req->header_title() +
                                  (m_req->header_subtitle().empty() ? "" : "  \xC2\xB7  " + m_req->header_subtitle());
        gfx::text(lx, ly + 30, title, {gfx::Bold, 28, lw}, kText);
        gfx::text(lx, ly + 76, m_req->description, {gfx::Regular, 22, lw, 4, 32}, kText2);
        gfx::text(lx, ly + 240, "Avspilling", {gfx::SemiBold, 20}, kText3);
        gfx::text(lx, ly + 272, m_req->stream_title, {gfx::Medium, 22, lw}, kText);
        if (!st.quality_line.empty())
            gfx::text(lx, ly + 306, st.quality_line, {gfx::Medium, 22, lw}, kText2);
    } else if (tab == TabAudio) {
        if (st.audio.empty())
            gfx::text(lx, ly + 40, "Ingen andre lydspor", {gfx::Medium, 24}, kText3);
        for (size_t i = 0; i < st.audio.size(); i++) {
            const NuvioAudioTrack &t = st.audio[i];
            std::string label = language_name(t.lang);
            if (!t.title.empty() && t.title != t.codec)
                label += "  \xC2\xB7  " + t.title;
            row((int)i, label, t.codec + (t.channels.empty() ? "" : " " + t.channels), (int)i == st.audio_active);
        }
    } else if (tab == TabSubs) {
        const int n = nuvio_subs_count(), sel = nuvio_subs_selected();
        row(0, "Av", "", sel < 0);
        for (int i = 0; i < n; i++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(i, &t) != 0)
                continue;
            std::string label = language_name(t.lang);
            std::string right;
            if (t.forced) right += "Tvungen  ";
            if (t.hearing_impaired) right += "SDH  ";
            if (t.bitmap) right += "Bilde  ";
            if (t.external) right += "Ekstern  ";
            if (t.title[0] && std::string(t.title) != label)
                label += "  \xC2\xB7  " + std::string(t.title);
            row(i + 1, label, right, i == sel, t.state < 0);
        }
        row(n + 1, m_advanced ? "Avansert  \xE2\x96\xB4" : "Avansert  \xE2\x96\xBE", "Stil og timing", false);
        if (m_advanced) {
            nuvio_sub_style s;
            nuvio_subs_get_style(&s);
            char v[64];
            std::snprintf(v, sizeof v, "\xE2\x80\xB9  %+.1f s  \xE2\x80\xBA", nuvio_subs_delay_ms() / 1000.0);
            row(n + 2, "Forsinkelse", v, false);
            std::snprintf(v, sizeof v, "\xE2\x80\xB9  %d %%  \xE2\x80\xBA", s.size_pct);
            row(n + 3, "Størrelse", v, false);
            std::snprintf(v, sizeof v, "\xE2\x80\xB9  %.0f %%  \xE2\x80\xBA", s.offset_pct);
            row(n + 4, "Posisjon", v, false);
            std::snprintf(v, sizeof v, "\xE2\x80\xB9  %s  \xE2\x80\xBA", s.background < 0.05f ? "Av" : (std::to_string((int)(s.background * 100)) + " %").c_str());
            row(n + 5, "Bakgrunn", v, false);
            row(n + 6, "Kontur", s.outline ? "På" : "Av", false);
        }
    } else if (tab == TabEpisodes) {
        for (size_t i = 0; i < m_req->episodes.size(); i++) {
            const NuvioEpisode &e = m_req->episodes[i];
            char label[300];
            std::snprintf(label, sizeof label, "%d. %s", e.episode, e.title.c_str());
            const bool here = e.season == m_req->season && e.episode == m_req->episode;
            row((int)i, label, e.watched ? "Sett" : "", here);
        }
    }
    gfx::pop_opacity();
}

void PlayerUi::draw_error(const NuvioStatus &st)
{
    a_error.to(st.error.empty() ? 0.f : 1.f);
    if (a_error.value <= 0.01f)
        return;
    gfx::push_opacity(a_error.value);
    gfx::fill({0, 0, W, H}, 0xe6080b10u);
    gfx::text(W / 2, 470, "Kunne ikke spille av", {gfx::Bold, 52}, kText, 1);
    gfx::text(W / 2, 530, st.error, {gfx::Medium, 26, 1300, 2, 36}, kText2, 1);
    const gfx::Rect b{W / 2 - 130, 620, 260, 76};
    gfx::fill(b, 0xfff5f5f7u, 16);
    gfx::text(W / 2, 668, "Tilbake", {gfx::Bold, 26}, 0xff0b0b0fu, 1);
    gfx::pop_opacity();
}

void PlayerUi::draw(const NuvioStatus &st)
{
    if (!m_req)
        return;
    const float dt = (float)std::min(0.1, std::max(0.0, st.now - m_last));
    m_last = st.now;
    m_dirty = false;

    a_loading.to(st.started || !st.error.empty() ? 0.f : 1.f);
    a_loading.step(dt, 8.f);
    a_controls.to(m_controls || m_seeking || m_panel || st.paused ? 1.f : 0.f);
    a_controls.step(dt, 12.f);
    a_panel.step(dt, 12.f);
    a_skip.step(dt, 12.f);
    a_next.step(dt, 10.f);
    a_error.step(dt, 10.f);
    a_spinner.to(st.started && st.buffering ? 1.f : 0.f);
    a_spinner.step(dt, 10.f);
    a_toast.to(st.now < m_toast_until ? 1.f : 0.f);
    a_toast.step(dt, 10.f);
    a_flash.to(0.f);
    a_flash.step(dt, 4.f);

    draw_loading(st);
    draw_controls(st);
    draw_skip_next(st);

    /* Play / pause flash in the centre (concept .center-ico). */
    if (a_flash.value > 0.01f) {
        const float k = 0.85f + 0.15f * a_flash.value, d = 140 * k;
        gfx::push_opacity(a_flash.value);
        gfx::fill({W / 2 - d / 2, H / 2 - d / 2, d, d}, 0x8c000000u, d / 2);
        if (m_flash_icon == "pause")
            pause_glyph(W / 2, H / 2, 54 * k, kText);
        else
            play_glyph(W / 2 - 18 * k, H / 2, 54 * k, kText);
        gfx::pop_opacity();
    }

    /* Buffering: twelve dots chasing round (the Apple TV spinner). */
    if (a_spinner.value > 0.01f) {
        gfx::push_opacity(a_spinner.value);
        for (int i = 0; i < 12; i++) {
            const float ang = (float)i / 12.f * 6.2832f;
            const float phase = std::fmod((float)st.now * 1.2f + 1.f - (float)i / 12.f, 1.f);
            gfx::fill({W / 2 + std::cos(ang) * 34 - 5, H / 2 + std::sin(ang) * 34 - 5, 10, 10},
                      alpha(kText, 0.2f + 0.8f * (1.f - phase)), 5);
        }
        gfx::pop_opacity();
    }

    draw_panel(st);

    if (a_toast.value > 0.01f && !m_toast.empty()) {
        const gfx::TextStyle ts{gfx::SemiBold, 22};
        const float w = gfx::text_width(m_toast, ts) + 60;
        const gfx::Rect r{W / 2 - w / 2, 50 - (1.f - a_toast.value) * 20, w, 58};
        gfx::push_opacity(a_toast.value);
        gfx::fill(r, kGlass, 29);
        gfx::text(W / 2, r.y + 38, m_toast, ts, kText, 1);
        gfx::pop_opacity();
    }
    draw_error(st);
}

} // namespace ui
