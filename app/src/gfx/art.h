/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Artwork on the GPU: posters, thumbs, backdrops and logos by URL. Fetching
 * and decoding run on ui_image's workers; finished images are uploaded as
 * textures (a few per frame, so scrolling never hitches) and kept in an LRU
 * cache with a memory budget. Keyed by URL, so a texture can never be shown
 * for the wrong title. BlurHash placeholders are decoded locally and drawn
 * until the real image has faded in over them.
 */
#pragma once

#include "gfx.h"

#include <string>

namespace art {

/* The texture for url once it is ready (requesting it as needed), else null.
 * max_w/max_h bound the decoded size. */
const gfx::Texture *get(const std::string &url, int max_w, int max_h);
/* Starts fetching url ahead of its first draw (a tab not opened yet): get() at
 * the same size then finds it ready. No texture is made until it is drawn. */
void prefetch(const std::string &url, int max_w, int max_h);
/* The picture at url could not be had (missing on the server, broken). */
bool failed(const std::string &url);

/* 0..1: how far the image at url has faded in (eased); 0 until it is ready. */
float fade(const std::string &url, float seconds = 0.35f);

/* A small texture decoded from a BlurHash string (cached), else null. */
const gfx::Texture *blurhash(const std::string &hash);

/* Image, blurhash placeholder underneath, fading in: the usual way to draw art.
 * Without a BlurHash (Emby has none): empty_color, or while the image of a
 * rounded picture left at the default colour (a poster, a card) is coming, the
 * placeholder the screens set. */
constexpr uint32_t kEmpty = 0xff1a1a20u;
void draw(const gfx::Rect &r, const std::string &url, const std::string &hash, int max_w, int max_h,
          float radius, float opacity = 1.f, uint32_t empty_color = kEmpty);
using Placeholder = void (*)(const gfx::Rect &r, float radius, float opacity);
void set_placeholder(Placeholder p);

/* True while some image is still fading in (keep drawing frames). */
bool animating();

/* For the periodic health line in the log: bytes held, images, BlurHash placeholders. */
void stats(size_t *bytes, size_t *images, size_t *placeholders);

/* Once per frame: uploads arrived images, evicts over budget. */
void tick();

/* Drops everything (the player frees ui_image's cache between playbacks). */
void reset();

} // namespace art
