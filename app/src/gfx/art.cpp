/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "art.h"
#include "gfx_pool.h"

#include "ui_canvas.h"
#include "ui_image.h"

#include "evo_boot_trace.h"

#include <algorithm>
#include <cmath>
#include <time.h>
#include <unordered_map>
#include <vector>

namespace art {
namespace {

constexpr int kUploadsPerFrame = 3;

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct Entry {
    int handle = -1;              /* ui_image request */
    int max_w = 0, max_h = 0;
    gfx::Texture *tex = nullptr;
    double ready_at = 0;
    uint64_t used = 0;
    size_t bytes = 0;
    bool failed = false;
};

std::unordered_map<std::string, Entry> s_art;
/* BlurHash placeholders (32x20), kept while used; the oldest go past kMaxHashes. */
struct HashEntry {
    gfx::Texture *tex;
    uint64_t used;
};
std::unordered_map<std::string, HashEntry> s_hash;
constexpr size_t kMaxHashes = 1200;
uint64_t s_tick = 0;
size_t s_bytes = 0;
bool s_animating = false;
int s_uploads = 0;
bool s_pressure = false;        /* an upload found the pool full */

/* Art may use ~75% of the texture pool; text and placeholders need the rest. */
size_t budget() { return gfx::pool_size() * 3 / 4; }

/* ---- BlurHash (https://blurha.sh), decoded to 32x20 ------------------------------- */
int d83(const std::string &s, size_t from, size_t to)
{
    static const char *chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";
    int v = 0;
    for (size_t i = from; i < to && i < s.size(); i++) {
        const char *p = std::strchr(chars, s[i]);
        if (!p)
            return -1;
        v = v * 83 + (int)(p - chars);
    }
    return v;
}
float to_linear(int v)
{
    const float x = v / 255.f;
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
uint32_t to_srgb(float v)
{
    v = std::max(0.f, std::min(1.f, v));
    return (uint32_t)((v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f) * 255.f + 0.5f);
}
float sign_pow(float v, float e) { return std::copysign(std::pow(std::fabs(v), e), v); }

gfx::Texture *decode_blurhash(const std::string &h)
{
    if (h.size() < 6)
        return nullptr;
    const int sf = d83(h, 0, 1);
    if (sf < 0)
        return nullptr;
    const int ny = sf / 9 + 1, nx = sf % 9 + 1;
    if ((int)h.size() != 4 + 2 * nx * ny)
        return nullptr;
    const float max_v = (d83(h, 1, 2) + 1) / 166.f;
    std::vector<float> col((size_t)nx * ny * 3);
    for (int i = 0; i < nx * ny; i++) {
        if (i == 0) {
            const int v = d83(h, 2, 6);
            col[0] = to_linear(v >> 16);
            col[1] = to_linear((v >> 8) & 255);
            col[2] = to_linear(v & 255);
        } else {
            const int v = d83(h, 4 + i * 2, 6 + i * 2);
            col[i * 3 + 0] = sign_pow((v / 361 - 9) / 9.f, 2) * max_v;
            col[i * 3 + 1] = sign_pow(((v / 19) % 19 - 9) / 9.f, 2) * max_v;
            col[i * 3 + 2] = sign_pow((v % 19 - 9) / 9.f, 2) * max_v;
        }
    }
    const int W = 32, H = 20;
    std::vector<uint32_t> px((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    const float basis = std::cos((float)M_PI * x * i / W) * std::cos((float)M_PI * y * j / H);
                    const float *c = &col[(size_t)(i + j * nx) * 3];
                    r += c[0] * basis;
                    g += c[1] * basis;
                    b += c[2] * basis;
                }
            px[(size_t)y * W + x] = to_srgb(r) | (to_srgb(g) << 8) | (to_srgb(b) << 16) | 0xff000000u;
        }
    return gfx::texture_from_pixels(px.data(), W, H, W);
}

void evict(Entry &e)
{
    if (e.tex) {
        gfx::texture_release(e.tex);
        s_bytes -= e.bytes;
    }
    e.tex = nullptr;
    e.bytes = 0;
}

} // namespace

bool failed(const std::string &url)
{
    auto it = s_art.find(url);
    return it != s_art.end() && it->second.failed;
}

const gfx::Texture *get(const std::string &url, int max_w, int max_h)
{
    if (url.empty())
        return nullptr;
    Entry &e = s_art[url];
    e.used = s_tick;
    if (e.tex || e.failed)
        return e.tex;
    e.max_w = max_w;
    e.max_h = max_h;
    if (e.handle < 0 || !ui_image_alive(e.handle))
        e.handle = ui_image_request(url.c_str(), max_w, max_h, 0);
    int failed = 0;
    const ui_image *img = ui_image_get(e.handle, &failed);
    if (img && s_uploads < kUploadsPerFrame) {
        s_uploads++;
        e.tex = gfx::texture_from_image(img);
        if (!e.tex)
            s_pressure = true;   /* pool full: tick() frees some, then this retries */
        if (e.tex) {
            e.bytes = (size_t)img->w * img->h * 4;
            s_bytes += e.bytes;
            e.ready_at = now_s();
        }
    } else if (!img && failed && ui_image_alive(e.handle)) {
        e.failed = true;
    }
    if (!e.tex)
        s_animating = true;   /* still waiting: keep frames coming */
    return e.tex;
}

float fade(const std::string &url, float seconds)
{
    auto it = s_art.find(url);
    if (it == s_art.end() || !it->second.tex)
        return 0.f;
    const float t = (float)std::min(1.0, (now_s() - it->second.ready_at) / seconds);
    if (t < 1.f)
        s_animating = true;
    return t * t * (3.f - 2.f * t);
}

const gfx::Texture *blurhash(const std::string &hash)
{
    if (hash.empty())
        return nullptr;
    auto it = s_hash.find(hash);
    if (it != s_hash.end()) {
        it->second.used = s_tick;
        return it->second.tex;
    }
    gfx::Texture *t = decode_blurhash(hash);
    if (t)   /* a failure (pool full) is tried again later, not remembered */
        s_hash.emplace(hash, HashEntry{t, s_tick});
    return t;
}

void draw(const gfx::Rect &r, const std::string &url, const std::string &hash, int max_w, int max_h,
          float radius, float opacity, uint32_t empty_color)
{
    const gfx::Texture *t = get(url, max_w, max_h);
    const float a = t ? fade(url) : 0.f;
    if (a < 1.f) {
        if (const gfx::Texture *ph = blurhash(hash))
            gfx::image(r, ph, opacity, radius, true);
        else
            gfx::fill(r, empty_color, radius);
    }
    if (t)
        gfx::image(r, t, opacity * a, radius, true);
}

bool animating() { return s_animating; }

void stats(size_t *bytes, size_t *images, size_t *placeholders)
{
    *bytes = s_bytes;
    *images = s_art.size();
    *placeholders = s_hash.size();
}

void tick()
{
    s_tick++;
    s_uploads = 0;
    s_animating = false;
    const bool pressure = s_pressure;
    s_pressure = false;
    if (s_hash.size() > kMaxHashes) {   /* placeholders: back to 3/4 of the cap, oldest first */
        std::vector<std::pair<uint64_t, std::string>> old;
        for (auto &kv : s_hash)
            if (kv.second.used + 2 < s_tick)
                old.emplace_back(kv.second.used, kv.first);
        std::sort(old.begin(), old.end());
        for (auto &p : old) {
            if (s_hash.size() <= kMaxHashes * 3 / 4)
                break;
            gfx::texture_release(s_hash[p.second].tex);
            s_hash.erase(p.second);
        }
    }
    if (s_bytes <= budget() && !pressure)
        return;
    /* Over budget: drop the least recently drawn textures (not this frame's). */
    std::vector<std::pair<uint64_t, std::string>> lru;
    for (auto &kv : s_art)
        if (kv.second.tex && kv.second.used + 2 < s_tick)
            lru.emplace_back(kv.second.used, kv.first);
    std::sort(lru.begin(), lru.end());
    for (auto &p : lru) {
        if (s_bytes <= budget() * 3 / 4)
            break;
        evict(s_art[p.second]);
        s_art.erase(p.second);
    }
}

void reset()
{
    for (auto &kv : s_art)
        evict(kv.second);
    s_art.clear();
}

} // namespace art
