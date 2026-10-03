/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPU-visible memory for UI textures: one block of direct memory mapped for
 * CPU and GPU, carved first-fit. Unlike the engine's pool it never falls back
 * to the heap (the GPU cannot read heap memory: sampling it faults), so a full
 * pool is reported and the artwork cache gives memory back.
 */
#pragma once

#include <cstddef>

namespace gfx {

/* Reserves the pool, stepping down from want_bytes; returns the size got. */
size_t pool_init(size_t want_bytes);
void *pool_alloc(size_t bytes);     /* 256-byte aligned, or null when full */
void pool_free(void *p);
size_t pool_size();
size_t pool_used();

} // namespace gfx
