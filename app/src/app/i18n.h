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

enum class Lang {
    Norwegian, English, Spanish, French, German, Portuguese, Italian,
    /* every other PS5 system language */
    Japanese, Dutch, Russian, Korean, ChineseTraditional, ChineseSimplified, Finnish, Swedish, Danish, Polish, Turkish, Arabic, Czech, Hungarian, Greek, Romanian, Thai, Vietnamese, Indonesian, Ukrainian,
    Count
};
/* The setting: Auto, or a language (Lang + 1). */
enum Choice { Auto = 0, Norwegian = 1, English = 2, ChoiceCount = (int)Lang::Count + 1 };
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
/* A count in the interface's language: nb_one / nb_other are the Norwegian
 * formats ("%d sesong", "%d sesonger"), n goes in their %d. A table's entry for
 * nb_other holds the language's forms separated by '|', in CLDR's order and only
 * those whole numbers use (see plural_form in i18n.cpp): "%d season|%d seasons";
 * Russian "%d сезон|%d сезона|%d сезонов"; Japanese one form. */
std::string TN(int n, const char *nb_one, const char *nb_other);
