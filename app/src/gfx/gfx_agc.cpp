/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gfx on the bare-metal sceAgc runtime. Each primitive is one indexed draw
 * of the engine's ui_screen_2d pipeline: vertices (pos, premultiplied RGBA,
 * uv), constants and descriptors come from the per-frame transient ring, as
 * in evo_agc_composite_overlay. The pipeline multiplies the texture by the
 * vertex colour and applies a rounded-box clip, which gives rounded corners
 * with a one-pixel feather.
 */
#include "gfx.h"
#include "gfx_pool.h"

#include "ui_canvas.h"
#include "ui_image.h"
#include "ui_text.h"

#include "evo_agc_runtime.h"
#include "evo_agc_transient_ring.h"
#include "evo_agc_writer.h"
#include "evo_boot_trace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

namespace gfx {

struct Texture {
    uint8_t *mem = nullptr;     /* in the texture pool, 256-byte aligned */
    int w = 0, h = 0;
    uint32_t pitch = 0;
    bool tiled = false;         /* a colour target (64KB_R_X tiled), sampled as rendered */
    bool bgra = false;          /* memory order B,G,R,A (the scanout) */
};

namespace {

struct Vertex {
    float x, y;
    uint32_t rgba;              /* premultiplied, bytes R,G,B,A */
    float u, v;
};

bool s_ready = false;
float s_scale = 1.f;            /* panel px per logical px */
int s_pw = 1920, s_ph = 1080;
uint64_t s_frame = 0;
Texture *s_white = nullptr;
Texture *s_shadow = nullptr;    /* a blurred rounded box for drop shadows */
Texture *s_rim = nullptr;       /* a rounded box's lit edge, for glass */
std::vector<Rect> s_scissors;
std::vector<float> s_opacity_stack;
float s_opacity = 1.f;            /* product of the stack */

struct Pending {
    Texture *t;
    uint64_t frame;
};
std::vector<Pending> s_graveyard;   /* textures waiting out the frames in flight */

uint32_t premul(uint32_t argb, float opacity = 1.f)
{
    const float a = ((argb >> 24) & 0xff) / 255.f * std::max(0.f, std::min(1.f, opacity)) * s_opacity;
    const uint32_t r = (uint32_t)(((argb >> 16) & 0xff) * a + 0.5f);
    const uint32_t g = (uint32_t)(((argb >> 8) & 0xff) * a + 0.5f);
    const uint32_t b = (uint32_t)((argb & 0xff) * a + 0.5f);
    const uint32_t A = (uint32_t)(a * 255.f + 0.5f);
    return r | (g << 8) | (b << 16) | (A << 24);
}

void free_texture_now(Texture *t)
{
    if (!t)
        return;
    pool_free(t->mem);
    delete t;
}

/* One draw: nv vertices, ni uint16 indices, texture t, rounded clip at r. */
/* The descriptor a texture is sampled through: linear RGBA for uploads; colour
 * targets (the blur's layers, the scanout) tiled, the scanout with its blue and
 * red swapped back. */
bool build_tsharp(const Texture *t, uint32_t *td)
{
    if (!t->tiled)
        return evo_agc_build_tsharp_rgba8(td, (uint64_t)(uintptr_t)t->mem, (uint32_t)t->w, (uint32_t)t->h,
                                          t->pitch) == 0;
    if (evo_agc_build_tsharp_render_target(td, (uint64_t)(uintptr_t)t->mem, (uint32_t)t->w, (uint32_t)t->h, 0) != 0)
        return false;
    if (t->bgra) {   /* DST_SEL_X <-> DST_SEL_Z (word 3, bits 0-2 and 6-8), as the bgra8 builder does */
        const uint32_t x = td[3] & 7u, z = (td[3] >> 6) & 7u;
        td[3] = (td[3] & ~(7u | (7u << 6))) | z | (x << 6);
    }
    return true;
}

void draw_mesh(const Vertex *v, int nv, const uint16_t *idx, int ni, const Texture *t, const Rect *clip,
               float radius, bool bilinear = true)
{
    if (!s_ready || nv <= 0 || ni <= 0)
        return;
    if (!t)
        t = s_white;
    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    const uint32_t slot = evo_agc_runtime_get_current_slot();
    evo_agc_transient_slice_t cons, cons_d, verts, vsh, tex_d, ib;
    if (evo_agc_transient_ring_alloc(ring, slot, 128, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, (uint32_t)nv * sizeof(Vertex), 16, &verts) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48, 16, &tex_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, (uint32_t)ni * 2u, 16, &ib) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return;
    }

    /* Logical -> panel pixels happens here, so everything is sharp at 4K. */
    Vertex *out = (Vertex *)verts.cpu;
    for (int i = 0; i < nv; i++) {
        out[i] = v[i];
        out[i].x *= s_scale;
        out[i].y *= s_scale;
    }
    std::memcpy(ib.cpu, idx, (size_t)ni * 2u);

    float *m = (float *)cons.cpu;
    std::memset(m, 0, 128);
    m[0] = 2.0f / (float)s_pw;
    m[5] = -2.0f / (float)s_ph;
    m[10] = 1.0f;
    m[12] = -1.0f;
    m[13] = 1.0f;
    m[15] = 1.0f;
    if (clip && radius > 0) {
        m[20] = clip->x * s_scale;
        m[21] = clip->y * s_scale;
        m[22] = (clip->x + clip->w) * s_scale;
        m[23] = (clip->y + clip->h) * s_scale;
        m[25] = 1.0f;
        m[28] = m[29] = m[30] = m[31] = radius * s_scale;
    }

    evo_agc_build_constant_vsharp((uint32_t *)cons_d.cpu, cons.gpu_addr, 128);
    evo_agc_build_vsharp((uint32_t *)vsh.cpu, verts.gpu_addr, sizeof(Vertex), (uint32_t)nv);
    if (!build_tsharp(t, (uint32_t *)tex_d.cpu))
        return;
    evo_agc_build_ssharp((uint32_t *)tex_d.cpu + 8, 1, bilinear ? 1 : 0);

    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 || ud.vs_vertex_table_dword < 0 ||
        ud.ps_texture_table_dword < 0 || ud.vs_count > 16 || ud.ps_count > 16)
        return;
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword] = (uint32_t)cons_d.gpu_addr;
    vs_user[ud.vs_vertex_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_d.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);
    evo_agc_writer_draw_index(cb, (uint32_t)ni, (const uint16_t *)(uintptr_t)ib.gpu_addr);
    evo_agc_runtime_note_draw();
}

const uint16_t kQuad[6] = {0, 1, 2, 2, 1, 3};

/* The geometric fade (push_fade_mask): quads are cut where a fade starts and
 * ends, and each corner takes the mask's opacity there, so the GPU's own
 * interpolation draws the ramp exactly. */
static bool s_mask_on;
static Rect s_mask_r;
static float s_mask_t, s_mask_b, s_mask_l, s_mask_rt;

static float ramp(float d, float len) { return len <= 0.f ? 1.f : d <= 0.f ? 0.f : d >= len ? 1.f : d / len; }

static float mask_at(float x, float y)
{
    return ramp(x - s_mask_r.x, s_mask_l) * ramp(s_mask_r.x + s_mask_r.w - x, s_mask_rt) *
           ramp(y - s_mask_r.y, s_mask_t) * ramp(s_mask_r.y + s_mask_r.h - y, s_mask_b);
}

/* Premultiplied ARGB times a. */
static uint32_t scale_color(uint32_t c, float a)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8)
        out |= (uint32_t)std::lround(((c >> sh) & 0xff) * a) << sh;
    return out;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const float x = (float)((a >> sh) & 0xff), y = (float)((b >> sh) & 0xff);
        out |= (uint32_t)std::lround(x + (y - x) * t) << sh;
    }
    return out;
}


void quad(const Rect &r, const Texture *t, float u0, float v0, float u1, float v1, uint32_t c_tl,
          uint32_t c_tr, uint32_t c_bl, uint32_t c_br, float radius)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    if (s_mask_on) {
        /* Cut at the mask's ramps that cross r; the rounding keeps r as its shape. */
        float xs[6], ys[6];
        int nx = 0, ny = 0;
        const float bx[4] = {s_mask_r.x, s_mask_r.x + s_mask_l, s_mask_r.x + s_mask_r.w - s_mask_rt, s_mask_r.x + s_mask_r.w};
        const float by[4] = {s_mask_r.y, s_mask_r.y + s_mask_t, s_mask_r.y + s_mask_r.h - s_mask_b, s_mask_r.y + s_mask_r.h};
        xs[nx++] = r.x;
        for (float b : bx)
            if (b > r.x && b < r.x + r.w && b > xs[nx - 1])
                xs[nx++] = b;
        xs[nx++] = r.x + r.w;
        ys[ny++] = r.y;
        for (float b : by)
            if (b > r.y && b < r.y + r.h && b > ys[ny - 1])
                ys[ny++] = b;
        ys[ny++] = r.y + r.h;
        for (int j = 0; j + 1 < ny; j++)
            for (int i = 0; i + 1 < nx; i++) {
                Vertex v[4];
                for (int k = 0; k < 4; k++) {
                    const float x = xs[i + (k & 1)], y = ys[j + (k >> 1)];
                    const float fx = (x - r.x) / r.w, fy = (y - r.y) / r.h;
                    const uint32_t c = lerp_color(lerp_color(c_tl, c_tr, fx), lerp_color(c_bl, c_br, fx), fy);
                    v[k] = {x, y, scale_color(c, mask_at(x, y)), u0 + (u1 - u0) * fx, v0 + (v1 - v0) * fy};
                }
                draw_mesh(v, 4, kQuad, 6, t, &r, radius);
            }
        return;
    }
    const Vertex v[4] = {
        {r.x, r.y, c_tl, u0, v0},
        {r.x + r.w, r.y, c_tr, u1, v0},
        {r.x, r.y + r.h, c_bl, u0, v1},
        {r.x + r.w, r.y + r.h, c_br, u1, v1},
    };
    draw_mesh(v, 4, kQuad, 6, t, &r, radius);
}

Texture *make_shadow_texture()
{
    /* A 128x128 rounded box inset by 32 px, blurred: drawn as nine slices,
     * so one texture serves every card size. */
    ui_canvas c;
    if (ui_canvas_init(&c, 128, 128) != 0)
        return nullptr;
    ui_canvas_clear(&c);
    ui_fill_rrect(&c, 32, 32, 64, 64, 16, UI_BLACK);
    ui_image img;
    img.px = c.px;
    img.w = c.w;
    img.h = c.h;
    ui_image_blur(&img, 12);
    Texture *t = texture_from_pixels(img.px, img.w, img.h, img.w);
    ui_canvas_free(&c);
    return t;
}

/* A 256x256 rounded box (corner 64) as a thin line of light, brightest where
 * light from the top left meets it, with a fainter glint opposite: the rim of a
 * glass pane (Apple's Liquid Glass). Drawn as nine slices at any size. */
Texture *make_rim_texture()
{
    const int n = 256;
    const float cx = n / 2.f, half = n / 2.f - 2.f, rad = 64.f;
    std::vector<uint32_t> px((size_t)n * n, 0);
    const float lx = -0.6f, ly = -0.8f;   /* toward the light */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            /* Signed distance to the rounded box, and the outward normal there. */
            const float qx = std::fabs(x + 0.5f - cx) - (half - rad), qy = std::fabs(y + 0.5f - cx) - (half - rad);
            const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rad;
            const float line = std::max(0.f, 1.f - std::fabs(d + 0.8f) / 1.6f);
            if (line <= 0.f)
                continue;
            float nx = 0, ny = 0;
            if (ox > 0 || oy > 0) {
                const float l = std::sqrt(ox * ox + oy * oy);
                nx = ox / l;
                ny = oy / l;
            } else if (qx > qy) {
                nx = 1;
            } else {
                ny = 1;
            }
            nx *= (x + 0.5f < cx) ? -1.f : 1.f;
            ny *= (y + 0.5f < cx) ? -1.f : 1.f;
            const float lit = nx * lx + ny * ly;
            const float k = std::min(1.f, 0.28f + 0.72f * std::max(lit, 0.f) + 0.35f * std::max(-lit, 0.f));
            const uint32_t a = (uint32_t)(line * k * 255.f + 0.5f);
            px[(size_t)y * n + x] = (a << 24) | (a << 16) | (a << 8) | a;   /* premultiplied white */
        }
    return texture_from_pixels(px.data(), n, n, n);
}

} // namespace

bool init()
{
    if (s_ready)
        return true;
    evo_agc_runtime_get_size(&s_pw, &s_ph);
    s_scale = (float)s_pw / W;
    if (!pool_init(512u * 1024u * 1024u))
        return false;
    s_ready = true;
    const uint32_t white[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    s_white = texture_from_pixels(white, 2, 2, 2);
    s_shadow = make_shadow_texture();
    s_rim = make_rim_texture();
    evo_bt("gfx: %dx%d scale %.2f white=%p shadow=%p", s_pw, s_ph, s_scale, (void *)s_white, (void *)s_shadow);
    return s_white != nullptr;
}

float scale() { return s_scale; }

void begin_frame()
{
    s_frame++;
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_clear_black();
    evo_agc_runtime_set_scissor(0, 0, s_pw, s_ph);
    s_scissors.clear();
    s_opacity_stack.clear();
    s_opacity = 1.f;
}

void begin_overlay()
{
    s_frame++;
    evo_agc_runtime_set_scissor(0, 0, s_pw, s_ph);
    s_scissors.clear();
    s_opacity_stack.clear();
    s_opacity = 1.f;
}

void end_frame()
{
    evo_agc_runtime_present();
    /* Free what the GPU can no longer be reading (3 frames in flight). */
    auto it = std::remove_if(s_graveyard.begin(), s_graveyard.end(), [](const Pending &p) {
        if (s_frame - p.frame < 4)
            return false;
        free_texture_now(p.t);
        return true;
    });
    s_graveyard.erase(it, s_graveyard.end());
}

Texture *texture_from_pixels(const uint32_t *rgba, int w, int h, int stride_px)
{
    if (!rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return nullptr;
    const uint32_t pitch = ((uint32_t)w * 4u + 255u) & ~255u;
    const size_t bytes = (size_t)pitch * (size_t)h;
    uint8_t *mem = (uint8_t *)pool_alloc(bytes);
    if (!mem)
        return nullptr;   /* full: the artwork cache evicts and retries */
    for (int y = 0; y < h; y++)
        std::memcpy(mem + (size_t)y * pitch, rgba + (size_t)y * stride_px, (size_t)w * 4u);
    evo_agc_runtime_cache_flush(mem, bytes);
    Texture *t = new Texture;
    t->mem = mem;
    t->w = w;
    t->h = h;
    t->pitch = pitch;
    return t;
}

Texture *texture_from_image(const ui_image *img)
{
    return img ? texture_from_pixels(img->px, img->w, img->h, img->w) : nullptr;
}

void texture_release(Texture *t)
{
    if (t)
        s_graveyard.push_back({t, s_frame});
}

int texture_width(const Texture *t) { return t ? t->w : 0; }
int texture_height(const Texture *t) { return t ? t->h : 0; }

void push_opacity(float a)
{
    s_opacity_stack.push_back(s_opacity);
    s_opacity *= std::max(0.f, std::min(1.f, a));
}

void pop_opacity()
{
    if (!s_opacity_stack.empty()) {
        s_opacity = s_opacity_stack.back();
        s_opacity_stack.pop_back();
    }
}

void push_scissor(const Rect &r)
{
    Rect c = r;
    if (!s_scissors.empty()) {
        const Rect &p = s_scissors.back();
        const float x0 = std::max(c.x, p.x), y0 = std::max(c.y, p.y);
        const float x1 = std::min(c.x + c.w, p.x + p.w), y1 = std::min(c.y + c.h, p.y + p.h);
        c = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    s_scissors.push_back(c);
    evo_agc_runtime_set_scissor((int)std::floor(c.x * s_scale), (int)std::floor(c.y * s_scale),
                                (int)std::ceil(c.w * s_scale), (int)std::ceil(c.h * s_scale));
}

void pop_scissor()
{
    if (!s_scissors.empty())
        s_scissors.pop_back();
    if (s_scissors.empty()) {
        evo_agc_runtime_set_scissor(0, 0, s_pw, s_ph);
    } else {
        const Rect &c = s_scissors.back();
        evo_agc_runtime_set_scissor((int)std::floor(c.x * s_scale), (int)std::floor(c.y * s_scale),
                                    (int)std::ceil(c.w * s_scale), (int)std::ceil(c.h * s_scale));
    }
}

/* The scissor of the current clip (after a pass that set its own). */
static void restore_scissor()
{
    if (s_scissors.empty()) {
        evo_agc_runtime_set_scissor(0, 0, s_pw, s_ph);
    } else {
        const Rect &c = s_scissors.back();
        evo_agc_runtime_set_scissor((int)std::floor(c.x * s_scale), (int)std::floor(c.y * s_scale),
                                    (int)std::ceil(c.w * s_scale), (int)std::ceil(c.h * s_scale));
    }
}

void fill(const Rect &r, uint32_t color, float radius)
{
    const uint32_t c = premul(color);
    quad(r, s_white, 0, 0, 1, 1, c, c, c, c, radius);
}

void fill_vgradient(const Rect &r, uint32_t top, uint32_t bottom, float radius)
{
    const uint32_t t = premul(top), b = premul(bottom);
    quad(r, s_white, 0, 0, 1, 1, t, t, b, b, radius);
}

void fill_hgradient(const Rect &r, uint32_t left, uint32_t right, float radius)
{
    const uint32_t l = premul(left), rr = premul(right);
    quad(r, s_white, 0, 0, 1, 1, l, rr, l, rr, radius);
}

void image(const Rect &r, const Texture *t, float opacity, float radius, bool cover)
{
    if (!t || opacity <= 0.f)
        return;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (cover && t->w > 0 && t->h > 0) {
        const float ia = (float)t->w / (float)t->h, ra = r.w / r.h;
        if (ia > ra) {          /* wider: crop the sides */
            const float k = ra / ia;
            u0 = (1.f - k) * 0.5f;
            u1 = 1.f - u0;
        } else {                /* taller: crop top and bottom */
            const float k = ia / ra;
            v0 = (1.f - k) * 0.5f;
            v1 = 1.f - v0;
        }
    }
    const uint32_t c = premul(0xffffffffu, opacity);
    quad(r, t, u0, v0, u1, v1, c, c, c, c, radius);
}

void image_uv(const Rect &r, const Texture *t, float u0, float v0, float u1, float v1, float opacity, float radius)
{
    if (!t || opacity <= 0.f)
        return;
    const uint32_t c = premul(0xffffffffu, opacity);
    quad(r, t, u0, v0, u1, v1, c, c, c, c, radius);
}

/* One pass of the separable Gaussian (EVO's ui_backdrop_blur pipe): src is
 * sampled, the bound target written; the taps cover 3 sigma. */
#ifdef JELLY5_LOG_HOST
/* Dev builds: why the glass fell back, logged when the reason changes. */
static void glass_why(const char *why)
{
    static const char *s_last = nullptr;
    if (why != s_last) {
        s_last = why;
        evo_boot_log("gfx: glass step: %s", why);
    }
}
#else
static void glass_why(const char *) {}
#endif

static bool blur_pass(const Texture &src, float sigma, bool horizontal)
{
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    const uint32_t slot = evo_agc_runtime_get_current_slot();
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI_BLUR);
    if (!cb || !ring || ud.ps_count == 0 || ud.ps_count > 16 || ud.ps_const_table_dword < 0 ||
        ud.ps_texture_table_dword < 0 || ud.vs_count > 16) {
        glass_why(!cb ? "blur: no command buffer" : !ring ? "blur: no ring" : "blur: user data layout");
        return false;
    }
    evo_agc_transient_slice_t consts, cdesc, tdesc, ib;
    if (evo_agc_transient_ring_alloc(ring, slot, 33u * 16u, 16, &consts) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16u, 16, &cdesc) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48u, 16, &tdesc) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 12u, 16, &ib) != EVO_AGC_TRANSIENT_OK) {
        glass_why("blur: ring full");
        evo_agc_runtime_note_drop(0);
        return false;
    }
    /* BlurConstants: uParams, uOffsets[16], uWeights[16] (33 vec4). */
    const int taps = std::max(1, std::min(15, (int)std::ceil(3.f * std::max(sigma, 1.f))));
    const float radius = 3.f * sigma, two_s2 = 2.f * sigma * sigma;
    float *c = (float *)consts.cpu;
    std::memset(c, 0, 33u * 16u);
    c[0] = 1.f / (float)src.w;
    c[1] = 1.f / (float)src.h;
    c[2] = (float)taps;
    float w[16] = {0}, total = 1.f;
    for (int i = 1; i <= taps; i++) {
        const float d = radius * (float)i / (float)taps;
        w[i] = std::exp(-(d * d) / two_s2);
        total += 2.f * w[i];
    }
    c[68] = 1.f / total;
    for (int i = 1; i <= taps; i++) {
        const float d = radius * (float)i / (float)taps;
        c[4 + 4 * i + 0] = horizontal ? d / (float)src.w : 0.f;
        c[4 + 4 * i + 1] = horizontal ? 0.f : d / (float)src.h;
        c[68 + 4 * i] = w[i] / total;
    }
    evo_agc_build_constant_vsharp((uint32_t *)cdesc.cpu, consts.gpu_addr, 33u * 16u);
    std::memset(tdesc.cpu, 0, 48u);
    if (!build_tsharp(&src, (uint32_t *)tdesc.cpu)) {
        glass_why(src.bgra ? "blur: scanout tsharp (address not 64 KB aligned?)" : "blur: layer tsharp");
        return false;
    }
    evo_agc_build_ssharp((uint32_t *)tdesc.cpu + 8, 1, 1);
    std::memcpy(ib.cpu, kQuad, sizeof kQuad);

    uint32_t vs_user[16] = {0};
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_const_table_dword] = (uint32_t)cdesc.gpu_addr;
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tdesc.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);
    evo_agc_writer_draw_index_modifier(cb, 6, (const uint16_t *)(uintptr_t)ib.gpu_addr,
                                       evo_agc_runtime_get_pipe_draw_modifier(EVO_AGC_PIPE_UI_BLUR));
    evo_agc_runtime_note_draw();
    return true;
}

/* The liquid glass pane in one pass (app/shaders/liquid_glass.pipe) over the
 * scissored pane: refraction at the rim, vibrancy, tint, lit rim and sheen. */
/* Before a pass samples what the pass before it drew (the screen, then each blur
 * layer): a real barrier, so the GPU never reads a half-written surface (that read
 * showed as pixel noise on the glass). The fence word lives in the frame's ring. */
static void gpu_barrier()
{
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    evo_agc_transient_slice_t f;
    if (!cb || !ring ||
        evo_agc_transient_ring_alloc(ring, evo_agc_runtime_get_current_slot(), 8, 8, &f) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_writer_wait_idle(cb, f.gpu_addr, (volatile uint32_t *)f.cpu, 1) != 0)
        evo_agc_flush_color_target();   /* no room: at least the flush, as before */
}

static float s_light_x = -0.55f, s_light_y = -0.83f;

bool set_glass_light(float x, float y)
{
    const float l = std::sqrt(x * x + y * y);
    if (l < 1e-3f)
        return false;
    x /= l, y /= l;
    const bool moved = std::fabs(x - s_light_x) + std::fabs(y - s_light_y) > 0.02f;
    if (moved)
        s_light_x = x, s_light_y = y;
    return moved;
}

static bool glass_pass(const Texture &src, const Rect &r, float radius, float opacity, float lift)
{
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    const uint32_t slot = evo_agc_runtime_get_current_slot();
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI_GLASS);
    if (!cb || !ring || ud.ps_count == 0 || ud.ps_count > 16 || ud.ps_const_table_dword < 0 ||
        ud.ps_texture_table_dword < 0 || ud.vs_count > 16)
        return false;
    evo_agc_transient_slice_t consts, cdesc, tdesc, ib;
    if (evo_agc_transient_ring_alloc(ring, slot, 6u * 16u, 16, &consts) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16u, 16, &cdesc) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48u, 16, &tdesc) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 12u, 16, &ib) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return false;
    }
    float *c = (float *)consts.cpu;
    const float s = s_scale;
    /* The shader's lens bends by up to ~0.74 of the pane's half size at the rim:
     * gentle on a button, but a large sheet pulled in what lay a few hundred pixels
     * inside (big text), split into colours by the dispersion - streaks along its
     * edge. Capped to what a 32 px half-height pane bends. */
    const float half = std::max(1.f, std::min(r.w, r.h) / 2);
    const float bend = std::min(1.f, 32.f / half);
    const float k[24] = {
        r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s,              /* uRect */
        radius * s, std::min(30.f, std::min(r.w, r.h) * 0.3f) * s,      /* uShape: radius, bevel */
        1.f / (float)src.w, 1.f / (float)src.h,
        s_light_x, s_light_y, 1.f, (1.f - 0.55f * lift) * bend,          /* uLight: dir, rim, refraction */
        1.3f, 1.05f, 0.07f, opacity,                                     /* uTone: saturation, brightness, tint */
        0.92f, 0.94f, 1.f, 0.05f,                                        /* uTint: a milky white, sheen */
        lift, 0.28f * (1.f - lift), 0.f, 0.10f * (1.f - lift),           /* uMore: lift, legibility, magnify, dispersion */
    };
    std::memcpy(c, k, sizeof k);
    evo_agc_build_constant_vsharp((uint32_t *)cdesc.cpu, consts.gpu_addr, 6u * 16u);
    std::memset(tdesc.cpu, 0, 48u);
    if (!build_tsharp(&src, (uint32_t *)tdesc.cpu))
        return false;
    evo_agc_build_ssharp((uint32_t *)tdesc.cpu + 8, 1, 1);
    std::memcpy(ib.cpu, kQuad, sizeof kQuad);

    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI_GLASS);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    uint32_t vs_user[16] = {0};
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_const_table_dword] = (uint32_t)cdesc.gpu_addr;
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tdesc.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);
    evo_agc_writer_draw_index_modifier(cb, 6, (const uint16_t *)(uintptr_t)ib.gpu_addr,
                                       evo_agc_runtime_get_pipe_draw_modifier(EVO_AGC_PIPE_UI_GLASS));
    evo_agc_runtime_note_draw();
    return true;
}

/* The screen under (bx0, by0)-(bx1, by1), physical pixels, blurred (sigma sp,
 * physical) into a layer: *out samples it. Two layers, alternating between two
 * pairs from one call to the next, so a call's passes never write the layer the
 * call just before is still reading on the GPU. (No CPU clear: the passes write
 * all that is read.) *h stays null when there was no layer; else the caller
 * restores the scissor and releases *h and *v. */
static bool blur_layers(int bx0, int by0, int bx1, int by1, float sp, evo_agc_layer_surface_t **ph,
                        evo_agc_layer_surface_t **pv, Texture *out)
{
    const float reach = 3.f * sp + 2.f;
    const int gy0 = std::max(0, (int)(by0 - reach)), gy1 = std::min(s_ph, (int)(by1 + reach));
    static bool s_no_clear = (evo_agc_layers_set_clear(0), true);
    (void)s_no_clear;
    static unsigned s_pane = 0;
    evo_agc_layer_surface_t *got[4] = {nullptr, nullptr, nullptr, nullptr};
    int n = 0;
    while (n < 4 && evo_agc_layer_acquire(&got[n]) == 0 && got[n])
        n++;
    if (n < 2) {
        glass_why(n == 0 ? "no free layer" : "no second layer");
        for (int i = 0; i < n; i++)
            evo_agc_layer_release(got[i]);
        restore_scissor();
        return false;
    }
    const int pair = (n >= 4 && (s_pane++ & 1)) ? 2 : 0;
    evo_agc_layer_surface_t *h = got[pair], *v = got[pair + 1];
    for (int i = 0; i < n; i++)
        if (i != pair && i != pair + 1)
            evo_agc_layer_release(got[i]);
    evo_agc_layer_surface_t scan;
    evo_agc_get_scanout_layer(&scan);
    Texture src_scan, src_h;
    src_scan.mem = (uint8_t *)(uintptr_t)scan.gpu_addr;
    src_scan.w = (int)scan.width;
    src_scan.h = (int)scan.height;
    src_scan.tiled = src_scan.bgra = true;
    src_h.mem = (uint8_t *)(uintptr_t)h->gpu_addr;
    src_h.w = (int)h->width;
    src_h.h = (int)h->height;
    src_h.tiled = true;
    *out = src_h;
    out->mem = (uint8_t *)(uintptr_t)v->gpu_addr;

    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI_BLUR);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);
    gpu_barrier();                                      /* what is drawn so far, finished and readable */
    evo_agc_set_layer_target(h);
    evo_agc_runtime_set_scissor(bx0, gy0, bx1 - bx0, gy1 - gy0);
    bool ok = blur_pass(src_scan, sp, true);
    gpu_barrier();
    evo_agc_set_layer_target(v);
    evo_agc_runtime_set_scissor(bx0, by0, bx1 - bx0, by1 - by0);
    ok = ok && blur_pass(src_h, sp, false);
    gpu_barrier();
    evo_agc_set_layer_target(nullptr);                  /* back on the scanout */
    *ph = h;
    *pv = v;
    return ok;
}

int backdrop_blur(const Rect &r, float radius, float sigma, float opacity, float lift)
{
#ifdef JELLY5_LOG_HOST
    static int s_why = -1;   /* why the glass is flat, logged when it changes */
    const int why = !evo_agc_has_layers() ? 1 : !evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_UI_BLUR) ? 2 : 0;
    if (why != s_why) {
        s_why = why;
        evo_boot_log("gfx: backdrop layers=%d blur=%d glass=%d", evo_agc_has_layers(),
                     evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_UI_BLUR),
                     evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_UI_GLASS));
    }
#endif
    if (opacity <= 0.f || !evo_agc_has_layers() || !evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_UI_BLUR))
        return 0;
    /* Panel pixels: the rect, and the rect grown by the blur's reach for the first pass. */
    /* The real glass shows the picture behind it, only softened, so the bending at
     * the rim can be seen; without the shader a stronger blur carries the look. */
    const bool shader = evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_UI_GLASS);
    const float sp = (shader ? sigma * 0.3f : sigma) * s_scale;
    const int x0 = std::max(0, (int)std::floor(r.x * s_scale)), y0 = std::max(0, (int)std::floor(r.y * s_scale));
    const int x1 = std::min(s_pw, (int)std::ceil((r.x + r.w) * s_scale));
    const int y1 = std::min(s_ph, (int)std::ceil((r.y + r.h) * s_scale));
    if (x1 <= x0 || y1 <= y0) {
        glass_why("empty rect");
        return 0;
    }
    /* The blurred area: the pane, grown for the shader's refraction, which samples
     * from just outside the rim. */
    const int m = shader ? (int)std::ceil(30.f * s_scale) : 0;
    const int bx0 = std::max(0, x0 - m), by0 = std::max(0, y0 - m), bx1 = std::min(s_pw, x1 + m),
              by1 = std::min(s_ph, y1 + m);
    evo_agc_layer_surface_t *h = nullptr, *v = nullptr;
    Texture out_v;
    const bool ok = blur_layers(bx0, by0, bx1, by1, sp, &h, &v, &out_v);
    if (!h)
        return 0;
    int result = ok ? 1 : 0;
    if (ok && shader) {   /* the real glass */
        evo_agc_runtime_set_scissor(x0, y0, x1 - x0, y1 - y0);
        if (glass_pass(out_v, r, radius, opacity, lift))
            result = 2;
        else
            glass_why("glass pass failed");
    }
#ifdef JELLY5_LOG_HOST
    static int s_logged = -1;
    if (result != s_logged) {
        s_logged = result;
        evo_boot_log("gfx: glass %s", result == 2 ? "shader" : result == 1 ? "blur" : "flat");
    }
#endif
    restore_scissor();
    if (result == 1) {
        /* The blurred backdrop into the panel's rounded shape, as through a lens: a
         * little magnified inside, and along the straight edges what lies just outside
         * is drawn in, bent toward the rim. */
        const float W = (float)out_v.w / s_scale, H = (float)out_v.h / s_scale;   /* logical */
        const float zoom = 0.035f, mx = r.w * zoom / 2, my = r.h * zoom / 2;
        const uint32_t col = premul(0xffffffffu, opacity);
        quad(r, &out_v, (r.x + mx) / W, (r.y + my) / H, (r.x + r.w - mx) / W, (r.y + r.h - my) / H, col, col, col, col,
             radius);
        const float e = std::min(14.f, std::min(r.w, r.h) / 6), bend = e * 1.4f;
        const float sx0 = r.x + radius, sx1 = r.x + r.w - radius, sy0 = r.y + radius, sy1 = r.y + r.h - radius;
        if (sx1 > sx0) {
            quad({sx0, r.y, sx1 - sx0, e}, &out_v, sx0 / W, (r.y - bend) / H, sx1 / W, (r.y + e) / H, col, col, col,
                 col, 0);
            quad({sx0, r.y + r.h - e, sx1 - sx0, e}, &out_v, sx0 / W, (r.y + r.h - e) / H, sx1 / W,
                 (r.y + r.h + bend) / H, col, col, col, col, 0);
        }
        if (sy1 > sy0) {
            quad({r.x, sy0, e, sy1 - sy0}, &out_v, (r.x - bend) / W, sy0 / H, (r.x + e) / W, sy1 / H, col, col, col,
                 col, 0);
            quad({r.x + r.w - e, sy0, e, sy1 - sy0}, &out_v, (r.x + r.w - e) / W, sy0 / H, (r.x + r.w + bend) / W,
                 sy1 / H, col, col, col, col, 0);
        }
    }
    evo_agc_layer_release(h);
    evo_agc_layer_release(v);
    return result;
}

void push_fade_mask(const Rect &r, float top, float bottom, float left, float right)
{
    s_mask_on = top > 0 || bottom > 0 || left > 0 || right > 0;
    s_mask_r = r;
    s_mask_t = std::min(top, r.h / 2), s_mask_b = std::min(bottom, r.h / 2);
    s_mask_l = std::min(left, r.w / 2), s_mask_rt = std::min(right, r.w / 2);
}

void pop_fade_mask() { s_mask_on = false; }

void rim(const Rect &r, float radius, float opacity)
{
    if (!s_rim || opacity <= 0.f || r.w < 2 * radius || r.h < 2 * radius)
        return;
    /* Nine slices: the texture's 64-pixel corners drawn at the panel's radius. */
    const float xs[4] = {r.x, r.x + radius, r.x + r.w - radius, r.x + r.w};
    const float ys[4] = {r.y, r.y + radius, r.y + r.h - radius, r.y + r.h};
    const float us[4] = {0.f, 0.25f, 0.75f, 1.f};
    const uint32_t c = premul(0xffffffffu, opacity);
    Vertex v[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            v[j * 4 + i] = {xs[i], ys[j], c, us[i], us[j]};
    uint16_t idx[54];
    int n = 0;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (i == 1 && j == 1)
                continue;   /* the middle is empty */
            const uint16_t a = (uint16_t)(j * 4 + i);
            const uint16_t q[6] = {a, (uint16_t)(a + 1), (uint16_t)(a + 4), (uint16_t)(a + 4),
                                   (uint16_t)(a + 1), (uint16_t)(a + 5)};
            std::memcpy(idx + n, q, sizeof q);
            n += 6;
        }
    draw_mesh(v, 16, idx, n, s_rim, nullptr, 0);
}

void shadow(const Rect &r, float radius, float blur, float opacity, float dy)
{
    if (!s_shadow || opacity <= 0.f)
        return;
    /* Nine slices of the shadow texture: corners fixed at `blur` px, edges stretched. */
    const float e = blur * 1.6f;
    const Rect o{r.x - e, r.y - e + dy, r.w + 2 * e, r.h + 2 * e};
    /* A side shorter than both corners (a 60 px note under a 40 px blur): the
     * corners meet in the middle, each cut short, instead of folding over each
     * other (that drew a dark band with a hard edge). */
    auto slices = [e](float from, float size, float *at, float *uv) {
        const float c = std::min(2 * e, size / 2);
        at[0] = from, at[1] = from + c, at[2] = from + size - c, at[3] = from + size;
        uv[0] = 0.f, uv[1] = 0.5f * c / (2 * e), uv[2] = 1.f - uv[1], uv[3] = 1.f;
    };
    float xs[4], ys[4], us[4], vs[4];
    slices(o.x, o.w, xs, us);
    slices(o.y, o.h, ys, vs);
    const uint32_t c = premul(0xff000000u, opacity);
    Vertex v[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            v[j * 4 + i] = {xs[i], ys[j], c, us[i], vs[j]};
    uint16_t idx[54];
    int n = 0;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            const uint16_t a = (uint16_t)(j * 4 + i);
            const uint16_t q[6] = {a, (uint16_t)(a + 1), (uint16_t)(a + 4), (uint16_t)(a + 4),
                                   (uint16_t)(a + 1), (uint16_t)(a + 5)};
            std::memcpy(idx + n, q, sizeof q);
            n += 6;
        }
    (void)radius;
    draw_mesh(v, 16, idx, n, s_shadow, nullptr, 0);
}

/* ---- text ------------------------------------------------------------------------ */

namespace {

struct TextKey {
    std::string s;
    int weight, size10, maxw, lines;
    bool operator==(const TextKey &o) const
    {
        return weight == o.weight && size10 == o.size10 && maxw == o.maxw && lines == o.lines && s == o.s;
    }
};
struct TextKeyHash {
    size_t operator()(const TextKey &k) const
    {
        return std::hash<std::string>()(k.s) ^ ((size_t)k.weight << 1) ^ ((size_t)k.size10 << 4) ^
               ((size_t)k.maxw << 12) ^ ((size_t)k.lines << 24);
    }
};
struct TextEntry {
    Texture *tex = nullptr;
    float w = 0, h = 0;        /* logical size of the texture */
    float ascent = 0;          /* logical px from top to the first baseline */
    float advance = 0;         /* logical width of the text */
    uint64_t used = 0;
};
std::unordered_map<TextKey, TextEntry, TextKeyHash> s_text;

ui_weight to_ui(Weight w)
{
    switch (w) {
    case Regular: return UI_REGULAR;
    case Medium: return UI_MEDIUM;
    case SemiBold: return UI_SEMIBOLD;
    default: return UI_BOLD;
    }
}

TextEntry &text_entry(const std::string &s, const TextStyle &st)
{
    TextKey key{s, (int)st.weight, (int)(st.size * 10), (int)st.max_w, st.max_lines};
    auto it = s_text.find(key);
    if (it != s_text.end()) {
        it->second.used = s_frame;
        return it->second;
    }
    TextEntry e;
    e.used = s_frame;
    /* Rasterised at panel resolution: 2x on a 4K panel. */
    const float k = s_scale;
    const ui_weight w = to_ui(st.weight);
    const float size = st.size * k;
    float asc = 0, desc = 0;
    ui_text_metrics(w, size, &asc, &desc);
    const float pad = 2 * k;
    int cw, ch, lines = 1;
    const float line_h = (st.line_h > 0 ? st.line_h : st.size * 1.4f) * k;
    if (st.max_lines > 1 && st.max_w > 0) {
        lines = ui_text_draw_wrapped(nullptr, w, size, 0, asc, st.max_w * k, line_h, st.max_lines, 0, s.c_str());
        cw = (int)std::ceil(st.max_w * k + 2 * pad);
        ch = (int)std::ceil(asc + desc + (lines - 1) * line_h + 2 * pad);
        e.advance = st.max_w;
    } else {
        float tw = ui_text_width(w, size, s.c_str());
        if (st.max_w > 0)
            tw = std::min(tw, st.max_w * k);
        cw = (int)std::ceil(tw + 2 * pad);
        ch = (int)std::ceil(asc + desc + 2 * pad);
        e.advance = tw / k;
    }
    ui_canvas c;
    if (cw > 0 && ch > 0 && ui_canvas_init(&c, cw, ch) == 0) {
        ui_canvas_clear(&c);
        if (st.max_lines > 1 && st.max_w > 0)
            ui_text_draw_wrapped(&c, w, size, pad, pad + asc, st.max_w * k, line_h, st.max_lines, UI_WHITE,
                                 s.c_str());
        else
            ui_text_draw(&c, w, size, pad, pad + asc, UI_WHITE, s.c_str(), st.max_w > 0 ? st.max_w * k : 0);
        e.tex = texture_from_pixels(c.px, cw, ch, cw);
        ui_canvas_free(&c);
    }
    e.w = cw / k;
    e.h = ch / k;
    e.ascent = (pad + asc) / k;
    return s_text.emplace(std::move(key), e).first->second;
}

} // namespace

float text_width(const std::string &s, const TextStyle &st)
{
    return s.empty() ? 0.f : text_entry(s, st).advance;
}

float text(float x, float baseline, const std::string &s, const TextStyle &st, uint32_t color, int align)
{
    if (s.empty())
        return 0;
    TextEntry &e = text_entry(s, st);
    if (!e.tex)
        return e.advance;
    /* The raster carries 2 logical px of padding on every side. */
    const float left = (align == 1 ? x - e.advance * 0.5f : align == 2 ? x - e.advance : x) - 2.f;
    const Rect r{left, baseline - e.ascent, e.w, e.h};
    const uint32_t c = premul(color);
    quad(r, e.tex, 0, 0, 1, 1, c, c, c, c, 0);
    return e.advance;
}

size_t text_cache_size() { return s_text.size(); }

void collect()
{
    /* Text not drawn for ~10 s goes; the cache never holds more than 1500. */
    for (auto it = s_text.begin(); it != s_text.end();) {
        if (s_frame - it->second.used > 600 || s_text.size() > 1500) {
            texture_release(it->second.tex);
            it = s_text.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace gfx
