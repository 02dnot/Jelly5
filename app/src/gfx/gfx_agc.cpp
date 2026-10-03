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
    if (evo_agc_build_tsharp_rgba8((uint32_t *)tex_d.cpu, (uint64_t)(uintptr_t)t->mem, (uint32_t)t->w,
                                   (uint32_t)t->h, t->pitch) != 0)
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

void quad(const Rect &r, const Texture *t, float u0, float v0, float u1, float v1, uint32_t c_tl,
          uint32_t c_tr, uint32_t c_bl, uint32_t c_br, float radius)
{
    if (r.w <= 0 || r.h <= 0)
        return;
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

void shadow(const Rect &r, float radius, float blur, float opacity, float dy)
{
    if (!s_shadow || opacity <= 0.f)
        return;
    /* Nine slices of the shadow texture: corners fixed at `blur` px, edges stretched. */
    const float e = blur * 1.6f;
    const Rect o{r.x - e, r.y - e + dy, r.w + 2 * e, r.h + 2 * e};
    const float xs[4] = {o.x, o.x + 2 * e, o.x + o.w - 2 * e, o.x + o.w};
    const float ys[4] = {o.y, o.y + 2 * e, o.y + o.h - 2 * e, o.y + o.h};
    const float us[4] = {0.f, 0.5f, 0.5f, 1.f};
    const uint32_t c = premul(0xff000000u, opacity);
    Vertex v[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            v[j * 4 + i] = {xs[i], ys[j], c, us[i], us[j]};
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
