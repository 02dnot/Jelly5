/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The UI renderer: immediate-mode 2D on the GPU (the engine's sceAgc runtime
 * and its ui_screen_2d pipeline). Everything is laid out in a 1920x1080
 * logical space and drawn at the panel's resolution (4K on most TVs), so
 * geometry and text are sharp. The pipeline's rounded-box clip gives every
 * quad anti-aliased rounded corners for free.
 *
 * Colours are 0xAARRGGBB with straight alpha (as in ui_canvas.h); the
 * renderer premultiplies.
 */
#pragma once

#include <cstdint>
#include <string>

struct ui_image;

namespace gfx {

constexpr float W = 1920.f, H = 1080.f;   /* logical canvas */

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

/* A GPU texture (RGBA8, premultiplied). Owned by the renderer. */
struct Texture;

/* Brings the renderer up once the display runs. */
bool init();

/* Panel pixels per logical pixel (2 on a 4K panel). */
float scale();

void begin_frame();          /* frame_begin + clear */
/* Drawing into a frame someone else began (the player's): resets the
 * renderer's state without starting a frame. */
void begin_overlay();
void end_frame();            /* present */
/* Presents the frame begin_overlay drew into and frees the textures released
 * frames ago: the player's loop never reaches end_frame. */
void end_overlay();

/* Textures. Release is deferred until the GPU is done with the frames in flight. */
Texture *texture_from_pixels(const uint32_t *rgba, int w, int h, int stride_px);
Texture *texture_from_image(const ui_image *img);
void texture_release(Texture *t);
int texture_width(const Texture *t);
/* Text kept as textures (for the periodic health line). */
size_t text_cache_size();
int texture_height(const Texture *t);

/* Multiply the opacity of everything that follows (nests). */
void push_opacity(float a);
void pop_opacity();

/* Clip everything that follows to r (axis-aligned scissor), or reset. */
void push_scissor(const Rect &r);
void pop_scissor();

/* A rectangle with rounded corners, a solid colour. */
void fill(const Rect &r, uint32_t color, float radius = 0);
/* A vertical gradient (top to bottom colours). */
void fill_vgradient(const Rect &r, uint32_t top, uint32_t bottom, float radius = 0);
/* A horizontal gradient (left to right colours). */
void fill_hgradient(const Rect &r, uint32_t left, uint32_t right, float radius = 0);
/* A texture into r: stretched, or cropped to fill it (cover). */
void image(const Rect &r, const Texture *t, float opacity = 1, float radius = 0, bool cover = true);
/* Frosted glass: blurs what is already drawn under r (Gaussian, sigma in logical
 * pixels) and puts it back into r's rounded shape. Draw the panel's tint over
 * it. false (nothing drawn) when the GPU has no room for it this frame. */
/* Returns 0 (nothing drawn), 1 (the blurred backdrop, as through a lens: draw a
 * tint and rim over it) or 2 (the whole liquid glass pane, by its shader). */
int backdrop_blur(const Rect &r, float radius, float sigma = 24.f, float opacity = 1.f, float lift = 0.f);
/* A fade at a scrolling list's edges: every quad drawn until pop_fade_mask is cut
 * where the ramps start and end and faded per corner, so it goes from opaque
 * inside r to clear at its edges over the given lengths (0: that edge stays
 * hard). Done in the geometry: no layer, nothing read back. */
void push_fade_mask(const Rect &r, float top, float bottom, float left = 0, float right = 0);
void pop_fade_mask();
/* Whether a fade mask is on (masks do not nest: an inner one would end the outer). */
bool fade_mask_active();
/* The frame being drawn: counts begin_frame and begin_overlay. */
uint64_t frame_index();
/* Where the glass's light comes from (screen direction, x right, y down); the
 * shell tilts it with the DualSense. Returns true when it moved enough to redraw. */
bool set_glass_light(float x, float y);
/* The lit rim of a glass pane: a thin line of light around r's rounded shape. */
void rim(const Rect &r, float radius, float opacity = 1.f);
/* Part of a texture (u, v in 0..1), e.g. one thumbnail of a sheet. */
void image_uv(const Rect &r, const Texture *t, float u0, float v0, float u1, float v1, float opacity = 1,
              float radius = 0);
/* A soft drop shadow for a rounded box at r. */
void shadow(const Rect &r, float radius, float blur, float opacity, float dy = 0);

/* Text. Laid out and rasterised once per (text, style), then cached. */
enum Weight { Regular, Medium, SemiBold, Bold };

struct TextStyle {
    Weight weight = Medium;
    float size = 26;                   /* logical px */
    float max_w = 0;                   /* single line: ellipsis past this */
    int max_lines = 1;                 /* > 1: wrap within max_w */
    float line_h = 0;                  /* wrapped line advance (0 = 1.4 x size) */
};

/* Draws text with its first baseline at (x, baseline). Returns the width. */
float text(float x, float baseline, const std::string &s, const TextStyle &st, uint32_t color,
           int align = 0 /* 0 left, 1 centre, 2 right */);
float text_width(const std::string &s, const TextStyle &st);

/* Drops cached textures that were not drawn for a while (call once per frame). */
void collect();

} // namespace gfx
