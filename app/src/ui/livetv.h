/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Direkte-TV: the server's channels as a programme guide, in Jelly5's look.
 * At the top the focused programme as a hero (its picture, channel, time, how
 * far it has come, what it is about); under it filters (Alle · Favoritter ·
 * Filmer · Sport · Nyheter · Barn · Serier) and the guide: channels down the
 * left, time along the top, a line where it is now. ✕ on what airs now (or on
 * the channel) watches it, on what comes later opens its sheet (record it, or
 * every episode); Options opens the sheet on anything, □ makes the channel a
 * favourite. The guide opens on the channel last watched. Opptak (when the
 * user may record, or has recordings): what was recorded, and what is set to be.
 */
#pragma once

#include "app/livetv.h"
#include "jf/jf_client.h"
#include "ui/anim.h"
#include "ui/screen.h"

#include <string>
#include <vector>

namespace ui {

/* A programme's sheet (or a channel's, when nothing is known of what it airs):
 * what it is, and what can be done with it. */
class ProgramSheet {
public:
    /* program may be a gap (id empty): the channel's only. */
    void open(const jf::Item &program, const jf::Item &channel, bool can_record);
    bool active() const { return m_open; }
    float visibility() const { return m_alpha.value; }
    /* *action: Play (the channel) or nothing; *note: a line to show (a recording set). */
    void input(uint32_t pressed, Action *action, std::string *note);
    void draw(jf::Client &c, float dt, bool *animating);

private:
    enum Option { Watch, Record, CancelRecord, RecordSeries, CancelSeries, Favorite };
    jf::Item m_program, m_channel;
    std::vector<Option> m_options;
    int m_focus = 0;
    bool m_open = false;
    Anim m_alpha;
    Drop m_drop;
};

/* The screen while a channel opens: its logo, number and name, and what airs on it
 * over a faint picture of it. The player's loading veil draws it; show_tuning draws
 * a whole frame of it (the moment a channel is picked, before the player is back). */
void draw_tuning(const std::string &channel_id, const std::string &fallback_name, float opacity);
void show_tuning(const std::string &channel_id, const std::string &fallback_name);

class LiveTv : public Screen {
public:
    explicit LiveTv(jf::Client &client) : m_client(client) {}

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override;
    float nav_alpha() const override { return 1.f - m_sheet.visibility(); }
    bool modal() const override { return m_sheet.active(); }
    void enter_from_top() override { m_zone = Zone::Filters; }

private:
    enum class Zone { Filters, Guide };
    enum Filter { All, Favorites, Movies, Sports, News, Kids, Series, Recordings, FilterCount };

    /* One block of a row: a programme, or a stretch the guide says nothing about. */
    struct Cell {
        int64_t start = 0, end = 0;
        const jf::Item *program = nullptr;   /* null: no listing */
    };
    std::vector<Cell> cells_of(const jf::Item &channel) const;
    bool matches(const jf::Item &program) const;
    /* The rows for the guide and filter; focus stays on the channel focused_id
     * (taken before the guide changed: the old rows' indices mean nothing in a new one). */
    void rebuild(const std::string &focused_id);
    const jf::Item *row_channel(int row) const;
    /* The focused cell of the focused row (false: the channel itself has focus). */
    bool focused_cell(Cell *out) const;
    void focus_at(int64_t t);                /* the cell under t in the focused row */
    void keep_in_view();
    void place_on_last_channel();

    void draw_hero(const jf::Item *channel, const Cell *cell, float dt);
    /* The hero for a programme or recording p (null: the channel only); fades as focus moves. */
    void draw_hero_item(const std::string &id, const jf::Item *p, const jf::Item *channel, float dt);
    void draw_filters(float dt);
    void draw_guide(float dt);

    /* Opptak: row 0 the recordings, 1 those set for later. */
    bool has_recordings() const;
    const std::vector<jf::Item> &rec_row(int row) const;
    const jf::Item *rec_focused() const;
    Action recordings_input(uint32_t p);
    void draw_recordings(float dt);
    int m_rec_row = 0;
    int m_rec_col[2] = {0, 0};
    Anim m_rec_scroll[2];
    Drop m_rec_drop;

    jf::Client &m_client;
    livetv::GuideRef m_guide = livetv::guide();
    unsigned m_version = 0;
    std::vector<int> m_rows;                 /* indices into m_guide->channels */
    Filter m_filter = All;
    Zone m_zone = Zone::Guide;
    int m_row = 0;
    bool m_on_channel = false;               /* the channel column has focus */
    int64_t m_focus_start = 0;               /* the focused cell, by its start */
    int64_t m_anchor = 0;                    /* the time up and down keep to */
    bool m_placed = false;                   /* opened on the last channel once */

    Anim m_t0;                               /* the guide's left edge, minutes after m_base */
    int64_t m_base = 0;
    Anim m_scroll;                           /* rows scrolled */
    Drop m_drop, m_pill_drop;
    ProgramSheet m_sheet;
    std::string m_note;
    double m_note_until = 0;
    std::string m_hero_id;
    Anim m_hero_alpha;
    double m_focus_changed = 0;

    double m_now = 0;
    float m_dt = 0;
    mutable int64_t m_drawn_minute = 0;
    bool m_animating = false;
};

} // namespace ui
