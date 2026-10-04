/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Time-stretching for playback speed (WSOLA): the sound plays faster or
 * slower without changing pitch. One stream at a time (the player's).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* in: `frames` interleaved samples of `ch` channels. Returns how many frames it
 * made, at *out (valid until the next call); fewer when speed > 1. */
int jelly5_tempo_process(const float *in, int frames, int ch, float speed, const float **out);
/* Forget what is buffered (a seek, a new stream, back to 1x). */
void jelly5_tempo_reset(void);

#ifdef __cplusplus
}
#endif
