/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HDMI bitstream ("Innstillinger: HDMI-bitstrøm"): Dolby Digital, Dolby Digital
 * Plus (with Atmos) and DTS go to the TV or receiver undecoded, as IEC 61937
 * bursts on the "Ex" audio ports the PS5's disc player uses. Only the formats
 * the TV or receiver lists (its EDID, read from the system) and the viewer
 * allows; anything else is decoded to PCM as before. Proven on the console
 * 2026-10-07 (private/discovery/passthrough-results.md); the port calls, modes
 * and rates were found by SlopFin.
 *
 * C API for the engine: the playback controller opens and closes it, the audio
 * decode thread feeds it packets, the output thread plays it a grain at a time.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JELLY5_BS_AC3 = 1,    /* Dolby Digital */
    JELLY5_BS_EAC3 = 2,   /* Dolby Digital Plus (Atmos as DD+ JOC) */
    JELLY5_BS_DTS = 4,    /* DTS core (DTS-HD keeps decoding to lossless PCM) */
};

/* What the viewer allows for the next playback (0: off). Set before each run. */
void jelly5_bs_set_allowed(int mask);
/* What the TV or receiver lists (JELLY5_BS_*), read from the system; -1 when its
 * list could not be read. Cheap after the first call of a playback. */
int jelly5_bs_sink_formats(void);

/* At open: when this stream may go out as bitstream, switches HDMI to it and
 * opens the port. Returns the port handle (> 0), or -1 with nothing changed. */
int jelly5_bs_open(int codec_id, int profile, int sample_rate, int channels);
/* At close: the port, and HDMI back to the system's own output. */
void jelly5_bs_close(int handle);
/* Whether the stream playing now is bitstream, and as what ("Dolby Digital" ...). */
int jelly5_bs_active(void);
const char *jelly5_bs_name(void);

/* Decode thread: a demuxed packet. Waits while the buffer is full (as long as
 * *running holds and no reset comes). */
void jelly5_bs_feed(const uint8_t *data, int size, volatile int *running);
/* Output thread: the next grain to write (NULL when none is ready yet), how
 * many frames a grain is, and how many 48 kHz media samples one grain plays. */
const int16_t *jelly5_bs_pop(void);
int jelly5_bs_grain_frames(void);
int jelly5_bs_media_samples(void);
/* A grain of silence (pause, underrun): keeps the port and the receiver locked. */
const int16_t *jelly5_bs_silence(void);
/* Seek / flush: everything buffered goes. */
void jelly5_bs_reset(void);

#ifdef __cplusplus
}
#endif
