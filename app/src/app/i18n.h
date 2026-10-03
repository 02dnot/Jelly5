/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interface's language: Norwegian or English. The text in the code is
 * Norwegian and is its own key: T("Spill av") is "Spill av" in Norwegian and
 * "Play" in English. A text without a translation stays Norwegian (and is
 * logged once), so nothing goes blank.
 *
 * The language follows the PS5's system language (Norwegian there gives
 * Norwegian, anything else English) unless Innstillinger -> Språk picks one.
 */
#pragma once

#include <string>

namespace i18n {

enum class Lang { Norwegian, English };
enum Choice { Auto = 0, Norwegian = 1, English = 2 };

/* Applies the setting (Auto reads the system language). */
void set_choice(int choice);
Lang lang();
inline bool english() { return lang() == Lang::English; }
/* Bumped on every change: screens that keep built text rebuild it. */
unsigned generation();

} // namespace i18n

/* The text in the interface's language (the argument is the Norwegian). */
const char *T(const char *nb);
inline std::string T(const std::string &nb) { return T(nb.c_str()); }
