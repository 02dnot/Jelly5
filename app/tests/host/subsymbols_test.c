/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host test of sub_symbols_wrap: symbols in a cue go to the symbol font.
 */
#include "sub_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static int covered(uint32_t cp, void *ctx)
{
    (void)ctx;
    return (cp >= 0x2669 && cp <= 0x267e) || (cp >= 0x2460 && cp <= 0x24ff);
}

static void check(const char *in, const char *want)
{
    char *got = sub_symbols_wrap(in, "Sym", covered, NULL);
    const int ok = want ? got && !strcmp(got, want) : !got;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n  want %s\n  got  %s\n", in, want ? want : "(unchanged)",
                got ? got : "(unchanged)");
        fails++;
    }
    free(got);
}

int main(void)
{
    check("Just like a plane", NULL);
    check("", NULL);
    check("\xe2\x99\xaa Just like a plane \xe2\x99\xaa",
          "{\\fnSym}\xe2\x99\xaa{\\fn} Just like a plane {\\fnSym}\xe2\x99\xaa{\\fn}");
    check("\xe2\x99\xaa\xe2\x99\xab la", "{\\fnSym}\xe2\x99\xaa\xe2\x99\xab{\\fn} la");
    check("{\\i1}\xe2\x99\xaa sing{\\i0}", "{\\i1}{\\fnSym}\xe2\x99\xaa{\\fn} sing{\\i0}");
    /* The author's font comes back after the run. */
    check("{\\fnArial\\b1}a \xe2\x99\xaa b", "{\\fnArial\\b1}a {\\fnSym}\xe2\x99\xaa{\\fnArial} b");
    check("{\\fnArial}a{\\r}\xe2\x99\xaa", "{\\fnArial}a{\\r}{\\fnSym}\xe2\x99\xaa{\\fn}");
    /* A run that reaches an override block closes before it. */
    check("\xe2\x99\xaa{\\i1}x", "{\\fnSym}\xe2\x99\xaa{\\fn}{\\i1}x");
    /* Non-ASCII outside the font is left alone; line breaks too. */
    check("bl\xc3\xa5\\N\xe2\x91\xa0", "bl\xc3\xa5\\N{\\fnSym}\xe2\x91\xa0{\\fn}");
    /* Characters in an override block are never wrapped. */
    check("{comment \xe2\x99\xaa}x", NULL);
    /* An unclosed block: a tag inserted after it would close it. */
    check("{oops \xe2\x99\xaa", NULL);
    /* Broken UTF-8 is copied as it is. */
    check("\xe2\x99 \xe2\x99\xaa", "\xe2\x99 {\\fnSym}\xe2\x99\xaa{\\fn}");
    check("\xe2", NULL);
    if (fails)
        return 1;
    puts("sub_symbols: ok");
    return 0;
}
