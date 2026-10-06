/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The home screen (concept/: Netflix-style hero and rows, Apple TV focus):
 * a featured title with its logo and backdrop, then the rows. Focus drives an
 * ambient backdrop and an info panel; rows ease into place and remember
 * where they were left. Circle walks back: to the start of the row, then to
 * the hero.
 *
 * The same screen is Seerr's tab (discover): rows of Seerr's titles
 * (jf::Item::ext), drawn as the library's with where each stands, no hero
 * and no options sheet; the top bar shows while the first row has focus.
 */
#pragma once

#include "jf/jf_client.h"
#include "ui/anim.h"
#include "ui/item_menu.h"
#include "ui/screen.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ui {

struct HomeRow {
    enum Kind { Resume, NextUp, MyList, Latest, Recommended, Genre, Libraries };
    std::string title;
    std::vector<jf::Item> items;
    bool plays = false;     /* continue watching / next up: Cross plays; else it opens */
    Kind kind = Latest;
    int shelf = -1;         /* Seerr's tab: which of its lists, for the next page (-1: none) */
    int page = 1;           /* the pages loaded so far */
    bool more = true;       /* the list may go on */
};

struct HomeModel {
    std::vector<jf::Item> hero;
    std::vector<HomeRow> rows;
};

class Home : public Screen {
public:
    explicit Home(jf::Client &client, bool discover = false) : m_client(client), m_discover(discover) {}

    void set_model(HomeModel model);
    /* Seerr's tab: a row whose focus is near its end, with more to come (its shelf
     * and the next page), or false. The caller loads it and sets the model again. */
    bool wants_more(int *shelf, int *page) const;
    bool empty() const { return m_model.hero.empty() && m_model.rows.empty(); }
    /* A change made here or on another screen, shown at once. */
    void apply(const UserDataChange &c);

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override
    {
        if (m_discover)   /* no hero: the bar shows over the first row, fading as the rows move up */
            return std::max(0.f, std::min(1.f, 1.f - m_rows_y.value));
        return m_hero_mode.value * (1.f - m_menu.visibility());
    }
    bool focused_card(Card *c) const override
    {
        if (m_has_card && m_row >= 0)
            *c = m_card;
        return m_has_card && m_row >= 0;
    }

private:
    const jf::Item *focused_item() const;
    void draw_backdrop(float dt);
    void draw_info(const jf::Item &it, float bottom, bool hero, float alpha);
    void draw_rows(float dt);
    std::string card_url(const jf::Item &it) const;
    std::string backdrop_url(const jf::Item &it) const;
    void draw_card_art(const jf::Item &it, const gfx::Rect &r, float radius, float opacity) const;

    jf::Client &m_client;
    bool m_discover = false;            /* Seerr's tab */
    HomeModel m_model;

    int m_row = -1;                     /* -1: the hero */
    std::vector<int> m_cols;            /* column each row was left at */
    int m_hero = 0;                     /* featured title shown */
    int m_hero_button = 0;              /* 0 play, 1 more info */
    double m_hero_since = 0;

    Anim m_rows_y;                      /* focused row index, eased */
    std::vector<Anim> m_scroll;         /* per-row horizontal offset (cards) */
    std::map<std::string, Anim> m_lift; /* per-card focus lift 0..1 */
    Anim m_hero_mode;                   /* 1 = hero, 0 = rows */

    /* Ambient backdrop: a stack, bottom fully shown, each one above fading in
     * over what is on screen. A new title is pushed on top, so a change of mind
     * mid-fade never jumps back to an older picture. */
    struct BackdropLayer {
        std::string url, hash;
        Anim mix;
        double since = 0;   /* when it arrived: the slow drift starts there */
    };
    std::vector<BackdropLayer> m_bd;

    /* Info panel: fades out, swaps, fades in when focus settles on a new title. */
    std::string m_info_id;
    Anim m_info_alpha;
    double m_focus_changed = 0;

    ItemMenu m_menu;                    /* Options on a title */

public:
    bool modal() const override { return m_menu.active(); }

private:
    Drop m_hero_drop;                   /* the focus on the hero's buttons */
    float m_dt = 0;
    Card m_card;                        /* the focused card as drawn (for the page it opens) */
    bool m_has_card = false;

    bool m_animating = false;
    double m_now = 0;
};

} // namespace ui
