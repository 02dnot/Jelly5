/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The player's interface, drawn on the GPU over the video: the Netflix TV
 * layout in Jelly5's look (glass panels, white focus pills, soft shadows).
 * It replaces NuvioOsd behind the same contract: the player feeds it a status
 * every frame and the controller's input, and it answers with the same
 * commands (pause, seek, pick a track, next episode).
 *
 *   loading     the title's backdrop, its logo filling as the stream opens
 *   controls    at the bottom: the title, the progress bar (intro and credits
 *               marked) with the time left, and a row of buttons:
 *               Pause · Episoder · Lyd og undertekster · Neste episode
 *   scrubbing   Left/Right on the bar (or with the controls hidden) moves a
 *               playhead, accelerating; a bubble shows the time. L2/R2 scrub
 *               the same way (hold to go faster); L1/R1 jump 10 s
 *   overlays    glass panels over the dimmed picture. Lyd og undertekster:
 *               audio and subtitle columns, "Tilpass undertekster" opens style
 *               and timing in a third. Episoder: seasons left, episodes right
 *   skip / next "Hopp over intro" and the next-episode card, Cross acts
 *   music       a now-playing screen instead: the album's cover, the track,
 *               artist and album, the bar; Cross pauses, L1/R1 change track
 */
#pragma once

#include "gfx/gfx.h"
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
    bool post_play_active() const { return false; }

private:
    enum class Overlay { None, Tracks, Episodes };
    enum class Zone { Bar, Buttons };
    enum class Button { PlayPause, Episodes, Tracks, Next };

    void show_controls(double now, Zone zone);
    void seek_step(int dir, const NuvioStatus &st, double now);
    int current_skip(const NuvioStatus &st) const;
    bool next_card(const NuvioStatus &st) const;
    std::vector<Button> buttons() const;
    std::vector<int> seasons() const;
    std::vector<int> episodes_in(int season) const;   /* indices into m_req->episodes */
    void open_overlay(Overlay o);

    void tracks_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out);
    void episodes_input(uint32_t p, std::vector<OsdCommand> &out);

    void draw_loading(const NuvioStatus &st);
    void draw_controls(const NuvioStatus &st);
    void draw_bar(const NuvioStatus &st, float a);
    void draw_skip_next(const NuvioStatus &st);
    void draw_tracks(const NuvioStatus &st, float a);
    void find_input(uint32_t p);
    void draw_episodes(float a, float dt);
    void draw_error(const NuvioStatus &st);
    void music_input(uint32_t p, const NuvioStatus &st, std::vector<OsdCommand> &out);
    void previous_track(const NuvioStatus &st, std::vector<OsdCommand> &out);
    /* Commands from a phone controlling the PS5 (app/remote). */
    void remote_poll(const NuvioStatus &st, std::vector<OsdCommand> &out);
    void draw_music(const NuvioStatus &st);

    const NuvioRequest *m_req = nullptr;
    bool m_music = false;               /* an audio track: the now-playing screen */
    double m_now = 0, m_last = 0, m_load_since = 0;
    bool m_dirty = true;

    bool m_controls = false, m_shown_once = false;
    double m_hide_at = 0;
    Zone m_zone = Zone::Buttons;
    int m_button = 0;
    Anim a_controls, a_loading, a_overlay, a_skip, a_next, a_spinner, a_toast, a_error, a_flash;
    std::string m_flash_icon;           /* "play" / "pause", flashed in the centre */

    bool m_seeking = false;
    double m_seek_target = 0, m_seek_commit_at = 0, m_seek_last_step = 0;
    float m_seek_step = 10;

    bool m_skip_done[16] = {};
    bool m_card_dismissed = false;
    double m_card_since = -1;           /* the next-episode countdown */

    Overlay m_overlay = Overlay::None;
    Overlay m_overlay_drawn = Overlay::None;   /* kept while it fades out */
    /* Tracks: 0 audio, 1 subtitles, 2 style (when open); the row in each. */
    int m_col = 0;
    int m_rows[3] = {0, 0, 0};
    bool m_style_open = false;
    bool m_find_open = false;           /* column 2 is "Søk etter undertekster" */
    std::vector<std::string> m_find_langs;
    int m_find_lang = 0;
    /* Episodes: 0 seasons, 1 episodes. */
    int m_ep_col = 1, m_ep_season = 0, m_ep_index = 0;
    Anim m_ep_scroll;

    std::string m_toast;
    double m_toast_until = 0;
};

} // namespace ui
