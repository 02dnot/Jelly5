/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A person's page (from cast and crew): portrait, name, birth, biography,
 * then what they are in here - films and series, newest first. Each card
 * opens its detail page. Circle goes back.
 */
#pragma once

#include "ui/screen.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class Person : public Screen {
public:
    Person(jf::Client &client, const jf::Item &person);

    void activate() override;
    Action input(uint32_t pressed) override;
    void draw(double now, float dt) override;
    bool animating() const override { return m_animating; }
    float nav_alpha() const override { return 0.f; }
    float enter() const override { return m_enter.value; }

private:
    struct Content {
        jf::Item person;
        bool loaded = false;
        std::vector<jf::Item> movies, series;
    };
    struct Data {
        std::mutex lock;
        Content c;
    };

    jf::Client &m_client;
    std::shared_ptr<Data> m_data = std::make_shared<Data>();
    Content m_view;
    int m_row = 0;                     /* 0 movies, 1 series (whichever exist) */
    int m_cols[2] = {0, 0};
    Anim m_scroll[2], m_page, m_enter, m_content;
    Lifts m_lifts;
    Ambient m_ambient;
    double m_opened = -1;
    bool m_animating = false;
};

} // namespace ui
