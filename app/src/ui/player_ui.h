/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The player's interface, drawn on the GPU over the video (concept/: the
 * Apple TV-style transport). It replaces NuvioOsd behind the same contract:
 * the player feeds it a status every frame and the controller's input, and
 * it answers with the same commands (pause, seek, pick a track, next episode).
 *
 *   loading     the title's backdrop, its logo filling as the stream opens
 *   controls    logo / title and clock on top; the progress bar with intro,
 *               recap and credits marked, elapsed · "Slutter kl." · remaining
 *   scrubbing   Left/Right moves a playhead (accelerating), a bubble shows
 *               the time and, when the server has them, a trickplay frame
 *   panel       Down: a glass panel from the top - Info · Lyd · Undertekster
 *               · Episoder; subtitle style and timing behind "Avansert"
 *   skip / next "Hopp over intro" and the next-episode card, Cross acts
 */
#pragma once

#include "nuvio_osd.h"   /* NuvioStatus, OsdCommand */
#include "ui/anim.h"

#include <string>
#include <vector>

namespace ui {

class PlayerUi {
public:
    void begin(const NuvioRequest *req, double now);
    void end() { m_req = nullptr; }

    void input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out);
    void tick(const NuvioStatus &st, std::vector<OsdCommand> &out);
    /* Draws into the current frame (after the video and subtitles). */
    void draw(const NuvioStatus &st);
    /* Something is moving or changed: the player should present a frame. */
    bool wants_frame(const NuvioStatus &st);

    bool visible() const { return true; }
    float subtitle_lift() const;
    void toast(const std::string &text, double now);
    void playback_ended(const NuvioStatus &st, std::vector<OsdCommand> &out);
    void note_subtitle_choice() { m_dirty = true; }
    bool post_play_active() const { return m_post_play; }

private:
    enum Tab { TabInfo, TabAudio, TabSubs, TabEpisodes };

    void show_controls(double now);
    void seek_step(int dir, const NuvioStatus &st, double now);
    int current_skip(const NuvioStatus &st) const;
    bool next_card(const NuvioStatus &st) const;
    std::vector<Tab> tabs() const;
    void panel_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out);

    void draw_loading(const NuvioStatus &st);
    void draw_controls(const NuvioStatus &st);
    void draw_bar(const NuvioStatus &st, float a);
    void draw_skip_next(const NuvioStatus &st);
    void draw_panel(const NuvioStatus &st);
    void draw_error(const NuvioStatus &st);

    const NuvioRequest *m_req = nullptr;
    double m_now = 0, m_last = 0, m_load_since = 0;
    bool m_dirty = true;

    bool m_controls = false;
    bool m_shown_once = false;
    double m_hide_at = 0;
    Anim a_controls, a_loading, a_panel, a_skip, a_next, a_spinner, a_toast, a_error, a_flash;
    std::string m_flash_icon;           /* "play" / "pause", flashed in the centre */

    bool m_seeking = false;
    double m_seek_target = 0, m_seek_commit_at = 0, m_seek_last_step = 0;
    float m_seek_step = 10;

    bool m_skip_done[16] = {};
    bool m_card_dismissed = false;
    bool m_post_play = false;
    double m_card_since = -1;           /* the next-episode countdown */

    bool m_panel = false;
    int m_tab = 0;                      /* index into tabs() */
    int m_row = -1;                     /* -1: the tab strip */
    bool m_advanced = false;
    int m_scroll = 0;

    std::string m_toast;
    double m_toast_until = 0;
};

} // namespace ui
