/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A value that eases toward its target, frame-rate independent: an
 * exponential approach (the feel of the concept's cubic-bezier ease-out).
 */
#pragma once

#include <cmath>

namespace ui {

struct Anim {
    float value = 0, target = 0;

    void to(float t) { target = t; }
    void snap(float t) { value = target = t; }

    /* rate: how fast it closes the gap (per second; 12 ~ 250 ms to settle).
     * Returns true while still moving. */
    bool step(float dt, float rate = 12.f)
    {
        const float d = target - value;
        if (std::fabs(d) < 0.0005f * (1.f + std::fabs(target))) {
            value = target;
            return false;
        }
        value += d * (1.f - std::exp(-rate * dt));
        return true;
    }
};

inline float smoothstep(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3.f - 2.f * t);
}

} // namespace ui
