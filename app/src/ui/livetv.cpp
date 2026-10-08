/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sizes and colours follow concept/style.css: the hero's type as the home
 * screen's info panel, the filters as the library's pills, the guide's cells as
 * its glass panes with the focus drop over them.
 */
#include "ui/livetv.h"

#include "app/i18n.h"
#include "gfx/art.h"
#include "gfx/gfx.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ui {
namespace {

/* The guide's geometry (1920 x 1080). */
constexpr float kFiltersY = 500;             /* the pills */
constexpr float kRulerY = 612;               /* the times' baseline */
constexpr float kGridY = 640;                /* the first row */
constexpr float kRowH = 84, kCellH = 72;     /* a row, and the cell in it */
constexpr float kChanW = 232;                /* the channel column */
constexpr float kGridX = kPad + kChanW + 12; /* where time starts */
constexpr float kGridR = gfx::W - kPad;
constexpr float kPxPerMin = 10.f;            /* 2 h 24 min across */
constexpr float kCellR = 12;
constexpr uint32_t kLive = 0xffff453au;      /* the now line and "DIREKTE" */

float view_minutes() { return (kGridR - kGridX) / kPxPerMin; }

std::string span(const jf::Item &p)
{
    return livetv::clock(p.start_utc) + "\xE2\x80\x93" + livetv::clock(p.end_utc);   /* 21:00–21:30 */
}

std::string minutes_label(int64_t secs)
{
    const int min = (int)((secs + 59) / 60);
    char b[48];
    if (min >= 60 && min % 60 == 0)
        std::snprintf(b, sizeof b, T("%d t"), min / 60);
    else if (min >= 60)
        std::snprintf(b, sizeof b, T("%d t %d min"), min / 60, min % 60);
    else
        std::snprintf(b, sizeof b, "%d min", std::max(1, min));
    return b;
}

/* A channel's logo, whole, inside box (logos come in every shape). */
bool draw_logo(jf::Client &, const jf::Item &ch, const gfx::Rect &box, float a)
{
    return draw_logo_fit(livetv::logo_url(ch, 320), box, a);
}

std::string program_art(jf::Client &, const jf::Item &p, int width) { return livetv::picture_url(p, width); }

/* A small pill with a word in it ("DIREKTE", "NY"); returns its width. */
float tag(float x, float baseline, const char *word, uint32_t fill, uint32_t ink, float a)
{
    const gfx::TextStyle ts{gfx::Bold, 16};
    const float w = gfx::text_width(word, ts) + 22;
    gfx::fill({x, baseline - 21, w, 28}, alpha(fill, a), 8);
    gfx::text(x + 11, baseline - 1, word, ts, alpha(ink, a));
    return w;
}

/* A recording's mark: a red dot (two for a whole series). */
void rec_mark(float x, float cy, bool series, float a)
{
    gfx::fill({x, cy - 6, 12, 12}, alpha(kLive, a), 6);
    if (series)
        gfx::fill({x + 9, cy - 6, 12, 12}, alpha(kLive, a * 0.7f), 6);
}

} // namespace

/* ---- the programme sheet --------------------------------------------------------- */

void ProgramSheet::open(const jf::Item &program, const jf::Item &channel, bool can_record)
{
    m_program = program;
    m_channel = channel;
    m_options.clear();
    const int64_t t = livetv::now();
    const bool listed = !program.id.empty();
    const bool airing = !listed || (program.start_utc <= t && t < program.end_utc);
    const bool ended = listed && program.end_utc <= t;
    if (airing)
        m_options.push_back(Watch);
    if (listed && can_record && !ended) {
        m_options.push_back(program.timer_id.empty() ? Record : CancelRecord);
        if (program.is_series || !program.series_timer_id.empty())
            m_options.push_back(program.series_timer_id.empty() ? RecordSeries : CancelSeries);
    }
    if (!airing)
        m_options.push_back(Watch);   /* the channel as it is now, after what concerns this programme */
    m_options.push_back(Favorite);
    m_focus = 0;
    m_open = true;
    m_alpha.to(1.f);
}

void ProgramSheet::input(uint32_t p, Action *action, std::string *note)
{
    if (p & (NUVIO_BTN_CIRCLE | NUVIO_BTN_OPTIONS)) {
        m_open = false;
        m_alpha.to(0.f);
        return;
    }
    if (p & NUVIO_BTN_UP)
        m_focus = std::max(0, m_focus - 1);
    else if (p & NUVIO_BTN_DOWN)
        m_focus = std::min((int)m_options.size() - 1, m_focus + 1);
    if (!(p & NUVIO_BTN_CROSS))
        return;
    m_open = false;
    m_alpha.to(0.f);
    switch (m_options[m_focus]) {
    case Watch:
        action->kind = Action::Play;
        action->item = m_channel;
        break;
    case Record:
        livetv::record(m_program, false, nullptr);
        *note = T("Tas opp: ") + m_program.name;
        break;
    case RecordSeries:
        livetv::record(m_program, true, nullptr);
        *note = T("Alle episoder tas opp: ") + m_program.name;
        break;
    case CancelRecord:
        livetv::cancel(m_program, false, nullptr);
        *note = T("Opptaket er avbrutt");
        break;
    case CancelSeries:
        livetv::cancel(m_program, true, nullptr);
        *note = T("Serieopptaket er avbrutt");
        break;
    case Favorite:
        livetv::set_favorite(m_channel.id, !m_channel.favorite);
        *note = m_channel.favorite ? T("Fjernet fra favoritter: ") + m_channel.name
                                   : T("Lagt til i favoritter: ") + m_channel.name;
        break;
    }
}

void ProgramSheet::draw(jf::Client &c, float dt, bool *animating)
{
    if (m_alpha.step(dt, 14.f))
        *animating = true;
    const float a = m_alpha.value;
    if (a <= 0.01f)
        return;
    gfx::fill({0, 0, gfx::W, gfx::H}, alpha(0x99000000u, a));

    std::vector<std::string> labels;
    for (const Option o : m_options) {
        switch (o) {
        case Watch: {
            char b[256];
            std::snprintf(b, sizeof b, T("Se %s"), m_channel.name.c_str());
            labels.push_back(b);
            break;
        }
        case Record: labels.push_back(T("Ta opp")); break;
        case CancelRecord: labels.push_back(T("Avbryt opptaket")); break;
        case RecordSeries: labels.push_back(T("Ta opp alle episoder")); break;
        case CancelSeries: labels.push_back(T("Avbryt serieopptaket")); break;
        case Favorite:
            labels.push_back(m_channel.favorite ? T("Fjern kanalen fra favoritter")
                                                : T("Legg kanalen til i favoritter"));
            break;
        }
    }
    const bool listed = !m_program.id.empty();
    const float w = 1080, pic_w = 400, pic_h = 225, row_h = 64;
    const float head = 64 + pic_h + 40;
    const float h = head + m_options.size() * (row_h + 6) + 40 + 52;
    const float rise = 24 * (1.f - a);
    const gfx::Rect r{(gfx::W - w) / 2, (gfx::H - h) / 2 + rise, w, h};
    glass_panel(r, 28, a);

    /* The picture (the channel's logo on glass when the programme has none). */
    const gfx::Rect pic{r.x + 48, r.y + 56, pic_w, pic_h};
    const std::string url = listed ? program_art(c, m_program, 640) : std::string();
    if (!url.empty() && !art::failed(url)) {
        art::draw(pic, url, m_program.primary_blurhash, 640, 360, 16, a);
    } else {
        gfx::fill(pic, alpha(0x14ffffffu, a), 16);
        draw_logo(c, m_channel, {pic.x + 60, pic.y + 40, pic.w - 120, pic.h - 80}, a);
    }
    const float tx = pic.x + pic_w + 36, tw = r.x + w - 48 - tx;
    float y = r.y + 92;
    gfx::text(tx, y, listed ? m_program.name : m_channel.name, {gfx::Bold, 34, tw, 2, 42}, alpha(kText, a));
    if (gfx::text_width(listed ? m_program.name : m_channel.name, {gfx::Bold, 34}) > tw)
        y += 42;
    if (listed && !m_program.episode_title.empty()) {
        y += 36;
        gfx::text(tx, y, m_program.episode_title, {gfx::SemiBold, 24, tw}, alpha(kText2, a));
    }
    y += 40;
    std::string meta = (m_channel.channel_number.empty() ? std::string() : m_channel.channel_number + "  ") + m_channel.name;
    if (listed) {
        meta += " \xC2\xB7 " + livetv::day_label(m_program.start_utc) + " " + span(m_program);
        const std::string kind = livetv::kind_label(m_program);
        if (!kind.empty())
            meta += " \xC2\xB7 " + kind;
    }
    gfx::text(tx, y, meta, {gfx::Medium, 22, tw}, alpha(kText2, a));
    if (listed && !m_program.overview.empty())
        gfx::text(tx, y + 44, m_program.overview, {gfx::Regular, 22, tw, 3, 31}, alpha(kText3, a));
    if (listed && (!m_program.timer_id.empty() || !m_program.series_timer_id.empty())) {
        const float ry = pic.y + pic.h - 22;
        rec_mark(pic.x + 18, ry, !m_program.series_timer_id.empty(), a);
    }

    y = r.y + head;
    m_drop.to({r.x + 30, y + m_focus * (row_h + 6), w - 60, row_h}, m_focus, r.x, r.y);
    m_drop.draw(dt, a, animating, 14);
    for (size_t i = 0; i < m_options.size(); i++) {
        const bool focus = (int)i == m_focus;
        gfx::text(r.x + 56, y + 42, labels[i], {focus ? gfx::Bold : gfx::SemiBold, 26, w - 112},
                  alpha(focus ? kText : kText2, a));
        y += row_h + 6;
    }
    draw_pad_hints(r.x + 56, r.y + r.h - 52, {{PadButton::Cross, T("Velg")}, {PadButton::Circle, T("Lukk")}}, 0, 26,
                   a);
}

/* ---- the guide ---------------------------------------------------------------------- */

void LiveTv::activate()
{
    livetv::refresh();
    m_focus_changed = m_now;
}

bool LiveTv::animating() const
{
    /* The now line moves on its own: a frame every half minute; a new guide shows at once. */
    const int64_t half_minute = livetv::now() / 30;
    if (half_minute != m_drawn_minute)
        return true;
    return m_animating || livetv::version() != m_version;
}

bool LiveTv::matches(const jf::Item &p) const
{
    switch (m_filter) {
    case Movies: return p.is_movie;
    case Sports: return p.is_sports;
    case News: return p.is_news;
    case Kids: return p.is_kids;
    case Series: return p.is_series && !p.is_movie;
    default: return true;
    }
}

void LiveTv::rebuild(const std::string &focused_id)
{
    m_rows.clear();
    const int64_t t = livetv::now();
    for (size_t i = 0; i < m_guide->channels.size(); i++) {
        const jf::Item &ch = m_guide->channels[i];
        bool keep = true;
        if (m_filter == Favorites) {
            keep = ch.favorite;
        } else if (m_filter != All) {
            /* A category: the channels with such a programme from now on in what is loaded. */
            keep = false;
            if (const std::vector<jf::Item> *list = m_guide->programs_of(ch.id))
                for (const jf::Item &p : *list)
                    if (p.end_utc > t && matches(p)) {
                        keep = true;
                        break;
                    }
        }
        if (keep)
            m_rows.push_back((int)i);
    }
    m_row = std::max(0, std::min(m_row, (int)m_rows.size() - 1));
    for (size_t r = 0; r < m_rows.size(); r++)
        if (m_guide->channels[m_rows[r]].id == focused_id)
            m_row = (int)r;
}

const jf::Item *LiveTv::row_channel(int row) const
{
    return row >= 0 && row < (int)m_rows.size() ? &m_guide->channels[m_rows[row]] : nullptr;
}

std::vector<LiveTv::Cell> LiveTv::cells_of(const jf::Item &channel) const
{
    std::vector<Cell> out;
    const int64_t from = m_guide->from, to = m_guide->horizon;
    /* A stretch with no programme: nothing listed there, or not loaded yet (as many
     * cells as it takes to tell the two apart). */
    auto gap = [&](int64_t a, int64_t b) {
        while (a < b) {
            const bool known = m_guide->covers(a);
            int64_t e = b;
            for (const auto &r : m_guide->covered) {
                if (known && r.first <= a && a < r.second)
                    e = std::min(e, r.second);
                if (!known && r.first > a)
                    e = std::min(e, r.first);
            }
            out.push_back({a, e, nullptr, !known});
            a = e;
        }
    };
    int64_t at = from;
    if (const std::vector<jf::Item> *list = m_guide->programs_of(channel.id))
        for (const jf::Item &p : *list) {
            if (p.end_utc <= at || p.start_utc >= to)
                continue;
            if (p.start_utc > at)
                gap(at, p.start_utc);
            out.push_back({p.start_utc, p.end_utc, &p, false});
            at = p.end_utc;
        }
    gap(at, to);
    return out;
}

bool LiveTv::focused_cell(Cell *out) const
{
    const jf::Item *ch = row_channel(m_row);
    if (!ch || m_on_channel)
        return false;
    const std::vector<Cell> cells = cells_of(*ch);
    for (const Cell &c : cells)
        if (c.start <= m_focus_start && m_focus_start < c.end) {
            *out = c;
            return true;
        }
    if (cells.empty())
        return false;
    *out = cells.front();
    return true;
}

void LiveTv::focus_at(int64_t t)
{
    const jf::Item *ch = row_channel(m_row);
    if (!ch)
        return;
    for (const Cell &c : cells_of(*ch))
        if (c.start <= t && t < c.end) {
            m_focus_start = c.start;
            return;
        }
    m_focus_start = std::max(t, m_guide->from);
}

/* The focused cell's start in view: the guide moves by half hours. */
void LiveTv::keep_in_view()
{
    Cell c;
    if (!focused_cell(&c))
        return;
    const int64_t left = m_base + (int64_t)std::lround(m_t0.target * 60.f);
    const int64_t width = (int64_t)(view_minutes() * 60);
    const int64_t start = std::max(c.start, m_guide->from);
    int64_t want = left;
    if (start < left)
        want = livetv::half_hour(start);
    else if (start > left + width - 45 * 60)   /* room to see it, and some of what follows */
        want = livetv::half_hour(start) - 30 * 60;
    want = std::max(want, m_guide->from);
    m_t0.to((float)(want - m_base) / 60.f);
    livetv::ensure(want - 3600, want + width + 6 * 3600);   /* what is in view, and some hours on, loaded */
}

int64_t LiveTv::day_time() const
{
    const int64_t left = m_base + (int64_t)std::lround(m_t0.target * 60.f) + 600;
    Cell c;
    if (m_zone != Zone::Filters && focused_cell(&c))
        return std::max(c.start, left - 600);   /* (one running since before the view: the view's day) */
    return left;
}

int LiveTv::view_day() const
{
    const time_t a = (time_t)day_time(), b = (time_t)livetv::now();
    struct tm x, y;
    localtime_r(&a, &x);
    localtime_r(&b, &y);
    auto day = [](const struct tm &t) {   /* days since the epoch, by the calendar */
        return jf::utc_of(std::to_string(t.tm_year + 1900) + "-" + (t.tm_mon < 9 ? "0" : "") +
                          std::to_string(t.tm_mon + 1) + "-" + (t.tm_mday < 10 ? "0" : "") + std::to_string(t.tm_mday)) /
               86400;
    };
    return (int)std::max<int64_t>(0, day(x) - day(y));
}

void LiveTv::jump_to_day(int day)
{
    day = std::max(0, std::min(kDays - 1, day));
    const int64_t t = livetv::now() + (int64_t)day * 86400;   /* the same time of day (a DST change aside) */
    m_anchor = t;
    if (!m_on_channel)
        focus_at(t);
    const int64_t left = std::max(m_guide->from, livetv::half_hour(t) - (day == 0 ? 0 : 1800));
    m_t0.to((float)(left - m_base) / 60.f);
    livetv::ensure(left - 3600, left + (int64_t)(view_minutes() * 60) + 6 * 3600);
    m_focus_changed = m_now;
}

void LiveTv::place_on_last_channel()
{
    const std::string last = livetv::last_channel();
    for (size_t r = 0; r < m_rows.size(); r++)
        if (m_guide->channels[m_rows[r]].id == last)
            m_row = (int)r;
    m_scroll.snap((float)std::max(0, m_row - 1));
}

Action LiveTv::input(uint32_t p)
{
    Action action;
    if (m_sheet.active()) {
        std::string note;
        m_sheet.input(p, &action, &note);
        if (!note.empty()) {
            m_note = note;
            m_note_until = m_now + 3.0;
        }
        return action;   /* (a channel played is noted as watched by the play chain) */
    }
    const int before_row = m_row;
    const int64_t before_focus = m_focus_start;
    const bool before_channel = m_on_channel;
    const int n = (int)m_rows.size();
    const int64_t t = livetv::now();

    if (m_zone == Zone::Filters) {
        int f = (int)m_filter;
        const int last = has_recordings() ? Recordings : Series;
        if (p & NUVIO_BTN_LEFT) {
            if (f > 0) f--;
            else m_bump = true;
        } else if (p & NUVIO_BTN_RIGHT) {
            if (f < last) f++;
            else m_bump = true;
        } else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) {
            if (m_filter == Recordings) {
                if (!rec_row(0).empty() || !rec_row(1).empty()) {
                    m_zone = Zone::Guide;
                    m_rec_row = rec_row(0).empty() ? 1 : 0;
                }
            } else if (n > 0) {
                m_zone = Zone::Guide;
                if (m_focus_start == 0)
                    focus_at(t);
            }
        } else if (p & (NUVIO_BTN_UP | NUVIO_BTN_CIRCLE)) {
            action.kind = Action::ToNav;
        }
        if (f != (int)m_filter) {   /* the filter applies as it is focused (tvOS) */
            m_filter = (Filter)f;
            rebuild(std::string());
            m_row = 0;
            m_scroll.to(0);
            m_on_channel = false;
            m_anchor = t;
            focus_at(t);
            m_t0.to((float)(m_guide->from - m_base) / 60.f);
        }
        return action;
    }

    if (m_filter == Recordings)
        return recordings_input(p);
    if (m_zone == Zone::Day) {   /* ‹ I dag ›: ← → another day, ↓ back to the channels */
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT | NUVIO_BTN_L2 | NUVIO_BTN_R2)) {
            const int d = view_day() + ((p & (NUVIO_BTN_RIGHT | NUVIO_BTN_R2)) ? 1 : -1);
            if (d < 0 || d >= kDays)
                m_bump = true;
            else
                jump_to_day(d);
        } else if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) {
            m_zone = Zone::Guide;
        } else if (p & (NUVIO_BTN_UP | NUVIO_BTN_CIRCLE)) {
            m_zone = Zone::Filters;
        }
        return action;
    }
    if (n == 0) {   /* no channels (yet, or under this filter): only the way back up */
        if (p & (NUVIO_BTN_UP | NUVIO_BTN_CIRCLE))
            m_zone = Zone::Filters;
        return action;
    }

    const jf::Item *ch = row_channel(m_row);
    Cell cell;
    const bool has_cell = focused_cell(&cell);
    if (p & (NUVIO_BTN_L2 | NUVIO_BTN_R2)) {   /* a day back / on, the same time of day */
        const int d = view_day() + ((p & NUVIO_BTN_R2) ? 1 : -1);
        if (d < 0 || d >= kDays)
            m_bump = true;
        else
            jump_to_day(d);
        return action;
    }
    if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
        int d = (p & NUVIO_BTN_DOWN) ? 1 : -1;
        if (m_row + d < 0) {   /* above the first row: the day picker (from the channels), else the filters */
            m_zone = m_on_channel ? Zone::Day : Zone::Filters;
            return action;
        }
        if (m_row + d >= n)
            d = n - 1 - m_row;
        if (d == 0)
            m_bump = true;
        m_row += d;
        if (!m_on_channel)
            focus_at(std::max(m_anchor, m_guide->from));
    } else if (p & NUVIO_BTN_RIGHT) {
        if (m_on_channel) {
            m_on_channel = false;
            focus_at(std::max(m_anchor, t));
        } else if (ch && has_cell) {
            if (cell.end < m_guide->horizon) {
                m_focus_start = cell.end;
                m_anchor = cell.end;
            } else {
                m_bump = true;   /* the end of the guide */
            }
        }
    } else if (p & NUVIO_BTN_LEFT) {
        if (!m_on_channel && has_cell && cell.start > m_guide->from) {
            focus_at(cell.start - 1);
            Cell prev;
            if (focused_cell(&prev))
                m_anchor = std::max(prev.start, m_guide->from);
        } else if (!m_on_channel) {
            m_on_channel = true;   /* past the first: the channel itself */
        } else {
            m_bump = true;
        }
    } else if (p & NUVIO_BTN_CROSS) {
        if (has_cell && cell.loading && !m_on_channel)
            return action;   /* not loaded yet: nothing to act on */
        if (ch) {
            const bool airing = m_on_channel || !has_cell || (cell.start <= t && t < cell.end) || !cell.program;
            if (airing) {
                action.kind = Action::Play;
                action.item = *ch;
            } else {
                m_sheet.open(*cell.program, *ch, m_client.can_record());
            }
        }
    } else if (p & NUVIO_BTN_OPTIONS) {
        if (ch)
            m_sheet.open(has_cell && cell.program && !m_on_channel ? *cell.program : jf::Item(), *ch,
                         m_client.can_record());
    } else if (p & NUVIO_BTN_SQUARE) {
        if (ch) {
            livetv::set_favorite(ch->id, !ch->favorite);
            m_note = ch->favorite ? T("Fjernet fra favoritter: ") + ch->name : T("Lagt til i favoritter: ") + ch->name;
            m_note_until = m_now + 3.0;
        }
    } else if (p & NUVIO_BTN_CIRCLE) {
        /* Back, as on Netflix: to what airs now, then to the top, then the filters. */
        const bool at_now = m_on_channel || (has_cell && cell.start <= t);
        if (!at_now) {
            m_on_channel = false;
            m_anchor = t;
            focus_at(t);
        } else if (m_row > 0) {
            m_row = 0;
            m_on_channel = false;
            m_anchor = t;
            focus_at(t);
        } else {
            m_zone = Zone::Filters;
        }
    }
    if (m_row != before_row || m_focus_start != before_focus || m_on_channel != before_channel) {
        m_focus_changed = m_now;
        keep_in_view();
    }
    return action;
}

void LiveTv::draw_hero(const jf::Item *ch, const Cell *cell, float dt)
{
    /* The programme focused, or what airs on the focused channel. */
    const int64_t t = livetv::now();
    const jf::Item *p = cell && cell->program ? cell->program : ch ? m_guide->on_at(ch->id, t) : nullptr;
    draw_hero_item(p ? p->id : ch ? ch->id : std::string(), p, ch, dt);
}

void LiveTv::draw_hero_item(const std::string &id, const jf::Item *p, const jf::Item *ch, float dt)
{
    const int64_t t = livetv::now();
    if (id != m_hero_id) {
        m_hero_alpha.to(0.f);
        if (m_hero_alpha.value < 0.05f && m_now - m_focus_changed > 0.1) {
            m_hero_id = id;
            m_hero_alpha.to(1.f);
        }
    } else {
        m_hero_alpha.to(1.f);
    }
    if (m_hero_alpha.step(dt, 16.f))
        m_animating = true;
    const float a = m_hero_alpha.value;
    if ((!ch && !p) || a <= 0.01f || m_hero_id != id)
        return;
    /* A recording or a timer names its channel itself (the guide may not hold it). */
    jf::Item named;
    if (!ch) {
        named.name = p->channel_name;
        named.id = p->channel_id;
        named.primary_tag = p->channel_primary_tag;
        ch = &named;
    }

    /* The programme's picture, large on the right, fading into the page. */
    const std::string url = p ? program_art(m_client, *p, 1280) : std::string();
    const gfx::Rect pic{780, 0, gfx::W - 780, (gfx::W - 780) * 9.f / 16.f};
    if (!url.empty() && !art::failed(url)) {
        art::draw(pic, url, p->primary_blurhash, 1280, 720, 0, a * 0.9f, kBg);
        gfx::fill_hgradient({pic.x, 0, 520, pic.h}, kBg, alpha(kBg, 0.f));
        gfx::fill_vgradient({pic.x, pic.h - 300, pic.w, 300}, alpha(kBg, 0.f), kBg);
        gfx::fill_vgradient({pic.x, 0, pic.w, 200}, alpha(kBg, 0.55f), alpha(kBg, 0.f));
    }

    /* The channel: its logo and number. */
    float y = 196;
    float x = kPad;
    const gfx::Rect logo{x, y - 34, 84, 46};
    if (draw_logo(m_client, *ch, logo, a))
        x += 100;
    x += gfx::text(x, y, (ch->channel_number.empty() ? std::string() : ch->channel_number + "  ") + ch->name,
                   {gfx::SemiBold, 24, 560}, alpha(kText2, a));
    const bool timed = p && p->start_utc > 0 && p->end_utc > p->start_utc;   /* a recording may say neither */
    const bool airing = timed && p->start_utc <= t && t < p->end_utc;
    if (airing)
        tag(x + 18, y, T("DIREKTE"), kLive, kText, a);
    else if (timed)
        gfx::text(x + 18, y, livetv::day_label(p->start_utc), {gfx::SemiBold, 22}, alpha(kText3, a));

    /* Title, then what and when. */
    y += 88;
    const bool episode = p && p->type == "Episode" && !p->series_name.empty();   /* a recorded episode */
    const std::string title = episode ? p->series_name : p ? p->name : ch->name;
    gfx::text(kPad, y, title, {gfx::Bold, 56, 980}, alpha(kText, a));
    if (!p) {
        gfx::text(kPad, y + 50, T("Ingen programinformasjon for denne kanalen."), {gfx::Medium, 24, 980},
                  alpha(kText2, a));
        return;
    }
    y += 50;
    x = kPad;
    const gfx::TextStyle meta{gfx::Medium, 24};
    auto sep = [&] {   /* a dot between two things (none before the first) */
        if (x <= kPad)
            return;
        gfx::fill({x + 12, y - 10, 5, 5}, alpha(kText3, a), 2.5f);
        x += 29;
    };
    if (timed) {
        x += gfx::text(x, y, span(*p), meta, alpha(kText2, a));
        sep();
        x += gfx::text(x, y, minutes_label(p->end_utc - p->start_utc), meta, alpha(kText2, a));
    } else if (p->runtime_ticks > 0) {
        x += gfx::text(x, y, minutes_label(p->runtime_ticks / jf::kTicksPerSecond), meta, alpha(kText2, a));
    }
    const std::string &sub = episode ? p->name : p->episode_title;
    if (!sub.empty()) {
        sep();
        x += gfx::text(x, y, sub, {gfx::Medium, 24, 420}, alpha(kText2, a));
    }
    const std::string kind = livetv::kind_label(*p);
    if (!kind.empty()) {
        sep();
        x += gfx::text(x, y, kind, meta, alpha(kText2, a));
    }
    if (p->year > 0 && p->is_movie) {
        sep();
        x += gfx::text(x, y, std::to_string(p->year), meta, alpha(kText2, a));
    }
    if (x > kPad)
        x += 18;
    if (p->is_live && !airing)   /* a live broadcast to come (on now, the channel line says so) */
        x += tag(x, y, T("DIREKTESENDT"), 0x33ffffffu, kText, a) + 10;
    if (p->is_premiere)
        x += tag(x, y, T("PREMIERE"), 0x33ffffffu, kText, a) + 10;
    else if (p->is_new)
        x += tag(x, y, T("NY"), 0x33ffffffu, kText, a) + 10;
    if (!p->timer_id.empty() || !p->series_timer_id.empty()) {
        rec_mark(x + 2, y - 8, !p->series_timer_id.empty(), a);
        x += (p->series_timer_id.empty() ? 12 : 21) + 10;
        gfx::text(x, y, !p->series_timer_id.empty() ? T("Serien tas opp") : T("Tas opp"), {gfx::SemiBold, 22},
                  alpha(kText2, a));
    }

    /* How far it has come. */
    if (airing) {
        y += 34;
        const float bw = 320, f = (float)(t - p->start_utc) / (float)std::max<int64_t>(1, p->end_utc - p->start_utc);
        gfx::fill({kPad, y - 6, bw, 6}, alpha(0x47ffffffu, a), 3);
        gfx::fill({kPad, y - 6, bw * std::min(1.f, f), 6}, alpha(kText, a), 3);
        char left[64];
        std::snprintf(left, sizeof left, T("%s igjen"), minutes_label(p->end_utc - t).c_str());
        gfx::text(kPad + bw + 18, y + 2, left, {gfx::Medium, 21}, alpha(kText3, a));
    }
    if (!p->overview.empty())
        gfx::text(kPad, y + 50, p->overview, {gfx::Regular, 24, 980, 2, 34}, alpha(kText2, a));
}

void LiveTv::draw_filters(float dt)
{
    std::vector<std::string> labels{T("Alle"), T("Favoritter"), T("Filmer"), T("Sport"), T("Nyheter"), T("Barn"),
                                    T("Serier")};
    if (has_recordings())
        labels.push_back(T("Opptak"));
    pill_bar(kPad - 6, kFiltersY, labels, (int)m_filter, m_focused && m_zone == Zone::Filters, m_pill_drop, dt, 1.f,
             &m_animating);
    /* What the guide's other buttons do (nothing on screen says it otherwise). */
    if (m_focused && m_zone == Zone::Guide && m_filter != Recordings && !m_rows.empty())
        draw_pad_hints(gfx::W - kPad, kFiltersY + 38,
                       {{PadButton::L2, ""}, {PadButton::R2, T("Dag")}, {PadButton::Square, T("Favoritt")},
                        {PadButton::Options, T("Valg")}},
                       2, 26);
}

/* ---- Opptak ------------------------------------------------------------------------ */

bool LiveTv::has_recordings() const
{
    return m_client.can_record() || !m_guide->recordings.empty() || !m_guide->timers.empty();
}

const std::vector<jf::Item> &LiveTv::rec_row(int row) const
{
    return row == 0 ? m_guide->recordings : m_guide->timers;
}

const jf::Item *LiveTv::rec_focused() const
{
    if (m_zone != Zone::Guide)
        return nullptr;
    const std::vector<jf::Item> &items = rec_row(m_rec_row);
    const int c = m_rec_col[m_rec_row];
    return c >= 0 && c < (int)items.size() ? &items[c] : nullptr;
}

Action LiveTv::recordings_input(uint32_t p)
{
    Action action;
    const int before_row = m_rec_row, before_col = m_rec_col[m_rec_row];
    int &col = m_rec_col[m_rec_row];
    const int n = (int)rec_row(m_rec_row).size();
    if (p & NUVIO_BTN_UP) {
        if (m_rec_row == 1 && !rec_row(0).empty())
            m_rec_row = 0;
        else
            m_zone = Zone::Filters;
    } else if (p & NUVIO_BTN_DOWN) {
        if (m_rec_row == 0 && !rec_row(1).empty())
            m_rec_row = 1;
        else
            m_bump = true;
    } else if (p & NUVIO_BTN_LEFT) {
        if (col > 0) col--;
        else m_bump = true;
    } else if (p & NUVIO_BTN_RIGHT) {
        if (col + 1 < n) col++;
        else m_bump = true;
    } else if (p & NUVIO_BTN_CIRCLE) {
        if (col > 0)
            col = 0;
        else
            m_zone = Zone::Filters;
    } else if (p & (NUVIO_BTN_CROSS | NUVIO_BTN_OPTIONS)) {
        if (const jf::Item *it = rec_focused()) {
            if (m_rec_row == 0 && (p & NUVIO_BTN_CROSS)) {
                action.kind = Action::Play;   /* a recording plays as any title does */
                action.item = *it;
            } else if (m_rec_row == 1) {
                jf::Item ch;
                if (const jf::Item *known = m_guide->channel(it->channel_id)) {
                    ch = *known;
                } else {
                    ch.id = it->channel_id;
                    ch.name = it->channel_name;
                    ch.type = "TvChannel";
                }
                m_sheet.open(*it, ch, m_client.can_record());
            }
        }
    }
    m_rec_col[m_rec_row] = std::max(0, std::min(m_rec_col[m_rec_row], (int)rec_row(m_rec_row).size() - 1));
    if (m_rec_row != before_row || m_rec_col[m_rec_row] != before_col)
        m_focus_changed = m_now;
    return action;
}

void LiveTv::draw_recordings(float dt)
{
    const float cw = 288, ch = 162, gap = 24;
    const char *const titles[2] = {T("Opptak"), T("Planlagte opptak")};
    if (rec_row(0).empty() && rec_row(1).empty()) {
        gfx::text(gfx::W / 2, 800, T("Ingen opptak ennå."), {gfx::Bold, 30}, kText2, 1);
        gfx::text(gfx::W / 2, 848, T("Velg et program som kommer i guiden, og trykk \xE2\x9C\x95 for å ta det opp."),
                  {gfx::Medium, 24, 1400}, kText3, 1);
        return;
    }
    float y = 640;
    gfx::Rect focus_rect{0, 0, 0, 0};
    int focus_key = -1;
    struct Card {
        gfx::Rect r;
        const jf::Item *it;
        bool timer;
    };
    std::vector<Card> cards;
    for (int row = 0; row < 2; row++) {
        const std::vector<jf::Item> &items = rec_row(row);
        if (items.empty())
            continue;
        gfx::text(kPad, y + 24, titles[row], {gfx::Bold, 26}, alpha(0xebffffffu, 1.f));
        const float top = y + 40;
        const int c = m_rec_col[row];
        const float max_scroll = std::max(0.f, items.size() * (cw + gap) - gap - (gfx::W - 2 * kPad));
        m_rec_scroll[row].to(std::min(max_scroll, std::max(0.f, (c - 1) * (cw + gap))));
        if (m_rec_scroll[row].step(dt, 12.f))
            m_animating = true;
        for (size_t i = 0; i < items.size(); i++) {
            const float x = kPad + i * (cw + gap) - m_rec_scroll[row].value;
            if (x > gfx::W + 20 || x + cw < -20)
                continue;
            const gfx::Rect r{x, top, cw, ch};
            cards.push_back({r, &items[i], row == 1});
            if (m_focused && m_zone == Zone::Guide && row == m_rec_row && (int)i == c) {
                focus_rect = {r.x - 6, r.y - 6, r.w + 12, r.h + 12};
                focus_key = row * 4096 + (int)i;
            }
        }
        y = top + ch + 36;
    }
    /* The focus: the drop behind the focused card, then the cards and their labels. */
    if (focus_key >= 0 && !m_sheet.active())
        m_rec_drop.to(focus_rect, focus_key);
    else
        m_rec_drop.hide();
    m_rec_drop.draw(dt, 1.f, &m_animating, 18);
    for (const Card &c : cards) {
        const std::string url = program_art(m_client, *c.it, 640);
        if (!url.empty() && !art::failed(url)) {
            art::draw(c.r, url, c.it->primary_blurhash, 640, 360, kCellR, 1.f);
            gfx::fill_vgradient({c.r.x, c.r.y + c.r.h * 0.4f, c.r.w, c.r.h * 0.6f}, 0x00000000u, 0xcc000000u, kCellR);
        } else {
            draw_glass_placeholder(c.r, kCellR, 1.f);
        }
        /* (a recorded episode is filed as its series' episode: the series names it) */
        const bool episode = c.it->type == "Episode" && !c.it->series_name.empty();
        gfx::text(c.r.x + 14, c.r.y + c.r.h - (c.timer || episode ? 40 : 16), episode ? c.it->series_name : c.it->name,
                  {gfx::SemiBold, 20, c.r.w - 28}, kText);
        if (episode && !c.timer)
            gfx::text(c.r.x + 14, c.r.y + c.r.h - 15, c.it->name, {gfx::Medium, 17, c.r.w - 28}, kText2);
        if (c.timer) {   /* when, and that it will be recorded */
            rec_mark(c.r.x + 14, c.r.y + c.r.h - 22, !c.it->series_timer_id.empty(), 1.f);
            const float dx = c.it->series_timer_id.empty() ? 20 : 29;
            gfx::text(c.r.x + 14 + dx, c.r.y + c.r.h - 15,
                      c.it->timer_status == "InProgress" ? std::string(T("Tas opp nå"))
                                                         : livetv::day_label(c.it->start_utc) + " " + livetv::clock(c.it->start_utc),
                      {gfx::Medium, 17, c.r.w - 28 - dx}, kText2);
        }
    }
}

void LiveTv::draw_guide(float dt)
{
    const int64_t t = livetv::now();
    const int n = (int)m_rows.size();

    if (!m_guide->loaded) {
        const std::string msg = m_guide->failed ? T("Fikk ikke kontakt med serveren.") : T("Henter kanalene \xE2\x80\xA6");
        gfx::text(gfx::W / 2, 820, msg, {gfx::Medium, 28}, kText2, 1);
        return;
    }
    if (n == 0) {
        const char *msg = m_guide->channels.empty() ? T("Serveren har ingen kanaler.")
                          : m_filter == Favorites   ? T("Ingen favorittkanaler ennå. Trykk \xE2\x96\xA1 på en kanal i guiden.")
                                                    : T("Ingen kanaler sender noe slikt de neste timene.");
        gfx::text(gfx::W / 2, 820, msg, {gfx::Medium, 28, 1400}, kText2, 1);
        return;
    }

    /* Scroll: time eases by half hours, rows keep the focused one second from the top. */
    if (m_t0.step(dt, 10.f))
        m_animating = true;
    const int visible = (int)((gfx::H - kGridY) / kRowH);
    m_scroll.to((float)std::max(0, std::min(m_row - 1, n - visible)));
    if (m_scroll.step(dt, 12.f))
        m_animating = true;
    const int64_t t0 = m_base + (int64_t)std::lround(m_t0.value * 60.f);
    const float x_of_scale = kPxPerMin / 60.f;
    auto x_of = [&](int64_t s) { return kGridX + (float)(s - m_base) * x_of_scale - m_t0.value * kPxPerMin; };
    const int64_t t1 = t0 + (int64_t)(view_minutes() * 60);

    /* The ruler: the day, then every half hour (a time the now pill covers is left out). */
    {   /* the day picker: ‹ I dag › (its chevrons while it has focus) */
        const bool on = m_focused && m_zone == Zone::Day;
        const gfx::Rect r{kPad - 8, kRulerY - 30, kChanW + 8, 42};
        if (on)
            m_day_drop.to(r, view_day());
        else
            m_day_drop.hide();
        m_day_drop.draw(dt, 1.f, &m_animating, 21);
        const std::string day = livetv::day_label(day_time());
        const gfx::TextStyle ds{gfx::Bold, 22, kChanW - 70};
        if (on) {
            const int d = view_day();
            gfx::text(r.x + 16, kRulerY - 2, "\xE2\x80\xB9", {gfx::Bold, 26}, d > 0 ? kText : alpha(kText3, 0.5f));
            gfx::text(r.x + r.w / 2, kRulerY, day, ds, kText, 1);
            gfx::text(r.x + r.w - 16, kRulerY - 2, "\xE2\x80\xBA", {gfx::Bold, 26}, d < kDays - 1 ? kText : alpha(kText3, 0.5f), 2);
        } else {
            gfx::text(kPad, kRulerY, day, ds, kText2);
        }
    }
    gfx::push_scissor({kGridX - 4, kRulerY - 40, kGridR - kGridX + 8, 60});
    const float now_x = x_of(t), now_half = gfx::text_width(livetv::clock(t), {gfx::Bold, 18}) / 2 + 10;
    for (int64_t s = livetv::half_hour(t0 - 1800); s < t1 + 1800; s += 1800) {
        const float x = x_of(s);
        /* Midnight says which day begins there ("I morgen", "Lørdag"): the day changes in view. */
        const time_t st = (time_t)s;
        struct tm lt;
        localtime_r(&st, &lt);
        const bool midnight = lt.tm_hour == 0 && lt.tm_min == 0;
        const std::string label = midnight ? livetv::day_label(s) : livetv::clock(s);
        const gfx::TextStyle ls{midnight ? gfx::Bold : gfx::SemiBold, 20};
        const float label_w = gfx::text_width(label, ls);
        if (x + 10 + label_w + 6 > now_x - now_half && x - 6 < now_x + now_half)
            continue;
        gfx::fill({x, kRulerY - 20, 2, 22}, midnight ? kText2 : alpha(kText3, 0.6f), 1);
        gfx::text(x + 10, kRulerY, label, ls, midnight ? kText : kText3);
    }
    gfx::pop_scissor();

    const float sy = m_scroll.value * kRowH;
    const gfx::Rect view{kPad - 30, kGridY - 8, kGridR - kPad + 60, gfx::H - kGridY + 8};
    const float more_below = std::max(0.f, (n - visible) * kRowH - sy + 30);
    gfx::push_scissor(view);
    gfx::push_fade_mask(view, edge_fade(sy, 48), edge_fade(more_below, 96));

    struct Drawn {
        gfx::Rect r;
        const jf::Item *program;
        int64_t start, end;
        bool focus, channel;
        const jf::Item *ch;
        bool loading;
    };
    std::vector<Drawn> drawn;
    gfx::Rect focus_rect{0, 0, 0, 0};
    int focus_key = -1;
    const int first = std::max(0, (int)std::floor(m_scroll.value) - 1);
    for (int r = first; r < n && r < first + visible + 3; r++) {
        const jf::Item &ch = m_guide->channels[m_rows[r]];
        const float y = kGridY + r * kRowH - sy;
        if (y > gfx::H + 10)
            break;
        const bool row_focus = m_focused && m_zone == Zone::Guide && r == m_row;
        /* The channel. */
        const gfx::Rect cr{kPad, y, kChanW, kCellH};
        gfx::fill(cr, row_focus ? 0x1fffffffu : 0x12ffffffu, kCellR);
        drawn.push_back({cr, nullptr, 0, 0, row_focus && m_on_channel, true, &ch, false});
        if (row_focus && m_on_channel) {
            focus_rect = cr;
            focus_key = r * 4096;
        }
        /* Its programmes. */
        for (const Cell &c : cells_of(ch)) {
            if (c.end <= t0 - 60 || c.start >= t1 + 60)
                continue;
            const float x0 = std::max(kGridX, x_of(c.start)) + 2, x1 = std::min(kGridR, x_of(c.end)) - 2;
            if (x1 - x0 < 4)
                continue;
            const gfx::Rect rr{x0, y, x1 - x0, kCellH};
            const bool airing = c.start <= t && t < c.end;
            const bool past = c.end <= t;
            uint32_t fill = c.loading ? 0x05ffffffu : !c.program ? 0x08ffffffu : airing ? 0x1cffffffu : past ? 0x0affffffu : 0x12ffffffu;
            gfx::fill(rr, fill, kCellR);
            if (airing && c.program) {   /* how far it has come: a line along its foot */
                const float f = (float)(t - c.start) / (float)std::max<int64_t>(1, c.end - c.start);
                const float fx0 = x_of(c.start), fw = (x_of(c.end) - fx0) * f;
                const float lx = std::max(x0 + 10, fx0 + 10), rx = std::min(x1 - 10, fx0 + fw);
                if (rx > lx)
                    gfx::fill({lx, y + kCellH - 8, rx - lx, 3}, 0x8cffffffu, 1.5f);
            }
            if (c.program && m_filter != All && m_filter != Favorites && !matches(*c.program))
                gfx::fill(rr, alpha(kBg, 0.55f), kCellR);   /* not what the filter asks: in the background */
            const bool f = row_focus && !m_on_channel && c.start <= m_focus_start && m_focus_start < c.end;
            if (f) {
                focus_rect = rr;
                focus_key = r * 4096 + (int)((c.start / 60) % 4096);
            }
            drawn.push_back({rr, c.program, c.start, c.end, f, false, &ch, c.loading});
        }
    }
    /* Now. */
    const float nx = x_of(t);
    if (nx >= kGridX && nx <= kGridR)
        gfx::fill({nx - 1, kGridY - 8, 2, gfx::H - kGridY + 8}, alpha(kLive, 0.9f), 1);
    gfx::pop_fade_mask();
    gfx::pop_scissor();
    if (nx >= kGridX && nx <= kGridR) {   /* its time on the ruler, on a red pill */
        const std::string now_s = livetv::clock(t);
        const gfx::TextStyle ts{gfx::Bold, 18};
        const float w = gfx::text_width(now_s, ts) + 20;
        gfx::fill({nx - w / 2, kRulerY - 24, w, 30}, kLive, 15);
        gfx::text(nx, kRulerY - 3, now_s, ts, kText, 1);
    }

    /* The focus: the drop over the panes, then every label crisp on top. */
    gfx::push_scissor(view);
    if (focus_key >= 0 && m_focused && m_zone == Zone::Guide && !m_sheet.active())
        m_drop.to(focus_rect, focus_key, 0, kGridY - sy);
    else
        m_drop.hide();
    m_drop.draw(dt, 1.f, &m_animating, kCellR);
    gfx::push_fade_mask(view, edge_fade(sy, 48), edge_fade(more_below, 96));
    for (const Drawn &d : drawn) {
        if (d.channel) {
            const jf::Item &ch = *d.ch;
            const float lx = d.r.x + 70;
            gfx::text(d.r.x + 18, d.r.y + 45, ch.channel_number, {gfx::SemiBold, 22, 50},
                      d.focus ? kText : kText3);
            if (!draw_logo(m_client, ch, {lx, d.r.y + 10, d.r.w - 70 - 16, d.r.h - 20}, 1.f))
                gfx::text(lx, d.r.y + 45, ch.name, {gfx::SemiBold, 22, d.r.w - 70 - 16}, d.focus ? kText : kText2);
            if (ch.favorite)
                gfx::text(d.r.x + d.r.w - 12, d.r.y + 24, "\xE2\x99\xA5", {gfx::Bold, 16}, alpha(kLive, 0.9f), 2);
            continue;
        }
        if (d.r.w < 40 || d.loading)
            continue;   /* (not loaded yet: an empty pane, filled in a moment) */
        const bool past = d.end <= t;
        const uint32_t ink = d.focus ? kText : past ? kText3 : kText2;
        float tx = d.r.x + 16;
        const uint32_t kc = d.program ? livetv::kind_color(*d.program) : 0;
        if (kc && d.r.w > 60) {
            gfx::fill({d.r.x + 10, d.r.y + 14, 4, d.r.h - 28}, alpha(kc, past ? 0.5f : 1.f), 2);
            tx += 8;
        }
        const float tw = d.r.x + d.r.w - tx - 12;
        if (tw < 24)
            continue;
        const bool rec = d.program && (!d.program->timer_id.empty() || !d.program->series_timer_id.empty());
        const float rec_w = rec ? (d.program->series_timer_id.empty() ? 20.f : 30.f) : 0.f;
        const std::string title = d.program ? d.program->name : T("Ingen programinformasjon");
        gfx::text(tx, d.r.y + 32, title, {d.focus ? gfx::Bold : gfx::SemiBold, 22, tw - rec_w},
                  d.program ? ink : kText3);
        if (rec && tw > 60)
            rec_mark(std::min(tx + gfx::text_width(title, {gfx::SemiBold, 22, tw - rec_w}) + 8, d.r.x + d.r.w - 12 - rec_w + 6),
                     d.r.y + 25, !d.program->series_timer_id.empty(), 1.f);
        if (d.program && tw > 70)
            gfx::text(tx, d.r.y + 58, span(*d.program), {gfx::Medium, 18, tw}, d.focus ? kText2 : kText3);
    }
    gfx::pop_fade_mask();
    gfx::pop_scissor();
}

void LiveTv::draw(double now, float dt)
{
    m_now = now;
    m_dt = dt;
    m_animating = false;
    m_drawn_minute = livetv::now() / 30;
    livetv::refresh();   /* (does nothing while what it has is fresh) */
    if (livetv::version() != m_version) {
        std::string focused_id;   /* (from the rows of the guide they were built for) */
        if (const jf::Item *ch = row_channel(m_row))
            focused_id = ch->id;
        m_version = livetv::version();
        m_guide = livetv::guide();
        if (m_base == 0 || m_base != m_guide->from) {
            /* A new half hour: the guide starts there; what was in view stays put. */
            const int64_t left = m_base + (int64_t)std::lround(m_t0.target * 60.f);
            m_base = m_guide->from;
            const float t0 = m_base ? (float)(std::max(left, m_base) - m_base) / 60.f : 0.f;
            m_t0.snap(left > 0 ? t0 : 0.f);
        }
        rebuild(focused_id);
        if (!m_placed && m_guide->loaded && !m_rows.empty()) {
            m_placed = true;
            place_on_last_channel();
            m_anchor = livetv::now();
            focus_at(m_anchor);
        }
        keep_in_view();
    }

    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, kBg);
    if (m_filter == Recordings && !has_recordings())
        m_filter = All;   /* (the right to record went, and nothing is recorded) */
    if (m_filter == Recordings) {
        const jf::Item *rec = rec_focused();
        draw_hero_item(rec ? rec->id : std::string(), rec, rec ? m_guide->channel(rec->channel_id) : nullptr, dt);
    } else {
        Cell cell;
        const bool has_cell = focused_cell(&cell);
        draw_hero(row_channel(m_row), has_cell ? &cell : nullptr, dt);
    }
    gfx::fill_vgradient({0, 0, gfx::W, 220}, 0x8c000000u, 0x00000000u);   /* under the top bar */
    draw_filters(dt);
    if (m_filter == Recordings)
        draw_recordings(dt);
    else
        draw_guide(dt);
    m_sheet.draw(m_client, dt, &m_animating);
    if (now < m_note_until) {   /* a note on what was just done: in quickly, out over the last half second */
        const double in = std::min(1.0, (now - (m_note_until - 3.0)) * 4), out = std::min(1.0, (m_note_until - now) * 2);
        draw_note(m_note, (float)std::max(0.0, std::min(in, out)), kLive);
        m_animating = true;
    }
    if (art::animating())
        m_animating = true;
}

/* ---- tuning ---------------------------------------------------------------------- */

void draw_tuning(const std::string &channel_id, const std::string &fallback_name, float a)
{
    const livetv::GuideRef g = livetv::guide();
    const jf::Item *ch = g->channel(channel_id);
    const int64_t t = livetv::now();
    const jf::Item *p = g->on_at(channel_id, t);
    if (!p && ch)
        p = ch->now_on();
    const gfx::Rect full{0, 0, gfx::W, gfx::H};
    gfx::fill(full, alpha(0xff080b10u, a));
    /* What airs, faint: such pictures often spell out their title, which is written below. */
    const std::string pic = p ? livetv::picture_url(*p, 1920) : std::string();
    if (const gfx::Texture *tex = pic.empty() ? nullptr : art::get(pic, 1920, 1080))
        gfx::image(full, tex, 0.3f * a, 0, true);
    gfx::fill_vgradient({0, 0, gfx::W, 378}, alpha(0x4d000000u, a), alpha(0x99000000u, a));
    gfx::fill_vgradient({0, 378, gfx::W, 378}, alpha(0x99000000u, a), alpha(0xcc000000u, a));
    gfx::fill_vgradient({0, 756, gfx::W, 324}, alpha(0xcc000000u, a), alpha(0xe6000000u, a));
    const float y = gfx::H / 2 - 40;
    if (ch)
        draw_logo_fit(livetv::logo_url(*ch, 400), {gfx::W / 2 - 130, y - 60 - 120, 260, 120}, a);
    const std::string name = ch ? (ch->channel_number.empty() ? "" : ch->channel_number + "  ") + ch->name : fallback_name;
    gfx::text(gfx::W / 2, y, name, {gfx::SemiBold, 30, 1200}, alpha(kText2, a), 1);
    if (p)
        gfx::text(gfx::W / 2, y + 56, p->name, {gfx::Bold, 44, 1400}, alpha(kText, a), 1);
}

void show_tuning(const std::string &channel_id, const std::string &fallback_name)
{
    gfx::begin_frame();
    draw_tuning(channel_id, fallback_name, 1.f);
    gfx::end_frame();
}

} // namespace ui
