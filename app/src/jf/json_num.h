/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A JSON number as an integer. A server can send any number (1e+11, 1e999 =
 * inf), and a cast of one outside the type's range, or of NaN, is undefined
 * behaviour. Truncated as the cast is; NaN gives the fallback, a number beyond
 * the range the nearest end.
 */
#pragma once

#include <cmath>
#include <limits>

namespace jf {

template <typename T>
T to_int(double v, T fallback = 0)
{
    if (std::isnan(v))
        return fallback;
    if (v <= (double)std::numeric_limits<T>::min())
        return std::numeric_limits<T>::min();
    if (v >= (double)std::numeric_limits<T>::max())   /* rounds up for 64 bits: still out of range */
        return std::numeric_limits<T>::max();
    return (T)v;
}

} // namespace jf
