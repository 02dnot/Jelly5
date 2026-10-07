/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HDMI bitstream (see jelly5_bitstream.h). The port sequence (ExConfigureOutput,
 * ExOpen, ExClose, restore with mode 255), the modes and their carrier rates are
 * SlopFin's findings (Copyright (C) 2026 Brett, GPL-3.0-or-later; src/audio.cpp).
 */
#include "jelly5_bitstream.h"
#include "iec61937.hpp"

#include "evo_boot_trace.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/defs.h>
}

#include <atomic>
#include <cstring>
#include <mutex>
#include <unistd.h>

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutExConfigureOutput(int zero, long unused, int mode, int device, long unused2);
int sceAudioOutExOpen(int user, int mode);
int sceAudioOutExClose(int handle);
int sceAudioOutExGetMonitorInfo(int zero, long unused, void *buf, int size);
}

namespace {

using jelly5::iec61937::Format;

std::atomic<int> s_allowed{0};
std::atomic<int> s_active{0};
const char *s_name = "";
Format s_format = Format::ac3;
unsigned s_carrier = 48000;   /* the stereo S16 carrier the bursts ride on */
int s_grain = 256;            /* frames per sceAudioOutOutput on that port */

/* Decode thread only (and open, before the threads start). */
jelly5::iec61937::Packer s_packer;
std::atomic<unsigned> s_gen{0};   /* bumped by a reset: the packer and a waiting feed start over */
unsigned s_packer_gen = 0;

/* The bursts, as 16-bit words: one writer (decode), one reader (output). 1 M words
 * is 2.7 s of E-AC-3's 192 kHz carrier, 10.9 s of AC-3's or DTS's 48 kHz. */
constexpr size_t kRing = size_t{1} << 20;
int16_t s_ring[kRing];
size_t s_read = 0, s_write = 0, s_count = 0;
std::mutex s_lock;
int16_t s_out[1024 * 2];
const int16_t s_zero[1024 * 2] = {};

/* What the TV or receiver lists. The system returns its EDID audio part: a
 * count at 0x98, then 8-byte entries from 0xa0 led by the CEA-861 audio format
 * code (2 AC-3, 7 DTS, 10 E-AC-3, 12 MAT) and the channel count. Read from a
 * Samsung TV (2026-10-07); anything that does not look like that is "unknown". */
int parse_sink(const uint8_t *b, int n)
{
    uint32_t count = 0;
    std::memcpy(&count, b + 0x98, 4);
    if (count == 0 || count > 32 || 0xa0 + (int)count * 8 > n)
        return -1;
    int mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t format = b[0xa0 + i * 8], channels = b[0xa0 + i * 8 + 1];
        if (format < 1 || format > 15 || channels < 1 || channels > 8)
            return -1;
        if (format == 2)
            mask |= JELLY5_BS_AC3;
        else if (format == 7)
            mask |= JELLY5_BS_DTS;
        else if (format == 10)
            mask |= JELLY5_BS_EAC3;
    }
    return mask;
}

void push(const uint16_t *words, size_t n, unsigned gen, volatile int *running)
{
    for (;;) {   /* wait for room: the output thread drains a grain every 5 ms */
        if ((running && !*running) || s_gen.load() != gen)
            return;
        {
            std::lock_guard<std::mutex> g(s_lock);
            if (kRing - s_count >= n) {
                for (size_t i = 0; i < n; i++)
                    s_ring[(s_write + i) % kRing] = (int16_t)words[i];
                s_write = (s_write + n) % kRing;
                s_count += n;
                return;
            }
        }
        usleep(2000);
    }
}

} // namespace

extern "C" {

void jelly5_bs_set_allowed(int mask) { s_allowed = mask; }

int jelly5_bs_sink_formats(void)
{
    static uint8_t info[0x400];
    std::memset(info, 0, sizeof info);
    const int rc = sceAudioOutExGetMonitorInfo(0, 0, info, sizeof info);
    return rc < 0 ? -1 : parse_sink(info, sizeof info);
}

int jelly5_bs_open(int codec_id, int profile, int sample_rate, int channels)
{
    const int allowed = s_allowed.load();
    if (!allowed || sample_rate != 48000)
        return -1;
    int want = 0, mode = -1;
    if (codec_id == AV_CODEC_ID_AC3) {
        want = JELLY5_BS_AC3, mode = 0, s_format = Format::ac3, s_name = "Dolby Digital";
    } else if (codec_id == AV_CODEC_ID_EAC3) {
        want = JELLY5_BS_EAC3, mode = 3, s_format = Format::eac3, s_name = "Dolby Digital Plus";
    } else if (codec_id == AV_CODEC_ID_DTS) {
        /* DTS-HD (MA, HRA, :X) and Express decode here to lossless or full PCM;
         * as a bitstream only their lossy core would reach the receiver. */
        if (profile == AV_PROFILE_DTS_HD_MA || profile == AV_PROFILE_DTS_HD_HRA ||
            profile == AV_PROFILE_DTS_HD_MA_X || profile == AV_PROFILE_DTS_HD_MA_X_IMAX ||
            profile == AV_PROFILE_DTS_EXPRESS)
            return -1;
        want = JELLY5_BS_DTS, mode = 2, s_format = Format::dts, s_name = "DTS";
    } else {
        return -1;
    }
    if (!(allowed & want))
        return -1;
    sceAudioOutInit();
    int sink = jelly5_bs_sink_formats();
    if (sink < 0)
        sink = JELLY5_BS_AC3;   /* an unreadable list: only what every HDMI sink with surround takes */
    if (!(sink & want)) {
        evo_bt("bitstream: %s not listed by the TV/receiver (formats %#x): decoded here", s_name, sink);
        return -1;
    }
    const int cfg = sceAudioOutExConfigureOutput(0, 0, mode, 255, 0);
    const int h = cfg >= 0 ? sceAudioOutExOpen(0xff, mode) : -1;
    if (h < 1) {
        if (cfg >= 0)
            sceAudioOutExConfigureOutput(0, 0, 255, 255, 0);
        evo_bt("bitstream: %s refused (configure %#x, open %#x): decoded here", s_name, (unsigned)cfg,
               (unsigned)h);
        return -1;
    }
    s_carrier = s_format == Format::eac3 ? 192000 : 48000;
    s_grain = s_format == Format::eac3 ? 1024 : 256;
    jelly5_bs_reset();
    s_packer.reset(s_format);
    s_packer_gen = s_gen.load();
    s_active = 1;
    evo_bt("bitstream: %s %dch to HDMI (mode %d, %u Hz carrier, handle %#x)", s_name, channels, mode, s_carrier,
           (unsigned)h);
    return h;
}

void jelly5_bs_close(int handle)
{
    if (!s_active.exchange(0))
        return;
    const int c = sceAudioOutExClose(handle);
    const int r = sceAudioOutExConfigureOutput(0, 0, 255, 255, 0);
    if (s_packer.dropped())
        evo_bt("bitstream: %llu bytes the packer could not place", (unsigned long long)s_packer.dropped());
    evo_bt("bitstream: closed (close %#x, restore %#x)", (unsigned)c, (unsigned)r);
    jelly5_bs_reset();
}

int jelly5_bs_active(void) { return s_active.load(); }
const char *jelly5_bs_name(void) { return s_active.load() ? s_name : ""; }

void jelly5_bs_feed(const uint8_t *data, int size, volatile int *running)
{
    if (!s_active.load() || !data || size <= 0)
        return;
    const unsigned gen = s_gen.load();
    if (s_packer_gen != gen) {   /* a seek: the next frame starts a fresh burst */
        s_packer.reset(s_format);
        s_packer_gen = gen;
    }
    s_packer.feed(data, (size_t)size, -1, [&](const uint16_t *words, size_t n, int64_t) {
        push(words, n, gen, running);
    });
}

const int16_t *jelly5_bs_pop(void)
{
    const size_t n = (size_t)s_grain * 2;
    std::lock_guard<std::mutex> g(s_lock);
    if (s_count < n)
        return nullptr;
    for (size_t i = 0; i < n; i++)
        s_out[i] = s_ring[(s_read + i) % kRing];
    s_read = (s_read + n) % kRing;
    s_count -= n;
    return s_out;
}

int jelly5_bs_grain_frames(void) { return s_grain; }
int jelly5_bs_media_samples(void) { return (int)((long long)s_grain * 48000 / s_carrier); }
const int16_t *jelly5_bs_silence(void) { return s_zero; }

void jelly5_bs_reset(void)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_read = s_write = s_count = 0;
    s_gen++;
}

} // extern "C"
