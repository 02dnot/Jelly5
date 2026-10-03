/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Language codes and names for the player's track lists (from Nuvio's player
 * interface, nuvio_osd.cpp, which Jelly5's PlayerUi replaced).
 */
#include "nuvio_osd.h"

#include <cctype>
#include <string>

std::string nuvio_language_key(const std::string &code)
{
    static const struct { const char *three, *two; } k[] = {
        {"eng", "en"}, {"spa", "es"}, {"fre", "fr"}, {"fra", "fr"}, {"ger", "de"}, {"deu", "de"},
        {"ita", "it"}, {"por", "pt"}, {"rus", "ru"}, {"jpn", "ja"}, {"kor", "ko"}, {"chi", "zh"},
        {"zho", "zh"}, {"ara", "ar"}, {"hin", "hi"}, {"tur", "tr"}, {"pol", "pl"}, {"dut", "nl"},
        {"nld", "nl"}, {"swe", "sv"}, {"nor", "no"}, {"nob", "nb"}, {"dan", "da"}, {"fin", "fi"},
        {"gre", "el"}, {"ell", "el"}, {"heb", "he"}, {"cze", "cs"}, {"ces", "cs"}, {"hun", "hu"},
        {"rum", "ro"}, {"ron", "ro"}, {"bul", "bg"}, {"hrv", "hr"}, {"srp", "sr"}, {"slv", "sl"},
        {"slo", "sk"}, {"slk", "sk"}, {"ukr", "uk"}, {"tha", "th"}, {"vie", "vi"}, {"ind", "id"},
        {"may", "ms"}, {"msa", "ms"}, {"per", "fa"}, {"fas", "fa"}, {"urd", "ur"}, {"ben", "bn"},
        {"tam", "ta"}, {"tel", "te"}, {"mal", "ml"}, {"kan", "kn"}, {"mar", "mr"}, {"cat", "ca"},
        {"baq", "eu"}, {"eus", "eu"}, {"glg", "gl"}, {"ice", "is"}, {"isl", "is"}, {"est", "et"},
        {"lav", "lv"}, {"lit", "lt"}, {"fil", "tl"}, {"tgl", "tl"}, {"alb", "sq"}, {"sqi", "sq"},
        {"mac", "mk"}, {"mkd", "mk"}, {"bos", "bs"}, {"geo", "ka"}, {"kat", "ka"}, {"arm", "hy"},
        {"hye", "hy"}, {"aze", "az"}, {"kaz", "kk"}, {"uzb", "uz"}, {"mon", "mn"}, {"khm", "km"},
        {"lao", "lo"}, {"bur", "my"}, {"mya", "my"}, {"nep", "ne"}, {"sin", "si"}, {"swa", "sw"},
        {"afr", "af"}, {"amh", "am"}, {"wel", "cy"}, {"cym", "cy"}, {"gle", "ga"}, {"lat", "la"},
        {"pob", "pt-br"}, {"pt-BR", "pt-br"}, {"es-419", "es-419"},
    };
    std::string c;
    for (char ch : code)
        c += (char)(ch == '_' ? '-' : std::tolower((unsigned char)ch));
    for (const auto &e : k)
        if (c == e.three)
            return e.two;
    return c;
}

std::string nuvio_language_name(const std::string &code)
{
    static const struct { const char *k, *name; } n[] = {
        {"en", "English"}, {"es", "Spanish"}, {"es-419", "Spanish (Latin America)"},
        {"es-mx", "Spanish (Mexico)"}, {"fr", "French"}, {"fr-ca", "French (Canada)"},
        {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"}, {"pt-br", "Portuguese (Brazil)"},
        {"pt-pt", "Portuguese (Portugal)"}, {"ru", "Russian"}, {"ja", "Japanese"}, {"ko", "Korean"},
        {"zh", "Chinese"}, {"zh-cn", "Chinese (Simplified)"}, {"zh-tw", "Chinese (Traditional)"},
        {"zh-hk", "Chinese (Hong Kong)"}, {"ar", "Arabic"}, {"hi", "Hindi"}, {"tr", "Turkish"},
        {"pl", "Polish"}, {"nl", "Dutch"}, {"sv", "Swedish"}, {"no", "Norwegian"}, {"nb", "Norwegian"},
        {"da", "Danish"}, {"fi", "Finnish"}, {"el", "Greek"}, {"he", "Hebrew"}, {"cs", "Czech"},
        {"hu", "Hungarian"}, {"ro", "Romanian"}, {"bg", "Bulgarian"}, {"hr", "Croatian"},
        {"sr", "Serbian"}, {"sl", "Slovenian"}, {"sk", "Slovak"}, {"uk", "Ukrainian"}, {"th", "Thai"},
        {"vi", "Vietnamese"}, {"id", "Indonesian"}, {"ms", "Malay"}, {"fa", "Persian"}, {"ur", "Urdu"},
        {"bn", "Bengali"}, {"ta", "Tamil"}, {"te", "Telugu"}, {"ml", "Malayalam"}, {"kn", "Kannada"},
        {"mr", "Marathi"}, {"ca", "Catalan"}, {"eu", "Basque"}, {"gl", "Galician"}, {"is", "Icelandic"},
        {"et", "Estonian"}, {"lv", "Latvian"}, {"lt", "Lithuanian"}, {"tl", "Filipino"},
        {"sq", "Albanian"}, {"mk", "Macedonian"}, {"bs", "Bosnian"}, {"ka", "Georgian"},
        {"hy", "Armenian"}, {"az", "Azerbaijani"}, {"kk", "Kazakh"}, {"uz", "Uzbek"}, {"mn", "Mongolian"},
        {"km", "Khmer"}, {"lo", "Lao"}, {"my", "Burmese"}, {"ne", "Nepali"}, {"si", "Sinhala"},
        {"sw", "Swahili"}, {"af", "Afrikaans"}, {"am", "Amharic"}, {"cy", "Welsh"}, {"ga", "Irish"},
        {"la", "Latin"}, {"und", ""}, {"unknown", ""}, {"mul", "Multiple"}, {"zxx", ""},
    };
    const std::string key = nuvio_language_key(code);
    for (const auto &e : n)
        if (key == e.k)
            return e.name;
    /* "en-gb" and similar: the base language. */
    const size_t dash = key.find('-');
    if (dash != std::string::npos) {
        const std::string base = key.substr(0, dash);
        for (const auto &e : n)
            if (base == e.k)
                return e.name;
    }
    if (code.empty())
        return "";
    std::string out = code;
    if (out.size() <= 3)
        for (auto &ch : out)
            ch = (char)std::toupper((unsigned char)ch);
    return out;
}
