/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Symbols in text subtitles (♪ in lyrics, ①, arrows). libass runs without a
 * system font provider and never falls back by glyph: a character is drawn in
 * the style's font or the default family, never in another embedded font. A
 * symbol the subtitle font lacks would be a box, so a cue's runs of symbols are
 * pointed at the symbol font with \fn override tags.
 */
#ifndef JELLY5_SUB_SYMBOLS_H
#define JELLY5_SUB_SYMBOLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*sub_symbols_covered_fn)(uint32_t cp, void *ctx);

/* An ASS event's text with every run of characters `covered` accepts drawn in
 * `family`: {\fn<family>}run{\fn<font before>}. Override blocks are kept as
 * they are, and the font an earlier \fn chose is restored after the run.
 * Returns a malloc'd string, or NULL when nothing changes (no such character,
 * an unclosed override block, or no memory). */
char *sub_symbols_wrap(const char *text, const char *family, sub_symbols_covered_fn covered,
                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif
