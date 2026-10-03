/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gfx_pool.h"

#include "evo_boot_trace.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <sys/types.h>
#include <unordered_map>

extern "C" {
int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t len, size_t alignment, int memoryType,
                                  off_t *physAddrOut);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t directMemoryStart,
                             size_t alignment);
}

namespace gfx {
namespace {

constexpr size_t kAlign = 256;
constexpr size_t k2M = 2u * 1024u * 1024u;

std::mutex s_lock;
uint8_t *s_base = nullptr;
size_t s_size = 0, s_used = 0;
std::map<size_t, size_t> s_free;               /* offset -> length, coalesced */
std::unordered_map<size_t, size_t> s_live;     /* offset -> length */

} // namespace

size_t pool_init(size_t want)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_base)
        return s_size;
    for (size_t sz = want; sz >= 64u * 1024u * 1024u && !s_base; sz = sz * 3 / 4) {
        const size_t len = (sz + k2M - 1) & ~(k2M - 1);
        off_t phys = 0;
        /* Type 3: WB_ONION, CPU-cached and GPU-coherent, as the engine uses. */
        if (sceKernelAllocateDirectMemory(0, (off_t)16 * 1024 * 1024 * 1024ULL, len, k2M, 3, &phys) != 0)
            continue;
        void *mapped = nullptr;
        if (sceKernelMapDirectMemory(&mapped, len, 0x33 /* CPU RW | GPU RW */, 0, phys, k2M) != 0 || !mapped)
            continue;
        s_base = (uint8_t *)mapped;
        s_size = len;
        s_free[0] = len;
    }
    evo_bt("gfx: texture pool %zu MB at %p", s_size >> 20, (void *)s_base);
    return s_size;
}

void *pool_alloc(size_t bytes)
{
    std::lock_guard<std::mutex> g(s_lock);
    const size_t len = (bytes + kAlign - 1) & ~(kAlign - 1);
    for (auto it = s_free.begin(); it != s_free.end(); ++it) {
        if (it->second < len)
            continue;
        const size_t off = it->first, rest = it->second - len;
        s_free.erase(it);
        if (rest)
            s_free[off + len] = rest;
        s_live[off] = len;
        s_used += len;
        return s_base + off;
    }
    return nullptr;
}

void pool_free(void *p)
{
    if (!p)
        return;
    std::lock_guard<std::mutex> g(s_lock);
    const size_t off = (size_t)((uint8_t *)p - s_base);
    auto live = s_live.find(off);
    if (live == s_live.end())
        return;
    size_t start = off, len = live->second;
    s_used -= len;
    s_live.erase(live);
    auto next = s_free.find(start + len);
    if (next != s_free.end()) {
        len += next->second;
        s_free.erase(next);
    }
    auto prev = s_free.lower_bound(start);
    if (prev != s_free.begin()) {
        --prev;
        if (prev->first + prev->second == start) {
            start = prev->first;
            len += prev->second;
            s_free.erase(prev);
        }
    }
    s_free[start] = len;
}

size_t pool_size() { return s_size; }
size_t pool_used() { std::lock_guard<std::mutex> g(s_lock); return s_used; }

} // namespace gfx
