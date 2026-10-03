/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Frame timing for the log: how long frames take to draw (CPU and the wait for
 * the GPU, which the runtime does before every flip), how far apart they are
 * presented, and how many miss the display's refresh. One line every 10 s of
 * drawing ("perf ui: ..." / "perf video: ...").
 */
#pragma once

namespace perf {

class Frames {
public:
    explicit Frames(const char *name) : m_name(name) {}
    /* draw_ms: the frame's drawing and presenting; at: when it was presented (s). */
    void note(double draw_ms, double at, const char *extra = nullptr);

private:
    const char *m_name;
    double m_window = 0, m_last = 0;
    int m_n = 0, m_late = 0;
    double m_sum = 0, m_max = 0, m_gap_max = 0;
    float m_draws[2048];
};

} // namespace perf
