/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PS5 system keyboard (sceImeDialog): the console's own text entry, with
 * its predictive text, password mode and the PlayStation app's phone
 * keyboard. Ported from ProsperoTV (iptv_ime.c, GPL-3.0-or-later).
 *
 * Request it, then call ime::poll() every frame; the callback runs on the
 * caller's thread once the viewer confirms (not when they cancel).
 */
#pragma once

#include <functional>
#include <string>

namespace ime {

enum class Kind { Text, Url, Password, Search };

bool init();
bool active();    /* the dialog is up (the app should ignore the pad meanwhile) */
void request(Kind kind, const std::string &title, const std::string &initial,
             std::function<void(const std::string &)> done);
void poll();
void cancel();

} // namespace ime
