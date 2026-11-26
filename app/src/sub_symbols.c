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

/* The font an override block leaves in effect: its last \fn, or the style's
 * after \r. */
static void scan_block(const char *s, size_t n, const char **font, size_t *font_n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\')
            continue;
        if (i + 2 < n && s[i + 1] == 'f' && s[i + 2] == 'n') {
            size_t a = i + 3, e = a;
            while (e < n && s[e] != '\\')
                e++;
            *font = s + a;
            *font_n = e - a;
        } else if (i + 1 < n && s[i + 1] == 'r') {
            *font = "";
            *font_n = 0;
        }
    }
}

char *sub_symbols_wrap(const char *text, const char *family, sub_symbols_covered_fn covered,
                       void *ctx)
{
    if (!text || !family || !covered)
        return NULL;
    const size_t len = strlen(text);
    const size_t fam_n = strlen(family);
    const char *font = "";            /* empty: the style's own font */
    size_t font_n = 0;
    buf b = {0};
    int in_run = 0, changed = 0;
    size_t i = 0;
    while (i < len) {
        if (text[i] == '{') {
            const char *close = memchr(text + i, '}', len - i);
            if (!close) {
                free(b.p);
                return NULL;          /* a tag inserted after it would close it */
            }
            const size_t bn = (size_t)(close - text) + 1 - i;
            if (in_run) {
                put_fn(&b, font, font_n);
                in_run = 0;
            }
            scan_block(text + i + 1, bn - 2, &font, &font_n);
            put(&b, text + i, bn);
            i += bn;
            continue;
        }
        uint32_t cp = 0;
        size_t n = utf8_next((const unsigned char *)text + i, len - i, &cp);
        const int sym = n > 0 && cp >= 0x80 && covered(cp, ctx);
        if (n == 0)
            n = 1;
        if (sym && !in_run) {
            put_fn(&b, family, fam_n);
            in_run = changed = 1;
        } else if (!sym && in_run) {
            put_fn(&b, font, font_n);
            in_run = 0;
        }
        put(&b, text + i, n);
        i += n;
    }
    if (in_run)
        put_fn(&b, font, font_n);
    if (!changed || b.oom) {
        free(b.p);
        return NULL;
    }
    return b.p;
}
