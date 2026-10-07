/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * IEC 61937 packing for HDMI bitstream audio, adapted from SlopFin
 * (src/iec61937.hpp and ac3_frame_bytes from src/ac3.hpp):
 */
/*
 * SlopFin - IEC 61937 packing for HDMI bitstream audio.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The TV decodes AC-3, E-AC-3 and DTS itself when they arrive wrapped the way
 * S/PDIF and HDMI carry compressed audio: bursts that look like stereo 16-bit
 * PCM, each opening with the preamble F872 4E1F, a data type and a length,
 * followed by the frame with every pair of bytes swapped and zero padding to
 * a fixed repetition period. The layout follows IEC 61937 as FFmpeg's spdif
 * muxer writes it (libavformat/spdifenc.c, LGPL-2.1-or-later), which is also
 * what produced the test streams the console played on 2026-09-15. Output
 * words are native 16-bit values, so the bytes land little-endian exactly as
 * the muxer writes them.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jelly5::iec61937
{

/* Bytes in one AC-3 syncframe at 48 kHz (bsid <= 8), 0 when not one. From SlopFin src/ac3.hpp. */
inline std::size_t ac3_frame_bytes(const std::uint8_t *data, std::size_t bytes) noexcept
{
    if (bytes < 7 || data[0] != 0x0b || data[1] != 0x77 || (data[4] >> 6) != 0 ||
        (data[5] >> 3) > 8)
        return 0;
    constexpr unsigned bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                     192, 224, 256, 320, 384, 448, 512, 576, 640};
    const unsigned code = data[4] & 63;
    return code < 38 ? bitrates[code / 2] * 4 : 0;
}

enum class Format : std::uint8_t
{
    ac3,  /* 48 kHz stereo carrier, bursts of 1536 frames */
    eac3, /* 192 kHz stereo carrier, bursts of 6144 frames */
    dts,  /* 48 kHz stereo carrier, core only, bursts of 512..2048 frames */
};

inline constexpr std::uint16_t kSync1 = 0xf872;
inline constexpr std::uint16_t kSync2 = 0x4e1f;
inline constexpr std::uint16_t kTypeAc3 = 0x01;
inline constexpr std::uint16_t kTypeDts1 = 0x0b;
inline constexpr std::uint16_t kTypeEac3 = 0x15;
inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kMaxBurstBytes = 24576;

/* Bytes in one E-AC-3 (or AC-3, bsid <= 8) syncframe, 0 when not one. */
inline std::size_t eac3_frame_bytes(const std::uint8_t *data, std::size_t bytes) noexcept
{
    if (bytes < 6 || data[0] != 0x0b || data[1] != 0x77)
        return 0;
    const unsigned bsid = data[5] >> 3;
    if (bsid <= 8)
        return ac3_frame_bytes(data, bytes);
    if (bsid < 11 || bsid > 16)
        return 0;
    return ((((static_cast<std::size_t>(data[2]) & 7) << 8) | data[3]) + 1) * 2;
}

/* A dependent substream belongs to the independent frame before it. */
inline bool eac3_dependent(const std::uint8_t *data) noexcept
{
    return (data[5] >> 3) > 10 && (data[2] >> 6) == 1;
}

/* How many syncframes make the six audio blocks one burst carries. */
inline unsigned eac3_repeat(const std::uint8_t *data) noexcept
{
    static constexpr unsigned kRepeat[4] = {6, 3, 2, 1};
    if ((data[5] >> 3) <= 10 || (data[4] & 0xc0) == 0xc0)
        return 1;
    return kRepeat[(data[4] >> 4) & 3];
}

struct DtsCore
{
    std::size_t bytes = 0;     /* the core frame alone */
    std::uint32_t samples = 0; /* 512, 1024 or 2048 */
};

/* A 48 kHz big-endian DTS core frame header, which is what TS carries. */
inline DtsCore dts_core(const std::uint8_t *data, std::size_t bytes) noexcept
{
    if (bytes < 9 || data[0] != 0x7f || data[1] != 0xfe || data[2] != 0x80 || data[3] != 0x01)
        return {};
    const unsigned blocks = ((((unsigned{data[4]} << 8) | data[5]) >> 2) & 0x7f) + 1;
    const std::size_t size =
        ((((std::size_t{data[5]} << 16) | (std::size_t{data[6]} << 8) | data[7]) >> 4) & 0x3fff) +
        1;
    const unsigned rate_code = (data[8] >> 2) & 0x0f;
    const std::uint32_t samples = blocks << 5;
    if (rate_code != 13 || size < 96 || (samples != 512 && samples != 1024 && samples != 2048))
        return {};
    return {size, samples};
}

/* Bytes of the DTS-HD extension substream at `data`, 0 when there is none. */
inline std::size_t dts_substream_bytes(const std::uint8_t *data, std::size_t bytes) noexcept
{
    if (bytes < 11 || data[0] != 0x64 || data[1] != 0x58 || data[2] != 0x20 || data[3] != 0x25)
        return 0;
    /* After the sync: 8 user bits, 2 index bits, 1 size-type bit, then the
       header size and the frame size, 8+16 bits or 12+20 bits wide. */
    std::uint64_t bits = 0;
    for (int i = 4; i < 11; ++i)
        bits = (bits << 8) | data[i];
    const bool wide = (bits >> (56 - 11)) & 1;
    const unsigned header_bits = wide ? 12 : 8, frame_bits = wide ? 20 : 16;
    const std::size_t frame =
        ((bits >> (56 - 11 - header_bits - frame_bits)) & ((1u << frame_bits) - 1)) + 1;
    return frame;
}

/*
 * Assembles frames arriving in arbitrary pieces into complete bursts. The
 * consumer receives `words` native 16-bit values (burst bytes / 2) and the
 * 90 kHz timestamp of the burst's first frame.
 */
class Packer
{
  public:
    void reset(Format format) noexcept
    {
        format_ = format;
        used_ = 0;
        pending_ = 0;
        pending_frames_ = 0;
        pending_repeat_ = 1;
        pending_pts_ = -1;
        frame_pts_ = -1;
        skip_ = 0;
        dropped_ = 0;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept
    {
        return dropped_;
    }

    /* Frames per second of the stereo carrier the bursts are paced at. */
    [[nodiscard]] std::uint32_t carrier_rate() const noexcept
    {
        return format_ == Format::eac3 ? 192000 : 48000;
    }

    template <typename Consumer>
    void feed(const std::uint8_t *data, std::size_t bytes, std::int64_t pts, Consumer consume)
    {
        while (bytes > 0)
        {
            if (skip_ > 0)
            {
                const std::size_t take = bytes < skip_ ? bytes : skip_;
                skip_ -= take;
                data += take;
                bytes -= take;
                continue;
            }
            if (used_ == 0)
                frame_pts_ = pts;
            /* Take only what the frame being assembled still needs, so the
               rest of the packet is scanned afresh. */
            const std::size_t want = expected_ ? expected_ - used_ : kProbe - used_;
            const std::size_t step = bytes < want ? bytes : want;
            std::memcpy(frame_.data() + used_, data, step);
            used_ += step;
            data += step;
            bytes -= step;
            pts = -1;
            advance(consume);
        }
    }

    /* Emits whatever E-AC-3 is still gathered, at end of stream. */
    template <typename Consumer> void flush(Consumer consume)
    {
        if (format_ == Format::eac3 && pending_ > 0)
            emit(consume, kTypeEac3, pending_data_.data(), pending_, 24576, pending_pts_, false);
        pending_ = 0;
        pending_frames_ = 0;
    }

  private:
    static constexpr std::size_t kProbe = 12;

    template <typename Consumer> void advance(Consumer consume)
    {
        while (used_ >= kProbe || (expected_ && used_ >= expected_))
        {
            if (!expected_)
            {
                bool extension = false;
                expected_ = measure(extension);
                if (!expected_ || expected_ < used_)
                {
                    expected_ = 0;
                    std::memmove(frame_.data(), frame_.data() + 1, --used_);
                    ++dropped_;
                    frame_pts_ = -1;
                    continue;
                }
                if (extension)
                {
                    /* A DTS-HD extension follows the core in the same access
                       unit. The TV is sent the core, so it is stepped over. */
                    skip_ = expected_ - used_;
                    used_ = expected_ = 0;
                    return;
                }
                if (expected_ > frame_.size())
                {
                    expected_ = 0;
                    std::memmove(frame_.data(), frame_.data() + 1, --used_);
                    ++dropped_;
                    continue;
                }
            }
            if (used_ < expected_)
                return;
            complete(consume);
            std::memmove(frame_.data(), frame_.data() + expected_, used_ - expected_);
            used_ -= expected_;
            expected_ = 0;
            frame_pts_ = -1;
        }
    }

    [[nodiscard]] std::size_t measure(bool &extension) const noexcept
    {
        if (format_ == Format::dts)
        {
            if (const std::size_t hd = dts_substream_bytes(frame_.data(), used_); hd != 0)
            {
                extension = true;
                return hd;
            }
            return dts_core(frame_.data(), used_).bytes;
        }
        if (format_ == Format::ac3)
            return ac3_frame_bytes(frame_.data(), used_);
        return eac3_frame_bytes(frame_.data(), used_);
    }

    template <typename Consumer> void complete(Consumer consume)
    {
        const std::uint8_t *frame = frame_.data();
        if (format_ == Format::ac3)
        {
            emit(consume, static_cast<std::uint16_t>(kTypeAc3 | ((frame[5] & 7) << 8)), frame,
                 expected_, 1536 * 4, frame_pts_, true);
            return;
        }
        if (format_ == Format::dts)
        {
            const DtsCore core = dts_core(frame, expected_);
            const auto type = static_cast<std::uint16_t>(kTypeDts1 + (core.samples == 1024   ? 1
                                                                      : core.samples == 2048 ? 2
                                                                                             : 0));
            emit(consume, type, frame, core.bytes, core.samples * 4, frame_pts_, true);
            return;
        }
        /* E-AC-3: a burst holds six audio blocks' worth of independent
           frames, each followed by any dependent substreams. It closes when
           the next independent frame would start a seventh block. */
        if (!eac3_dependent(frame))
        {
            if (pending_frames_ >= pending_repeat_ && pending_ > 0)
            {
                emit(consume, kTypeEac3, pending_data_.data(), pending_, 24576, pending_pts_,
                     false);
                pending_ = 0;
                pending_frames_ = 0;
            }
            if (pending_frames_ == 0)
            {
                pending_repeat_ = eac3_repeat(frame);
                pending_pts_ = frame_pts_;
            }
            ++pending_frames_;
        }
        else if (pending_frames_ == 0)
        {
            ++dropped_; /* a dependent frame with nothing to belong to */
            return;
        }
        if (pending_ + expected_ > pending_data_.size() - kHeaderBytes)
        {
            ++dropped_;
            pending_ = 0;
            pending_frames_ = 0;
            return;
        }
        std::memcpy(pending_data_.data() + pending_, frame, expected_);
        pending_ += expected_;
    }

    template <typename Consumer>
    void emit(Consumer consume, std::uint16_t type, const std::uint8_t *payload, std::size_t bytes,
              std::size_t period, std::int64_t pts, bool length_in_bits)
    {
        const std::size_t aligned = (bytes + 1) & ~std::size_t{1};
        if (period > kMaxBurstBytes || aligned + kHeaderBytes > period)
        {
            ++dropped_;
            return;
        }
        std::uint16_t *out = burst_.data();
        out[0] = kSync1;
        out[1] = kSync2;
        out[2] = type;
        out[3] = static_cast<std::uint16_t>(length_in_bits ? aligned << 3 : bytes);
        std::size_t word = 4;
        for (std::size_t i = 0; i + 1 < bytes; i += 2)
            out[word++] = static_cast<std::uint16_t>((payload[i] << 8) | payload[i + 1]);
        if (bytes & 1)
            out[word++] = static_cast<std::uint16_t>(payload[bytes - 1] << 8);
        for (; word < period / 2; ++word)
            out[word] = 0;
        consume(burst_.data(), period / 2, pts);
    }

  private:
    Format format_ = Format::ac3;
    std::array<std::uint8_t, 16384> frame_{};
    std::size_t used_ = 0, expected_ = 0, skip_ = 0;
    std::array<std::uint8_t, kMaxBurstBytes> pending_data_{};
    std::size_t pending_ = 0;
    unsigned pending_frames_ = 0, pending_repeat_ = 1;
    std::int64_t pending_pts_ = -1, frame_pts_ = -1;
    std::array<std::uint16_t, kMaxBurstBytes / 2> burst_{};
    std::uint64_t dropped_ = 0;
};

} // namespace jelly5::iec61937

