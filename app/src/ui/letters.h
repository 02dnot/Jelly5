/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The letters a library sorted by name jumps through (L2/R2), in the server's
 * own order. Jellyfin writes every SortName in Latin letters (Москва is
 * "moskva", さくら "sakura", 北京 "bei jing", Æ "ae"), so A-Z covers all of
 * it. Emby only drops accents (Å is A, É is E) and keeps the rest, in this
 * order after Z: Æ, Ø, Greek, Cyrillic, Arabic, Thai, Hangul, kana, Han.
 * Checked against Jellyfin 12.2 and Emby 4.10 (NameLessThan and SortBy=SortName).
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace letters {

struct Letter {
    const char *query;   /* NameLessThan: the titles before it are before the letter */
    const char *label;   /* shown large after a jump */
};

/* latin: the server's sort names are all Latin (Features::latin_sort_names). */
const std::vector<Letter> &table(bool latin);

/* The letter a sort name falls under: its index in table(latin), -1 before the
 * first (digits, "#"), -2 a character the table doesn't place. */
int index_of(const std::string &sort_name, bool latin);

/* What to show for a sort name: its letter's label, "#" before the first, "" unknown. */
std::string label_of(const std::string &sort_name, bool latin);

/* The jump from the title at `at` (of `total`, under letter `hint`) to the start
 * of the next letter (dir > 0) or of this one / the previous one (dir < 0).
 * before(i) is the server's count of titles before letter i (NameLessThan), -1
 * when it failed; the counts grow along the table, so this is a search over them,
 * starting where the hint says. Returns the title to land on, or -1 for none (the
 * last letter, the first title, a failed count); *letter gets the table index
 * it lands on (-1 for "#"). */
int jump(int dir, int at, int total, int hint, int n_letters, const std::function<int(int)> &before, int *letter);

} // namespace letters
