/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/now_playing.h"

#include "app/i18n.h"
#include "app/remote.h"
#include "gfx/art.h"
#include "nuvio_input.h"
#include "jelly5_playback.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

namespace ui {

namespace {
double mono_now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
} // namespace

bool NowPlaying::refresh()
{
    NuvioRequest req;
    unsigned track = 0;
    /* Between two tracks the player has none for a moment: the last one stays up
     * (for up to 2 s), so the page does not go dark at every change. */
    const double t = mono_now();
    if (!nuvio_player_now_playing(&m_st, &req, &track))
        return m_req && t - m_seen_at < 2.0;
    m_seen_at = t;
    if (track != m_track || !m_req) {   /* a new track: the interface starts over on it */
        m_req.reset(new NuvioRequest(req));
        m_ui.begin(m_req.get(), m_st.now);
        m_track = track;
    }
    return true;
}

void NowPlaying::activate()
{
    m_enter.snap(0.f);
    m_enter.to(1.f);
    refresh();
}

Action NowPlaying::input(uint32_t p)
{
    Action a;
    if (m_queue) {
        queue_input(p);
        return a;
    }
    if (p & NUVIO_BTN_TRIANGLE) {   /* the queue sheet */
        m_queue = true;
        m_qrow = 1;
        m_qcol = 0;
        m_qa.to(1.f);
        m_qscroll.snap(0);
        return a;
    }
    if (p & NUVIO_BTN_CIRCLE) {   /* off the page; the music plays on */
        a.kind = Action::Back;
        return a;
    }
    if (p & NUVIO_BTN_SQUARE) {   /* stop: the music ends and the page closes behind it */
        remote::Command rc;
        rc.kind = remote::Command::Stop;
        remote::send(rc);
        a.kind = Action::Back;
        return a;
    }
    if (!refresh())
        return a;
    nuvio_input_state in;
    std::memset(&in, 0, sizeof in);
    in.pressed = p;
    std::vector<OsdCommand> cmds;
    m_ui.input(in, m_st, cmds);
    for (const OsdCommand &c : cmds) {
        remote::Command rc;
        switch (c.cmd) {
        case OsdCmd::TogglePause: rc.kind = remote::Command::PlayPause; break;
        case OsdCmd::SeekTo:
            rc.kind = remote::Command::Seek;
            rc.seek_ticks = (int64_t)(c.value * 10000000.0);
            break;
        case OsdCmd::PlayNext: rc.kind = remote::Command::Next; break;
        case OsdCmd::PlayEpisode: rc.kind = remote::Command::Previous; break;   /* L1: the track before */
        case OsdCmd::Stop: rc.kind = remote::Command::Stop; break;
        default: continue;
        }
        remote::send(rc);
    }
    return a;
}

void NowPlaying::draw(double now, float dt)
{
    m_enter.step(dt, 9.f);
    if (!refresh() || !m_req) {
        gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
        return;
    }
    m_st.now = now;
    /* A scrub commits when the presses stop (the player's own rule), sent like the rest. */
    std::vector<OsdCommand> cmds;
    m_ui.tick(m_st, cmds, false);
    for (const OsdCommand &c : cmds)
        if (c.cmd == OsdCmd::SeekTo) {
            remote::Command rc;
            rc.kind = remote::Command::Seek;
            rc.seek_ticks = (int64_t)(c.value * 10000000.0);
            remote::send(rc);
        }
    m_ui.draw(m_st);
    {   /* what △ does here, and the play mode when it is not the plain one */
        std::vector<jf::Item> up;
        std::vector<int> idx;
        int cur = 0, rep = 0;
        bool sh = false;
        jelly5_music_state(&up, &idx, &cur, &sh, &rep);
        std::string mode;
        if (sh)
            mode = T("Bland");
        if (rep)
            mode += std::string(mode.empty() ? "" : "  \xC2\xB7  ") + (rep == 2 ? T("Gjenta én") : T("Gjenta alle"));
        /* △ the queue, □ stop (the music ends, and with it the mini player). */
        const float hw = draw_pad_hints(gfx::W - kPad, 90, {{PadButton::Square, T("Stopp")}, {PadButton::Triangle, T("Kø")}}, 2,
                                        26, 1.f - m_qa.value);
        const float hx = gfx::W - kPad - hw;
        if (!mode.empty())
            gfx::text(hx - 24, 99, mode, {gfx::SemiBold, 22}, alpha(kText2, 1.f - m_qa.value), 2);
    }
    draw_queue(dt);
}

void NowPlaying::queue_input(uint32_t p)
{
    std::vector<jf::Item> up;
    std::vector<int> idx;
    int cur = 0, rep = 0;
    bool sh = false;
    jelly5_music_state(&up, &idx, &cur, &sh, &rep);
    const int rows = 1 + (int)up.size();
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_TRIANGLE)) {
        m_queue = false;
        m_qa.to(0.f);
    } else if (p & NUVIO_BTN_UP) {
        m_qrow = std::max(0, m_qrow - 1);
    } else if (p & NUVIO_BTN_DOWN) {
        m_qrow = std::min(rows - 1, m_qrow + 1);
    } else if (m_qrow == 0 && (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT))) {
        m_qcol = (p & NUVIO_BTN_RIGHT) ? 1 : 0;
    } else if (p & NUVIO_BTN_CROSS) {
        if (m_qrow == 0) {
            if (m_qcol == 0)
                jelly5_music_set_shuffle(!sh);
            else
                jelly5_music_set_repeat((rep + 1) % 3);   /* off, all, one */
        } else if (m_qrow - 1 < (int)idx.size()) {   /* play this one now */
            jelly5_music_jump(idx[m_qrow - 1]);
            remote::Command rc;
            rc.kind = remote::Command::Next;
            remote::send(rc);
            m_queue = false;
            m_qa.to(0.f);
        }
    }
}

/* The queue: a glass sheet on the right - Bland and Gjenta, then what plays next. */
void NowPlaying::draw_queue(float dt)
{
    m_qa.step(dt, 12.f);
    const float a = m_qa.value;
    if (a <= 0.01f)
        return;
    std::vector<jf::Item> up;
    std::vector<int> idx;
    int cur = 0, rep = 0;
    bool sh = false;
    jelly5_music_state(&up, &idx, &cur, &sh, &rep);
    m_qrow = std::min(m_qrow, (int)up.size());
    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x66000000u, a));
    const float w = 720, x0 = gfx::W - 80 - w + (1.f - a) * 60;
    const gfx::Rect r{x0, 100, w, gfx::H - 200};
    glass_panel(r, 28, a);
    gfx::text(r.x + 44, r.y + 70, T("Kø"), {gfx::Bold, 36}, alpha(kText, a));
    /* The toggles. */
    const std::string shuffle = T("Bland");
    const std::string repeat = rep == 2 ? T("Gjenta én") : rep == 1 ? T("Gjenta alle") : T("Gjenta");
    const gfx::TextStyle ts{gfx::SemiBold, 23};
    const float tw0 = gfx::text_width(shuffle, ts) + 64, tw1 = gfx::text_width(repeat, ts) + 64, ty = r.y + 104;
    const gfx::Rect t0{r.x + 36, ty, tw0, 56}, t1{t0.x + tw0 + 12, ty, tw1, 56};
    glass_panel(t0, 28, a, false, sh ? 0.6f : 0.f);
    glass_panel(t1, 28, a, false, rep ? 0.6f : 0.f);
    /* The tracks after the current one, scrolled to keep the focus in view. */
    const float lt = ty + 90, row_h = 72, view_h = r.y + r.h - 40 - lt;
    const int vis = std::max(1, (int)(view_h / row_h));
    m_qscroll.to(std::max(0.f, (float)(m_qrow - 1 - vis / 2) * row_h));
    m_qscroll.step(dt, 12.f);
    bool moving = false;
    if (m_queue && m_qrow == 0)
        m_qdrop.to(m_qcol == 0 ? t0 : t1, m_qcol, r.x, r.y);
    else if (m_queue)
        m_qdrop.to({r.x + 24, lt + (m_qrow - 1) * row_h - m_qscroll.value, w - 48, row_h - 6}, 100 + m_qrow, r.x,
                   r.y - m_qscroll.value);
    else
        m_qdrop.hide();
    const gfx::Rect q_view{r.x, lt - 4, w, view_h + 8};
    if (m_qrow == 0)   /* on Bland / Gjenta: above the list, so outside its clip */
        m_qdrop.draw(dt, a, &moving, 28.f);
    gfx::push_scissor(q_view);
    if (m_qrow != 0)
        m_qdrop.draw(dt, a, &moving, 14.f);
    gfx::push_fade_mask(q_view, edge_fade(m_qscroll.value),
                        edge_fade(std::max(0.f, up.size() * row_h - view_h) - m_qscroll.value));   /* the rows fade out where more lie beyond */
    for (int i = 0; i < (int)up.size(); i++) {
        const float y = lt + i * row_h - m_qscroll.value;
        if (y > r.y + r.h || y + row_h < lt)
            continue;
        const bool focus = m_queue && m_qrow == i + 1;
        gfx::text(r.x + 64, y + 32, up[i].name, {focus ? gfx::Bold : gfx::SemiBold, 24, w - 140},
                  alpha(focus ? kText : kText2, a));
        gfx::text(r.x + 64, y + 58, up[i].album_artist.empty() ? up[i].album : up[i].album_artist,
                  {gfx::Medium, 19, w - 140}, alpha(kText3, a));
    }
    gfx::pop_fade_mask();
    gfx::pop_scissor();
    gfx::text(t0.x + t0.w / 2, ty + 36, shuffle, {sh || (m_qrow == 0 && m_qcol == 0) ? gfx::Bold : gfx::SemiBold, 23},
              alpha(sh || (m_qrow == 0 && m_qcol == 0) ? kText : kText2, a), 1);
    gfx::text(t1.x + t1.w / 2, ty + 36, repeat, {rep || (m_qrow == 0 && m_qcol == 1) ? gfx::Bold : gfx::SemiBold, 23},
              alpha(rep || (m_qrow == 0 && m_qcol == 1) ? kText : kText2, a), 1);
    if (up.empty())
        gfx::text(r.x + 64, lt + 40, rep == 1 ? T("Starter forfra etter denne") : T("Ingenting mer i køen"),
                  {gfx::Medium, 22}, alpha(kText3, a));
    (void)cur;
}

/* A glass card at the bottom right: the cover, the track and artist, a thin bar,
 * and how to open the page. */
void draw_mini_player(double now, float a)
{
    /* The last track, kept through the gap before the next (as the page does). */
    static NuvioStatus st;
    static NuvioRequest req;
    static double seen_at;
    if (a <= 0.01f)
        return;
    if (nuvio_player_now_playing(&st, &req, nullptr))
        seen_at = mono_now();
    else if (seen_at == 0 || mono_now() - seen_at >= 2.0)
        return;
    (void)now;
    const float w = 560, h = 112;
    const gfx::Rect r{gfx::W - kPad - w, gfx::H - 56 - h, w, h};
    gfx::push_opacity(a);
    glass_panel(r, 22);
    const gfx::Rect cover{r.x + 14, r.y + 14, h - 28, h - 28};
    art::draw(cover, req.cover, req.cover_blurhash, 800, 800, 12, 1.f, 0xff2a2a30u);
    const float tx = cover.x + cover.w + 20, tw = r.x + r.w - tx - 20;
    gfx::text(tx, r.y + 44, req.title, {gfx::SemiBold, 24, tw - 130}, kText);
    gfx::text(tx, r.y + 76, req.artist, {gfx::Medium, 20, tw - 130}, kText2);
    /* The touchpad opens the page: the button, then what it does. */
    const float hint_w = pad_hint_width(PadButton::Touchpad, T("Åpne"), 30);
    draw_pad_hint(r.x + r.w - 20 - hint_w, r.y + 36, PadButton::Touchpad, T("Åpne"), 30);
    if (st.paused) {   /* paused: two bars over the cover */
        gfx::fill(cover, 0x8c000000u, 12);
        gfx::fill({cover.x + cover.w / 2 - 12, cover.y + cover.h / 2 - 14, 8, 28}, kText, 2);
        gfx::fill({cover.x + cover.w / 2 + 4, cover.y + cover.h / 2 - 14, 8, 28}, kText, 2);
    }
    const float pct = st.duration > 0 ? (float)std::min(1.0, std::max(0.0, st.position / st.duration)) : 0.f;
    gfx::fill({tx, r.y + r.h - 22, tw, 4}, 0x38ffffffu, 2);
    gfx::fill({tx, r.y + r.h - 22, tw * pct, 4}, 0xffffffffu, 2);
    gfx::pop_opacity();
}

} // namespace ui
