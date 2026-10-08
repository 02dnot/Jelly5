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
#include "app/i18n_cldr.h"
#include "jelly5_playback.h"
#include "evo_boot_trace.h"
#include "gfx/art.h"
#include "gfx/gfx.h"
#include "nuvio_subs.h"
#include "ui/screen.h"
#include "ui/livetv.h"

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

/* Language names in the interface's language (CLDR's), the player's own English
 * names for a code CLDR lacks. */
std::string language_name(const std::string &code)
{
    if (code.empty() || code == "und")
        return T("Ukjent språk");
    std::string n = i18n::language_name(code);
    if (n.empty())
        n = nuvio_language_name(code);
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

void PlayerUi::begin(const NuvioRequest *req, double now, bool reopen)
{
    /* A reopen (a reconnect, the software decoder) is the same playback: a
     * dismissed card, the skips done and the group's pending commands stay.
     * m_group_ready does not: Ready again re-syncs the group after the gap. */
    PlayerUi kept;
    if (reopen) {
        kept.m_card_dismissed = m_card_dismissed;
        std::copy(std::begin(m_skip_done), std::end(m_skip_done), kept.m_skip_done);
        std::copy(std::begin(m_skip_since), std::end(m_skip_since), kept.m_skip_since);   /* no button again for 8 s */
        kept.m_scheduled.swap(m_scheduled);
        kept.m_still = m_still;
        kept.m_asking = m_asking;   /* the question stays up across the gap */
        kept.m_ask_hold = m_ask_hold;
        kept.a_ask.snap(m_asking ? 1.f : 0.f);
    }
    *this = std::move(kept);
    m_req = req;
    m_music = req && req->item_type == "audio";
    m_live = req && req->live;
    m_channel = m_live && req->item_type == "live";
    if (m_live) {
        m_guide = livetv::guide();
        m_guide_version = livetv::version();
    }
    if (req && !reopen) {   /* "Ser du fortsatt på?": the run so far, from the episode before */
        m_still.limit_episodes = req->prefs.still_watching_episodes;
        m_still.limit_seconds = req->prefs.still_watching_seconds;
        m_still.count = req->autoplay_count;
        m_still.idle = req->autoplay_idle;
    }
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

segments::Action PlayerUi::action_of(int type) const
{
    if (!m_req || type < 0 || type >= segments::TypeCount)
        return segments::Nothing;
    const int a = m_req->prefs.segment[type];
    return a >= 0 && a < segments::ActionCount ? (segments::Action)a : segments::Nothing;
}

/* The next-episode card can come up at the credits (when there is one to play on
 * to): then it, not a button, is what "Spør" offers there (#32). */
bool PlayerUi::card_possible() const
{
    return m_req && !m_music && m_req->has_next && m_req->prefs.autoplay_next && !in_group();
}

/* Where a skip lands: the segment's end, short of the file's end (credits that
 * run to the end: the last moment plays and playback ends as it would). */
double PlayerUi::skip_target(const NuvioStatus &st, int i) const
{
    const double end = m_req->skips[i].end;
    return st.duration > 0 ? std::min(end, st.duration - 1.0) : end;
}

/* The segment whose "Hopp over" button is up, or -1: one set to "Spør", or one
 * to skip that the viewer seeked into (segments::decide). The credits have the
 * next-episode card when it can come; without it (a film, the last episode,
 * autoplay off, a group) they get the button too. It hides 8 s after it came
 * up (Android TV), and is there again while the controls are up. */
int PlayerUi::current_skip(const NuvioStatus &st) const
{
    if (!m_req || !st.started || m_music)
        return -1;
    for (size_t i = 0; i < m_req->skips.size() && i < 16; i++) {
        const NuvioSkip &k = m_req->skips[i];
        const int t = segments::type_of(k.type);
        if (t < 0 || m_skip_done[i] || st.position < k.start || st.position >= k.end - segments::kEndMargin)
            continue;
        if (t == segments::Outro && card_possible())
            continue;   /* the next-episode card covers the end */
        if (next_card(st) && k.start >= card_start(st))
            continue;   /* a preview after the credits: the card has the spot and ✕ */
        if (segments::decide(action_of(t), k.end - k.start, false) != segments::Ask)
            continue;
        if (m_skip_since[i] >= 0 && st.now - m_skip_since[i] >= segments::kAskHide && !m_controls)
            continue;
        return (int)i;
    }
    return -1;
}

/* Each tick: a segment the position has left is forgotten (playback reaching it
 * again asks or skips again, as on Android TV); one that playback ran into and is
 * set to "Hopp over automatisk" is skipped - never one the viewer scrubbed or
 * seeked into (a jump between two ticks), nor while scrubbing. In a group the
 * seek is the group's (tick's to_group). */
void PlayerUi::segment_tick(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    if (!st.started || m_music)
        return;
    const double pos = st.position;
    double prev = m_prev_pos;
    if (prev < 0)   /* the first tick: from the very start, playback runs into a segment at 0 */
        prev = segments::first_prev(pos, m_req->start_position);
    m_prev_pos = pos;
    /* Our own skip (automatic or ✕) jumps: where it lands still counts as played
     * into (a recap right before the intro). Forgotten once there, or after 10 s
     * (a group's seek takes a moment). */
    const double target = m_auto_target;
    if (m_auto_target >= 0 && (std::fabs(pos - m_auto_target) <= segments::kLandSlack || st.now - m_auto_at > 10.0))
        m_auto_target = -1;
    for (size_t i = 0; i < m_req->skips.size() && i < 16; i++) {
        const NuvioSkip &k = m_req->skips[i];
        const int t = segments::type_of(k.type);
        if (t < 0 || pos < k.start || pos >= k.end - segments::kEndMargin) {
            m_skip_done[i] = false;
            m_skip_since[i] = -1;
            continue;
        }
        if (m_skip_since[i] < 0)
            m_skip_since[i] = st.now;
        const bool natural = segments::entered_naturally(prev, pos, k.start) || segments::landed_into(target, pos, k.start);
        if (m_skip_done[i] || m_seeking || !natural || segments::decide(action_of(t), k.end - k.start, true) != segments::Skip)
            continue;
        if (t != segments::Outro && next_card(st) && k.start >= card_start(st))
            continue;   /* a preview after the credits: the card has the spot */
        m_skip_done[i] = true;
        evo_bt("jelly5: skipping the %s (%.0f-%.0f s)", k.type.c_str(), k.start, k.end);
        if (t == segments::Outro && card_possible() && k.end >= st.duration - 1.0) {
            m_card_dismissed = true;   /* credits to the end: the next episode now */
            autoplay_next(st, out, false);
        } else {
            out.push_back({OsdCmd::SeekTo, skip_target(st, (int)i)});
            m_auto_target = skip_target(st, (int)i);
            m_auto_at = st.now;
        }
    }
}

bool PlayerUi::has_next() const
{
    if (!m_req)
        return false;
    return m_music ? jelly5_music_has_next(m_req->has_next) : m_req->has_next;
}

/* Where the credits start (Jellyfin's Outro segment, Emby's CreditsStart
 * marker; not when Rulletekst is set to Ingenting); with none, the old rule: the user's threshold or the last 45 s. */
double PlayerUi::card_start(const NuvioStatus &st) const
{
    double start = -1;
    for (const NuvioSkip &k : m_req->skips)
        /* Credits that end near the end of this file: not a marker past its end
         * (another cut), nor one in the middle (a detection gone wrong). */
        if (segments::type_of(k.type) == segments::Outro && action_of(segments::Outro) != segments::Nothing &&
            k.start < st.duration - 1.0 &&
            k.end >= st.duration - 180.0 && (start < 0 || k.start < start))
            start = k.start;
    if (start >= 0)
        return start;
    const NuvioPrefs &p = m_req->prefs;
    const double rule = p.next_by_minutes
                            ? st.duration - p.next_minutes * 60.0
                            : std::min(st.duration * std::min(99.0, p.next_percent) / 100.0, st.duration - 45.0);
    return std::max(rule, st.duration * 0.5);   /* a short file is watched first */
}

/* The next-episode card: from the credits to the end, only when the next
 * episode plays by itself, never for the last one. */
bool PlayerUi::next_card(const NuvioStatus &st) const
{
    if (!m_req || m_music || !m_req->has_next || !m_req->prefs.autoplay_next || m_card_dismissed || !st.started ||
        st.duration <= 0 || in_group())   /* in a group, the group's queue goes on (playback_ended) */
        return false;
    return st.position >= card_start(st);
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
    if (m_live) {   /* a channel: no episodes, chapters, speed or next; the channels instead */
        std::vector<Button> b{Button::PlayPause, Button::Tracks};
        if (!m_channel)
            return b;   /* (a recording still being made) */
        b.insert(b.begin() + 1, Button::Channels);
        const std::string prev = livetv::previous_channel();
        if (!prev.empty() && m_req && prev != m_req->id && m_guide && m_guide->channel(prev))
            b.push_back(Button::PrevChannel);
        return b;
    }
    std::vector<Button> b{Button::PlayPause};
    if (m_req && m_req->episodes.size() > 1)
        b.push_back(Button::Episodes);
    if (m_req && m_req->chapters.size() > 1)
        b.push_back(Button::Chapters);
    b.push_back(Button::Tracks);
    b.push_back(Button::Speed);
    if (has_next())
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
        m_subs_seen = nuvio_subs_count();
    } else if (o == Overlay::Channels) {
        livetv::refresh();   /* what airs on each, fresh when it is old */
        m_ch_index = std::max(0, playing_channel());
        m_ch_scroll.snap(-1);   /* placed on the first draw */
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
    if (m_live) {   /* a channel airs where it airs: the controls come up, nothing moves */
        show_controls(now, Zone::Buttons);
        return;
    }
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

void PlayerUi::playback_ended(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    /* In a group: its queue goes on, not this one. Ask the group for its next entry
     * (jellyfin-web does at an end; the server takes one request per entry) and close;
     * the next entry comes as the group's Play. */
    if (in_group()) {
        m_group_end = true;   /* nor does the music queue here play on (jelly5_playback) */
        if (m_req && (m_req->prefs.autoplay_next || m_music) && syncplay::queue_has_next()) {
            syncplay::request_next();
            m_group_handoff = true;
        }
        out.push_back({OsdCmd::Stop});
        return;
    }
    /* An album always plays on; episodes follow the autoplay setting. */
    if (has_next() && (m_req->prefs.autoplay_next || m_music))
        autoplay_next(st, out, true);
    else
        out.push_back({OsdCmd::Stop});
}

/* Issue #24: after a run of episodes that played on by themselves (Innstillinger:
 * Spør om du fortsatt ser på), the next one waits for the viewer: the picture stays
 * paused on this one's end with the question over it. Never for music, nor in a
 * SyncPlay group (the group decides). */
void PlayerUi::autoplay_next(const NuvioStatus &, std::vector<OsdCommand> &out, bool ended)
{
    if (m_music || in_group() || !m_still.ask()) {
        if (!m_music && !in_group())
            m_still.autoplayed();
        out.push_back({OsdCmd::PlayNext});
        return;
    }
    if (m_asking)
        return;
    evo_bt("jelly5: still watching? after %d autoplayed episode(s), %.0f s without a press", m_still.count,
           m_still.idle);
    m_asking = true;
    m_card_dismissed = true;
    m_controls = false;
    m_seeking = false;
    m_overlay = Overlay::None;
    m_dirty = true;
    m_ask_hold = !ended;   /* held where the countdown ran out (tick keeps it paused) */
    m_ask_paused_at = -1;
}

/* ✕ (any button but ○): the next episode, the run starts again; ○: stop here. */
void PlayerUi::answer_still(bool go, std::vector<OsdCommand> &out)
{
    m_asking = false;
    m_dirty = true;
    if (go) {
        m_still.press();
        out.push_back({OsdCmd::PlayNext});
    } else {
        out.push_back({OsdCmd::Stop});
    }
}

void PlayerUi::tick(const NuvioStatus &st, std::vector<OsdCommand> &out, bool poll_remote)
{
    const double step = std::max(0.0, st.now - m_now);   /* since the last tick */
    m_now = st.now;
    if (!m_req)
        return;
    if (poll_remote)
        remote_poll(st, out);
    if (st.started && !st.paused && !st.buffering && !m_asking && !m_music)
        m_still.played(step);   /* time played since the last press */
    /* Asked where the countdown ran out: paused under the question, also after a
     * reopen (a reconnect opens it playing). Once per half second, as the pause
     * shows in the status only once the engine has taken it. */
    if (m_asking && m_ask_hold && st.started && !st.paused && st.now - m_ask_paused_at > 0.5) {
        out.push_back({OsdCmd::TogglePause});
        m_ask_paused_at = st.now;
    }
    const size_t mine = out.size();   /* from here on this PS5's own, the group's in a group */
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
    segment_tick(st, out);   /* skips what is set to be skipped (Innstillinger) */
    /* A skip button or the next-episode card that appears takes the focus:
     * controls that came up by themselves step aside, unless the viewer has been
     * pressing something in the last 2 s. */
    {
        const int k = current_skip(st);
        const bool card = next_card(st);
        /* (a button back with the controls after its 8 s has not arrived) */
        const bool arrived = (k != m_skip_seen && k >= 0 && st.now - m_skip_since[k] < segments::kAskHide) ||
                             (card && !m_card_seen);
        if (k != m_skip_seen || card != m_card_seen)
            m_dirty = true;   /* (a button hiding after its 8 s, too) */
        m_skip_seen = k;
        m_card_seen = card;
        if (arrived && m_controls && m_zone == Zone::Buttons && m_overlay == Overlay::None && !st.paused &&
            st.now - m_input_at > 2.0) {
            m_controls = false;
            m_dirty = true;
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
    /* A card put away comes back when the credits are reached again (a seek back). */
    if (m_card_dismissed && m_req && st.started && st.duration > 0 && st.position < card_start(st))
        m_card_dismissed = false;
    /* The next-episode countdown (10 s). */
    if (next_card(st)) {
        if (m_card_since < 0)
            m_card_since = st.now;
        else if (st.paused || m_overlay != Overlay::None || m_seeking || (m_controls && m_zone == Zone::Buttons))
            m_card_since += step;   /* held while paused, scrubbing, in the buttons, or the card is hidden */
        if (m_req->prefs.autoplay_next && st.now - m_card_since >= 10.0 && !st.paused) {
            m_card_dismissed = true;
            autoplay_next(st, out, false);
        }
    } else {
        m_card_since = -1;
    }
    if (in_group())
        to_group(st, out, mine);
}

/* Subtitle tracks can arrive while Lyd og undertekster is open (the fetch, a
 * download): a focus on Tilpass or Søk, under the tracks, moves down with them. */
void PlayerUi::keep_sub_rows()
{
    const int ns = nuvio_subs_count();
    if (m_rows[1] > m_subs_seen)
        m_rows[1] = std::max(0, m_rows[1] + ns - m_subs_seen);
    m_subs_seen = ns;
}

/* Lyd og undertekster: columns 0 audio, 1 subtitles (+ "Tilpass"), 2 style. */
void PlayerUi::tracks_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    keep_sub_rows();
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
            if (r != st.audio_active)   /* the playing track: nothing to reload */
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
            c.video_id = e.video_id;
            out.push_back(c);
            m_overlay = Overlay::None;
        }
    }
}

void PlayerUi::end()
{
    /* Closed while a group item played (not for the group's next one): the group
     * no longer waits for this PS5, until it plays the group's queue again. */
    if (m_group_ready && !m_group_handoff)
        syncplay::player_stopped();
    m_req = nullptr;
}

void PlayerUi::input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const size_t first = out.size();
    if (in.pressed) {
        m_input_at = st.now;
        m_still.press();   /* awake: the run of autoplayed episodes starts again */
    }
    if (m_asking) {   /* "Ser du fortsatt på?": ○ stops, anything else plays on */
        if (in.pressed & ~in.repeats)
            answer_still(!(in.pressed & NUVIO_BTN_CIRCLE), out);
        return;
    }
    input_local(in, st, out);
    if (in_group())
        to_group(st, out, first);
}

/* In a group: pause, seek and next (from out[first] on) are asked of the group
 * instead of done here, whether the viewer or a phone asked. */
void PlayerUi::to_group(const NuvioStatus &st, std::vector<OsdCommand> &out, size_t first)
{
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
    if (!m_req || m_music || m_live || !m_seeking || l == r || !st.error.empty() || m_overlay != Overlay::None ||
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
        if (m_channel && (p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)))   /* a channel that will not open: on to the next */
            zap_step((p & NUVIO_BTN_R1) ? 1 : -1, out);
        else if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE))
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
    if (m_overlay == Overlay::Channels) {
        channels_input(p, out);
        return;
    }
    if (m_live) {
        if (p & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) {   /* the channel before / after, as on a TV remote */
            if (m_channel)
                zap_step((p & NUVIO_BTN_R1) ? 1 : -1, out);
            return;
        }
        if (p & (NUVIO_BTN_L2 | NUVIO_BTN_R2))
            return;   /* nothing to scrub */
        if ((p & NUVIO_BTN_TRIANGLE) && m_channel) {
            open_overlay(Overlay::Channels);
            return;
        }
        if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
            m_zone = Zone::Buttons;   /* (no bar to stand on) */
            show_controls(now, Zone::Buttons);
            return;
        }
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
        const double last = st.duration > 0 ? st.duration - 1 : 1e9;   /* no duration yet: no end */
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
                out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(last, ch[to].start))});
                if (!ch[to].name.empty())
                    toast(ch[to].name, now);
            }
        } else {   /* no chapters: quick jumps, as on Netflix, 10 s back / forward */
            const double to = from + (forward ? 10.0 : -10.0);
            out.push_back({OsdCmd::SeekTo, std::max(0.0, std::min(last, to))});
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
            out.push_back({OsdCmd::SeekTo, skip_target(st, skip)});
            m_auto_target = skip_target(st, skip);   /* a segment right after it is played into */
            m_auto_at = now;
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
            case Button::Channels: open_overlay(Overlay::Channels); break;
            case Button::PrevChannel: zap(livetv::previous_channel(), out); break;
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
                        a_stats.value != a_stats.target || a_ask.value != a_ask.target ||
                        m_ch_scroll.value != m_ch_scroll.target;
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
    /* The segments, under the played part (concept .bar .seg), a calm colour each:
     * intro blue, credits green, recap violet, preview amber, commercials rose. */
    static const uint32_t kSegColour[segments::TypeCount] = {0x8c00a4dcu, 0x8c4fbf8cu, 0x8caa5cc3u, 0x8cd9a441u,
                                                             0x8cd96c7au};
    for (const NuvioSkip &k : m_req->skips) {
        const int t = segments::type_of(k.type);
        if (t < 0)
            continue;
        const float sx = x0 + w * (float)(k.start / d), ex = x0 + w * (float)(std::min(k.end, d) / d);
        gfx::fill({sx, y, std::max(2.f, ex - sx), h}, alpha(kSegColour[t], a), 0);
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
            const int i = tp.index_at(pos);
            const std::string url = tp.sheet_url(i / per);
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
    const int i = tp.index_at(pos);
    const std::string url = tp.sheet_url(i / per);
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
            const float pw = gfx::text_width(T("SPILLER NÅ"), {gfx::Bold, 16}) + 32;
            gfx::fill({hx + 14, y + 24, pw, 30}, alpha(0xe600a4dcu, a), 15);
            gfx::text(hx + 14 + pw / 2, y + 46, T("SPILLER NÅ"), {gfx::Bold, 16}, alpha(kText, a), 1);
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

/* "Ser du fortsatt på?": the picture dims, a glass panel in the centre with the
 * question, the episode waiting, and what ✕ and ○ do. */
void PlayerUi::draw_still(const NuvioStatus &)
{
    const float a = smoothstep(a_ask.value);
    if (a <= 0.01f)
        return;
    gfx::fill({0, 0, W, H}, alpha(0x8c000000u, a));
    const gfx::TextStyle ts{gfx::Bold, 44}, es{gfx::Medium, 26, W - 2 * kPad - 112};
    std::string ep;
    if (m_req->has_next) {
        const NuvioEpisode &n = m_req->next;
        char b[64] = "";
        if (n.season > 0 && n.episode > 0)
            std::snprintf(b, sizeof b, "S%d:E%d", n.season, n.episode);
        ep = b;
        if (!n.title.empty())
            ep += (ep.empty() ? "" : " \xC2\xB7 ") + n.title;
    }
    const std::vector<PadHint> hints{{PadButton::Cross, T("Fortsett å se")}, {PadButton::Circle, T("Stopp")}};
    const float hw = pad_hint_width(PadButton::Cross, T("Fortsett å se"), 26) + 26 * 0.9f +
                     pad_hint_width(PadButton::Circle, T("Stopp"), 26);
    const float w = std::min(W - 2 * kPad, std::max({760.f, gfx::text_width(T("Ser du fortsatt på?"), ts) + 112,
                                                     ep.empty() ? 0.f : gfx::text_width(ep, es) + 112, hw + 112}));
    const float h = ep.empty() ? 236.f : 284.f;
    const gfx::Rect r{W / 2 - w / 2, H / 2 - h / 2 + (1.f - a) * 24, w, h};   /* rises into place */
    gfx::push_opacity(a);
    glass(r, 1.f);
    float y = r.y + 100;
    gfx::text(W / 2, y, T("Ser du fortsatt på?"), {gfx::Bold, 44, w - 112}, kText, 1);
    if (!ep.empty()) {
        y += 52;
        gfx::text(W / 2, y, ep, {gfx::Medium, 26, w - 112}, kText2, 1);
    }
    draw_pad_hints(W / 2, r.y + r.h - 58, hints, 1, 26);
    gfx::pop_opacity();
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

    if (m_live) {
        draw_live_info(st, a);
    } else {
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
    }

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
        case Button::Channels: return T("Kanaler");
        case Button::PrevChannel: return T("Forrige kanal");
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
        case Button::Channels: label = T("Kanaler"); break;
        case Button::PrevChannel: label = T("Forrige kanal"); break;
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
        case Button::Channels:   /* a screen with its stand */
            gfx::fill({ix, cy - 11, 30, 19}, fg, 4);
            gfx::fill({ix + 3, cy - 8, 24, 13}, alpha(0xff141418u, a), 2);
            gfx::fill({ix + 10, cy + 10, 10, 3}, fg, 1.5f);
            break;
        case Button::PrevChannel:   /* two chevrons back (the speed button's, turned round) */
            for (int k2 = 0; k2 < 2; k2++)
                for (int s2 = 0; s2 < 9; s2++) {
                    const float yy = s2 < 5 ? (float)s2 : (float)(8 - s2);
                    gfx::fill({ix + 22 - k2 * 11 - yy * 1.8f, cy - 9 + s2 * 2.2f, 3, 3}, fg, 1.5f);
                }
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
    if (m_live) {
        ui::draw_tuning(m_req->id, m_req->title, 1.f);   /* (the frame show_tuning put up, on) */
    } else {
        const gfx::Rect full{0, 0, W, H};
        gfx::fill(full, 0xff080b10u);
        if (const gfx::Texture *t = art::get(m_req->backdrop, 1920, 1080))
            gfx::image(full, t, 0.92f, 0, true);
        gfx::fill_vgradient({0, 0, W, 378}, 0x4d000000u, 0x99000000u);
        gfx::fill_vgradient({0, 378, W, 378}, 0x99000000u, 0xcc000000u);
        gfx::fill_vgradient({0, 756, W, 324}, 0xcc000000u, 0xe6000000u);
    }
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
    /* Hopp over intro / rulletekst / ...: a glass pill, Cross acts (concept .skip.focus). */
    const int k = current_skip(st);
    a_skip.to(k >= 0 ? 1.f : 0.f);
    if (a_skip.value > 0.01f) {
        static std::string label;   /* kept while it fades out (no T() in a static initializer) */
        if (label.empty())
            label = T("Hopp over intro");
        if (k >= 0)
            switch (segments::type_of(m_req->skips[k].type)) {
            case segments::Outro: label = T("Hopp over rulletekst"); break;
            case segments::Recap: label = T("Hopp over oppsummering"); break;
            case segments::Preview: label = T("Hopp over forhåndsvisning"); break;
            case segments::Commercial: label = T("Hopp over reklame"); break;
            default: label = T("Hopp over intro"); break;
            }
        /* As every control: glass, and the glass drop when it has the focus, which
         * it has whenever ✕ presses it (the controls hidden, or on the bar). While
         * the viewer moves through the buttons, ✕ is theirs and it has none. */
        const bool focus = k >= 0 && !m_seeking && (!m_controls || m_zone == Zone::Bar);
        const gfx::TextStyle st2{focus ? gfx::Bold : gfx::SemiBold, 26};
        const float w = gfx::text_width(label, {gfx::Bold, 26}) + 72;
        const float y = H - 350 - (1.f - a_skip.value) * 20 + (m_controls ? 0 : 200);
        const gfx::Rect r{W - kPad - w, y, w, 72};
        gfx::push_opacity(a_skip.value);
        glass_panel(r, 14, 1.f, true);
        if (focus)
            m_skip_drop.to(r, k, 0, y);
        else
            m_skip_drop.hide();
        bool moving = false;
        m_skip_drop.draw(m_dt, 1.f, &moving, 14);
        if (moving)
            m_dirty = true;
        gfx::text(r.x + r.w / 2, r.y + 46, label, st2, focus ? kText : kText2, 1);
        gfx::pop_opacity();
    }

    /* Next episode: a glass card with the still, the title and the countdown. */
    const bool card = next_card(st);
    a_next.to(card ? 1.f : 0.f);
    if (a_next.value > 0.01f && m_req->has_next) {
        const NuvioEpisode &n = m_req->next;
        /* 560 wide, wider when the line under the title needs it (a long language). */
        const bool counting = m_req->prefs.autoplay_next && m_card_since >= 0;
        char c[48] = "";
        if (counting) {
            const double left = std::max(0.0, 10.0 - (st.now - m_card_since));
            std::snprintf(c, sizeof c, T("Spilles om %d s"), (int)std::ceil(left));
        }
        const float line_w = counting ? gfx::text_width(T("Spilles om %d s"), {gfx::Medium, 20}) + 18 +
                                            pad_hint_width(PadButton::Cross, T("Nå"), 24)
                                      : pad_hint_width(PadButton::Cross, T("Spill av"), 24) + 24 * 0.9f +
                                            pad_hint_width(PadButton::Circle, T("Se rulletekst"), 24);
        const float cw_card = std::min(std::max(560.f, 270 + line_w + 8), W - 2 * kPad);
        const gfx::Rect r{W - kPad - cw_card, H - 440 - (1.f - a_next.value) * 20 + (m_controls ? 0 : 290), cw_card,
                          156};
        /* As the skip button: the glass drop when ✕ plays it (the controls
         * hidden, or on the bar); while the viewer moves through the buttons, ✕
         * is theirs, and the card's ✕ hints step back. It keeps the last state
         * while it fades out. */
        if (card)
            m_card_focus = !m_seeking && (!m_controls || m_zone == Zone::Bar);
        const bool focus = m_card_focus;
        gfx::push_opacity(a_next.value);
        glass(r, 1.f);
        if (focus)
            m_card_drop.to(r, 0, 0, r.y);
        else
            m_card_drop.hide();
        bool moving = false;
        m_card_drop.draw(m_dt, 1.f, &moving, std::min(28.f, r.h / 2));
        if (moving)
            m_dirty = true;
        art::draw({r.x + 18, r.y + 18, 213, 120}, n.thumbnail, n.blurhash, 480, 270, 10);
        const float tx = r.x + 250;
        gfx::text(tx, r.y + 44, T("NESTE EPISODE"), {gfx::Bold, 17}, kText3);
        char title[256];
        std::snprintf(title, sizeof title, "S%d:E%d \xC2\xB7 %s", n.season, n.episode, n.title.c_str());
        gfx::text(tx, r.y + 80, title, {gfx::Bold, 24, r.w - 270}, kText);
        if (counting) {
            const double left = std::max(0.0, 10.0 - (st.now - m_card_since));
            const float cw = gfx::text(tx, r.y + 116, c, {gfx::Medium, 20}, kText2);
            gfx::push_opacity(focus ? 1.f : 0.4f);
            draw_pad_hint(tx + cw + 18, r.y + 109, PadButton::Cross, T("Nå"), 24);
            gfx::pop_opacity();
            gfx::fill({tx, r.y + 132, r.w - 270, 4}, 0x33ffffffu, 2);
            gfx::fill({tx, r.y + 132, (r.w - 270) * (float)(1.0 - left / 10.0), 4}, kAccent, 2);
        } else {
            gfx::push_opacity(focus ? 1.f : 0.4f);
            draw_pad_hints(tx, r.y + 109, {{PadButton::Cross, T("Spill av")}, {PadButton::Circle, T("Se rulletekst")}}, 0,
                           24);
            gfx::pop_opacity();
        }
        gfx::pop_opacity();
    }
}

/* Lyd og undertekster: a glass panel with an audio column and a subtitle
 * column; "Tilpass undertekster" opens style and timing in a third. */
void PlayerUi::draw_tracks(const NuvioStatus &st, float a)
{
    keep_sub_rows();
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
            /* The tags on the right keep their width; the name gives way (at most half the column). */
            const float rw = right.empty() ? 0
                                           : std::min(gfx::text_width(right, {gfx::Medium, 20}) + 20, widths[c] / 2);
            gfx::text(lx, y + 40, label, {selected ? gfx::Bold : gfx::Medium, 25, widths[c] - 60 - rw}, alpha(fg, a));
            if (!right.empty())
                gfx::text(cols[c] + widths[c] - 18, y + 39, right, {gfx::Medium, 20, widths[c] / 2 - 20},
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
            if (right.empty() && x.downloads > 0) right = TN(x.downloads, "%d nedl.", "%d nedl.");
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
            const float pw = gfx::text_width(T("SPILLER NÅ"), {gfx::Bold, 16}) + 32;
            gfx::fill({hx + 14, y + 24, pw, 30}, alpha(0xe600a4dcu, a), 15);
            gfx::text(hx + 14 + pw / 2, y + 46, T("SPILLER NÅ"), {gfx::Bold, 16}, alpha(kText, a), 1);
        } else if (e.watched) {
            const float bw = gfx::text_width(T("Sett"), {gfx::SemiBold, 17}) + 24;
            gfx::fill({hx + 14, y + 24, bw, 30}, alpha(0x33ffffffu, a), 15);
            gfx::text(hx + 14 + bw / 2, y + 46, T("Sett"), {gfx::SemiBold, 17}, alpha(kText, a), 1);
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
        if (has_next())
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
    for (size_t i = 0; i < m_req->episodes.size() && here < 0; i++)   /* by id: numbers can repeat */
        if (m_req->episodes[i].video_id == m_req->id)
            here = (int)i;
    for (size_t i = 0; i < m_req->episodes.size() && here < 0; i++)
        if (m_req->episodes[i].season == m_req->season && m_req->episodes[i].episode == m_req->episode)
            here = (int)i;
    /* Music: the queue's play order decides which track is before (the album's
     * list lacks what a phone queued); the chain steps back in it. */
    const bool before = m_music ? jelly5_music_has_previous(here > 0) : here > 0;
    if (st.position > 3.0 || !before) {
        out.push_back({OsdCmd::SeekTo, 0.0});
    } else {
        OsdCommand c{OsdCmd::PlayEpisode};
        c.season = here > 0 ? m_req->episodes[here - 1].season : m_req->season;
        c.episode = here > 0 ? m_req->episodes[here - 1].episode : m_req->episode;
        c.video_id = here > 0 ? m_req->episodes[here - 1].video_id : m_req->id;
        out.push_back(c);
    }
}

/* In a group, and this is the group's item (a theme song, say, is not). */
bool PlayerUi::in_group() const { return m_req && !m_req->not_group && syncplay::active(); }

void PlayerUi::remote_poll(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const bool group = in_group();
    /* The group set its queue anew around what plays here: paused, Ready again. */
    if (group && syncplay::ready_asked() && m_group_ready && m_group_seek < 0) {
        m_group_ready = false;
        m_group_stalled = false;
    }
    /* SyncPlay: an opened group item waits paused until the group says go. */
    if (group && st.started && !m_group_ready) {
        m_group_ready = true;
        if (!st.paused)
            out.push_back({OsdCmd::TogglePause});
        syncplay::player_started(st.position, false);
    }
    /* A group seek: play from the new place until the picture moves there, then
     * pause and tell the group (jellyfin-web waits for the player's "playing" so). */
    if (m_group_seek >= 0) {
        /* Landed: near the target, or moved away from where it was further than
         * playing on would take it - a seek lands on a keyframe, maybe seconds off. */
        const double drift = st.position - (m_group_seek_from + (st.now - m_group_seek_since));
        const bool jumped = std::fabs(st.position - m_group_seek_from) > 1.0 && std::fabs(drift) > 1.0;
        if (m_group_seek_landed < 0 && !st.buffering && (std::fabs(st.position - m_group_seek) < 1.0 || jumped))
            m_group_seek_landed = st.position;
        const bool moving = m_group_seek_landed >= 0 && !st.paused && !st.buffering &&
                            st.position > m_group_seek_landed + 0.1;
        if (moving || st.now - m_group_seek_since > 30.0) {
            if (!st.paused)
                out.push_back({OsdCmd::TogglePause});
            syncplay::seeked(st.position);
            m_group_seek = -1;
        }
    } else if (m_group_ready && group && st.buffering != m_group_stalled) {
        /* Stalled while the group plays: the group waits for this PS5 (Buffering)
         * and goes on once it plays again (Ready). */
        m_group_stalled = st.buffering;
        syncplay::buffering(st.buffering, st.position, !st.paused);
    }
    /* A group seek overtaken by another group command (a Pause, say): the group
     * still waits for this PS5's Ready, sent once that command has been done. */
    if (m_group_seek_owed && m_group_seek < 0 && !st.buffering &&
        std::none_of(m_scheduled.begin(), m_scheduled.end(), [](const remote::Command &s) { return s.syncplay; })) {
        m_group_seek_owed = false;
        syncplay::buffering(false, st.position, !st.paused);
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
        if (c.syncplay && !group)
            continue;   /* about the group's item, which is not open here */
        if (c.syncplay) {
            /* The group's latest word replaces what it said before (jellyfin-web
             * clears its scheduled command too): an Unpause timed ahead must not
             * fire after a Pause that came since. */
            m_scheduled.erase(std::remove_if(m_scheduled.begin(), m_scheduled.end(),
                                             [](const remote::Command &s) { return s.syncplay; }),
                              m_scheduled.end());
            if (m_group_seek >= 0 && c.kind != remote::Command::Seek)
                m_group_seek_owed = true;
            m_group_seek = -1;
        }
        if (c.at > st.now + 0.005) {
            m_scheduled.push_back(c);
            continue;
        }
        const bool queued = c.kind == remote::Command::Play && (c.play_command == "PlayNext" || c.play_command == "PlayLast");
        if ((c.kind == remote::Command::Play && !queued) || c.kind == remote::Command::Stop) {
            remote_do(c, st, out);
            return;
        }
        remote_do(c, st, out);
    }
}

void PlayerUi::remote_do(const remote::Command &c, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    m_dirty = true;
    if (!c.syncplay && c.kind != remote::Command::Message) {   /* a phone's command: someone is awake */
        m_still.press();
        if (m_asking) {   /* the question up: play on, or stay paused under it */
            if (c.kind == remote::Command::Unpause || c.kind == remote::Command::PlayPause ||
                c.kind == remote::Command::Next) {
                answer_still(true, out);
                return;
            }
            if (c.kind == remote::Command::Pause || c.kind == remote::Command::Seek ||
                c.kind == remote::Command::Rewind || c.kind == remote::Command::FastForward)
                return;   /* a seek would play on silently under the question */
        }
    }
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
            /* Playing, to the new place; Ready once it plays there (remote_poll). */
            if (st.paused) out.push_back({OsdCmd::TogglePause});
            m_group_seek = std::max(0.0, std::min(d, target));
            out.push_back({OsdCmd::SeekTo, m_group_seek});
            m_group_seek_since = st.now;
            m_group_seek_from = st.position;
            m_group_seek_landed = -1;
            m_group_seek_owed = false;
            m_group_stalled = false;
            return;
        case remote::Command::Stop: out.push_back({OsdCmd::Stop}); return;
        default: return;
        }
    }
    const size_t first = out.size();
    {
        switch (c.kind) {
        case remote::Command::Play:
            /* "Play next" / "Add to queue": into the music queue; with nothing to queue
             * on (a video), not a reason to stop what plays. */
            if (c.play_command == "PlayNext" || c.play_command == "PlayLast") {
                if (!jelly5_music_enqueue(c.item_ids, c.play_command == "PlayNext"))
                    evo_bt("remote: %s ignored: no queue to add to", c.play_command.c_str());
                break;
            }
            /* something else to play: stop, the app starts it */
            m_group_handoff = c.play_command == "SyncPlay";
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
            if (has_next()) out.push_back({OsdCmd::PlayNext});
            break;
        case remote::Command::Previous: previous_track(st, out); break;
        case remote::Command::Message:
            toast(c.header.empty() ? c.text : c.text.empty() ? c.header : c.header + ": " + c.text, st.now);
            break;
        }
    }
    /* A phone's pause, seek or next while in a group: the group's to do. */
    if (in_group())
        to_group(st, out, first);
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
    skip(mid + 120, ty, 30, true, has_next() ? kText : kText3);
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
    a_controls.to((m_controls || m_seeking || st.paused) && !overlay && !m_asking ? 1.f : 0.f);
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
        else if (m_overlay_drawn == Overlay::Channels)
            draw_channels(oa, dt);
        gfx::pop_opacity();
    }

    a_ask.to(m_asking ? 1.f : 0.f);
    a_ask.step(dt, 10.f);
    draw_still(st);

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

/* ---- live TV ------------------------------------------------------------------- */

livetv::GuideRef PlayerUi::guide()
{
    if (!m_guide || livetv::version() != m_guide_version) {
        m_guide = livetv::guide();
        m_guide_version = livetv::version();
    }
    return m_guide;   /* (a copy: the caller keeps it alive while it reads) */
}

int PlayerUi::playing_channel()
{
    const livetv::GuideRef held = guide();
    const livetv::Guide &g = *held;
    for (size_t i = 0; i < g.channels.size(); i++)
        if (m_req && g.channels[i].id == m_req->id)
            return (int)i;
    return -1;
}

void PlayerUi::zap(const std::string &channel_id, std::vector<OsdCommand> &out)
{
    if (channel_id.empty() || !m_req || channel_id == m_req->id)
        return;
    OsdCommand c{OsdCmd::PlayEpisode};
    c.video_id = channel_id;
    out.push_back(c);
    m_overlay = Overlay::None;
}

void PlayerUi::zap_step(int dir, std::vector<OsdCommand> &out)
{
    const livetv::GuideRef held = guide();
    const livetv::Guide &g = *held;
    const int n = (int)g.channels.size();
    const int at = playing_channel();
    if (n < 2 || at < 0)
        return;   /* (the guide not loaded yet, or the channel gone from it) */
    const int to = ((at + dir) % n + n) % n;   /* round, as a TV goes */
    zap(g.channels[to].id, out);
}

void PlayerUi::channels_input(uint32_t p, std::vector<OsdCommand> &out)
{
    const livetv::GuideRef held = guide();
    const int n = (int)held->channels.size();
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_TRIANGLE)) {
        m_overlay = Overlay::None;
    } else if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN | NUVIO_BTN_L2 | NUVIO_BTN_R2 | NUVIO_BTN_L1 | NUVIO_BTN_R1)) {
        const int d = (p & NUVIO_BTN_DOWN) ? 1 : (p & NUVIO_BTN_UP) ? -1 : (p & (NUVIO_BTN_R2 | NUVIO_BTN_R1)) ? 7 : -7;
        m_ch_index = std::max(0, std::min(n - 1, m_ch_index + d));
    } else if ((p & NUVIO_BTN_CROSS) && m_ch_index < n) {
        const std::string id = held->channels[m_ch_index].id;
        if (m_req && id == m_req->id)
            m_overlay = Overlay::None;   /* the one playing: back to it */
        else
            zap(id, out);
    }
}

/* The channels over the picture: what airs on each now (how far it has come) and next. */
void PlayerUi::draw_channels(float a, float dt)
{
    const livetv::GuideRef held = guide();
    const livetv::Guide &g = *held;
    const gfx::Rect r{80, 80, 820, H - 160};
    glass(r, a);
    gfx::text(r.x + 48, r.y + 78, T("Kanaler"), {gfx::Bold, 40}, alpha(kText, a));
    gfx::text(r.x + r.w - 48, r.y + 76, clock_at(0), {gfx::SemiBold, 26}, alpha(kText3, a), 2);
    const int n = (int)g.channels.size();
    const float top = r.y + 120, row_h = 108, view_h = r.h - 120 - 96;
    if (n == 0) {
        gfx::text(r.x + 48, top + 60, livetv::loading() ? T("Henter kanalene \xE2\x80\xA6") : T("Ingen kanaler."),
                  {gfx::Medium, 26}, alpha(kText2, a));
        return;
    }
    m_ch_index = std::min(m_ch_index, n - 1);
    const float want = std::max(0.f, std::min(n * row_h - view_h, (m_ch_index - 2) * row_h));
    if (m_ch_scroll.value < 0)
        m_ch_scroll.snap(want);
    m_ch_scroll.to(want);
    bool moving = m_ch_scroll.step(dt, 12.f);
    const float sy = m_ch_scroll.value;
    const gfx::Rect view{r.x + 16, top - 8, r.w - 32, view_h + 8};
    gfx::push_scissor(view);
    m_ch_drop.to({r.x + 24, top + m_ch_index * row_h - sy, r.w - 48, row_h - 10}, m_ch_index, r.x, r.y - sy);
    m_ch_drop.draw(dt, a, &moving, 18);
    if (moving)
        m_dirty = true;
    gfx::push_fade_mask(view, edge_fade(sy), edge_fade(std::max(0.f, n * row_h - view_h) - sy));
    const int64_t t = livetv::now();
    for (int i = std::max(0, (int)(sy / row_h) - 1); i < n; i++) {
        const float y = top + i * row_h - sy;
        if (y > top + view_h)
            break;
        const jf::Item &ch = g.channels[i];
        const bool focus = i == m_ch_index, here = m_req && ch.id == m_req->id;
        gfx::text(r.x + 48, y + 56, ch.channel_number, {gfx::SemiBold, 24, 60}, alpha(focus ? kText : kText3, a));
        /* The logo, whole (or the name where there is none). */
        const gfx::Rect box{r.x + 112, y + 18, 120, 62};
        if (!draw_logo_fit(livetv::logo_url(ch, 320), box, a))
            gfx::text(box.x, y + 56, ch.name, {gfx::SemiBold, 22, box.w}, alpha(kText2, a));
        const float tx = box.x + box.w + 28, tw = r.x + r.w - 48 - tx;
        const jf::Item *p = g.on_at(ch.id, t);
        const jf::Item *next = g.after(ch.id, t);
        float title_w = tw;
        if (here) {   /* "SPILLER": the one on now */
            const float pw = gfx::text_width(T("SPILLER"), {gfx::Bold, 15}) + 24;
            gfx::fill({r.x + r.w - 48 - pw, y + 22, pw, 26}, alpha(0xe600a4dcu, a), 13);
            gfx::text(r.x + r.w - 48 - pw / 2, y + 41, T("SPILLER"), {gfx::Bold, 15}, alpha(kText, a), 1);
            title_w -= pw + 12;
        }
        gfx::text(tx, y + 44, p ? p->name : ch.name, {focus ? gfx::Bold : gfx::SemiBold, 24, title_w},
                  alpha(focus ? kText : kText2, a));
        if (p) {
            const float f = (float)(t - p->start_utc) / (float)std::max<int64_t>(1, p->end_utc - p->start_utc);
            gfx::fill({tx, y + 60, 120, 4}, alpha(0x47ffffffu, a), 2);
            gfx::fill({tx, y + 60, 120 * std::max(0.f, std::min(1.f, f)), 4}, alpha(kText, a), 2);
        }
        if (next)
            gfx::text(p ? tx + 136 : tx, y + 66, T("Neste: ") + livetv::clock(next->start_utc) + " " + next->name,
                      {gfx::Medium, 19, p ? tw - 136 : tw}, alpha(kText3, a));
    }
    gfx::pop_fade_mask();
    gfx::pop_scissor();
    draw_pad_hints(r.x + 48, r.y + r.h - 46, {{PadButton::Cross, T("Se")}, {PadButton::Circle, T("Lukk")}}, 0, 26, a);
}

/* The controls' lower part on a channel: the channel, what airs and how far it has
 * come (from its start to its end, by the clock), and what follows. */
void PlayerUi::draw_live_info(const NuvioStatus &, float a)
{
    const livetv::GuideRef held = guide();
    const livetv::Guide &g = *held;
    const int64_t t = livetv::now();
    const jf::Item *ch = g.channel(m_req->id);
    const jf::Item *p = g.on_at(m_req->id, t);
    const jf::Item *next = g.after(m_req->id, t);
    const std::string name = ch ? (ch->channel_number.empty() ? "" : ch->channel_number + "  ") + ch->name : m_req->title;

    float x = kPad;
    const float cy = kBarY - 104;
    x += gfx::text(x, cy, name, {gfx::SemiBold, 26, 800}, alpha(kText2, a));
    {
        const gfx::TextStyle ts{gfx::Bold, 16};
        const float w = gfx::text_width(T("DIREKTE"), ts) + 22;
        gfx::fill({x + 16, cy - 22, w, 28}, alpha(0xffff453au, a), 8);
        gfx::text(x + 27, cy - 2, T("DIREKTE"), ts, alpha(kText, a));
    }
    const float ty = kBarY - 44;
    gfx::text(kPad, ty, p ? p->name : name, {gfx::Bold, 36, 1100}, alpha(kText, a));
    if (next)
        gfx::text(W - kPad, ty, T("Neste: ") + livetv::clock(next->start_utc) + " " + next->name, {gfx::Medium, 24, 560},
                  alpha(kText3, a), 2);

    const float x0 = kPad, x1 = W - kPad, w = x1 - x0, h = 8, y = kBarY + 12;
    gfx::fill({x0, y - h / 2, w, h}, alpha(0x38ffffffu, a), h / 2);
    if (p) {
        const float f = (float)(t - p->start_utc) / (float)std::max<int64_t>(1, p->end_utc - p->start_utc);
        gfx::fill({x0, y - h / 2, w * std::max(0.f, std::min(1.f, f)), h}, alpha(kText, a), h / 2);
        gfx::text(x0, y + 40, livetv::clock(p->start_utc), {gfx::SemiBold, 22}, alpha(kText3, a));
        gfx::text(x1, y + 40, livetv::clock(p->end_utc), {gfx::SemiBold, 22}, alpha(kText3, a), 2);
    }
}


} // namespace ui
