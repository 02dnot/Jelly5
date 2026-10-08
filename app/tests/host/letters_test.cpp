/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The letter jump (ui/letters) against recorded server answers. A fixture is
 * "latin" or "emby", then the server's NameLessThan count for each letter of
 * the table, then the library's SortNames in the server's order. From every
 * title, R2 must land on the first title of the next letter that has any, L2
 * on the start of this letter or the previous one, the shown letter must be
 * the landed title's, and no press may take more than 10 counts.
 */
#include "ui/letters.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int s_fail;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        s_fail++;
        if (s_fail <= 20)
            std::printf("FAIL: %s\n", what.c_str());
    }
}

static void run(const char *path)
{
    std::ifstream f(path);
    std::string mode, line;
    std::getline(f, mode);
    std::getline(f, line);
    std::vector<int> counts;
    std::istringstream cs(line);
    for (int n; cs >> n;)
        counts.push_back(n);
    std::vector<std::string> names;
    while (std::getline(f, line))
        names.push_back(line);
    const bool latin = mode == "latin";
    const auto &tab = letters::table(latin);
    check(counts.size() == tab.size(), std::string(path) + ": one count per letter");
    const int total = (int)names.size(), n = (int)tab.size();
    /* Every title's letter as the server counts it: before letter i come exactly
     * the titles under an earlier one. */
    for (int i = 0; i < n && i < (int)counts.size(); i++) {
        int under = 0;
        for (const std::string &s : names)
            under += letters::index_of(s, latin) < i;
        char what[160];
        std::snprintf(what, sizeof what, "%s: %d titles before %s, the server counts %d", path, under, tab[i].query,
                      counts[i]);
        check(under == counts[i], what);
    }
    int presses = 0, most = 0, sum = 0;
    for (int at = 0; at < total; at++) {
        const int hint = letters::index_of(names[at], latin);
        for (int dir : {1, -1}) {
            int asked = 0;
            auto before = [&](int i) { asked++; return counts[i]; };
            int letter = -2;
            const int got = letters::jump(dir, at, total, hint, n, before, &letter);
            /* What it should be, from the counts alone. */
            int want = -1;
            for (int i = 0; i < n; i++) {
                if (dir > 0 && counts[i] > at) { want = counts[i]; break; }
                if (dir < 0 && counts[i] < at) want = counts[i];
            }
            if (dir < 0 && want < 0 && at > 0)
                want = 0;
            if (want >= total)
                want = -1;
            char what[256];
            std::snprintf(what, sizeof what, "%s: %s from %d (%s): landed on %d, want %d", path, dir > 0 ? "R2" : "L2",
                          at, names[at].c_str(), got, want);
            check(got == want, what);
            if (got >= 0) {
                const std::string own = letters::label_of(names[got], latin);
                const std::string shown = letter >= 0 ? tab[letter].label : "#";
                std::snprintf(what, sizeof what, "%s: landed on %s, shows %s (table %s)", path, names[got].c_str(),
                              own.c_str(), shown.c_str());
                check(!own.empty() && (own == shown || dir > 0), what);   /* R2 shows the landed title's own */
            }
            presses++;
            sum += asked;
            most = asked > most ? asked : most;
            check(asked <= 10, std::string(path) + ": more than 10 counts for one press");
        }
    }
    /* R2 through the whole list and L2 back, the counts kept between presses as
     * the library keeps them: what browsing a library costs. */
    std::vector<int> kept(n, -2);
    int asked = 0, stops = 0;
    auto before = [&](int i) {
        if (kept[i] == -2)
            kept[i] = counts[i], asked++;
        return kept[i];
    };
    for (int dir : {1, -1})
        for (int at = dir > 0 ? 0 : total - 1, letter;;) {
            const int to = letters::jump(dir, at, total, letters::index_of(names[at], latin), n, before, &letter);
            if (to < 0)
                break;
            check(dir > 0 ? to > at : to < at, std::string(path) + ": a press that does not move");
            at = to, stops++;
        }
    std::printf("%s: %d titles, %d letters: %.1f counts a press (most %d); through it and back, %d stops: %d counts\n",
                path, total, n, (double)sum / presses, most, stops, asked);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        run(argv[i]);
    /* Sort names the fixtures don't have. */
    check(letters::label_of("\"wuthering heights\"", true) == "#", "a leading quote sorts as #");
    check(letters::label_of("0000001917", true) == "#", "digits are #");
    check(letters::label_of("Ærlig", false) == "Æ" && letters::label_of("øya", false) == "Ø", "Æ Ø on Emby");
    check(letters::label_of("Йога", false) == "И" && letters::label_of("ёлка", false) == "Е", "Й Ё under И Е");
    check(letters::label_of("ไทย", false) == "ท", "a Thai vowel written first: the consonant after it");
    check(letters::label_of("까치", false) == "ㄱ" && letters::label_of("한강", false) == "ㅎ", "Hangul initials");
    check(letters::label_of("アニメ", false) == "あ" && letters::label_of("ぞう", false) == "さ", "kana rows");
    check(letters::label_of("ς", false) == "Σ" && letters::label_of("ωμέγα", false) == "Ω", "Greek");
    check(letters::label_of("أحمد", false) == "ا" && letters::label_of("پدر", false) == "ب", "Arabic and Persian");
    check(letters::label_of("東京", false) == "漢", "Han");
    check(letters::label_of("Ä", true).empty() && letters::index_of("~x", true) == -2, "unknown characters");
    if (s_fail) {
        std::printf("FAIL: %d checks\n", s_fail);
        return 1;
    }
    std::printf("PASS: letter jump\n");
    return 0;
}
