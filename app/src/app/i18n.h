/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interface's language. The text in the code is Norwegian and is its own
 * key: T("Spill av") is "Spill av" in Norwegian, "Play" in English, "Reproducir"
 * in Spanish. A language's table falls back to English, and English to the
 * Norwegian (logged once), so nothing goes blank. English is in i18n.cpp, the
 * others in i18n_<code>.cpp.
 *
 * The language follows the PS5's system language (English for one Jelly5 does
 * not have) unless Innstillinger -> Språk picks one.
 */
#pragma once

#include <string>

namespace i18n {

enum class Lang { Norwegian, English, Spanish, French, German, Portuguese, Italian };
/* The setting: Auto, or a language (Lang + 1). */
enum Choice { Auto = 0, Norwegian = 1, English = 2, ChoiceCount = 8 };
/* A choice's name in its own language ("Español"); Auto's is empty. */
const char *choice_name(int choice);

/* Applies the setting (Auto reads the system language). */
void set_choice(int choice);
Lang lang();
/* Not Norwegian: what is outside the tables (dates, genre and language names)
 * is then English. */
inline bool english() { return lang() != Lang::Norwegian; }
/* Bumped on every change: screens that keep built text rebuild it. */
unsigned generation();

} // namespace i18n

/* The text in the interface's language (the argument is the Norwegian). */
const char *T(const char *nb);
inline std::string T(const std::string &nb) { return T(nb.c_str()); }
