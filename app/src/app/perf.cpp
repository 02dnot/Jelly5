/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/perf.h"

#include "evo_agc_runtime.h"
#include "evo_boot_trace.h"

#include <algorithm>

namespace perf {

void Frames::note(double draw_ms, double at, const char *extra)
{
    const double period = evo_agc_runtime_is_120hz() ? 1000.0 / 119.88 : 1000.0 / 59.94;
    if (m_last > 0 && !m_paused) {
        const double gap = (at - m_last) * 1000.0;
        if (gap < 250.0) {   /* frames in a run (an idle pause is not a dropped frame) */
            m_gap_max = std::max(m_gap_max, gap);
            if (gap > period * 1.5)
                m_late++;
        }
    }
    m_last = at;
    m_paused = false;
    if (m_window == 0)
        m_window = at;
    if (m_n < 2048)
        m_draws[m_n] = (float)draw_ms;
    m_n++;
    m_sum += draw_ms;
    m_max = std::max(m_max, draw_ms);
    if (at - m_window < 10.0)
        return;
    const int n = std::min(m_n, 2048);
    std::sort(m_draws, m_draws + n);
    const double p95 = m_draws[std::min(n - 1, (int)(n * 0.95))];
    evo_bt("perf %s: %d frames in %.1f s (%.0f fps), draw avg %.2f p95 %.2f max %.2f ms, late %d, gap max %.1f ms "
           "(budget %.1f)%s%s",
           m_name, m_n, at - m_window, m_n / (at - m_window), m_sum / m_n, p95, m_max, m_late, m_gap_max, period,
           extra ? " " : "", extra ? extra : "");
    m_window = at;
    m_n = m_late = 0;
    m_sum = m_max = m_gap_max = 0;
}

} // namespace perf
