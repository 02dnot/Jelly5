/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The home screen (concept/: Netflix-style hero and rows, Apple TV focus):
 * a featured title with its logo and backdrop, then the rows. Focus drives an
 * ambient backdrop and an info panel; rows ease into place and remember
 * where they were left. Circle walks back: to the start of the row, then to
 * the hero.
 */
#pragma once

#include "jf/jf_client.h"
#include "ui/anim.h"
#include "ui/screen.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ui {

struct HomeRow {
    std::string title;
    std::vector<jf::Item> items;
    bool plays = false;     /* continue watching / next up: Cross plays; else it opens */
};

struct HomeModel {
    std::vector<jf::Item> hero;
    std::vector<HomeRow> rows;
};

class Home : public Screen {
public:
    explicit Home(jf::Client &client) : m_client(client) {}

    void set_model(HomeModel model);
    bool empty() const { return m_model.hero.empty() && m_model.rows.empty(); }

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return m_hero_mode.value; }

private:
    const jf::Item *focused_item() const;
    void draw_backdrop(float dt);
    void draw_info(const jf::Item &it, float bottom, bool hero, float alpha);
    void draw_rows(float dt);
    std::string card_url(const jf::Item &it) const;
    std::string backdrop_url(const jf::Item &it) const;

    jf::Client &m_client;
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
    };
    std::vector<BackdropLayer> m_bd;

    /* Info panel: fades out, swaps, fades in when focus settles on a new title. */
    std::string m_info_id;
    Anim m_info_alpha;
    double m_focus_changed = 0;

    bool m_animating = false;
    double m_now = 0;
};

} // namespace ui
