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
        Open,       /* open item's detail page */
        ToNav,      /* focus moves up into the tab bar */
        Back,       /* leave this (pushed) screen */
        SwitchUser, /* to "Hvem ser på?" */
        SignOut,    /* forget this account's sign-in */
        Changed,    /* change: write it, then refresh the home rows */
    } kind = None;
    jf::Item item;
    UserDataChange change;
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
    /* 0..1: how much of the top navigation shows over this screen. */
    virtual float nav_alpha() const = 0;
};

/* Per-card focus lift (0..1), eased; keyed by whatever identifies the card. */
class Lifts {
public:
    float step(const std::string &key, bool focused, float dt, bool *animating);

private:
    std::map<std::string, Anim> m_lift;
};

/* A 2:3 poster with blurhash placeholder, focus lift and shadow. focus_label
 * draws the title under it once lifted. */
void draw_poster(jf::Client &c, const jf::Item &it, const gfx::Rect &r, float lift, float opacity);

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

/* The brand: the mark (a rounded gradient square with a white J) and the
 * "Jelly5" wordmark, with its left edge at x and the text on baseline.
 * size is the wordmark's font size. Returns the total width. */
float draw_brand(float x, float baseline, float size, float opacity = 1.f, bool glow = false);
float brand_width(float size);

/* Landscape art for an item: an episode's still, else a thumb, else a backdrop. */
std::string landscape_url(jf::Client &c, const jf::Item &it, int width);
std::string landscape_blurhash(const jf::Item &it);

} // namespace ui
