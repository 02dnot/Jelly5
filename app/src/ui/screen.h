/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What every top-level screen (a tab under the top navigation) provides, and
 * the drawing pieces they share: colours from concept/style.css, the poster
 * card with its focus lift, and the blurred ambient background.
 */
#pragma once

#include "gfx/gfx.h"
#include "jf/jf_client.h"
#include "ui/anim.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ui {

constexpr float kPad = 120;
constexpr uint32_t kBg = 0xff07070a;
constexpr uint32_t kText = 0xfff5f5f7;
constexpr uint32_t kText2 = 0xadebebf5;
constexpr uint32_t kText3 = 0x6bebebf5;

uint32_t alpha(uint32_t c, float a);

/* What the viewer changed on a title (written to Jellyfin by the app; screens
 * apply it to their own lists at once). */
struct UserDataChange {
    std::string id;
    bool favorite_set = false, favorite = false;
    bool played_set = false, played = false;
    bool resume_cleared = false;
};

struct Action {
    enum Kind {
        None,
        Play,       /* play item (a series plays its next episode) */
        PlayFromStart,
        PlayShuffled, /* music: item first, the rest of its album in random order */
        PlayMix,      /* music: Jellyfin's Instant Mix from item */
        Open,       /* open item's detail page */
        ToNav,      /* focus moves up into the tab bar */
        Back,       /* leave this (pushed) screen */
        SwitchUser, /* to "Hvem ser på?" */
        SignOut,    /* forget this account's sign-in */
        Changed,    /* change: write it, then refresh the home rows */
    } kind = None;
    jf::Item item;
    UserDataChange change;
    std::vector<jf::Item> queue;   /* Play: a queue to play from queue_start (a playlist) */
    size_t queue_start = 0;
};

class Screen {
public:
    virtual ~Screen() = default;
    /* The tab was opened (or re-entered from the navigation bar). */
    virtual void activate() {}
    virtual Action input(uint32_t pressed) = 0;
    virtual void draw(double now, float dt) = 0;
    virtual bool animating() const = 0;
    /* 0..1 while a pushed screen fades in over the one below (1 = opaque). */
    virtual float enter() const { return 1.f; }
    /* The focused card as last drawn: where it is and its picture, for the page
     * it opens to grow out of. false when there is none. */
    struct Card {
        gfx::Rect rect;
        std::string url, blurhash;
        float radius = 14;
    };
    virtual bool focused_card(Card *) const { return false; }
    /* The last press ran into an edge (a row's end): a soft bump on the controller. */
    bool take_bump()
    {
        const bool b = m_bump;
        m_bump = false;
        return b;
    }

    /* The top bar has the focus, not this screen: it shows no focus of its own
     * (no drop, no lifted card), so there is only ever one place that looks focused. */
    void set_focused(bool f) { m_focused = f; }
    /* The focus came down from the top bar: where it lands (default: as it was). */
    virtual void enter_from_top() {}
    /* A sheet is open over the screen (it owns every button until it closes). */
    virtual bool modal() const { return false; }

protected:
    bool m_bump = false;
    bool m_focused = true;

public:
    /* 0..1: how much of the top navigation shows over this screen. */
    virtual float nav_alpha() const = 0;
};

/* Per-card focus lift (0..1), eased; keyed by whatever identifies the card. */
class Lifts {
public:
    float step(const std::string &key, bool focused, float dt, bool *animating);

private:
    struct Lift {
        Anim a;
    };
    std::map<std::string, Lift> m_lift;
};

/* The focus drop: the one focus marker for controls everywhere (buttons, pills,
 * rows, keys), a drop of brighter liquid glass. One per group of controls: give
 * it the focused control's rect every frame and draw it over the controls, then
 * the focused label crisp on top. It follows on an underdamped spring - it
 * overshoots and settles back - stretches along its speed, thins as it does,
 * and swells when it sets off for a new control. */
class Drop {
public:
    /* key: identifies the focused control; a new key nudges the swell. ox, oy:
     * where the group is (it may scroll or slide): the drop springs within the
     * group and moves with it rigidly. */
    void to(const gfx::Rect &r, int key, float ox = 0, float oy = 0);
    void hide() { m_shown = false; }
    /* Steps the springs (sets *animating while it moves) and draws the drop. */
    void draw(float dt, float opacity, bool *animating, float radius = -1.f);
    /* Where it is drawn now, for a label that rides on it. */
    gfx::Rect rect() const { return m_drawn; }

private:
    float m_x = 0, m_y = 0, m_w = 0, m_h = 0, m_vx = 0, m_vy = 0, m_vw = 0, m_vh = 0;
    float m_pop = 0, m_vpop = 0;
    gfx::Rect m_target{0, 0, 0, 0}, m_drawn{0, 0, 0, 0};   /* m_target: within the group */
    float m_ox = 0, m_oy = 0;
    int m_key = -1;
    bool m_shown = false, m_placed = false;
};

/* A check mark drawn from small squares along its two strokes, centred on (cx, cy). */
void draw_check(float cx, float cy, float size, uint32_t color);

/* A 2:3 poster with blurhash placeholder, focus lift and shadow, and its title
 * under it (an album's artist, an episode's series below that). A Seerr title
 * (jf::Item::ext) has its status chip, and its name on a card of its own
 * colours while its picture loads or when there is none. */
void draw_poster(jf::Client &c, const jf::Item &it, const gfx::Rect &r, float lift, float opacity);

/* Where a Seerr title stands (a seerr::Status), in words: short for a chip
 * (empty when not requested), full for its page. And the colour of its dot. */
const char *seerr_status_label(int status, bool full = false);
uint32_t seerr_status_color(int status);
/* The status chip, its top left at (x, y); size is the type's. Returns the width (0: none). */
float draw_status_chip(float x, float y, int status, float opacity, float size = 15.f, float max_w = 0);
/* The same chip with a label and a dot of its own ("Avslått" ...). */
float draw_label_chip(float x, float y, const char *label, uint32_t color, float opacity, float size = 15.f,
                      float max_w = 0);
/* A note at the top of the screen on a pill of glass (how a request went):
 * opacity 0 draws nothing. */
void draw_note(const std::string &text, float opacity, uint32_t dot = 0xff30d158u);
/* A card for a title without a picture: colours picked by seed, the name on it. */
void draw_title_card(const gfx::Rect &r, const std::string &title, int seed, float radius, float opacity);
/* Where a Seerr title's picture has not come (yet): a pane of glass, as the
 * small chips are made (tint, sheen, lit rim; no blur: there are dozens on screen).
 * TMDB has no BlurHash to show meanwhile. */
void draw_glass_placeholder(const gfx::Rect &r, float radius, float opacity);

/* The ambient background: the focused title's backdrop as a blur (its
 * BlurHash, upscaled), dimmed, cross-faded as focus moves. */
class Ambient {
public:
    /* Changes only once focus has rested (0.35 s), then fades slowly: fast
     * scrolling never makes the background flicker. */
    void set(const std::string &blurhash, double now);
    void draw(float dt, float dim, bool *animating);

private:
    std::string m_cur, m_next, m_pending;
    double m_pending_since = 0, m_now = 0;
    Anim m_mix;
};

std::string poster_url(jf::Client &c, const jf::Item &it, int width);

/* The launch backdrop (start-up, a server that does not answer): near black with
 * a blue and a violet glow, drawn here at full size (no picture to upscale) and
 * dithered so it never bands. The PS5's own launch picture (sce_sys/pic1.dds) is
 * the same, from scripts/make_backdrop.py: keep the numbers in step. Null when
 * the GPU had no room. */
const gfx::Texture *launch_backdrop();

/* The brand: the mark (a rounded gradient square with a white J) and the
 * "Jelly5" wordmark, with its left edge at x and the text on baseline.
 * size is the wordmark's font size. Returns the total width. */
float draw_brand(float x, float baseline, float size, float opacity = 1.f, bool glow = false);

/* Apple TV's living backdrop: t covering full, slowly zooming in (6 % over 30 s
 * from age 0) and drifting toward a corner chosen by key. */
void draw_drift(const gfx::Rect &full, const gfx::Texture *t, float opacity, double age, const std::string &key);

/* Frosted glass (Apple TV's panels): a soft shadow, what lies under r blurred,
 * a tint over it and a hairline of light along the top. The tint is lighter
 * when the blur is there (the colours behind show through) and the old solid
 * dark glass when the GPU had no room for it. shadow = false for small pieces
 * (pills) that sit on other glass. */
/* lift: 0 a pane, 1 the brighter glass drop that marks focus (the top bar's tab). */
void glass_panel(const gfx::Rect &r, float radius, float opacity = 1.f, bool shadow = true, float lift = 0.f);
/* A row of pills on one glass pane (the library's sources, the servers on "Hvem
 * ser på?"): the pane's left edge x and top y, 76 tall; the drop on `on`, full
 * while the row has focus and faint while it only marks the choice (-1: none).
 * ox, oy: where the row's group is, for the drop (Drop::to). max_w caps a pill
 * (its label ellipsised). Returns the pane's width. */
float pill_bar(float x, float y, const std::vector<std::string> &labels, int on, bool focused, Drop &drop, float dt,
               float opacity, bool *animating, float ox = 0, float oy = 0, float max_w = 0);
/* The width pill_bar draws, and where its pill i starts and has its centre (from x). */
float pill_bar_width(const std::vector<std::string> &labels, float max_w = 0);
float pill_bar_offset(const std::vector<std::string> &labels, int i, float max_w = 0);
float pill_bar_center(const std::vector<std::string> &labels, int i, float max_w = 0);
/* A row of row_w in room: how far it has slid (eased) to keep the focused item,
 * centred at c in the row, in the middle of the room; 0 while the row fits. */
float row_scroll(Anim &a, float row_w, float room, float c, float dt, bool *animating);
/* A one-line label that ends in an ellipsis past st.max_w (as gfx::text). While it is
 * focused and cut, it waits a moment, then runs as a ticker for as long as it has
 * focus: out to the left, its start following a gap behind, a rest each time the
 * start is back in place. Unfocused, or when it fits, it is plain gfx::text.
 * Returns the width drawn. */
float marquee_text(float x, float baseline, const std::string &s, const gfx::TextStyle &st, uint32_t color,
                   bool focused, int align = 0);
/* True when a marquee moved in the frame just drawn (the shell draws another).
 * Read once per frame; it clears. */
bool marquee_take_animating();
/* "jf.example.net:8096" from "https://jf.example.net:8096/path". */
std::string host_of(const std::string &url);

/* How far a scrolling list fades out at an edge (for gfx::pop_fade): `depth` where
 * more lies beyond it, easing in over the first 40 px scrolled; 0 at its end, so
 * the first and last rows stand whole. */
inline float edge_fade(float hidden, float depth = 72.f)
{
    return depth * (hidden <= 0.f ? 0.f : hidden >= 40.f ? 1.f : hidden / 40.f);
}

/* A controller hint, drawn the way the PS5's own hints look (our shapes, not
 * Sony's artwork): the button on a dark disc (Options and the shoulder buttons
 * as a pill), then the label. x is the left edge, cy the vertical centre, size
 * the disc's height. Returns the width drawn. */
enum class PadButton { Cross, Circle, Triangle, Square, Options, Touchpad, L1, R1, L2, R2 };
float draw_pad_hint(float x, float cy, PadButton b, const std::string &label, float size = 30.f,
                    float opacity = 1.f);
float pad_hint_width(PadButton b, const std::string &label, float size = 30.f);
/* A row of hints ("[✕] Spill av   [○] Lukk"); align 0 left of x, 1 centred on x,
 * 2 ending at x. Returns the width. */
struct PadHint {
    PadButton button;
    std::string label;
};
float draw_pad_hints(float x, float cy, const std::vector<PadHint> &hints, int align = 0, float size = 28.f,
                     float opacity = 1.f);
float brand_width(float size);

/* A name's first letter, whole: its lead byte and UTF-8 continuation bytes
 * ("Øystein" gives "Ø", not half of it). */
inline std::string first_letter(const std::string &s)
{
    size_t n = s.empty() ? 0 : 1;
    while (n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80)
        n++;
    return s.substr(0, n);
}

/* A logo (any shape) whole inside box, centred, fading in. false when there is
 * none to be had (no url, or it failed): the caller writes the name instead.
 * true while it is still coming (nothing drawn: no name flashing up first). */
bool draw_logo_fit(const std::string &url, const gfx::Rect &box, float opacity);

/* Landscape art for an item: an episode's still, else a thumb, else a backdrop. */
std::string landscape_url(jf::Client &c, const jf::Item &it, int width);
std::string landscape_blurhash(const jf::Item &it);

} // namespace ui
