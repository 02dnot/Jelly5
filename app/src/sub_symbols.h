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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the symbol font draws cp. */
typedef int (*sub_symbols_covered_fn)(uint32_t cp, void *ctx);
/* The FontName of the style named name[0..n), or NULL for an unknown style
 * (libass then uses the event's own style). */
typedef const char *(*sub_symbols_style_font_fn)(const char *name, size_t n, void *ctx);

/* An ASS event's text with every run of characters `covered` accepts drawn in
 * `family`: {\fn<family>}run{\fn<font before>}. Override blocks are kept as
 * they are; the font in effect before the run (an earlier \fn, the style a
 * \r<style> names, or the event's style) comes back after it. Text after a
 * '{' that never closes is copied unchanged. `style_font` may be NULL.
 * Returns a malloc'd string, or NULL when nothing changes or on no memory. */
char *sub_symbols_wrap(const char *text, const char *family, sub_symbols_covered_fn covered,
                       sub_symbols_style_font_fn style_font, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
