/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Netflix TV layout in the app's look; colours from concept/style.css.
 */
#include "ui/player_ui.h"
#include "evo_audio_out.h"
#include "jelly5_bitstream.h"

#include "app/remote.h"
#include "app/settings.h"
#include "app/syncplay.h"
#include "app/i18n.h"
#include "jelly5_playback.h"
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
constexpr float kBarY = H - 205;
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

/* Language names in the interface's language: Norwegian from the list below,
 * English (and anything not listed) from the player's own names. */
std::string language_name(const std::string &code)
{
    if (i18n::english()) {
        const std::string n = nuvio_language_name(code);
        return n.empty() ? T("Ukjent språk") : n;
    }
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
        return T("Ukjent språk");
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
    m_music = req && req->item_type == "audio";
    m_controls = m_music;   /* the music screen is all controls, always up */
    m_now = m_last = m_load_since = now;
    /* Video opens on the dark loading veil; music never does: its screen (the
     * cover, the controls) is up from the first frame, so going from one track
     * to the next only changes what is on it. */
    a_loading.snap(m_music ? 0.f : 1.f);
    if (m_music)
        a_controls.snap(1.f);
    m_dirty = true;
}

void PlayerUi::show_controls(double now, Zone zone)
{
    if (!m_controls) {
        m_zone = zone;
        m_button = 0;
    }
    m_controls = true;
    m_hide_at = now + 5.0;
    m_dirty = true;
}

void PlayerUi::toast(const std::string &text, double now)
{
    m_toast = text;
    m_toast_until = now + 3.0;
    m_dirty = true;
}

float PlayerUi::subtitle_lift() const { return a_controls.value * 210.f; }

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
    if (!m_req || m_music || !m_req->has_next || m_card_dismissed || !st.started || st.duration <= 0)
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

/* The speed button's label: "1×", "1.25×", ... (the current playback speed). */
static std::string speed_label()
{
    const float sp = evo_audio_speed();
    char b[16];
    if (std::fabs(sp - std::round(sp)) < 0.01f)
        std::snprintf(b, sizeof b, "%d\xC3\x97", (int)std::round(sp));
    else
        std::snprintf(b, sizeof b, "%g\xC3\x97", (double)sp);
    return b;
}

std::vector<PlayerUi::Button> PlayerUi::buttons() const
{
    std::vector<Button> b{Button::PlayPause};
    if (m_req && m_req->episodes.size() > 1)
        b.push_back(Button::Episodes);
    if (m_req && m_req->chapters.size() > 1)
        b.push_back(Button::Chapters);
    b.push_back(Button::Tracks);
    b.push_back(Button::Speed);
    if (m_req && m_req->has_next)
        b.push_back(Button::Next);
    return b;
}

std::vector<int> PlayerUi::seasons() const
{
    std::vector<int> out;
    for (const NuvioEpisode &e : m_req->episodes)
        if (std::find(out.begin(), out.end(), e.season) == out.end())
            out.push_back(e.season);
    std::sort(out.begin(), out.end(), [](int a, int b) { return (a == 0) != (b == 0) ? a != 0 : a < b; });
    return out;   /* specials (season 0) last */
}

std::vector<int> PlayerUi::episodes_in(int season) const
{
    std::vector<int> out;
    for (size_t i = 0; i < m_req->episodes.size(); i++)
        if (m_req->episodes[i].season == season)
            out.push_back((int)i);
    return out;
}

void PlayerUi::open_overlay(Overlay o)
{
    m_overlay = m_overlay_drawn = o;
    m_dirty = true;
    if (o == Overlay::Tracks) {
        m_col = 0;
        m_style_open = false;
        m_rows[0] = m_rows[1] = m_rows[2] = 0;
        /* Start on the subtitles when there is anything to choose there. */
        if (nuvio_subs_count() > 0)
            m_col = 1;
        m_rows[1] = nuvio_subs_selected() + 1;
    } else if (o == Overlay::Episodes) {
        m_ep_col = 1;
        m_ep_season = m_req->season;
        const std::vector<int> eps = episodes_in(m_ep_season);
        m_ep_index = 0;
        for (size_t i = 0; i < eps.size(); i++)
            if (m_req->episodes[eps[i]].episode == m_req->episode)
                m_ep_index = (int)i;
        m_ep_scroll.snap(std::max(0.f, (m_ep_index - 1) * 178.f));
        m_ep_season_scroll.snap(-1);   /* placed, not slid, on the first draw */
    }
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
    m_seek_target =
        std::max(0.0, std::min(st.duration > 0 ? st.duration - 1 : 1e9, m_seek_target + dir * m_seek_step));
    m_seek_commit_at = now + 0.75;
    show_controls(now, Zone::Bar);
    m_zone = Zone::Bar;
}

void PlayerUi::playback_ended(const NuvioStatus &, std::vector<OsdCommand> &out)
{
    /* An album always plays on; episodes follow the autoplay setting. */
    if (m_req && m_req->has_next && (m_req->prefs.autoplay_next || m_music))
        out.push_back({OsdCmd::PlayNext});
    else
        out.push_back({OsdCmd::Stop});
}

void PlayerUi::tick(const NuvioStatus &st, std::vector<OsdCommand> &out, bool poll_remote)
{
    m_now = st.now;
    if (!m_req)
        return;
    if (poll_remote)
        remote_poll(st, out);
    int track = -1;
    switch (jelly5_subs::download_state(&track)) {
    case jelly5_subs::Done:
        out.push_back({OsdCmd::SelectSubtitle, 0, track});
        toast(T("Undertekst lagt til"), st.now);
        break;
    case jelly5_subs::Failed: toast(T("Kunne ikke hente underteksten"), st.now); break;
    default: break;
    }
    if (st.started && !m_shown_once) {
        m_shown_once = true;
        show_controls(st.now, Zone::Buttons);   /* a moment as the picture appears */
        m_hide_at = st.now + 3.0;
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
        m_hide_at = st.now + 2.5;
    }
    if (m_controls && !m_music && !st.paused && m_overlay == Overlay::None && !m_seeking && st.now >= m_hide_at) {
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

/* Lyd og undertekster: columns 0 audio, 1 subtitles (+ "Tilpass"), 2 style. */
void PlayerUi::tracks_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const int na = (int)st.audio.size(), ns = nuvio_subs_count();
    const int nv = m_req->sources.size() > 1 ? (int)m_req->sources.size() : 0;   /* versions, under the audio */
    const bool find = jelly5_subs::available();
    const int rows[3] = {na + nv, ns + 2 + (find ? 1 : 0), 5};   /* subtitles: Av, tracks, Tilpass, Søk */
    int &r = m_rows[m_col];
    if (m_col == 2 && m_find_open && !(p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_LEFT)) ) {
        find_input(p);
        return;
    }
    if (m_col == 2 && m_find_open && (p & NUVIO_BTN_LEFT) && m_rows[2] == 0 && m_find_langs.size() > 1) {
        find_input(p);   /* Left on the language row changes language */
        return;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        if (m_col == 2) {
            m_style_open = m_find_open = false;
            m_col = 1;
        } else {
            m_overlay = Overlay::None;
        }
    } else if (p & NUVIO_BTN_UP) {
        r = std::max(0, r - 1);
    } else if (p & NUVIO_BTN_DOWN) {
        r = std::min(std::max(0, rows[m_col] - 1), r + 1);
    } else if (m_col == 2 && m_style_open && (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT | NUVIO_BTN_CROSS))) {
        const int d = (p & NUVIO_BTN_LEFT) ? -1 : 1;
        nuvio_sub_style s;
        nuvio_subs_get_style(&s);
        switch (r) {
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
        /* Kept for the next time (and shown in Innstillinger). */
        settings::Local l = settings::get().local;
        l.sub_size = s.size_pct;
        l.sub_offset = s.offset_pct;
        l.sub_background = s.background;
        l.sub_outline = s.outline != 0;
        settings::set_local(l);
    } else if (p & NUVIO_BTN_LEFT) {
        if (m_col > 0)
            m_col--;
    } else if (p & NUVIO_BTN_RIGHT) {
        if (m_col == 0)
            m_col = 1;
        else if (m_col == 1 && (m_style_open || m_find_open))
            m_col = 2;
    } else if (p & NUVIO_BTN_CROSS) {
        if (m_col == 0 && r < na) {
            out.push_back({OsdCmd::SelectAudio, 0, r});
        } else if (m_col == 0 && r >= na && r - na < nv) {
            if (r - na != m_req->source_index) {   /* another version, from this moment */
                out.push_back({OsdCmd::SwitchSource, 0, r - na});
                m_overlay = Overlay::None;
            }
        } else if (m_col == 1) {
            if (r == ns + 1) {
                m_style_open = true;   /* "Tilpass undertekster" */
                m_find_open = false;
                m_col = 2;
                m_rows[2] = 0;
            } else if (r == ns + 2) {
                /* "Søk etter undertekster": the preferred language first, then English. */
                m_find_open = true;
                m_style_open = false;
                m_col = 2;
                m_rows[2] = 0;
                m_find_langs.clear();
                for (const std::string &l : m_req->prefs.subtitle_langs)
                    if (l.size() == 3 && m_find_langs.empty())
                        m_find_langs.push_back(l);
                if (m_find_langs.empty())
                    m_find_langs.push_back("nor");
                if (m_find_langs[0] != "eng")
                    m_find_langs.push_back("eng");
                m_find_lang = 0;
                jelly5_subs::search(m_find_langs[0]);
            } else {
                out.push_back({OsdCmd::SelectSubtitle, 0, r - 1});   /* row 0 = Av */
            }
        }
    }
}

/* The search column: row 0 the language (Left/Right), then the results (Cross fetches). */
void PlayerUi::find_input(uint32_t p)
{
    std::vector<jf::RemoteSubtitle> found;
    std::string lang;
    jelly5_subs::results(&found, &lang);
    int &r = m_rows[2];
    if (p & NUVIO_BTN_UP) {
        r = std::max(0, r - 1);
    } else if (p & NUVIO_BTN_DOWN) {
        r = std::min((int)found.size(), r + 1);
    } else if ((p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) && r == 0 && m_find_langs.size() > 1) {
        m_find_lang = (m_find_lang + ((p & NUVIO_BTN_RIGHT) ? 1 : -1) + (int)m_find_langs.size()) %
                      (int)m_find_langs.size();
        jelly5_subs::search(m_find_langs[m_find_lang]);
    } else if ((p & NUVIO_BTN_CROSS) && r >= 1 && r <= (int)found.size()) {
        jelly5_subs::download(found[r - 1]);
        toast(T("Henter undertekst \xE2\x80\xA6"), m_now);
    }
}

/* Episoder: column 0 the seasons, 1 the episodes of the season shown. */
void PlayerUi::episodes_input(uint32_t p, std::vector<OsdCommand> &out)
{
    const std::vector<int> ss = seasons();
    const std::vector<int> eps = episodes_in(m_ep_season);
    int si = (int)(std::find(ss.begin(), ss.end(), m_ep_season) - ss.begin());
    if (p & NUVIO_BTN_CIRCLE) {
        /* Back one level: the episodes, then the seasons, then out. */
        if (m_ep_col == 1) m_ep_col = 0;
        else m_overlay = Overlay::None;
    } else if (p & NUVIO_BTN_LEFT) {
        m_ep_col = 0;
    } else if (p & NUVIO_BTN_RIGHT) {
        m_ep_col = 1;
    } else if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
        const int d = (p & NUVIO_BTN_DOWN) ? 1 : -1;
        if (m_ep_col == 0) {
            si = std::max(0, std::min((int)ss.size() - 1, si + d));
            if (ss[si] != m_ep_season) {
                m_ep_season = ss[si];
                m_ep_index = 0;
                m_ep_scroll.snap(0);
            }
        } else {
            m_ep_index = std::max(0, std::min((int)eps.size() - 1, m_ep_index + d));
        }
    } else if (p & NUVIO_BTN_CROSS) {
        if (m_ep_col == 0) {
            m_ep_col = 1;
        } else if (m_ep_index < (int)eps.size()) {
            const NuvioEpisode &e = m_req->episodes[eps[m_ep_index]];
            OsdCommand c{OsdCmd::PlayEpisode};
            c.season = e.season;
            c.episode = e.episode;
            out.push_back(c);
            m_overlay = Overlay::None;
        }
    }
}

void PlayerUi::input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const size_t first = out.size();
    input_local(in, st, out);
    if (!syncplay::active())
        return;
    /* In a group: what the viewer did here is asked of the group instead. */
    for (size_t i = first; i < out.size();) {
        const OsdCommand &c = out[i];
        if (c.cmd == OsdCmd::TogglePause) {
            syncplay::request_pause(!st.paused, st.position);
        } else if (c.cmd == OsdCmd::SeekTo) {
            syncplay::request_seek(c.value);
        } else if (c.cmd == OsdCmd::PlayNext) {
            syncplay::request_next();
        } else {
            i++;
            continue;
        }
        out.erase(out.begin() + i);
    }
}

/* A tap on L2/R2 jumps 10 s (seek_step); held, the scrub runs on by itself, from
 * a couple of seconds per second with a light touch to five minutes per second with
 * the trigger pressed home - against the resistance the adaptive triggers give. The
 * speed builds up over the first 0.6 s of the hold, so it never starts with a jump. */
void PlayerUi::analog_scrub(const nuvio_input_state &in, const NuvioStatus &st)
{
    const double now = st.now;
    const double dt = m_trig_at > 0 ? std::min(0.1, now - m_trig_at) : 0.0;
    m_trig_at = now;
    if (in.pressed & ~in.repeats & (NUVIO_BTN_L2 | NUVIO_BTN_R2))
        m_trig_down_at = now;
    const bool l = (in.held & NUVIO_BTN_L2) != 0, r = (in.held & NUVIO_BTN_R2) != 0;
    if (!m_req || m_music || !m_seeking || l == r || !st.error.empty() || m_overlay != Overlay::None ||
        now - m_trig_down_at < 0.4)
        return;
    /* A gentle curve: most of the travel is for fine scrubbing, the last part for speed. */
    const float q = std::max(0.f, ((r ? in.r2 : in.l2) - 0.25f) / 0.75f);
    const double ramp = std::min(1.0, (now - m_trig_down_at - 0.4) / 0.6);
    const double rate = 2.0 + 298.0 * std::pow(q, 2.5) * ramp;   /* 2 s/s lightly, 5 min/s pressed home */
    m_seek_target = std::max(0.0, std::min(st.duration > 0 ? st.duration - 1 : 1e9, m_seek_target + (r ? rate : -rate) * dt));
    m_seek_commit_at = now + 0.75;
    m_seek_last_step = now;
    m_hide_at = now + 4.0;
    m_dirty = true;
}

void PlayerUi::input_local(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    analog_scrub(in, st);
    /* Video: a held L2/R2 is the analog scrub above, not repeated steps. */
    const uint32_t p = m_music ? in.pressed : in.pressed & ~(in.repeats & (NUVIO_BTN_L2 | NUVIO_BTN_R2));
    if (!p || !m_req)
        return;
    m_dirty = true;
    const double now = st.now;

    if (!st.error.empty()) {
        if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE))
            out.push_back({OsdCmd::Stop});
        return;
    }
    if ((p & NUVIO_BTN_L3) && !m_music) {   /* the playback info panel, on and off */
        m_stats = !m_stats;
        a_stats.to(m_stats ? 1.f : 0.f);
        return;
    }
    if (m_music) {
        music_input(p, st, out);
        return;
    }
    if (m_overlay == Overlay::Tracks) {
        tracks_input(p, st, out);
        return;
    }
    if (m_overlay == Overlay::Episodes) {
        episodes_input(p, out);
        return;
    }
    if (m_overlay == Overlay::Chapters) {   /* left/right through them, Cross jumps, Circle closes */
        const int n = (int)m_req->chapters.size();
        if (p & (NUVIO_BTN_UP | NUVIO_BTN_LEFT))
            m_chap = std::max(0, m_chap - 1);
        else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_RIGHT))
            m_chap = std::min(n - 1, m_chap + 1);
        else if ((p & NUVIO_BTN_CROSS) && m_chap < n) {
            out.push_back({OsdCmd::SeekTo, std::max(0.0, m_req->chapters[m_chap].start)});
            m_overlay = Overlay::None;
        } else if (p & NUVIO_BTN_CIRCLE)
            m_overlay = Overlay::None;
        return;
    }
    if (p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) {
        const double from = m_seeking ? m_seek_target : st.position;
        const bool forward = (p & NUVIO_BTN_R1) != 0;
        m_seeking = false;
        const std::vector<NuvioChapter> &ch = m_req->chapters;
        if (ch.size() > 1) {
            /* Chapters: R1 the next, L1 back to this one's start (or, in its first
             * 3 s, the one before); its name shows a moment. */
            int cur = 0;
            for (size_t i = 0; i < ch.size(); i++)
                if (ch[i].start <= from + 0.5)
                    cur = (int)i;
            int to = forward ? cur + 1 : (from - ch[cur].start > 3.0 ? cur : cur - 1);
            if (to >= (int)ch.size()) {
                /* already in the last chapter */
            } else {
                to = std::max(0, to);
                out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(st.duration - 1, ch[to].start))});
                if (!ch[to].name.empty())
                    toast(ch[to].name, now);
            }
        } else {   /* no chapters: quick jumps, as on Netflix, 10 s back / forward */
            const double to = from + (forward ? 10.0 : -10.0);
            out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(st.duration - 1, to))});
        }
        show_controls(now, Zone::Bar);
        return;
    }
    if (p & (NUVIO_BTN_L2 | NUVIO_BTN_R2)) {   /* rewind / fast forward: scrub, faster while held */
        seek_step((p & NUVIO_BTN_R2) ? 1 : -1, st, now);
        return;
    }
    if ((p & NUVIO_BTN_TRIANGLE) && !m_music) {   /* △: the episodes (a film: its chapters) */
        if (m_req->episodes.size() > 1) {
            open_overlay(Overlay::Episodes);
            return;
        }
        if (m_req->chapters.size() > 1) {
            m_chap = 0;
            for (size_t i = 0; i < m_req->chapters.size(); i++)
                if (m_req->chapters[i].start <= st.position + 0.5)
                    m_chap = (int)i;
            m_chap_scroll.snap((float)std::max(0, m_chap - 1) * 178.f);
            open_overlay(Overlay::Chapters);
            return;
        }
    }
    if (p & NUVIO_BTN_SQUARE) {
        open_overlay(Overlay::Tracks);
        return;
    }
    const int skip = current_skip(st);
    const bool on_bar = !m_controls || m_zone == Zone::Bar;

    if (p & NUVIO_BTN_CROSS) {
        if (m_seeking) {
            out.push_back({OsdCmd::SeekTo, m_seek_target});
            m_seeking = false;
        } else if (on_bar && skip >= 0) {
            m_skip_done[skip] = true;
            out.push_back({OsdCmd::SeekTo, m_req->skips[skip].end});
        } else if (on_bar && next_card(st)) {
            m_card_dismissed = true;
            out.push_back({OsdCmd::PlayNext});
        } else if (m_controls && m_zone == Zone::Buttons) {
            const std::vector<Button> bs = buttons();
            switch (bs[std::min(m_button, (int)bs.size() - 1)]) {
            case Button::PlayPause:
                out.push_back({OsdCmd::TogglePause});
                show_controls(now, Zone::Buttons);
                break;
            case Button::Episodes: open_overlay(Overlay::Episodes); break;
            case Button::Chapters: {   /* opens on the chapter playing now */
                m_chap = 0;
                for (size_t i = 0; i < m_req->chapters.size(); i++)
                    if (m_req->chapters[i].start <= st.position + 0.5)
                        m_chap = (int)i;
                m_chap_scroll.snap((float)std::max(0, m_chap - 1) * 178.f);
                open_overlay(Overlay::Chapters);
                break;
            }
            case Button::Tracks: open_overlay(Overlay::Tracks); break;
            case Button::Speed: {   /* 1x, 1.25x, 1.5x, 2x, 0.75x, round again */
                if (jelly5_bs_active()) {   /* the receiver decodes it: it plays as it is */
                    toast(T("Hastighet virker ikke med HDMI-bitstr\xC3\xB8m"), now);
                    break;
                }
                static const float speeds[] = {1.0f, 1.25f, 1.5f, 2.0f, 0.75f};
                const float now_sp = evo_audio_speed();
                int k = 0;
                for (int i = 0; i < 5; i++)
                    if (std::fabs(speeds[i] - now_sp) < 0.01f)
                        k = i;
                evo_audio_set_speed(speeds[(k + 1) % 5]);
                show_controls(now, Zone::Buttons);
                break;
            }
            case Button::Next: out.push_back({OsdCmd::PlayNext}); break;
            }
        } else {
            out.push_back({OsdCmd::TogglePause});
            m_flash_icon = st.paused ? "play" : "pause";
            a_flash.snap(1.f);
            show_controls(now, Zone::Bar);
        }
        return;
    }
    if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
        const int d = (p & NUVIO_BTN_RIGHT) ? 1 : -1;
        if (m_controls && m_zone == Zone::Buttons) {
            m_button = std::max(0, std::min((int)buttons().size() - 1, m_button + d));
            show_controls(now, Zone::Buttons);
        } else {
            seek_step(d, st, now);
        }
        return;
    }
    if (p & NUVIO_BTN_DOWN) {
        if (m_controls && m_zone == Zone::Bar && !m_seeking)
            m_zone = Zone::Buttons;
        show_controls(now, Zone::Buttons);
        return;
    }
    if (p & NUVIO_BTN_UP) {
        if (m_controls && m_zone == Zone::Buttons)
            m_zone = Zone::Bar;
        show_controls(now, Zone::Bar);
        return;
    }
    if (p & (NUVIO_BTN_OPTIONS | NUVIO_BTN_TOUCHPAD)) {
        show_controls(now, Zone::Buttons);
        return;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        if (m_seeking)
            m_seeking = false;                 /* cancel the scrub */
        else if (next_card(st))
            m_card_dismissed = true;           /* watch the credits */
        else if (m_controls && !st.paused)
            m_controls = false;                /* first Back hides */
        else
            out.push_back({OsdCmd::Stop});
    }
}

bool PlayerUi::wants_frame(const NuvioStatus &st)
{
    if (m_dirty)
        return true;
    const bool moving = a_controls.value != a_controls.target || a_loading.value != a_loading.target ||
                        a_overlay.value != a_overlay.target || a_skip.value != a_skip.target ||
                        a_next.value != a_next.target || a_spinner.value != a_spinner.target ||
                        a_toast.value != a_toast.target || a_error.value != a_error.target ||
                        a_flash.value > 0.f || m_ep_scroll.value != m_ep_scroll.target ||
                        a_stats.value != a_stats.target;
    static double last_second = 0;
    const bool second = std::floor(st.now) != std::floor(last_second);
    last_second = st.now;
    /* art::animating(): an image still loading or fading in (the episode stills). */
    return moving || m_seeking || m_music || (second && (m_controls || m_card_since >= 0 || m_stats)) ||
           a_loading.value > 0.f ||
           st.buffering || ((m_overlay != Overlay::None || a_next.value > 0.f || m_music) && art::animating());
}

/* ---- drawing ---------------------------------------------------------------------- */

/* A glass panel over the dimmed picture (the app's look). */
static void glass(const gfx::Rect &r, float a)
{
    glass_panel(r, std::min(28.f, r.h / 2), a);   /* frosted: the picture shows through, blurred */
}

void PlayerUi::draw_bar(const NuvioStatus &st, float a)
{
    const float x0 = kPad, x1 = W - kPad - 150, w = x1 - x0;
    const double d = st.duration > 0 ? st.duration : 1;
    const double pos = m_seeking ? m_seek_target : st.position;
    const bool focus = m_zone == Zone::Bar || m_seeking;
    const float h = focus ? 12.f : 8.f;
    const float y = kBarY - h / 2;
    gfx::fill({x0, y, w, h}, alpha(0x38ffffffu, a), h / 2);
    gfx::fill({x0, y, w * (float)std::min(1.0, st.buffered / d), h}, alpha(0x47ffffffu, a), h / 2);
    for (const NuvioSkip &k : m_req->skips) {   /* intro, recap, credits */
        const float sx = x0 + w * (float)(k.start / d), ex = x0 + w * (float)(std::min(k.end, d) / d);
        gfx::fill({sx, y, std::max(2.f, ex - sx), h}, alpha(0x8c00a4dcu, a), 0);
    }
    const float px = x0 + w * (float)std::min(1.0, pos / d);
    gfx::fill({x0, y, px - x0, h}, alpha(0xffffffffu, a), h / 2);
    for (const NuvioChapter &ch : m_req->chapters) {   /* chapters: thin gaps in the bar */
        if (ch.start <= 1.0 || ch.start >= d - 1.0)
            continue;
        gfx::fill({x0 + w * (float)(ch.start / d) - 1.5f, y, 3, h}, alpha(0xb0000000u, a));
    }
    const float hd = focus ? (m_seeking ? 34.f : 28.f) : 18.f;
    gfx::shadow({px - hd / 2, kBarY - hd / 2, hd, hd}, hd / 2, 10, 0.5f * a, 2);
    gfx::fill({px - hd / 2, kBarY - hd / 2, hd, hd}, alpha(0xffffffffu, a), hd / 2);
    gfx::text(W - kPad, kBarY + 9, "\xE2\x88\x92" + fmt_time(d - pos), {gfx::SemiBold, 26}, alpha(kText, a), 2);

    if (m_seeking) {   /* the time under the playhead, the chapter, and the picture there */
        const std::string t = fmt_time(pos);
        std::string chapter;
        for (const NuvioChapter &ch : m_req->chapters)
            if (ch.start <= pos + 0.5)
                chapter = ch.name;
        if (m_req->chapters.size() < 2)
            chapter.clear();
        const gfx::TextStyle bs{gfx::Bold, 28}, cs{gfx::Medium, 21, 420};
        const NuvioTrickplay &tp = m_req->trickplay;
        const float pw = 400, ph = tp.valid() ? pw * tp.height / tp.width : 0;
        const float bw = std::max({gfx::text_width(t, bs) + 36, chapter.empty() ? 0.f : std::min(460.f, gfx::text_width(chapter, cs) + 36),
                                   ph > 0 ? pw + 16 : 0.f});
        const float bh = 52 + (chapter.empty() ? 0 : 30) + (ph > 0 ? ph + 8 : 0);
        const float bx = std::max(x0 + bw / 2, std::min(x1 - bw / 2, px));
        const gfx::Rect r{bx - bw / 2, kBarY - 32 - bh, bw, bh};
        glass_panel(r, std::min(28.f, r.h / 2), a, false);   /* no shadow: over the picture it read as a black box */
        float ty = r.y;
        if (ph > 0) {
            /* One thumbnail of a sheet of tile_w x tile_h: sheet = i / per, cell = i % per. */
            const int per = tp.tile_w * tp.tile_h;
            const int i = std::max(0, std::min(tp.count - 1, (int)(pos / tp.interval)));
            const std::string url = tp.url_base + std::to_string(i / per) + ".jpg" + tp.url_query;
            const gfx::Rect pr{r.x + 8, r.y + 8, bw - 16, ph};
            gfx::fill(pr, alpha(0xff101014u, a), 20);
            if (const gfx::Texture *sheet = art::get(url, tp.width * tp.tile_w, tp.height * tp.tile_h)) {
                /* The last sheet can be short: as many columns and rows as it holds. */
                const int held = std::min(per, tp.count - (i / per) * per);
                const int cols = std::min(tp.tile_w, held), rows = (held + tp.tile_w - 1) / tp.tile_w;
                const int cell = i % per, cx = cell % tp.tile_w, cy = cell / tp.tile_w;
                gfx::image_uv(pr, sheet, (float)cx / cols, (float)cy / rows, (float)(cx + 1) / cols,
                              (float)(cy + 1) / rows, a, 20);
            }
            ty += ph + 8;
        }
        if (!chapter.empty()) {
            gfx::text(bx, ty + 36, chapter, cs, alpha(kText2, a), 1);
            ty += 30;
        }
        gfx::text(bx, ty + 37, t, bs, alpha(kText, a), 1);
    }
}

bool PlayerUi::trick_thumb(const gfx::Rect &r, double pos, float a, float radius)
{
    const NuvioTrickplay &tp = m_req->trickplay;
    if (!tp.valid())
        return false;
    const int per = tp.tile_w * tp.tile_h;
    const int i = std::max(0, std::min(tp.count - 1, (int)(pos / tp.interval)));
    const std::string url = tp.url_base + std::to_string(i / per) + ".jpg" + tp.url_query;
    const gfx::Texture *sheet = art::get(url, tp.width * tp.tile_w, tp.height * tp.tile_h);
    if (!sheet)
        return true;   /* coming */
    const int held = std::min(per, tp.count - (i / per) * per);
    const int cols = std::min(tp.tile_w, held), rows = (held + tp.tile_w - 1) / tp.tile_w;
    const int cell = i % per, cx = cell % tp.tile_w, cy = cell / tp.tile_w;
    gfx::image_uv(r, sheet, (float)cx / cols, (float)cy / rows, (float)(cx + 1) / cols, (float)(cy + 1) / rows, a, radius);
    return true;
}

/* Kapitler: as the episode picker - one large glass panel, a row per chapter
 * (its picture, number and name, where it starts), the drop on the focused one. */
void PlayerUi::draw_chapters(const NuvioStatus &st, float a, float dt)
{
    const std::vector<NuvioChapter> &ch = m_req->chapters;
    const int n = (int)ch.size();
    m_chap = std::max(0, std::min(n - 1, m_chap));
    const gfx::Rect r{160, 120, W - 320, H - 240};
    glass(r, a);
    gfx::text(r.x + 56, r.y + 86, T("Kapitler"), {gfx::Bold, 40}, alpha(kText, a));
    gfx::text(r.x + 56 + gfx::text_width(T("Kapitler"), {gfx::Bold, 40}) + 22, r.y + 86, m_req->header_title(),
              {gfx::Medium, 26, 900}, alpha(kText3, a));
    const float top = r.y + 140, lx = r.x + 40, lw = r.w - 80, row_h = 178, view_h = r.h - 250;   /* clear of the hints at the foot */
    m_chap_scroll.to(std::max(0.f, std::min(std::max(0.f, n * row_h - view_h), (m_chap - 1) * row_h)));
    if (m_chap_scroll.step(dt, 12.f))
        m_dirty = true;
    int now_i = 0;
    for (int i = 0; i < n; i++)
        if (ch[i].start <= st.position + 0.5)
            now_i = i;
    const gfx::Rect chap_view{lx - 20, top - 10, lw + 40, view_h + 10};
    gfx::push_scissor(chap_view);
    bool moving = false;
    m_chap_drop.to({lx, top + m_chap * row_h - m_chap_scroll.value, lw, row_h - 14}, m_chap, r.x,
                   r.y - m_chap_scroll.value);
    m_chap_drop.draw(dt, a, &moving, 18);
    if (moving)
        m_dirty = true;
    gfx::push_fade_mask(chap_view, edge_fade(m_chap_scroll.value),
                        edge_fade(std::max(0.f, n * row_h - view_h) - m_chap_scroll.value));   /* the rows fade out where more lie beyond */
    for (int i = 0; i < n; i++) {
        const float y = top + i * row_h - m_chap_scroll.value;
        if (y > top + view_h || y + row_h < top - 10)
            continue;
        const bool focus = i == m_chap;
        /* Its picture: Jellyfin's chapter image, else a trickplay frame, else the backdrop. */
        const gfx::Rect th{lx + 18, y + 12, 250, 140};
        gfx::fill(th, alpha(0xff101014u, a), 10);
        if (!ch[i].image.empty())
            art::draw(th, ch[i].image, "", 480, 270, 10, a);
        else if (!trick_thumb(th, ch[i].start + 5.0, a, 10) && !m_req->backdrop.empty())
            art::draw(th, m_req->backdrop, "", 480, 270, 10, a * 0.55f);
        const float tx = th.x + th.w + 28;
        const std::string name = ch[i].name.empty() ? T("Kapittel ") + std::to_string(i + 1)
                                                    : std::to_string(i + 1) + ". " + ch[i].name;
        const float hx = tx + gfx::text(tx, y + 48, name, {gfx::Bold, 26, lw - 520}, alpha(focus ? kText : kText2, a));
        if (i == now_i) {
            gfx::fill({hx + 14, y + 24, 128, 30}, alpha(0xe600a4dcu, a), 15);
            gfx::text(hx + 78, y + 46, T("SPILLER NÅ"), {gfx::Bold, 16}, alpha(kText, a), 1);
        }
        const int s = (int)ch[i].start;
        char t[16];
        if (s >= 3600)
            std::snprintf(t, sizeof t, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        else
            std::snprintf(t, sizeof t, "%d:%02d", s / 60, s % 60);
        gfx::text(tx, y + 88, t, {gfx::Medium, 22}, alpha(kText3, a));
    }
    gfx::pop_fade_mask();
    gfx::pop_scissor();
    draw_pad_hints(r.x + 56, r.y + r.h - 48, {{PadButton::Cross, T("Spill herfra")}, {PadButton::Circle, T("Lukk")}}, 0,
                   26, a);
}

void PlayerUi::draw_controls(const NuvioStatus &st)
{
    const float a = smoothstep(a_controls.value);
    if (a <= 0.f)
        return;
    gfx::fill_vgradient({0, 0, W, 240}, alpha(0x99000000u, a), 0x00000000u);
    gfx::fill_vgradient({0, H - 420, W, 420}, 0x00000000u, alpha(0xe0000000u, a));

    /* Top: the logo and the clock. */
    if (const gfx::Texture *logo = m_req->logo.empty() ? nullptr : art::get(m_req->logo, 800, 800)) {
        const float iw = (float)gfx::texture_width(logo), ih = (float)gfx::texture_height(logo);
        const float k = std::min(300.f / iw, 84.f / ih);
        gfx::image({kPad, 60 + (84 - ih * k), iw * k, ih * k}, logo, a, 0, false);
    }
    if (m_req->prefs.show_clock)
        gfx::text(W - kPad, 104, clock_at(0), {gfx::SemiBold, 28}, alpha(kText2, a), 2);

    /* Bottom: the title line, with the end time on the right. */
    const float ty = kBarY - 52;
    float x = kPad;
    x += gfx::text(x, ty, m_req->header_title(), {gfx::Bold, 30, 900}, alpha(kText, a));
    std::string sub;
    if (m_req->season > 0 && m_req->episode > 0) {
        char b[64];
        std::snprintf(b, sizeof b, "S%d:E%d", m_req->season, m_req->episode);
        sub = b;
        if (!m_req->episode_title.empty())
            sub += " \xC2\xB7 " + m_req->episode_title;
    }
    if (!sub.empty())
        gfx::text(x + 18, ty, sub, {gfx::Medium, 26, W - x - 500}, alpha(kText2, a));
    if (st.duration > 0)
        gfx::text(W - kPad, ty, T("Slutter kl. ") + clock_at(st.duration - st.position), {gfx::Medium, 22},
                  alpha(kText3, a), 2);

    draw_bar(st, a);

    /* The button row: icon + label, on one glass bar like the top bar, the focus
     * drop on the focused one. */
    const std::vector<Button> bs = buttons();
    m_button = std::min(m_button, (int)bs.size() - 1);
    const float by = H - 128, bh = 60;
    auto button_label = [&](Button b) -> std::string {
        switch (b) {
        case Button::PlayPause: return st.paused ? T("Spill av") : "Pause";
        case Button::Episodes: return T("Episoder");
        case Button::Chapters: return T("Kapitler");
        case Button::Speed: return speed_label();
        case Button::Tracks: return T("Lyd og undertekster");
        case Button::Next: return T("Neste episode");
        }
        return "";
    };
    {
        float bw_all = 12, x = kPad - 14;
        for (size_t i = 0; i < bs.size(); i++) {
            const float bw = 26 + 28 + 12 + gfx::text_width(button_label(bs[i]), {gfx::SemiBold, 23}) + 26;
            if (m_zone == Zone::Buttons && (int)i == m_button)
                m_btn_drop.to({x, by, bw, bh}, (int)bs[i], 0, by);
            x += bw + 8;
            bw_all += bw + 8;
        }
        glass_panel({kPad - 20, by - 6, bw_all - 2, bh + 12}, (bh + 12) / 2, a, false);
        if (m_zone != Zone::Buttons)
            m_btn_drop.hide();
        bool moving = false;
        m_btn_drop.draw(m_dt, a, &moving);
        if (moving)
            m_dirty = true;
    }
    float bx = kPad - 14;
    for (size_t i = 0; i < bs.size(); i++) {
        std::string label;
        switch (bs[i]) {
        case Button::PlayPause: label = st.paused ? T("Spill av") : "Pause"; break;
        case Button::Episodes: label = T("Episoder"); break;
        case Button::Chapters: label = T("Kapitler"); break;
        case Button::Speed: label = speed_label(); break;
        case Button::Tracks: label = T("Lyd og undertekster"); break;
        case Button::Next: label = T("Neste episode"); break;
        }
        const gfx::TextStyle ls{gfx::SemiBold, 23};
        const float bw = 26 + 28 + 12 + gfx::text_width(label, ls) + 26;
        const bool focus = m_zone == Zone::Buttons && (int)i == m_button;
        const gfx::Rect r{bx, by, bw, bh};
        const uint32_t fg = alpha(focus ? kText : kText2, a);
        const float ix = r.x + 26, cy = r.y + bh / 2;
        switch (bs[i]) {
        case Button::PlayPause:
            if (st.paused)
                play_glyph(ix + 4, cy, 20, fg);
            else
                pause_glyph(ix + 12, cy, 20, fg);
            break;
        case Button::Episodes:   /* a stack of cards */
            gfx::fill({ix + 4, cy - 11, 22, 3}, fg, 1.5f);
            gfx::fill({ix + 1, cy - 6, 28, 17}, fg, 3);
            break;
        case Button::Speed:      /* two chevrons: forward, faster */
            for (int k2 = 0; k2 < 2; k2++)
                for (int s2 = 0; s2 < 9; s2++) {
                    const float yy = s2 < 5 ? (float)s2 : (float)(8 - s2);
                    gfx::fill({ix + 4 + k2 * 11 + yy * 1.8f, cy - 9 + s2 * 2.2f, 3, 3}, fg, 1.5f);
                }
            break;
        case Button::Chapters:   /* a list: three bars with dots */
            for (int k = -1; k <= 1; k++) {
                gfx::fill({ix + 1, cy + k * 8 - 2, 4, 4}, fg, 2);
                gfx::fill({ix + 9, cy + k * 8 - 1.5f, 20, 3}, fg, 1.5f);
            }
            break;
        case Button::Tracks:     /* a speech bubble with lines */
            gfx::fill({ix, cy - 11, 30, 21}, fg, 5);
            gfx::fill({ix + 5, cy + 9, 7, 6}, fg, 1);
            gfx::fill({ix + 6, cy - 5, 18, 2.5f}, alpha(0xff141418u, a), 1);
            gfx::fill({ix + 6, cy + 1, 12, 2.5f}, alpha(0xff141418u, a), 1);
            break;
        case Button::Next:
            play_glyph(ix + 2, cy, 18, fg);
            gfx::fill({ix + 22, cy - 10, 4, 20}, fg, 1);
            break;
        }
        gfx::text(ix + 28 + 12, cy + 8, label, ls, fg);
        bx += bw + 8;
    }
}

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

    /* Only the picture: no logo flashing up in the moment before playback. If the
     * stream is slow to open (over 2 s), quiet dots say it is still coming. */
    const float t = (float)(st.now - m_load_since);
    if (t > 2.f && st.error.empty()) {
        const float da = std::min(1.f, (t - 2.f) / 0.5f);
        for (int i = 0; i < 3; i++) {
            const float pulse = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(t * 5.f - i * 0.9f));
            gfx::fill({W / 2 - 40 + i * 32, H - 160, 14, 14}, alpha(kText, da * pulse), 7);
        }
        m_dirty = true;
    }
    gfx::pop_opacity();
}

void PlayerUi::draw_skip_next(const NuvioStatus &st)
{
    /* Skip intro / recap: a white pill, Cross acts (concept .skip.focus). */
    const int k = current_skip(st);
    a_skip.to(k >= 0 ? 1.f : 0.f);
    if (a_skip.value > 0.01f) {
        static std::string label;   /* kept while it fades out (no T() in a static initializer) */
        if (label.empty())
            label = T("Hopp over intro");
        if (k >= 0)
            label = m_req->skips[k].type == "recap" ? T("Hopp over oppsummering")
                    : m_req->skips[k].type == "preview" ? T("Hopp over forhåndsvisning") : T("Hopp over intro");
        const gfx::TextStyle st2{gfx::Bold, 26};
        const float w = gfx::text_width(label, st2) + 72;
        const float y = H - 350 - (1.f - a_skip.value) * 20 + (m_controls ? 0 : 200);
        const gfx::Rect r{W - kPad - w, y, w, 72};
        gfx::push_opacity(a_skip.value);
        glass_panel(r, 14, 1.f, true, 1.f);   /* the one thing to press: the focus glass */
        gfx::text(r.x + r.w / 2, r.y + 46, label, st2, kText, 1);
        gfx::pop_opacity();
    }

    /* Next episode: a glass card with the still, the title and the countdown. */
    const bool card = next_card(st);
    a_next.to(card ? 1.f : 0.f);
    if (a_next.value > 0.01f && m_req->has_next) {
        const NuvioEpisode &n = m_req->next;
        const gfx::Rect r{W - kPad - 560, H - 440 - (1.f - a_next.value) * 20 + (m_controls ? 0 : 290), 560, 156};
        gfx::push_opacity(a_next.value);
        glass(r, 1.f);
        art::draw({r.x + 18, r.y + 18, 213, 120}, n.thumbnail, n.blurhash, 480, 270, 10);
        const float tx = r.x + 250;
        gfx::text(tx, r.y + 44, T("NESTE EPISODE"), {gfx::Bold, 17}, kText3);
        char title[256];
        std::snprintf(title, sizeof title, "S%d:E%d \xC2\xB7 %s", n.season, n.episode, n.title.c_str());
        gfx::text(tx, r.y + 80, title, {gfx::Bold, 24, r.w - 270}, kText);
        if (m_req->prefs.autoplay_next && m_card_since >= 0) {
            const double left = std::max(0.0, 10.0 - (st.now - m_card_since));
            char c[48];
            std::snprintf(c, sizeof c, T("Spilles om %d s"), (int)std::ceil(left));
            const float cw = gfx::text(tx, r.y + 116, c, {gfx::Medium, 20}, kText2);
            draw_pad_hint(tx + cw + 18, r.y + 109, PadButton::Cross, T("Nå"), 24);
            gfx::fill({tx, r.y + 132, r.w - 270, 4}, 0x33ffffffu, 2);
            gfx::fill({tx, r.y + 132, (r.w - 270) * (float)(1.0 - left / 10.0), 4}, kAccent, 2);
        } else {
            draw_pad_hints(tx, r.y + 109, {{PadButton::Cross, T("Spill av")}, {PadButton::Circle, T("Se rulletekst")}}, 0,
                           24);
        }
        gfx::pop_opacity();
    }
}

/* Lyd og undertekster: a glass panel with an audio column and a subtitle
 * column; "Tilpass undertekster" opens style and timing in a third. */
void PlayerUi::draw_tracks(const NuvioStatus &st, float a)
{
    const gfx::Rect r{160, 120, W - 320, H - 240};
    glass(r, a);
    gfx::text(r.x + 56, r.y + 86, T("Lyd og undertekster"), {gfx::Bold, 40}, alpha(kText, a));

    const float top = r.y + 150, row_h = 62;
    const int visible = 8;   /* 10 ran into the hints at the foot */
    const float cols[3] = {r.x + 56, r.x + 640, r.x + 1180};
    const float widths[3] = {520, 480, 380};
    const std::string heads[3] = {T("Lyd"), T("Undertekster"), m_find_open ? T("S\xC3\xB8k") : T("Tilpass")};
    for (int c = 0; c < (m_style_open || m_find_open ? 3 : 2); c++)
        gfx::text(cols[c] + 18, top, heads[c], {gfx::SemiBold, 22}, alpha(kText3, a));

    auto column = [&](int c, int n, auto label_of) {
        int &sel_row = m_rows[c];
        sel_row = std::max(0, std::min(std::max(0, n - 1), sel_row));
        const int first = std::max(0, std::min(sel_row - visible / 2, n - visible));
        if (c == m_col && n > 0) {   /* the focus drop, under this column's rows */
            m_tracks_drop.to({cols[c], top + 30 + (sel_row - first) * (row_h + 4), widths[c], row_h}, c * 1000 + sel_row,
                             r.x, r.y);
            bool moving = false;
            m_tracks_drop.draw(m_dt, a, &moving, 14);
            if (moving)
                m_dirty = true;
        }
        for (int i = first; i < n && i < first + visible; i++) {
            std::string label, right;
            bool selected = false, dim = false;
            label_of(i, label, right, selected, dim);
            const float y = top + 30 + (i - first) * (row_h + 4);
            const bool focus = c == m_col && i == sel_row;
            const uint32_t fg = focus || selected ? kText : dim ? kText3 : kText2;
            float lx = cols[c] + 18;
            if (selected) {   /* a check, drawn: two bars */
                gfx::fill({lx, y + row_h / 2 - 1, 8, 3}, alpha(fg, a), 1.5f);
                gfx::fill({lx + 6, y + row_h / 2 - 8, 3, 13}, alpha(fg, a), 1.5f);
            }
            lx += 28;
            gfx::text(lx, y + 40, label, {selected ? gfx::Bold : gfx::Medium, 25, widths[c] - 60 - (right.empty() ? 0 : 150)},
                      alpha(fg, a));
            if (!right.empty())
                gfx::text(cols[c] + widths[c] - 18, y + 39, right, {gfx::Medium, 20},
                          alpha(focus ? kText2 : kText3, a), 2);
        }
    };

    /* Audio, then the title's versions when there are several. */
    const int na = (int)st.audio.size();
    const int nv = m_req->sources.size() > 1 ? (int)m_req->sources.size() : 0;
    if (na == 0)
        gfx::text(cols[0] + 18, top + 72, T("Ingen andre lydspor"), {gfx::Medium, 24}, alpha(kText3, a));
    column(0, na + nv, [&](int i, std::string &label, std::string &right, bool &sel, bool &) {
        if (i >= na) {
            const NuvioSource &v = m_req->sources[i - na];
            label = T("Versjon") + std::string(": ") + v.title;
            right = v.description;
            sel = i - na == m_req->source_index;
            return;
        }
        const NuvioAudioTrack &t = st.audio[i];
        label = language_name(t.lang);
        if (!t.title.empty() && t.title != t.codec)
            label += " \xC2\xB7 " + t.title;
        right = t.codec + (t.channels.empty() ? "" : " " + t.channels);
        sel = i == st.audio_active;
    });

    /* Subtitles: Av, the tracks, then "Tilpass undertekster". */
    const int ns = nuvio_subs_count(), cur = nuvio_subs_selected();
    const bool find = jelly5_subs::available();
    column(1, ns + 2 + (find ? 1 : 0), [&](int i, std::string &label, std::string &right, bool &sel, bool &dim) {
        if (i == 0) {
            label = T("Av");
            sel = cur < 0;
            return;
        }
        if (i == ns + 1) {
            label = T("Tilpass undertekster \xE2\x80\xBA");
            return;
        }
        if (i == ns + 2) {
            label = T("S\xC3\xB8k etter undertekster \xE2\x80\xBA");
            return;
        }
        nuvio_sub_track t;
        if (nuvio_subs_track(i - 1, &t) != 0)
            return;
        label = language_name(t.lang);
        if (t.title[0] && std::string(t.title) != label)
            label += " \xC2\xB7 " + std::string(t.title);
        if (t.forced) right += T("Tvungen ");
        if (t.hearing_impaired) right += "SDH ";
        if (t.bitmap) right += T("Bilde ");
        if (t.external) right += T("Ekstern");
        sel = i - 1 == cur;
        dim = t.state < 0;
    });

    /* Style and timing. */
    if (m_style_open) {
        nuvio_sub_style s;
        nuvio_subs_get_style(&s);
        column(2, 5, [&](int i, std::string &label, std::string &right, bool &, bool &) {
            char v[48];
            switch (i) {
            case 0: label = T("Forsinkelse"); std::snprintf(v, sizeof v, "\xE2\x80\xB9 %+.1f s \xE2\x80\xBA", nuvio_subs_delay_ms() / 1000.0); break;
            case 1: label = T("Størrelse"); std::snprintf(v, sizeof v, "\xE2\x80\xB9 %d %% \xE2\x80\xBA", s.size_pct); break;
            case 2: label = T("Posisjon"); std::snprintf(v, sizeof v, "\xE2\x80\xB9 %.0f %% \xE2\x80\xBA", s.offset_pct); break;
            case 3:
                label = T("Bakgrunn");
                if (s.background < 0.05f)
                    std::snprintf(v, sizeof v, "%s", T("\xE2\x80\xB9 Av \xE2\x80\xBA"));
                else
                    std::snprintf(v, sizeof v, "\xE2\x80\xB9 %d %% \xE2\x80\xBA", (int)(s.background * 100));
                break;
            default: label = T("Kontur"); std::snprintf(v, sizeof v, "%s", s.outline ? T("På") : T("Av")); break;
            }
            right = v;
        });
    }
    /* Subtitle search: the language, then what the server's plugins found. */
    if (m_find_open) {
        std::vector<jf::RemoteSubtitle> found;
        std::string lang;
        const jelly5_subs::State state = jelly5_subs::results(&found, &lang);
        column(2, 1 + (int)found.size(), [&](int i, std::string &label, std::string &right, bool &, bool &) {
            if (i == 0) {
                label = T("Spr\xC3\xA5k");
                right = (m_find_langs.size() > 1 ? "\xE2\x80\xB9 " : "") + language_name(lang) +
                        (m_find_langs.size() > 1 ? " \xE2\x80\xBA" : "");
                return;
            }
            const jf::RemoteSubtitle &x = found[i - 1];
            label = x.name.empty() ? x.provider : x.name;
            if (x.hash_match) right += T("Passer ");
            if (x.hearing_impaired) right += "SDH ";
            if (x.forced) right += T("Tvungen ");
            if (right.empty() && x.downloads > 0) right = std::to_string(x.downloads) + T(" nedl.");
        });
        const float sy = top + 30 + (row_h + 4) + 40;   /* where the first result goes */
        if (state == jelly5_subs::Busy)
            gfx::text(cols[2] + 18, sy, T("S\xC3\xB8ker \xE2\x80\xA6"), {gfx::Medium, 22}, alpha(kText3, a));
        else if (state == jelly5_subs::Done && found.empty())
            gfx::text(cols[2] + 18, sy, T("Fant ingen"), {gfx::Medium, 22}, alpha(kText3, a));
    }
    draw_pad_hints(r.x + 56, r.y + r.h - 48, {{PadButton::Circle, T("Lukk")}}, 0, 26, a);
}

/* Episoder: seasons on the left, the season's episodes as a list of stills
 * with title, runtime, synopsis and progress; "Spiller nå" on this one. */
void PlayerUi::draw_episodes(float a, float dt)
{
    const gfx::Rect r{160, 120, W - 320, H - 240};
    glass(r, a);
    gfx::text(r.x + 56, r.y + 86, T("Episoder"), {gfx::Bold, 40}, alpha(kText, a));
    gfx::text(r.x + 56 + gfx::text_width(T("Episoder"), {gfx::Bold, 40}) + 22, r.y + 86, m_req->header_title(),
              {gfx::Medium, 26, 900}, alpha(kText3, a));

    const std::vector<int> ss = seasons();
    const float top = r.y + 140, col_h = r.h - 250, season_h = 66;   /* clear of the hints at the foot */
    /* More seasons than fit: the column scrolls, the season shown kept in view. */
    int si = 0;
    for (size_t i = 0; i < ss.size(); i++)
        if (ss[i] == m_ep_season)
            si = (int)i;
    const float want = std::max(0.f, std::min(ss.size() * season_h - 8 - col_h, (si + 0.5f) * season_h - col_h / 2));
    if (m_ep_season_scroll.value < 0)
        m_ep_season_scroll.snap(want);
    m_ep_season_scroll.to(want);
    bool moving = m_ep_season_scroll.step(dt, 12.f);
    const float sy = m_ep_season_scroll.value;
    /* The drop is never cut (the season shown is always in view); only the labels
     * of an overflowing column are. */
    m_season_drop.to({r.x + 40, top + si * season_h - sy, 280, 58}, si, r.x, r.y - sy);   /* brighter while focused */
    m_season_drop.draw(dt, a * (m_ep_col == 0 ? 1.f : 0.5f), &moving, 14);
    const bool clip = ss.size() * season_h - 8 > col_h;
    const gfx::Rect season_view{r.x, top - 10, 360, col_h + 10};
    if (clip) {
        gfx::push_scissor(season_view);
        gfx::push_fade_mask(season_view, edge_fade(sy, 56), edge_fade(ss.size() * season_h - 8 - col_h - sy, 56));   /* the labels fade out where more lie beyond */
    }
    for (size_t i = 0; i < ss.size(); i++) {
        const float y = top + i * season_h - sy;
        if (y > top + col_h || y + season_h < top - 10)
            continue;
        char label[32];
        if (ss[i] == 0)
            std::snprintf(label, sizeof label, "%s", T("Spesialer"));
        else
            std::snprintf(label, sizeof label, T("Sesong %d"), ss[i]);
        const bool active = ss[i] == m_ep_season;
        gfx::text(r.x + 64, y + 38, label, {active ? gfx::Bold : gfx::Medium, 25}, alpha(active ? kText : kText2, a));
    }
    if (clip) {
        gfx::pop_fade_mask();
        gfx::pop_scissor();
    }

    const std::vector<int> eps = episodes_in(m_ep_season);
    const float lx = r.x + 360, lw = r.w - 400, row_h = 178;
    const float view_h = r.h - 180;   /* the list runs to the foot: the hints are under the seasons */
    m_ep_index = std::min(m_ep_index, std::max(0, (int)eps.size() - 1));
    m_ep_scroll.to(std::max(0.f, std::min(std::max(0.f, eps.size() * row_h - view_h), (m_ep_index - 1) * row_h)));
    m_ep_scroll.step(dt, 12.f);
    gfx::push_scissor({lx - 20, top - 10, lw + 40, view_h + 10});
    if (m_ep_col == 1 && m_ep_index < (int)eps.size())
        m_ep_drop.to({lx, top + m_ep_index * row_h - m_ep_scroll.value, lw, row_h - 14}, m_ep_index, r.x,
                     r.y - m_ep_scroll.value);
    else
        m_ep_drop.hide();
    m_ep_drop.draw(dt, a, &moving, 18);
    if (moving)
        m_dirty = true;
    const gfx::Rect ep_view{lx - 20, top - 10, lw + 40, view_h + 10};
    gfx::push_fade_mask(ep_view, edge_fade(m_ep_scroll.value),
                        edge_fade(std::max(0.f, eps.size() * row_h - view_h) - m_ep_scroll.value));   /* the rows fade out where more lie beyond */
    for (size_t i = 0; i < eps.size(); i++) {
        const float y = top + i * row_h - m_ep_scroll.value;
        if (y > top + view_h || y + row_h < top - 10)
            continue;
        const NuvioEpisode &e = m_req->episodes[eps[i]];
        const bool focus = m_ep_col == 1 && (int)i == m_ep_index;
        const bool here = e.season == m_req->season && e.episode == m_req->episode;
        const gfx::Rect row{lx, y, lw, row_h - 14};
        (void)row;
        const gfx::Rect th{lx + 18, y + 12, 250, 140};
        gfx::push_opacity(a);
        art::draw(th, e.thumbnail, e.blurhash, 480, 270, 10);
        if (e.progress > 0 && e.progress < 100) {
            gfx::fill({th.x + 10, th.y + th.h - 14, th.w - 20, 5}, 0x47ffffffu, 2.5f);
            gfx::fill({th.x + 10, th.y + th.h - 14, (th.w - 20) * (float)(e.progress / 100), 5}, 0xffffffffu, 2.5f);
        }
        gfx::pop_opacity();
        const float tx = th.x + th.w + 26;
        char title[300];
        std::snprintf(title, sizeof title, "%d. %s", e.episode, e.title.c_str());
        float hx = tx + gfx::text(tx, y + 48, title, {gfx::Bold, 26, lw - 520}, alpha(focus ? kText : kText2, a));
        if (here) {
            gfx::fill({hx + 14, y + 24, 128, 30}, alpha(0xe600a4dcu, a), 15);
            gfx::text(hx + 78, y + 46, T("SPILLER NÅ"), {gfx::Bold, 16}, alpha(kText, a), 1);
        } else if (e.watched) {
            gfx::fill({hx + 14, y + 24, 62, 30}, alpha(0x33ffffffu, a), 15);
            gfx::text(hx + 45, y + 46, T("Sett"), {gfx::SemiBold, 17}, alpha(kText, a), 1);
        }
        gfx::text(lx + lw - 24, y + 48, e.runtime, {gfx::Medium, 20}, alpha(kText3, a), 2);
        gfx::text(tx, y + 88, e.overview.empty() ? T("Ingen beskrivelse.") : e.overview,
                  {gfx::Regular, 21, lw - 320, 2, 30}, alpha(kText3, a));
    }
    gfx::pop_fade_mask();
    gfx::pop_scissor();
    if (eps.empty())
        gfx::text(lx, top + 50, T("Ingen episoder i denne sesongen."), {gfx::Medium, 24}, alpha(kText3, a));
    /* The hints stay under the season column: on two lines when one is too wide for it. */
    const std::vector<PadHint> hints{{PadButton::Cross, T("Spill av")}, {PadButton::Circle, T("Lukk")}};
    float hw = 26 * 0.9f;
    for (const PadHint &h : hints)
        hw += pad_hint_width(h.button, h.label, 26);
    if (hw <= lx - 20 - (r.x + 56)) {
        draw_pad_hints(r.x + 56, r.y + r.h - 48, hints, 0, 26, a);
    } else {
        draw_pad_hints(r.x + 56, r.y + r.h - 92, {hints[0]}, 0, 26, a);
        draw_pad_hints(r.x + 56, r.y + r.h - 48, {hints[1]}, 0, 26, a);
    }
}

void PlayerUi::draw_error(const NuvioStatus &st)
{
    a_error.to(st.error.empty() ? 0.f : 1.f);
    if (a_error.value <= 0.01f)
        return;
    gfx::push_opacity(a_error.value);
    gfx::fill({0, 0, W, H}, 0xe6080b10u);
    gfx::text(W / 2, 470, T("Kunne ikke spille av"), {gfx::Bold, 52}, kText, 1);
    gfx::text(W / 2, 530, st.error, {gfx::Medium, 26, 1300, 2, 36}, kText2, 1);
    const gfx::Rect b{W / 2 - 130, 620, 260, 76};
    glass_panel(b, 16, 1.f, false, 1.f);
    gfx::text(W / 2, 668, T("Tilbake"), {gfx::Bold, 26}, kText, 1);
    gfx::pop_opacity();
}

/* ---- music ------------------------------------------------------------------------ */

void PlayerUi::music_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const double now = st.now;
    if (p & NUVIO_BTN_CROSS) {
        if (m_seeking) {
            out.push_back({OsdCmd::SeekTo, m_seek_target});
            m_seeking = false;
        } else {
            out.push_back({OsdCmd::TogglePause});
        }
    } else if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT | NUVIO_BTN_L2 | NUVIO_BTN_R2)) {
        seek_step((p & (NUVIO_BTN_RIGHT | NUVIO_BTN_R2)) ? 1 : -1, st, now);
    } else if (p & NUVIO_BTN_R1) {
        if (m_req->has_next)
            out.push_back({OsdCmd::PlayNext});
    } else if (p & NUVIO_BTN_L1) {
        previous_track(st, out);
    } else if (p & NUVIO_BTN_CIRCLE) {
        if (m_seeking)
            m_seeking = false;
        else
            out.push_back({OsdCmd::Stop});
    }
}

/* Back: to the start of this track, or (within its first 3 s) the one before. */
void PlayerUi::previous_track(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    int here = -1;
    for (size_t i = 0; i < m_req->episodes.size(); i++)
        if (m_req->episodes[i].season == m_req->season && m_req->episodes[i].episode == m_req->episode)
            here = (int)i;
    if (st.position > 3.0 || here <= 0) {
        out.push_back({OsdCmd::SeekTo, 0.0});
    } else {
        OsdCommand c{OsdCmd::PlayEpisode};
        c.season = m_req->episodes[here - 1].season;
        c.episode = m_req->episodes[here - 1].episode;
        out.push_back(c);
    }
}

void PlayerUi::remote_poll(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    /* SyncPlay: an opened group item waits paused until the group says go. */
    if (syncplay::active() && st.started && !m_group_ready) {
        m_group_ready = true;
        if (!st.paused)
            out.push_back({OsdCmd::TogglePause});
        syncplay::player_started(st.position, false);
    }
    /* Commands due now (the group's carry a moment). */
    for (size_t i = 0; i < m_scheduled.size();) {
        if (m_scheduled[i].at <= st.now) {
            const remote::Command due = m_scheduled[i];
            m_scheduled.erase(m_scheduled.begin() + i);
            remote_do(due, st, out);
        } else {
            i++;
        }
    }
    remote::Command c;
    while (remote::take(&c)) {
        m_dirty = true;
        if (c.at > st.now + 0.005) {
            m_scheduled.push_back(c);
            continue;
        }
        if (c.kind == remote::Command::Play || c.kind == remote::Command::Stop) {
            remote_do(c, st, out);
            return;
        }
        remote_do(c, st, out);
    }
}

void PlayerUi::remote_do(const remote::Command &c, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    m_dirty = true;
    const double d = st.duration > 0 ? st.duration - 1 : 1e9;
    if (c.syncplay) {
        /* The group's command, as told: position first (where it should be by now), then state. */
        double target = c.seek_ticks >= 0 ? c.seek_ticks / 10000000.0 : st.position;
        if (c.kind == remote::Command::Unpause && c.at > 0)
            target += std::max(0.0, st.now - c.at);   /* late: catch up */
        const bool off = c.seek_ticks >= 0 && std::fabs(st.position - target) > 0.4;
        switch (c.kind) {
        case remote::Command::Unpause:
            if (off) out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(d, target))});
            if (st.paused) out.push_back({OsdCmd::TogglePause});
            return;
        case remote::Command::Pause:
            if (!st.paused) out.push_back({OsdCmd::TogglePause});
            if (off) out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(d, target))});
            return;
        case remote::Command::Seek:
            out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(d, target))});
            if (!st.paused) out.push_back({OsdCmd::TogglePause});
            syncplay::seeked(target);   /* ready at the new place */
            return;
        case remote::Command::Stop: out.push_back({OsdCmd::Stop}); return;
        default: return;
        }
    }
    {
        switch (c.kind) {
        case remote::Command::Play:   /* something else to play: stop, the app starts it */
            remote::put_back(c);
            out.push_back({OsdCmd::Stop});
            return;
        case remote::Command::Pause:
            if (!st.paused) out.push_back({OsdCmd::TogglePause});
            break;
        case remote::Command::Unpause:
            if (st.paused) out.push_back({OsdCmd::TogglePause});
            break;
        case remote::Command::PlayPause: out.push_back({OsdCmd::TogglePause}); break;
        case remote::Command::Stop: out.push_back({OsdCmd::Stop}); return;
        case remote::Command::Seek:
            out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(d, c.seek_ticks / 10000000.0))});
            if (!m_music) show_controls(st.now, Zone::Bar);
            break;
        case remote::Command::Rewind: out.push_back({OsdCmd::SeekTo, std::max(0.0, st.position - 10)}); break;
        case remote::Command::FastForward: out.push_back({OsdCmd::SeekTo, std::min(d, st.position + 30)}); break;
        case remote::Command::Next:
            if (m_req->has_next) out.push_back({OsdCmd::PlayNext});
            break;
        case remote::Command::Previous: previous_track(st, out); break;
        case remote::Command::Message:
            toast(c.header.empty() ? c.text : c.text.empty() ? c.header : c.header + ": " + c.text, st.now);
            break;
        }
    }
}

/* Now playing (Apple Music on tvOS): the cover on the left over its own colours,
 * the track on the right with the bar and the transport under it. */
void PlayerUi::draw_music(const NuvioStatus &st)
{
    const NuvioRequest &r = *m_req;
    gfx::fill({0, 0, W, H}, kBg);
    if (const gfx::Texture *bh = art::blurhash(r.cover_blurhash)) {
        /* The cover's colours, flowing: two windows onto its BlurHash drift slowly
         * past each other, so the light moves without ever repeating quickly. */
        const float t = (float)st.now;
        gfx::image({0, 0, W, H}, bh, 0.45f, 0, true);
        const float ax = 0.2f + 0.15f * std::sin(t * 0.07f), ay = 0.2f + 0.15f * std::cos(t * 0.05f);
        gfx::image_uv({0, 0, W, H}, bh, ax, ay, ax + 0.6f, ay + 0.6f, 0.35f, 0);
        const float bx = 0.2f + 0.15f * std::cos(t * 0.045f + 1.f), by = 0.2f + 0.15f * std::sin(t * 0.06f + 2.f);
        gfx::image_uv({0, 0, W, H}, bh, bx + 0.6f, by, bx, by + 0.6f, 0.25f, 0);   /* mirrored */
    }
    gfx::fill_hgradient({0, 0, W, H}, 0x8c07070au, 0xd907070au);

    const float cs = 600, cx = 200, cy = (H - cs) / 2 - 10;
    const gfx::Rect cover{cx, cy, cs, cs};
    gfx::shadow(cover, 24, 60, 0.75f, 26);
    art::draw(cover, r.cover, r.cover_blurhash, 800, 800, 24, 1.f, 0xff1c1c22u);
    /* Opening: dots chasing round on the cover - only when it takes a while (a track
     * normally starts in ~0.3 s, and a flash on every change read as a glitch). */
    const float opening = (float)(st.now - m_load_since);
    if (!st.started && st.error.empty() && opening > 0.8f) {
        const float da = std::min(1.f, (opening - 0.8f) / 0.4f);
        gfx::fill(cover, alpha(0x66000000u, da), 24);
        for (int i = 0; i < 12; i++) {
            const float ang = (float)i / 12.f * 6.2832f;
            const float phase = std::fmod((float)st.now * 1.2f + 1.f - (float)i / 12.f, 1.f);
            gfx::fill({cx + cs / 2 + std::cos(ang) * 34 - 5, cy + cs / 2 + std::sin(ang) * 34 - 5, 10, 10},
                      alpha(kText, da * (0.2f + 0.8f * (1.f - phase))), 5);
        }
    }
    if (!st.started && st.error.empty())
        m_dirty = true;   /* keep drawing, so the dots come in on time */

    const float x = cx + cs + 110, w = W - kPad - x;
    if (!r.lyrics.empty()) {
        draw_lyrics(st, x, w, cy - 40, cy + cs - 250);
    } else {
    float y = cy + 70;
    gfx::text(x, y, st.paused ? T("Satt på pause") : T("Spilles nå"), {gfx::SemiBold, 22}, kText3);
    y += 78;
    const std::string title = r.title;
    gfx::text(x, y, title, {gfx::Bold, 58, w, 2, 66}, kText);
    y += gfx::text_width(title, {gfx::Bold, 58}) > w ? 66 + 58 : 58;
    if (!r.artist.empty())
        gfx::text(x, y, r.artist, {gfx::Medium, 34, w}, kText2);
    y += 46;
    std::string album = r.album;
    if (!r.year.empty() && r.year != "0")
        album += (album.empty() ? "" : " · ") + r.year;
    if (!album.empty())
        gfx::text(x, y, album, {gfx::Medium, 26, w}, kText3);
    }

    /* The bar, the times under it. */
    const float by = cy + cs - 150;
    if (!r.lyrics.empty()) {   /* with lyrics: the track and artist sit over the bar */
        gfx::text(x, by - 74, r.title, {gfx::Bold, 32, w}, kText);
        gfx::text(x, by - 36, r.artist, {gfx::Medium, 24, w}, kText2);
    }
    const double d = st.duration > 0 ? st.duration : 1;
    const double pos = m_seeking ? m_seek_target : st.position;
    const float h = m_seeking ? 10.f : 8.f;
    gfx::fill({x, by - h / 2, w, h}, 0x38ffffffu, h / 2);
    const float px = x + w * (float)std::min(1.0, std::max(0.0, pos / d));
    gfx::fill({x, by - h / 2, px - x, h}, 0xffffffffu, h / 2);
    if (m_seeking)
        gfx::fill({px - 13, by - 13, 26, 26}, 0xffffffffu, 13);
    gfx::text(x, by + 44, fmt_time(pos), {gfx::SemiBold, 22}, kText2);
    if (st.duration > 0)
        gfx::text(x + w, by + 44, "−" + fmt_time(std::max(0.0, d - pos)), {gfx::SemiBold, 22}, kText2, 2);

    /* Transport: previous, play/pause, next (L1, Cross, R1). */
    const float ty = cy + cs - 30, mid = x + 160;
    auto skip = [](float gx, float gy, float size, bool forward, uint32_t c) {
        const int n = (int)(size / 1.5f);
        for (int i = 0; i < n; i++) {
            const float hh = size * 1.1f * (forward ? 1.f - (float)i / n : (float)(i + 1) / n);
            gfx::fill({gx + i * 1.5f, gy - hh / 2, 1.6f, hh}, c);
        }
        gfx::fill({forward ? gx + size + 2 : gx - 8, gy - size * 0.55f, 6, size * 1.1f}, c, 2);
    };
    const bool has_prev = !r.episodes.empty() && r.episodes.front().episode != r.episode;
    skip(mid - 150, ty, 30, false, has_prev ? kText : kText3);
    glass_panel({mid - 44, ty - 44, 88, 88}, 44, 1.f, false, 1.f);   /* the play button: a drop of glass */
    if (st.paused || !st.started)
        play_glyph(mid - 12, ty, 34, kText);
    else
        pause_glyph(mid, ty, 32, kText);
    skip(mid + 120, ty, 30, true, r.has_next ? kText : kText3);
    gfx::text(mid - 135, ty + 70, "L1", {gfx::SemiBold, 18}, kText3, 1);
    gfx::text(mid + 135, ty + 70, "R1", {gfx::SemiBold, 18}, kText3, 1);

    if (r.has_next && !r.next.title.empty())
        gfx::text(x, H - 90, T("Neste: ") + r.next.title, {gfx::Medium, 24, w}, kText3);
}

/* L3: Jellyfin's "Playback Info" for this player, top right over the picture:
 * how Jellyfin serves it, then the stream, video, and audio as the player sees them. */
void PlayerUi::draw_stats(const NuvioStatus &st)
{
    const float a = a_stats.value;
    if (a <= 0.01f)
        return;
    std::vector<std::pair<std::string, std::string>> rows;
    rows.push_back({"#" + std::string(T("Avspilling")), ""});
    const std::string &m = m_req->play_method;
    rows.push_back({T("Metode"), m == "DirectPlay" ? T("Direktespilling")
                                 : m == "DirectStream" ? T("Direktestrøm")
                                 : m == "Transcode" ? T("Transkodet av serveren") : m});
    if (!m_req->transcode_reasons.empty())
        rows.push_back({T("Hvorfor"), m_req->transcode_reasons});
    rows.insert(rows.end(), st.stats.begin(), st.stats.end());

    const float w = 640, lh = 34, head = 46;
    float h = 40;
    for (const auto &r : rows)
        h += r.first[0] == '#' ? head : lh;
    const gfx::Rect pr{W - kPad - w + (1.f - a) * 40, 90, w, h};
    gfx::push_opacity(a);
    glass_panel(pr, 24, 1.f);
    float y = pr.y + 20;
    for (const auto &r : rows) {
        if (r.first[0] == '#') {
            y += head;
            gfx::text(pr.x + 32, y - 10, r.first.substr(1), {gfx::Bold, 22}, kText);
            continue;
        }
        y += lh;
        gfx::text(pr.x + 32, y - 8, r.first, {gfx::Medium, 20, 170}, kText3);
        gfx::text(pr.x + 210, y - 8, r.second, {gfx::Medium, 20, w - 242}, kText2);
    }
    gfx::pop_opacity();
}

/* Lyrics in a column between top and bottom: timed lines follow the song with the
 * one being sung bright and large, the rest dim; untimed lyrics are simply shown. */
void PlayerUi::draw_lyrics(const NuvioStatus &st, float x, float w, float top, float bottom)
{
    const std::vector<NuvioLyric> &ly = m_req->lyrics;
    const bool timed = ly.front().start >= 0;
    int cur = -1;
    if (timed)
        for (size_t i = 0; i < ly.size(); i++)
            if (ly[i].start >= 0 && ly[i].start <= st.position + 0.15)
                cur = (int)i;
    const float lh = 58, mid = top + (bottom - top) * 0.38f;
    m_lyric_scroll.to(timed ? (float)std::max(cur, 0) * lh : 0.f);
    m_lyric_scroll.step(m_dt, 6.f);   /* the column glides up a line */
    if (m_lyric_scroll.value != m_lyric_scroll.target)
        m_dirty = true;
    gfx::push_scissor({x - 20, top, w + 40, bottom - top});
    for (size_t i = 0; i < ly.size(); i++) {
        const float y = (timed ? mid : top + 50) + i * lh - m_lyric_scroll.value;
        if (y < top - lh || y > bottom + lh)
            continue;
        /* Fade towards the edges of the column. */
        const float edge = std::min(y - top, bottom - y) / 90.f;
        const float a = std::max(0.f, std::min(1.f, edge));
        if (!timed) {
            gfx::text(x, y, ly[i].text, {gfx::SemiBold, 34.f, w}, alpha(kText2, a));
            continue;
        }
        /* The sung line lights up as the column reaches it, the last one dims as it
         * leaves: brightness follows the eased scroll, so it glides, not jumps. */
        const float near = 1.f - std::min(1.f, std::fabs(i * lh - m_lyric_scroll.value) / lh);
        const float lit = (int)i == cur ? near : near * 0.35f;
        const gfx::TextStyle ts{gfx::Bold, 38.f, w};
        if ((int)i == cur && !ly[i].cues.empty()) {
            /* Word by word (Jellyfin's cues): the line dim, each word lighting up as
             * it is sung, over a short fade in. */
            gfx::text(x, y, ly[i].text, ts, alpha(kText, a * 0.36f));
            for (const NuvioLyric::Cue &c : ly[i].cues) {
                const float on = std::max(0.f, std::min(1.f, (float)(st.position + 0.1 - c.start) / 0.18f));
                if (on <= 0.f)
                    continue;
                const float wx = x + gfx::text_width(ly[i].text.substr(0, c.from), ts);
                gfx::text(wx, y, ly[i].text.substr(c.from, c.to - c.from), ts, alpha(kText, a * on * near));
            }
            continue;
        }
        gfx::text(x, y, ly[i].text, ts, alpha(kText, a * (0.36f + 0.64f * lit)));
    }
    gfx::pop_scissor();
}

void PlayerUi::draw(const NuvioStatus &st)
{
    if (!m_req)
        return;
    const float dt = (float)std::min(0.1, std::max(0.0, st.now - m_last));
    m_last = st.now;
    m_dt = dt;
    m_dirty = false;
    art::tick();   /* the player's loop owns the frame: uploads and eviction run here */

    const bool overlay = m_overlay != Overlay::None;
    a_loading.to(st.started || !st.error.empty() || m_music ? 0.f : 1.f);
    a_loading.step(dt, 8.f);
    a_controls.to((m_controls || m_seeking || st.paused) && !overlay ? 1.f : 0.f);
    a_controls.step(dt, 12.f);
    a_overlay.to(overlay ? 1.f : 0.f);
    a_overlay.step(dt, 12.f);
    a_skip.step(dt, 12.f);
    a_next.step(dt, 10.f);
    a_error.step(dt, 10.f);
    a_spinner.to(st.started && st.buffering ? 1.f : 0.f);
    a_spinner.step(dt, 10.f);
    a_toast.to(st.now < m_toast_until ? 1.f : 0.f);
    a_toast.step(dt, 10.f);
    a_flash.to(0.f);
    a_flash.step(dt, 4.f);
    m_ep_scroll.step(dt, 12.f);

    if (m_music) {
        draw_music(st);
    } else {
        draw_loading(st);
        draw_controls(st);
        if (!overlay)
            draw_skip_next(st);
    }

    if (a_flash.value > 0.01f) {   /* play / pause, flashed in the centre */
        const float k = 0.85f + 0.15f * a_flash.value, d = 140 * k;
        gfx::push_opacity(a_flash.value);
        gfx::fill({W / 2 - d / 2, H / 2 - d / 2, d, d}, 0x8c000000u, d / 2);
        if (m_flash_icon == "pause")
            pause_glyph(W / 2, H / 2, 54 * k, kText);
        else
            play_glyph(W / 2 - 18 * k, H / 2, 54 * k, kText);
        gfx::pop_opacity();
    }

    if (a_spinner.value > 0.01f) {   /* twelve dots chasing round */
        gfx::push_opacity(a_spinner.value);
        for (int i = 0; i < 12; i++) {
            const float ang = (float)i / 12.f * 6.2832f;
            const float phase = std::fmod((float)st.now * 1.2f + 1.f - (float)i / 12.f, 1.f);
            gfx::fill({W / 2 + std::cos(ang) * 34 - 5, H / 2 + std::sin(ang) * 34 - 5, 10, 10},
                      alpha(kText, 0.2f + 0.8f * (1.f - phase)), 5);
        }
        gfx::pop_opacity();
    }

    /* Overlays: the picture dims, a glass panel rises into place. */
    const float oa = smoothstep(a_overlay.value);
    if (oa > 0.01f) {
        gfx::fill({0, 0, W, H}, alpha(0x8c000000u, oa));
        gfx::push_opacity(1.f);
        if (m_overlay_drawn == Overlay::Tracks)
            draw_tracks(st, oa);
        else if (m_overlay_drawn == Overlay::Episodes)
            draw_episodes(oa, dt);
        else if (m_overlay_drawn == Overlay::Chapters)
            draw_chapters(st, oa, dt);
        gfx::pop_opacity();
    }

    a_stats.step(dt, 12.f);
    draw_stats(st);

    if (a_toast.value > 0.01f && !m_toast.empty()) {
        const gfx::TextStyle ts{gfx::SemiBold, 22};
        const float w = gfx::text_width(m_toast, ts) + 60;
        const gfx::Rect r{W / 2 - w / 2, 50 - (1.f - a_toast.value) * 20, w, 58};
        gfx::push_opacity(a_toast.value);
        glass(r, 1.f);
        gfx::text(W / 2, r.y + 38, m_toast, ts, kText, 1);
        gfx::pop_opacity();
    }
    draw_error(st);
}
} // namespace ui
