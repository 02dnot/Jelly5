/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sub_symbols.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} buf;

static void put(buf *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (b->n + n + 1 > cap)
            cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) {
            b->oom = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void put_fn(buf *b, const char *font, size_t n)
{
    put(b, "{\\fn", 4);
    put(b, font, n);
    put(b, "}", 1);
}

/* One UTF-8 character at s (n bytes left): its length, and *cp, or 0 when it
 * is not a valid sequence. */
static size_t utf8_next(const unsigned char *s, size_t n, uint32_t *cp)
{
    size_t len;
    uint32_t c;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    else if ((s[0] & 0xe0) == 0xc0) { c = s[0] & 0x1f; len = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { c = s[0] & 0x0f; len = 3; }
    else if ((s[0] & 0xf8) == 0xf0) { c = s[0] & 0x07; len = 4; }
    else return 0;
    if (len > n)
        return 0;
    for (size_t k = 1; k < len; k++) {
        if ((s[k] & 0xc0) != 0x80)
            return 0;
        c = (c << 6) | (uint32_t)(s[k] & 0x3f);
    }
    *cp = c;
    return len;
}

/* Marks that belong to the character before them: a run keeps them, so a
 * symbol and its variation selector or combining mark stay in one font. */
static int is_mark(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0x200d;
}

typedef struct {
    const char *font;   /* the font in effect; empty: the event's style */
    size_t font_n;
} state;

/* The font an override block leaves in effect: its last \fn, or the style a
 * \r names. */
static void scan_block(const char *s, size_t n, state *st, sub_symbols_style_font_fn style_font,
                       void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\')
            continue;
        size_t a, e;
        if (i + 2 < n && s[i + 1] == 'f' && s[i + 2] == 'n') {
            a = i + 3;
        } else if (i + 1 < n && s[i + 1] == 'r') {
            a = i + 2;
        } else {
            continue;
        }
        e = a;
        while (e < n && s[e] != '\\')
            e++;
        if (s[i + 1] == 'f') {
            st->font = s + a;
            st->font_n = e - a;
            continue;
        }
        while (a < e && (s[a] == ' ' || s[a] == '\t'))
            a++;
        size_t z = e;
        while (z > a && (s[z - 1] == ' ' || s[z - 1] == '\t'))
            z--;
        const char *f = (z > a && style_font) ? style_font(s + a, z - a, ctx) : NULL;
        st->font = f ? f : "";
        st->font_n = f ? strlen(f) : 0;
    }
}

char *sub_symbols_wrap(const char *text, const char *family, sub_symbols_covered_fn covered,
                       sub_symbols_style_font_fn style_font, void *ctx)
{
    if (!text || !family || !covered)
        return NULL;
    const size_t len = strlen(text);
    const size_t fam_n = strlen(family);
    state st = {"", 0};
    buf b = {0};
    int in_run = 0, changed = 0;
    size_t i = 0;
    while (i < len) {
        /* \{ and \} are drawn as braces, not override blocks. */
        if (text[i] == '\\' && i + 1 < len && (text[i + 1] == '{' || text[i + 1] == '}')) {
            if (in_run) {
                put_fn(&b, st.font, st.font_n);
                in_run = 0;
            }
            put(&b, text + i, 2);
            i += 2;
            continue;
        }
        if (text[i] == '{') {
            if (in_run) {
                put_fn(&b, st.font, st.font_n);
                in_run = 0;
            }
            const char *close = memchr(text + i, '}', len - i);
            if (!close) {
                /* A tag inserted after it would close it: the rest as it is. */
                put(&b, text + i, len - i);
                break;
            }
            const size_t bn = (size_t)(close - text) + 1 - i;
            scan_block(text + i + 1, bn - 2, &st, style_font, ctx);
            put(&b, text + i, bn);
            i += bn;
            continue;
        }
        uint32_t cp = 0;
        size_t n = utf8_next((const unsigned char *)text + i, len - i, &cp);
        const int sym = n > 0 && cp >= 0x80 && (covered(cp, ctx) || (in_run && is_mark(cp)));
        if (n == 0)
            n = 1;
        if (sym && !in_run) {
            put_fn(&b, family, fam_n);
            in_run = changed = 1;
        } else if (!sym && in_run) {
            put_fn(&b, st.font, st.font_n);
            in_run = 0;
        }
        put(&b, text + i, n);
        i += n;
    }
    if (in_run)
        put_fn(&b, st.font, st.font_n);
    if (!changed || b.oom) {
        free(b.p);
        return NULL;
    }
    return b.p;
}
