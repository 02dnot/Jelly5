/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/letters.h"

#include <cstdint>

namespace letters {
namespace {

/* The groups after Z, in Emby's order. A group's first letter starts it. */
const char *const kLatin[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                              "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
const char *const kNordic[] = {"Æ", "Ø"};   /* Å is A in Emby's sort names */
const char *const kGreek[] = {"Α", "Β", "Γ", "Δ", "Ε", "Ζ", "Η", "Θ", "Ι", "Κ", "Λ", "Μ",
                              "Ν", "Ξ", "Ο", "Π", "Ρ", "Σ", "Τ", "Υ", "Φ", "Χ", "Ψ", "Ω"};
/* Russian without Ё and Й (Е and И once the accents are gone) and Ъ Ы Ь (no word starts with them). */
const char *const kCyrillic[] = {"А", "Б", "В", "Г", "Д", "Е", "Ж", "З", "И", "К", "Л", "М", "Н", "О",
                                 "П", "Р", "С", "Т", "У", "Ф", "Х", "Ц", "Ч", "Ш", "Щ", "Э", "Ю", "Я"};
const char *const kArabic[] = {"ا", "ب", "ت", "ث", "ج", "ح", "خ", "د", "ذ", "ر", "ز", "س", "ش", "ص",
                               "ض", "ط", "ظ", "ع", "غ", "ف", "ق", "ك", "ل", "م", "ن", "ه", "و", "ي"};
/* Hangul by initial consonant: the first syllable of each (ㄲ ㄸ ㅃ ㅆ ㅉ fall under their single one). */
const char *const kHangulQuery[] = {"가", "나", "다", "라", "마", "바", "사", "아", "자", "차", "카", "타", "파", "하"};
const char *const kHangulLabel[] = {"ㄱ", "ㄴ", "ㄷ", "ㄹ", "ㅁ", "ㅂ", "ㅅ", "ㅇ", "ㅈ", "ㅊ", "ㅋ", "ㅌ", "ㅍ", "ㅎ"};
/* Kana by gojūon row; Emby sorts hiragana and katakana together. */
const char *const kKana[] = {"あ", "か", "さ", "た", "な", "は", "ま", "や", "ら", "わ"};

/* Thai consonants U+0E01-U+0E2E, as UTF-8. */
struct Thai {
    char s[46][4];
    Thai()
    {
        for (int i = 0; i < 46; i++) {
            const uint32_t cp = 0x0E01 + i;
            s[i][0] = (char)(0xE0 | (cp >> 12));
            s[i][1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[i][2] = (char)(0x80 | (cp & 0x3F));
            s[i][3] = 0;
        }
    }
};
const Thai kThai;

enum Group { GLatin, GNordic, GGreek, GCyrillic, GArabic, GThai, GHangul, GKana, GHan, GCount };
const int kGroupSize[GCount] = {26, 2, 24, 28, 28, 46, 14, 10, 1};

/* Where each group starts in Emby's table. */
int start_of(Group g)
{
    int n = 0;
    for (int i = 0; i < g; i++)
        n += kGroupSize[i];
    return n;
}

std::vector<Letter> make_emby()
{
    std::vector<Letter> t;
    for (const char *s : kLatin) t.push_back({s, s});
    for (const char *s : kNordic) t.push_back({s, s});
    for (const char *s : kGreek) t.push_back({s, s});
    for (const char *s : kCyrillic) t.push_back({s, s});
    for (const char *s : kArabic) t.push_back({s, s});
    for (const auto &s : kThai.s) t.push_back({s, s});
    for (int i = 0; i < 14; i++) t.push_back({kHangulQuery[i], kHangulLabel[i]});
    for (const char *s : kKana) t.push_back({s, s});
    t.push_back({"一", "漢"});   /* Han: one group, U+4E00 sorts first */
    return t;
}

/* One code point from s at *i (advanced past it); 0 at the end, 0xFFFD for a bad byte. */
uint32_t next_cp(const std::string &s, size_t *i)
{
    if (*i >= s.size())
        return 0;
    const unsigned char c = (unsigned char)s[*i];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || *i + len > s.size()) {
        (*i)++;
        return 0xFFFD;
    }
    uint32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
    for (int k = 1; k < len; k++)
        cp = (cp << 6) | ((unsigned char)s[*i + k] & 0x3F);
    *i += len;
    return cp;
}

/* Index within a group, or -2. */
int greek(uint32_t cp)
{
    if (cp >= 0x03B1 && cp <= 0x03C9)
        cp -= 0x20;   /* lower to upper; final sigma U+03C2 becomes U+03A2, which is Σ's slot */
    if (cp == 0x03A2)
        cp = 0x03A3;
    if (cp < 0x0391 || cp > 0x03A9)
        return -2;
    const int i = (int)(cp - 0x0391);
    return i > 0x11 ? i - 1 : i;   /* U+03A2 is not a letter */
}

int cyrillic(uint32_t cp)
{
    switch (cp) {   /* the letters outside the table, under the one they sort after */
    case 0x0401: case 0x0451: case 0x0404: case 0x0454: cp = 0x0415; break;   /* Ё Є: Е */
    case 0x0419: case 0x0439: case 0x0406: case 0x0456: case 0x0407: case 0x0457: cp = 0x0418; break;   /* Й І Ї: И */
    case 0x0490: case 0x0491: cp = 0x0413; break;   /* Ґ: Г */
    default: break;
    }
    if (cp >= 0x0430 && cp <= 0x044F)
        cp -= 0x20;
    if (cp < 0x0410 || cp > 0x042F)
        return -2;
    static const char16_t order[] = u"АБВГДЕЖЗИКЛМНОПРСТУФХЦЧШЩЭЮЯ";
    if (cp == 0x0419) cp = 0x0418;
    if (cp >= 0x042A && cp <= 0x042C) cp = 0x0429;   /* Ъ Ы Ь: Щ */
    for (int i = 0; order[i]; i++)
        if (order[i] == cp)
            return i;
    return -2;
}

int arabic(uint32_t cp)
{
    switch (cp) {   /* alef with hamza or madda, Persian and Urdu letters: where they sort */
    case 0x0622: case 0x0623: case 0x0625: case 0x0671: cp = 0x0627; break;
    case 0x067E: cp = 0x0628; break;                    /* پ: ب */
    case 0x0629: cp = 0x062A; break;                    /* ة: ت */
    case 0x0686: cp = 0x062C; break;                    /* چ: ج */
    case 0x0698: cp = 0x0632; break;                    /* ژ: ز */
    case 0x06A9: case 0x06AF: cp = 0x0643; break;       /* ک گ: ك */
    case 0x0649: case 0x06CC: cp = 0x064A; break;       /* ى ی: ي */
    default: break;
    }
    static const char16_t order[] = u"ابتثجحخدذرزسشصضطظعغفقكلمنهوي";
    for (int i = 0; order[i]; i++)
        if (order[i] == cp)
            return i;
    return -2;
}

int hangul(uint32_t cp)
{
    /* initial consonant of a syllable: ㄱ ㄲ ㄴ ㄷ ㄸ ㄹ ㅁ ㅂ ㅃ ㅅ ㅆ ㅇ ㅈ ㅉ ㅊ ㅋ ㅌ ㅍ ㅎ */
    static const int to_base[19] = {0, 0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13};
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return to_base[(cp - 0xAC00) / 588];
    return -2;
}

int kana(uint32_t cp)
{
    if (cp >= 0x30A1 && cp <= 0x30F6)
        cp -= 0x60;   /* katakana to hiragana */
    if (cp < 0x3041 || cp > 0x3096)
        return -2;
    static const uint32_t row_start[] = {0x3041, 0x304B, 0x3055, 0x305F, 0x306A, 0x306F, 0x307E, 0x3083, 0x3089, 0x308E};
    int r = 0;
    for (int i = 0; i < 10; i++)
        if (cp >= row_start[i])
            r = i;
    return cp >= 0x3094 ? 0 : r;   /* ゔ ゕ ゖ: rare, under あ */
}

bool han(uint32_t cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

int in(Group g, int i) { return i < 0 ? -2 : start_of(g) + i; }

} // namespace

const std::vector<Letter> &table(bool latin)
{
    static const std::vector<Letter> jellyfin = [] {
        std::vector<Letter> t;
        for (const char *s : kLatin) t.push_back({s, s});
        return t;
    }();
    static const std::vector<Letter> emby = make_emby();
    return latin ? jellyfin : emby;
}

int index_of(const std::string &sort_name, bool latin)
{
    size_t i = 0;
    uint32_t cp = next_cp(sort_name, &i);
    /* A SortName keeps a leading quote or bracket, and the server sorts it with
     * the digits, before A ('"Wuthering Heights"'); only { | } ~ come after z. */
    if (cp == 0)
        return -1;
    if (cp < 0x80) {
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
            return (int)((cp | 0x20) - 'a');
        return cp >= '{' ? -2 : -1;
    }
    if (latin)
        return -2;
    if (cp == 0x00C6 || cp == 0x00E6) return start_of(GNordic);       /* Æ */
    if (cp == 0x00D8 || cp == 0x00F8) return start_of(GNordic) + 1;   /* Ø */
    if (cp >= 0x0370 && cp <= 0x03FF) return in(GGreek, greek(cp));
    if (cp >= 0x0400 && cp <= 0x04FF) return in(GCyrillic, cyrillic(cp));
    if (cp >= 0x0600 && cp <= 0x06FF) return in(GArabic, arabic(cp));
    if (cp >= 0x0E01 && cp <= 0x0E7F) {
        if (cp >= 0x0E40 && cp <= 0x0E44)
            cp = next_cp(sort_name, &i);   /* a vowel written first sorts by the consonant after it */
        return cp >= 0x0E01 && cp <= 0x0E2E ? start_of(GThai) + (int)(cp - 0x0E01) : -2;
    }
    if (cp >= 0xAC00 && cp <= 0xD7A3) return in(GHangul, hangul(cp));
    if (cp >= 0x3041 && cp <= 0x30FF) return in(GKana, kana(cp));
    if (han(cp)) return start_of(GHan);
    return -2;
}

int jump(int dir, int at, int total, int hint, int n_letters, const std::function<int(int)> &before, int *letter)
{
    bool failed = false;
    auto count = [&](int i) {   /* -1 and the end stand for 0 and all of them */
        if (i < 0)
            return 0;
        if (i >= n_letters)
            return total;
        const int n = before(i);
        failed = failed || n < 0;
        return n;
    };
    /* Forward: the first letter with titles past this one, before(lo) <= at < before(hi).
     * Back: the last letter starting before this title, before(lo) < at <= before(hi). */
    auto left = [&](int i) { return dir > 0 ? count(i) <= at : count(i) < at; };
    if (dir < 0 && at <= 0)
        return -1;
    int lo = -1, hi = n_letters;
    if (hint >= 0 && hint < n_letters) {
        (left(hint) ? lo : hi) = hint;
        if (lo == hint && hint + 1 < n_letters)
            (left(hint + 1) ? lo : hi) = hint + 1;
    }
    while (hi - lo > 1 && !failed) {
        const int mid = lo + (hi - lo) / 2;
        (left(mid) ? lo : hi) = mid;
    }
    if (failed)
        return -1;
    const int to = dir > 0 ? hi : lo;
    if (dir > 0 && to >= n_letters)
        return -1;
    const int target = count(to);
    if (failed || target >= total)
        return -1;
    *letter = to;
    return target;
}

std::string label_of(const std::string &sort_name, bool latin)
{
    const int i = index_of(sort_name, latin);
    if (i == -1)
        return "#";
    if (i < 0)
        return "";
    return table(latin)[i].label;
}

} // namespace letters
