#pragma once
// Fast CinemaDNG (DNG 1.4 / TIFF-EP) writer tailored for Blackmagic DaVinci Resolve.
// Writes a complete DNG file into a caller-reused byte buffer in one pass, then
// flushes to disk with a single write call.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct DngParams
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits = 14;             // sensor bit depth (12 or 14)
    bool pack_bits = false;         // false = 16-bit uncompressed (fastest), true = packed 12/14-bit
    uint32_t black_level = 2048;
    uint32_t white_level = 15300;
    uint8_t cfa_pattern[4] = {0, 1, 1, 2}; // 0=R, 1=G, 2=B (RGGB)
    uint32_t fps_x1000 = 23976;
    uint32_t frame_index = 0;       // 0-based sequential frame index for SMPTE TimeCode
    // AsShotNeutral (1 / WB multiplier, normalized so G = 1.0)
    double wb_r = 2.04;             // Canon EOS M50 Daylight D65 defaults
    double wb_g = 1.00;
    double wb_b = 1.61;
    // Active area & crop (when --full is used; otherwise 0,0,h,w)
    uint32_t active_x1 = 0;
    uint32_t active_y1 = 0;
    uint32_t active_x2 = 0;
    uint32_t active_y2 = 0;
};

namespace dng_detail
{
    enum TiffType : uint16_t
    {
        TIFF_BYTE      = 1,
        TIFF_ASCII     = 2,
        TIFF_SHORT     = 3,
        TIFF_LONG      = 4,
        TIFF_RATIONAL  = 5,
        TIFF_SRATIONAL = 10
    };

    struct IfdEntry
    {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        uint32_t value_or_offset;
        std::vector<uint8_t> extra; // non-empty if payload > 4 bytes
    };

    inline void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
    inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
    inline void push32(std::vector<uint8_t> &v, uint32_t x)
    {
        size_t o = v.size();
        v.resize(o + 4);
        memcpy(v.data() + o, &x, 4);
    }
    inline void push32s(std::vector<uint8_t> &v, int32_t x)
    {
        push32(v, static_cast<uint32_t>(x));
    }

    inline uint8_t to_bcd(uint32_t val)
    {
        return static_cast<uint8_t>(((val / 10) << 4) | (val % 10));
    }

    inline void fps_to_rational(uint32_t fps_x1000, int32_t &num, int32_t &den, uint32_t &tc_fps)
    {
        if (fps_x1000 >= 23900 && fps_x1000 <= 24050)      { num = 24000; den = 1001; tc_fps = 24; }
        else if (fps_x1000 >= 24900 && fps_x1000 <= 25100) { num = 25000; den = 1000; tc_fps = 25; }
        else if (fps_x1000 >= 29900 && fps_x1000 <= 30050) { num = 30000; den = 1001; tc_fps = 30; }
        else if (fps_x1000 >= 49900 && fps_x1000 <= 50100) { num = 50000; den = 1000; tc_fps = 50; }
        else if (fps_x1000 >= 59900 && fps_x1000 <= 60050) { num = 60000; den = 1001; tc_fps = 60; }
        else if (fps_x1000 > 0)                            { num = (int32_t)fps_x1000; den = 1000; tc_fps = (fps_x1000 + 500) / 1000; }
        else                                               { num = 24000; den = 1001; tc_fps = 24; }
        if (tc_fps == 0) tc_fps = 24;
    }

    // Pack 12-bit samples MSB-first (TIFF/DNG big-endian bit order within bytes)
    inline void pack_12bit_dng(uint8_t *dst, const uint16_t *src, size_t count)
    {
        size_t i = 0;
        for (; i + 1 < count; i += 2)
        {
            uint32_t a = src[i] & 0xFFF;
            uint32_t b = src[i + 1] & 0xFFF;
            *dst++ = static_cast<uint8_t>(a >> 4);
            *dst++ = static_cast<uint8_t>(((a & 0xF) << 4) | (b >> 8));
            *dst++ = static_cast<uint8_t>(b & 0xFF);
        }
        if (i < count)
        {
            uint32_t a = src[i] & 0xFFF;
            *dst++ = static_cast<uint8_t>(a >> 4);
            *dst++ = static_cast<uint8_t>((a & 0xF) << 4);
        }
    }

    // Pack 14-bit samples MSB-first (4 pixels -> 7 bytes)
    inline void pack_14bit_dng(uint8_t *dst, const uint16_t *src, size_t count)
    {
        size_t i = 0;
        for (; i + 3 < count; i += 4)
        {
            uint32_t p0 = src[i]     & 0x3FFF;
            uint32_t p1 = src[i + 1] & 0x3FFF;
            uint32_t p2 = src[i + 2] & 0x3FFF;
            uint32_t p3 = src[i + 3] & 0x3FFF;
            *dst++ = static_cast<uint8_t>(p0 >> 6);
            *dst++ = static_cast<uint8_t>(((p0 & 0x3F) << 2) | (p1 >> 12));
            *dst++ = static_cast<uint8_t>((p1 >> 4) & 0xFF);
            *dst++ = static_cast<uint8_t>(((p1 & 0x0F) << 4) | (p2 >> 10));
            *dst++ = static_cast<uint8_t>((p2 >> 2) & 0xFF);
            *dst++ = static_cast<uint8_t>(((p2 & 0x03) << 6) | (p3 >> 8));
            *dst++ = static_cast<uint8_t>(p3 & 0xFF);
        }
        if (i < count)
        {
            uint64_t acc = 0;
            int bits = 0;
            for (; i < count; ++i)
            {
                acc = (acc << 14) | (src[i] & 0x3FFF);
                bits += 14;
                while (bits >= 8)
                {
                    bits -= 8;
                    *dst++ = static_cast<uint8_t>((acc >> bits) & 0xFF);
                }
            }
            if (bits > 0)
                *dst++ = static_cast<uint8_t>((acc << (8 - bits)) & 0xFF);
        }
    }
}

// Build complete CinemaDNG file in `out_buf` from a contiguous `width x height` uint16_t Bayer frame.
// Optionally returns the byte offset of the 8-byte SMPTE TimeCodes payload so callers can
// emit duplicate hold-frames for dropped slots with zero re-encoding.
inline void build_cinemadng(
    const uint16_t *bayer,
    const DngParams &p,
    std::vector<uint8_t> &out_buf,
    uint32_t *timecode_offset_out = nullptr)
{
    using namespace dng_detail;

    const uint16_t bps = (p.pack_bits && (p.bits == 12 || p.bits == 14))
                         ? static_cast<uint16_t>(p.bits)
                         : 16;
    const size_t npixels = static_cast<size_t>(p.width) * p.height;
    const uint32_t strip_bytes = (bps == 16)
        ? static_cast<uint32_t>(npixels * 2)
        : static_cast<uint32_t>((npixels * bps + 7) / 8);

    const uint32_t ax1 = p.active_x1;
    const uint32_t ay1 = p.active_y1;
    const uint32_t ax2 = p.active_x2 ? p.active_x2 : p.width;
    const uint32_t ay2 = p.active_y2 ? p.active_y2 : p.height;
    const uint32_t crop_w = (ax2 > ax1) ? (ax2 - ax1) : p.width;
    const uint32_t crop_h = (ay2 > ay1) ? (ay2 - ay1) : p.height;

    int32_t fps_num = 24000, fps_den = 1001;
    uint32_t tc_fps = 24;
    fps_to_rational(p.fps_x1000, fps_num, fps_den, tc_fps);

    // Build IFD entries in STRICTLY ASCENDING tag ID order
    std::vector<IfdEntry> ifd;
    ifd.reserve(32);

    auto add_short = [&](uint16_t tag, uint16_t val) {
        ifd.push_back({tag, TIFF_SHORT, 1, val, {}});
    };
    auto add_short2 = [&](uint16_t tag, uint16_t a, uint16_t b) {
        ifd.push_back({tag, TIFF_SHORT, 2, (uint32_t)a | ((uint32_t)b << 16), {}});
    };
    auto add_long = [&](uint16_t tag, uint32_t val) {
        ifd.push_back({tag, TIFF_LONG, 1, val, {}});
    };
    auto add_longs = [&](uint16_t tag, std::initializer_list<uint32_t> vals) {
        IfdEntry e{tag, TIFF_LONG, (uint32_t)vals.size(), 0, {}};
        if (vals.size() == 1) e.value_or_offset = *vals.begin();
        else for (uint32_t v : vals) push32(e.extra, v);
        ifd.push_back(std::move(e));
    };
    auto add_bytes4 = [&](uint16_t tag, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
        uint32_t packed = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
        ifd.push_back({tag, TIFF_BYTE, 4, packed, {}});
    };
    auto add_bytes = [&](uint16_t tag, const uint8_t *data, uint32_t n) {
        IfdEntry e{tag, TIFF_BYTE, n, 0, {}};
        if (n <= 4) { memcpy(&e.value_or_offset, data, n); }
        else { e.extra.assign(data, data + n); }
        ifd.push_back(std::move(e));
    };
    auto add_ascii = [&](uint16_t tag, const char *s) {
        uint32_t n = (uint32_t)strlen(s) + 1;
        IfdEntry e{tag, TIFF_ASCII, n, 0, {}};
        if (n <= 4) { memcpy(&e.value_or_offset, s, n); }
        else { e.extra.assign(reinterpret_cast<const uint8_t *>(s), reinterpret_cast<const uint8_t *>(s) + n); }
        ifd.push_back(std::move(e));
    };
    auto add_rationals = [&](uint16_t tag, std::initializer_list<std::pair<uint32_t, uint32_t>> vals) {
        IfdEntry e{tag, TIFF_RATIONAL, (uint32_t)vals.size(), 0, {}};
        for (auto &kv : vals) { push32(e.extra, kv.first); push32(e.extra, kv.second); }
        ifd.push_back(std::move(e));
    };
    auto add_srationals = [&](uint16_t tag, std::initializer_list<std::pair<int32_t, int32_t>> vals) {
        IfdEntry e{tag, TIFF_SRATIONAL, (uint32_t)vals.size(), 0, {}};
        for (auto &kv : vals) { push32s(e.extra, kv.first); push32s(e.extra, kv.second); }
        ifd.push_back(std::move(e));
    };

    // --- Core TIFF / TIFF-EP tags ---
    add_long(254, 0);                                    // NewSubfileType = full-res
    add_long(256, p.width);                              // ImageWidth
    add_long(257, p.height);                             // ImageLength
    add_short(258, bps);                                 // BitsPerSample
    add_short(259, 1);                                   // Compression = Uncompressed
    add_short(262, 32803);                               // PhotometricInterpretation = CFA
    add_ascii(271, "Canon");                             // Make
    add_ascii(272, "Canon EOS M50");                     // Model
    size_t strip_off_idx = ifd.size();
    add_long(273, 0);                                    // StripOffsets (patched below)
    add_short(274, 1);                                   // Orientation = TopLeft
    add_short(277, 1);                                   // SamplesPerPixel
    add_long(278, p.height);                             // RowsPerStrip
    add_long(279, strip_bytes);                          // StripByteCounts
    add_short(284, 1);                                   // PlanarConfiguration = Chunky
    add_ascii(305, "crx2dng (Magic Lantern EOS M50)");   // Software
    add_short2(33421, 2, 2);                             // CFARepeatPatternDim
    add_bytes4(33422, p.cfa_pattern[0], p.cfa_pattern[1], p.cfa_pattern[2], p.cfa_pattern[3]); // CFAPattern

    // --- DNG 1.4 tags ---
    add_bytes4(50706, 1, 4, 0, 0);                       // DNGVersion = 1.4.0.0
    add_bytes4(50707, 1, 1, 0, 0);                       // DNGBackwardVersion = 1.1.0.0
    add_ascii(50708, "Canon EOS M50");                   // UniqueCameraModel
    add_long(50714, p.black_level);                      // BlackLevel
    add_long(50717, p.white_level);                      // WhiteLevel
    add_rationals(50718, {{1, 1}, {1, 1}});              // DefaultScale
    add_longs(50719, {ax1, ay1});                        // DefaultCropOrigin
    add_longs(50720, {crop_w, crop_h});                  // DefaultCropSize

    // Canon EOS M50 official Adobe / LibRaw D65 ColorMatrix1 (XYZ -> Camera, /10000)
    add_srationals(50721, {
        { 8532, 10000}, { -701, 10000}, {-1167, 10000},
        {-4095, 10000}, {11879, 10000}, { 2508, 10000},
        { -797, 10000}, { 2424, 10000}, { 7010, 10000}
    });

    // AsShotNeutral (1/R_mult, 1/G_mult, 1/B_mult)
    uint32_t nr = static_cast<uint32_t>(10000.0 / (p.wb_r > 0.01 ? p.wb_r : 2.04) + 0.5);
    uint32_t ng = static_cast<uint32_t>(10000.0 / (p.wb_g > 0.01 ? p.wb_g : 1.00) + 0.5);
    uint32_t nb = static_cast<uint32_t>(10000.0 / (p.wb_b > 0.01 ? p.wb_b : 1.61) + 0.5);
    add_rationals(50728, {{nr, 10000}, {ng, 10000}, {nb, 10000}}); // AsShotNeutral
    add_srationals(50730, {{0, 100}});                             // BaselineExposure
    add_short(50778, 21);                                          // CalibrationIlluminant1 = D65
    add_longs(50829, {ay1, ax1, ay2, ax2});                        // ActiveArea (top, left, bottom, right)

    // --- CinemaDNG tags (Resolve native video sequence recognition) ---
    uint32_t f = p.frame_index;
    uint32_t tc_f = f % tc_fps;
    uint32_t tot_s = f / tc_fps;
    uint32_t tc_s = tot_s % 60;
    uint32_t tc_m = (tot_s / 60) % 60;
    uint32_t tc_h = (tot_s / 3600) % 24;
    uint8_t tc[8] = { to_bcd(tc_f), to_bcd(tc_s), to_bcd(tc_m), to_bcd(tc_h), 0, 0, 0, 0 };
    size_t tc_idx = ifd.size();
    add_bytes(51043, tc, 8);                                       // TimeCodes (SMPTE BCD)
    add_srationals(51044, {{fps_num, fps_den}});                   // FrameRate
    add_ascii(51081, "Canon EOS M50");                             // ProfileName

    // Compute layout: 8-byte TIFF header + IFD + extra tag data, aligned to 4096 bytes
    const uint32_t ifd_offset = 8;
    const uint32_t ifd_size = 2 + static_cast<uint32_t>(ifd.size()) * 12 + 4;
    uint32_t extra_cursor = ifd_offset + ifd_size;
    for (auto &e : ifd)
    {
        if (!e.extra.empty())
        {
            if (extra_cursor & 3) extra_cursor = (extra_cursor + 3) & ~3u;
            e.value_or_offset = extra_cursor;
            extra_cursor += static_cast<uint32_t>(e.extra.size());
        }
    }
    const uint32_t pixel_offset = (extra_cursor + 4095u) & ~4095u;
    ifd[strip_off_idx].value_or_offset = pixel_offset;
    if (timecode_offset_out) *timecode_offset_out = ifd[tc_idx].value_or_offset;

    const size_t total_size = static_cast<size_t>(pixel_offset) + strip_bytes;
    out_buf.resize(total_size);
    memset(out_buf.data(), 0, pixel_offset);

    // TIFF little-endian header
    uint8_t *base = out_buf.data();
    base[0] = 'I'; base[1] = 'I';
    put16(base + 2, 42);
    put32(base + 4, ifd_offset);

    // Write IFD
    uint8_t *cur = base + ifd_offset;
    put16(cur, static_cast<uint16_t>(ifd.size()));
    cur += 2;
    for (const auto &e : ifd)
    {
        put16(cur + 0, e.tag);
        put16(cur + 2, e.type);
        put32(cur + 4, e.count);
        put32(cur + 8, e.value_or_offset);
        cur += 12;
        if (!e.extra.empty())
            memcpy(base + e.value_or_offset, e.extra.data(), e.extra.size());
    }
    put32(cur, 0); // next IFD = 0

    // Write pixel payload
    uint8_t *pix_dst = base + pixel_offset;
    if (bps == 16)
    {
        memcpy(pix_dst, bayer, strip_bytes);
    }
    else if (bps == 12)
    {
        pack_12bit_dng(pix_dst, bayer, npixels);
    }
    else
    {
        pack_14bit_dng(pix_dst, bayer, npixels);
    }
}

// Fast in-place SMPTE TimeCode patch for duplicate hold-frames (zero re-encoding).
inline void patch_dng_timecode(std::vector<uint8_t> &dng_buf, uint32_t tc_offset,
                               uint32_t frame_index, uint32_t fps_x1000)
{
    using namespace dng_detail;
    if (tc_offset == 0 || static_cast<size_t>(tc_offset) + 8 > dng_buf.size()) return;
    int32_t num = 0, den = 0;
    uint32_t tc_fps = 24;
    fps_to_rational(fps_x1000, num, den, tc_fps);
    uint32_t tc_f = frame_index % tc_fps;
    uint32_t tot_s = frame_index / tc_fps;
    uint32_t tc_s = tot_s % 60;
    uint32_t tc_m = (tot_s / 60) % 60;
    uint32_t tc_h = (tot_s / 3600) % 24;
    uint8_t *tc = dng_buf.data() + tc_offset;
    tc[0] = to_bcd(tc_f);
    tc[1] = to_bcd(tc_s);
    tc[2] = to_bcd(tc_m);
    tc[3] = to_bcd(tc_h);
}
