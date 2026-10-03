/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/now_playing.h"

#include "app/i18n.h"
#include "app/remote.h"
#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ui {

bool NowPlaying::refresh()
{
    NuvioRequest req;
    unsigned track = 0;
    if (!nuvio_player_now_playing(&m_st, &req, &track))
        return false;
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
    if (p & NUVIO_BTN_CIRCLE) {   /* off the page; the music plays on */
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
}

/* A glass card at the bottom right: the cover, the track and artist, a thin bar,
 * and how to open the page. */
void draw_mini_player(double now, float a)
{
    NuvioStatus st;
    NuvioRequest req;
    if (a <= 0.01f || !nuvio_player_now_playing(&st, &req, nullptr))
        return;
    (void)now;
    const float w = 560, h = 112;
    const gfx::Rect r{gfx::W - kPad - w, gfx::H - 56 - h, w, h};
    gfx::push_opacity(a);
    gfx::shadow(r, 22, 40, 0.7f, 16);
    gfx::fill(r, 0xe61c1c22u, 22);
    gfx::fill({r.x, r.y, r.w, 1.5f}, 0x24ffffffu);
    const gfx::Rect cover{r.x + 14, r.y + 14, h - 28, h - 28};
    art::draw(cover, req.cover, req.cover_blurhash, 800, 800, 12, 1.f, 0xff2a2a30u);
    const float tx = cover.x + cover.w + 20, tw = r.x + r.w - tx - 20;
    gfx::text(tx, r.y + 44, req.title, {gfx::SemiBold, 24, tw - 130}, kText);
    gfx::text(tx, r.y + 76, req.artist, {gfx::Medium, 20, tw - 130}, kText2);
    /* Options opens the page: the button, then what it does. */
    const float hint_w = 30 * 1.5f + 10 + gfx::text_width(T("Åpne"), {gfx::Medium, 30 * 0.72f});
    draw_pad_hint(r.x + r.w - 20 - hint_w, r.y + 36, PadButton::Options, T("Åpne"), 30);
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
