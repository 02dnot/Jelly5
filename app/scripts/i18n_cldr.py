#!/usr/bin/env python3
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run with Babel and pycountry installed: python3 scripts/i18n_cldr.py src/app/i18n_cldr.cpp
"""Generates app/src/app/i18n_cldr.cpp from CLDR (Babel) and ISO 639 (pycountry)."""
import babel, pycountry, re, sys
LOCS=['nb','en_US','es','fr','de','pt_BR','it','ja','nl','ru','ko','zh_Hant_TW','zh_Hans_CN','fi','sv','da','pl','tr','ar','cs','hu','el','ro','th','vi','id','uk']
NAMES=['Norwegian','English','Spanish','French','German','Portuguese','Italian','Japanese','Dutch','Russian','Korean','ChineseTraditional','ChineseSimplified','Finnish','Swedish','Danish','Polish','Turkish','Arabic','Czech','Hungarian','Greek','Romanian','Thai','Vietnamese','Indonesian','Ukrainian']
def c(s):
    s=s.replace(' ',' ')
    return '"'+s.replace('\\','\\\\').replace('"','\\"')+'"'
def template(loc, L):
    pat=L.date_formats['long'].pattern
    if loc=='th':   # Thai dates count the Buddhist era (CLDR's preferred calendar for th)
        pat="d MMMM 'พ.ศ.' B"
    out=''; i=0
    while i<len(pat):
        ch=pat[i]
        if ch=="'":
            j=pat.index("'",i+1); out+=pat[i+1:j] if j>i+1 else "'"; i=j+1; continue
        if ch.isascii() and ch.isalpha():
            j=i
            while j<len(pat) and pat[j]==ch: j+=1
            tok=pat[i:j]; i=j
            out+={'d':'{d}','dd':'{d}','M':'{m}','MM':'{m}','MMMM':'{M}','y':'{y}','yyyy':'{y}','B':'{b}'}[tok]; continue
        out+=ch; i+=1
    return out
codes=[]
for l in pycountry.languages:
    if hasattr(l,'alpha_2'):
        cs=[l.alpha_2,l.alpha_3]+([l.bibliographic] if hasattr(l,'bibliographic') else [])
        codes.append((l.alpha_2,cs))
codes.append(('yue',['yue'])); codes.append(('fil',['fil']))
locs=[babel.Locale.parse(l) for l in LOCS]
rows=[]
for key,cs in codes:
    names=[L.languages.get(key,'') for L in locs]
    if not names[1]: continue
    names=[n[:1].upper()+n[1:] if n else '' for n in names]
    rows.append((' '.join(dict.fromkeys(cs)),names))
o=[]
o.append('''/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Generated from the Unicode CLDR (via Babel) and ISO 639 (via pycountry): long
 * dates and language names in each of the interface's languages. CLDR data:
 * Copyright © 2004-2025 Unicode, Inc., Unicode License v3 (CLDR-LICENSE.txt). Don't edit by hand.
 */
#include "app/i18n_cldr.h"

#include <cstdio>
#include <cstring>

namespace i18n {
namespace {

struct DateFormat {
    const char *pattern;   /* {d} day, {m} month number, {M} month name, {y} year, {b} Buddhist year */
    const char *months[12];
};

const DateFormat kDates[(int)Lang::Count] = {''')
for loc,n,L in zip(LOCS,NAMES,locs):
    ms=L.months['format']['wide']
    o.append(f'    /* {n} */ {{{c(template(loc,L))}, {{'+', '.join(c(ms[i]) for i in range(1,13))+'}},')
o.append('};\n\nstruct LanguageName {\n    const char *codes;   /* ISO 639-1, -2/T, -2/B */\n    const char *names[(int)Lang::Count];\n};\n\nconst LanguageName kLanguages[] = {')
for cs,names in rows:
    o.append(f'    {{{c(cs)}, {{'+', '.join(c(x) for x in names)+'}},')
o.append('''};

} // namespace

std::string long_date(int y, int m, int d)
{
    if (m < 1 || m > 12)
        return std::string();
    const DateFormat &f = kDates[(int)lang()];
    std::string out;
    for (const char *p = f.pattern; *p;) {
        char b[16];
        if (std::strncmp(p, "{d}", 3) == 0)
            std::snprintf(b, sizeof b, "%d", d), out += b;
        else if (std::strncmp(p, "{m}", 3) == 0)
            std::snprintf(b, sizeof b, "%d", m), out += b;
        else if (std::strncmp(p, "{M}", 3) == 0)
            out += f.months[m - 1];
        else if (std::strncmp(p, "{y}", 3) == 0)
            std::snprintf(b, sizeof b, "%d", y), out += b;
        else if (std::strncmp(p, "{b}", 3) == 0)
            std::snprintf(b, sizeof b, "%d", y + 543), out += b;
        else {
            out += *p++;
            continue;
        }
        p += 3;
    }
    return out;
}

std::string language_name(const std::string &code)
{
    std::string lc;
    for (char ch : code) {
        if (ch == '-' || ch == '_')
            break;   /* "pt-BR": the language */
        lc += (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    if (lc.empty())
        return std::string();
    for (const LanguageName &l : kLanguages) {
        const char *p = std::strstr(l.codes, lc.c_str());
        for (; p; p = std::strstr(p + 1, lc.c_str())) {
            const bool starts = p == l.codes || p[-1] == ' ', ends = p[lc.size()] == ' ' || !p[lc.size()];
            if (starts && ends) {
                const char *n = l.names[(int)lang()];
                return *n ? n : l.names[(int)Lang::English];
            }
        }
    }
    return std::string();
}

} // namespace i18n
''')
open(sys.argv[1],'w',encoding='utf-8').write('\n'.join(o))
print(len(rows),'languages')
for loc,L in zip(LOCS,locs): print(loc, template(loc,L))
