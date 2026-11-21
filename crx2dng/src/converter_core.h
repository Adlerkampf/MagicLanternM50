#pragma once
// Shared CRX -> CinemaDNG inspection & multi-threaded conversion engine
// Used by both crx2dng.exe (CLI) and crx2dng_gui.exe (Win32 GUI).

#define _CRT_SECURE_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crx_standalone_shim.h"
#include "dng_writer.h"

namespace crx_engine
{
namespace fs = std::filesystem;

static constexpr uint32_t MAGIC_CRXV = 0x56585243u; // "CRXV"
static constexpr uint32_t MAGIC_CRX3 = 0x33585243u; // "CRX3"
static constexpr uint32_t MAGIC_CRX2 = 0x32585243u; // "CRX2"

struct CrxvHeader
{
    uint32_t version = 0;
    uint32_t sensor_w = 0, sensor_h = 0, pitch = 0, bits = 14;
    uint32_t mode = 0;          // 0 = lossless RAW, 4 = C-RAW
    uint32_t black_level = 0;
    uint32_t white_level = 0;
    uint32_t active_x1 = 0, active_y1 = 0, active_x2 = 0, active_y2 = 0;
    uint32_t fps_x1000 = 23976;
    uint32_t cfa_pattern = 0x02010100u;
    uint32_t chunk = 0;
    uint32_t enc_w = 0, enc_h = 0, enc_off = 0, enc_fw = 0;
};

struct FrameEntry
{
    uint32_t chunk_idx = 0;
    uint64_t offset = 0;
    uint32_t block_size = 0;
    uint32_t magic = 0;
    uint32_t frame_no = 0;
    uint32_t ts_us = 0;
    uint32_t cmp1_len = 0;
    uint32_t num_pieces = 0;
    uint32_t hdr_size = 0;
    uint32_t out_start = 0;     // 0-based timeline index when fill_drops is enabled
    uint32_t repeat_count = 1;  // 1 = normal, >1 = hold frame across dropped slot(s)
};

struct ClipInfo
{
    fs::path first_file;
    std::vector<fs::path> chunks;
    CrxvHeader hdr{};
    std::vector<FrameEntry> frames;
    uint64_t total_in_bytes = 0;
    uint32_t crop_x0 = 0, crop_y0 = 0, crop_w = 0, crop_h = 0;
    size_t dropped_frames = 0;
    size_t timeline_frames = 0;
};

struct ConvertOptions
{
    fs::path output_dir;
    int threads = 0;
    int start_frame = 0;
    int max_frames = -1;
    bool pack_bits = false;
    bool full_frame = false;
    bool fill_drops = true;
    double wb_r = 2.04, wb_g = 1.00, wb_b = 1.61;
};

struct ProgressInfo
{
    size_t done = 0;
    size_t total = 0;
    size_t errors = 0;
    double fps = 0.0;
    double mb_per_sec = 0.0;
    uint64_t bytes_written = 0;
    double elapsed_sec = 0.0;
};

inline bool read_at(HANDLE h, uint64_t off, void *dst, uint32_t len)
{
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, dst, len, &got, &ov))
        return false;
    return got == len;
}

inline bool write_whole_file(const std::wstring &path, const void *data, size_t len)
{
    HANDLE h = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t left = len;
    bool ok = true;
    while (left > 0)
    {
        DWORD chunk = (left > 0x40000000u) ? 0x40000000u : static_cast<DWORD>(left);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk)
        {
            ok = false;
            break;
        }
        p += chunk;
        left -= chunk;
    }
    CloseHandle(h);
    return ok;
}

inline bool parse_crxv_header(const uint32_t h[32], CrxvHeader &out)
{
    if (h[0] != MAGIC_CRXV) return false;
    out.version     = h[1];
    out.sensor_w    = h[2];
    out.sensor_h    = h[3];
    out.pitch       = h[4];
    out.bits        = h[5] ? h[5] : 14;
    out.mode        = h[6];
    out.black_level = h[7];
    out.white_level = h[8];
    out.active_x1   = h[9];
    out.active_y1   = h[10];
    out.active_x2   = h[11];
    out.active_y2   = h[12];
    out.fps_x1000   = h[13] ? h[13] : 23976;
    out.cfa_pattern = h[14];
    out.chunk       = h[15];
    out.enc_w       = h[18] ? h[18] : h[2];
    out.enc_h       = h[19] ? h[19] : h[3];
    out.enc_off     = h[20];
    out.enc_fw      = h[21];
    return true;
}

inline void compute_crop(const CrxvHeader &hdr, bool full_frame,
                         uint32_t &crop_x0, uint32_t &crop_y0,
                         uint32_t &crop_w, uint32_t &crop_h)
{
    uint32_t enc_w = hdr.enc_w;
    uint32_t enc_h = hdr.enc_h;
    crop_x0 = 0; crop_y0 = 0; crop_w = enc_w; crop_h = enc_h;
    if (!full_frame)
    {
        uint32_t ax1 = (hdr.active_x1 < enc_w) ? hdr.active_x1 : 0;
        uint32_t ay1 = (hdr.active_y1 < enc_h) ? hdr.active_y1 : 0;
        uint32_t ax2 = (hdr.active_x2 > ax1 && hdr.active_x2 <= enc_w) ? hdr.active_x2 : enc_w;
        uint32_t ay2 = (hdr.active_y2 > ay1 && hdr.active_y2 <= enc_h) ? hdr.active_y2 : enc_h;
        uint32_t aw = ax2 - ax1;
        uint32_t ah = ay2 - ay1;
        if (hdr.enc_fw > 0 && hdr.enc_fw < aw)
        {
            ax1 += ((aw - hdr.enc_fw) / 2) & ~1u;
            aw = hdr.enc_fw & ~1u;
        }
        crop_x0 = ax1 & ~1u;
        crop_y0 = ay1 & ~1u;
        crop_w  = aw & ~1u;
        crop_h  = ah & ~1u;
        if (crop_x0 + crop_w > enc_w) crop_w = (enc_w - crop_x0) & ~1u;
        if (crop_y0 + crop_h > enc_h) crop_h = (enc_h - crop_y0) & ~1u;
    }
}

inline bool inspect_crx_clip(const fs::path &first_file, ClipInfo &info, std::string &err_out)
{
    info = ClipInfo{};
    info.first_file = first_file;
    info.chunks.push_back(first_file);

    fs::path dir = first_file.parent_path();
    std::wstring stem = first_file.stem().wstring();
    for (int i = 1; i <= 99; ++i)
    {
        wchar_t ext[8];
        swprintf(ext, 8, L".C%02d", i);
        fs::path cand = dir / (stem + ext);
        if (fs::exists(cand)) info.chunks.push_back(cand);
        else break;
    }

    for (uint32_t ci = 0; ci < info.chunks.size(); ++ci)
    {
        HANDLE h = CreateFileW(
            info.chunks[ci].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            err_out = "Cannot open file: " + info.chunks[ci].string();
            return false;
        }
        LARGE_INTEGER fsz{};
        GetFileSizeEx(h, &fsz);
        uint64_t file_size = static_cast<uint64_t>(fsz.QuadPart);
        info.total_in_bytes += file_size;

        uint32_t raw_h[32] = {};
        if (file_size < 128 || !read_at(h, 0, raw_h, 128))
        {
            CloseHandle(h);
            err_out = "Truncated header in " + info.chunks[ci].string();
            return false;
        }
        CrxvHeader ch{};
        if (!parse_crxv_header(raw_h, ch))
        {
            CloseHandle(h);
            err_out = "Not a valid CRXV file: " + info.chunks[ci].string();
            return false;
        }
        if (ci == 0) info.hdr = ch;

        uint64_t pos = raw_h[17] ? raw_h[17] : 128;
        while (pos + 64 <= file_size)
        {
            uint32_t fh[16] = {};
            if (!read_at(h, pos, fh, 64)) break;
            if (fh[0] != MAGIC_CRX3 && fh[0] != MAGIC_CRX2) break;
            uint32_t blk_sz = fh[1];
            if (blk_sz < 64 || pos + blk_sz > file_size) break;

            FrameEntry fe{};
            fe.chunk_idx  = ci;
            fe.offset     = pos;
            fe.block_size = blk_sz;
            fe.magic      = fh[0];
            fe.frame_no   = fh[2];
            fe.ts_us      = fh[3];
            fe.cmp1_len   = fh[4];
            fe.num_pieces = fh[5];
            fe.hdr_size   = (fh[0] == MAGIC_CRX3) ? fh[6] : 48;
            info.frames.push_back(fe);
            pos += blk_sz;
        }
        CloseHandle(h);
    }

    if (info.frames.empty())
    {
        err_out = "No valid CRX frames found.";
        return false;
    }

    // Detect dropped frames from hardware microsecond timestamps (ts_us)
    double fps = info.hdr.fps_x1000 > 0 ? (info.hdr.fps_x1000 / 1000.0) : 23.976;
    double period_us = 1000000.0 / fps;
    uint32_t timeline_cursor = 0;
    info.dropped_frames = 0;
    for (size_t i = 0; i < info.frames.size(); ++i)
    {
        info.frames[i].out_start = timeline_cursor;
        info.frames[i].repeat_count = 1;
        if (i + 1 < info.frames.size())
        {
            uint32_t dt = info.frames[i + 1].ts_us - info.frames[i].ts_us;
            if (dt > static_cast<uint32_t>(period_us * 1.5) &&
                dt < static_cast<uint32_t>(period_us * 120.0))
            {
                uint32_t steps = static_cast<uint32_t>(dt / period_us + 0.5);
                if (steps > 1)
                {
                    info.frames[i].repeat_count = steps;
                    info.dropped_frames += (steps - 1);
                }
            }
        }
        timeline_cursor += info.frames[i].repeat_count;
    }
    info.timeline_frames = timeline_cursor;

    compute_crop(info.hdr, false, info.crop_x0, info.crop_y0, info.crop_w, info.crop_h);
    return true;
}

inline bool convert_crx_clip(
    const ClipInfo &clip,
    const ConvertOptions &opt,
    std::atomic<bool> &cancel_flag,
    std::function<void(const ProgressInfo &)> on_progress,
    ProgressInfo &final_stats,
    std::string &err_out)
{
    auto t_start = std::chrono::steady_clock::now();
    size_t first_idx = static_cast<size_t>(std::max(0, opt.start_frame));
    if (first_idx >= clip.frames.size())
    {
        err_out = "Start frame exceeds total frames.";
        return false;
    }
    size_t count = clip.frames.size() - first_idx;
    if (opt.max_frames >= 0 && static_cast<size_t>(opt.max_frames) < count)
        count = static_cast<size_t>(opt.max_frames);

    std::error_code ec;
    fs::create_directories(opt.output_dir, ec);
    if (ec)
    {
        err_out = "Cannot create output folder: " + ec.message();
        return false;
    }

    uint32_t crop_x0 = 0, crop_y0 = 0, crop_w = 0, crop_h = 0;
    compute_crop(clip.hdr, opt.full_frame, crop_x0, crop_y0, crop_w, crop_h);

    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads <= 0) hw_threads = 8;
    int num_threads = (opt.threads > 0) ? opt.threads : hw_threads;
    num_threads = std::min<int>(num_threads, static_cast<int>(count));
    if (num_threads < 1) num_threads = 1;

    uint8_t base_cfa[4] = {0, 1, 1, 2};
    if (clip.hdr.cfa_pattern)
    {
        uint8_t c0 = (clip.hdr.cfa_pattern >> 0) & 0xFF;
        uint8_t c1 = (clip.hdr.cfa_pattern >> 8) & 0xFF;
        uint8_t c2 = (clip.hdr.cfa_pattern >> 16) & 0xFF;
        uint8_t c3 = (clip.hdr.cfa_pattern >> 24) & 0xFF;
        if (c0 <= 2 && c1 <= 2 && c2 <= 2 && c3 <= 2)
        {
            base_cfa[0] = c0; base_cfa[1] = c1; base_cfa[2] = c2; base_cfa[3] = c3;
        }
    }

    std::string clip_stem = clip.first_file.stem().string();

    struct QueuedFrame
    {
        size_t job = 0;
        FrameEntry fe{};
        std::vector<uint8_t> blk_buf;
    };

    const size_t max_queue = static_cast<size_t>(std::max(32, num_threads * 2));
    std::mutex q_mtx;
    std::condition_variable q_not_empty;
    std::condition_variable q_not_full;
    std::deque<QueuedFrame> queue;
    bool reader_done = false;

    std::atomic<size_t> done_count{0};
    std::atomic<size_t> err_count{0};
    std::atomic<uint64_t> bytes_written{0};
    std::mutex err_mtx;
    std::string last_err;

    std::thread reader([&]() {
        uint32_t open_chunk = UINT32_MAX;
        HANDLE h = INVALID_HANDLE_VALUE;
        for (size_t job = 0; job < count && !cancel_flag.load(); ++job)
        {
            const FrameEntry &fe = clip.frames[first_idx + job];
            if (fe.chunk_idx != open_chunk)
            {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                h = CreateFileW(
                    clip.chunks[fe.chunk_idx].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                open_chunk = fe.chunk_idx;
            }
            if (h == INVALID_HANDLE_VALUE)
            {
                err_count++;
                continue;
            }
            QueuedFrame qf;
            qf.job = job;
            qf.fe = fe;
            qf.blk_buf.resize(fe.block_size);
            if (!read_at(h, fe.offset, qf.blk_buf.data(), fe.block_size))
            {
                err_count++;
                continue;
            }
            std::unique_lock<std::mutex> lk(q_mtx);
            q_not_full.wait(lk, [&]() { return queue.size() < max_queue || cancel_flag.load(); });
            if (cancel_flag.load()) break;
            queue.push_back(std::move(qf));
            lk.unlock();
            q_not_empty.notify_one();
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        {
            std::lock_guard<std::mutex> lk(q_mtx);
            reader_done = true;
        }
        q_not_empty.notify_all();
    });

    auto worker = [&]() {
        std::vector<uint8_t> mdat_buf;
        std::vector<uint16_t> raw_buf;
        std::vector<uint16_t> crop_buf;
        std::vector<uint8_t> dng_buf;

        while (true)
        {
            QueuedFrame qf;
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_not_empty.wait(lk, [&]() { return !queue.empty() || reader_done || cancel_flag.load(); });
                if (cancel_flag.load() || queue.empty()) break;
                qf = std::move(queue.front());
                queue.pop_front();
            }
            q_not_full.notify_one();

            const size_t job = qf.job;
            const FrameEntry &fe = qf.fe;
            const std::vector<uint8_t> &blk_buf = qf.blk_buf;

            const uint32_t *fh = reinterpret_cast<const uint32_t *>(blk_buf.data());
            uint32_t cmp1_len = fh[4];
            uint32_t n_pieces = fh[5];
            uint32_t hdr_sz   = (fe.magic == MAGIC_CRX3) ? fh[6] : 48;

            if (hdr_sz + ((cmp1_len + 31u) & ~31u) > fe.block_size)
            {
                err_count++;
                continue;
            }

            const uint8_t *cmp1_ptr = blk_buf.data() + hdr_sz;
            uint32_t off = hdr_sz + ((cmp1_len + 31u) & ~31u);

            if (fe.magic == MAGIC_CRX3)
            {
                size_t total_mdat = 0;
                for (uint32_t i = 0; i < n_pieces; ++i) total_mdat += fh[16 + i];
                mdat_buf.resize(total_mdat);
                uint8_t *dst = mdat_buf.data();
                bool ok_pieces = true;
                for (uint32_t i = 0; i < n_pieces; ++i)
                {
                    uint32_t plen = fh[16 + i];
                    if (plen == 0) continue;
                    uint32_t padded = (plen + 31u) & ~31u;
                    if (off + plen > fe.block_size) { ok_pieces = false; break; }
                    memcpy(dst, blk_buf.data() + off, plen);
                    dst += plen;
                    off += padded;
                }
                if (!ok_pieces) { err_count++; continue; }
            }
            else
            {
                size_t total_mdat = (fe.block_size > off) ? (fe.block_size - off) : 0;
                mdat_buf.assign(blk_buf.data() + off, blk_buf.data() + off + total_mdat);
            }

            CrxDecodedMeta meta{};
            std::string dec_err;
            if (!decode_crx_frame(cmp1_ptr, cmp1_len, mdat_buf.data(), mdat_buf.size(), raw_buf, meta, dec_err))
            {
                std::lock_guard<std::mutex> lk(err_mtx);
                last_err = dec_err;
                err_count++;
                continue;
            }

            const uint16_t *out_pixels = raw_buf.data();
            uint32_t out_w = static_cast<uint32_t>(meta.width);
            uint32_t out_h = static_cast<uint32_t>(meta.height);

            if (!opt.full_frame && (crop_x0 != 0 || crop_y0 != 0 || crop_w != out_w || crop_h != out_h)
                && crop_x0 + crop_w <= out_w && crop_y0 + crop_h <= out_h)
            {
                crop_buf.resize(static_cast<size_t>(crop_w) * crop_h);
                for (uint32_t y = 0; y < crop_h; ++y)
                {
                    memcpy(
                        crop_buf.data() + static_cast<size_t>(y) * crop_w,
                        raw_buf.data() + static_cast<size_t>(crop_y0 + y) * out_w + crop_x0,
                        static_cast<size_t>(crop_w) * sizeof(uint16_t));
                }
                out_pixels = crop_buf.data();
                out_w = crop_w;
                out_h = crop_h;
            }

            const uint32_t base_out_idx = opt.fill_drops
                ? fe.out_start
                : static_cast<uint32_t>(first_idx + job);
            const uint32_t reps = opt.fill_drops ? fe.repeat_count : 1u;

            DngParams dp{};
            dp.width = out_w;
            dp.height = out_h;
            dp.bits = meta.bits ? static_cast<uint32_t>(meta.bits) : clip.hdr.bits;
            dp.pack_bits = opt.pack_bits;
            dp.black_level = clip.hdr.black_level ? clip.hdr.black_level : (2048u >> (14 - dp.bits));
            dp.white_level = clip.hdr.white_level ? clip.hdr.white_level : ((1u << dp.bits) - 1u);
            memcpy(dp.cfa_pattern, base_cfa, 4);
            dp.fps_x1000 = clip.hdr.fps_x1000;
            dp.frame_index = base_out_idx;
            dp.wb_r = opt.wb_r;
            dp.wb_g = opt.wb_g;
            dp.wb_b = opt.wb_b;
            if (opt.full_frame && clip.hdr.active_x2 > clip.hdr.active_x1 && clip.hdr.active_y2 > clip.hdr.active_y1)
            {
                dp.active_x1 = clip.hdr.active_x1;
                dp.active_y1 = clip.hdr.active_y1;
                dp.active_x2 = clip.hdr.active_x2;
                dp.active_y2 = clip.hdr.active_y2;
            }

            uint32_t tc_off = 0;
            build_cinemadng(out_pixels, dp, dng_buf, &tc_off);

            bool write_ok = true;
            for (uint32_t r = 0; r < reps; ++r)
            {
                uint32_t seq_idx = base_out_idx + r;
                if (r > 0) patch_dng_timecode(dng_buf, tc_off, seq_idx, clip.hdr.fps_x1000);
                char fname[64];
                snprintf(fname, sizeof(fname), "%s_%06u.dng", clip_stem.c_str(), seq_idx + 1);
                fs::path out_file = opt.output_dir / fname;
                if (!write_whole_file(out_file.wstring(), dng_buf.data(), dng_buf.size()))
                {
                    std::lock_guard<std::mutex> lk(err_mtx);
                    last_err = "Write failed: " + out_file.string();
                    err_count++;
                    write_ok = false;
                    break;
                }
                bytes_written.fetch_add(dng_buf.size(), std::memory_order_relaxed);
            }
            if (!write_ok) continue;

            done_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t)
        pool.emplace_back(worker);

    while (done_count.load() + err_count.load() < count && !cancel_flag.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (on_progress)
        {
            ProgressInfo pi{};
            pi.done = done_count.load();
            pi.total = count;
            pi.errors = err_count.load();
            pi.bytes_written = bytes_written.load();
            pi.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            pi.fps = pi.elapsed_sec > 0.01 ? (pi.done / pi.elapsed_sec) : 0.0;
            pi.mb_per_sec = pi.elapsed_sec > 0.01 ? (pi.bytes_written / 1048576.0 / pi.elapsed_sec) : 0.0;
            on_progress(pi);
        }
    }

    // Wake any waiters on cancel
    q_not_full.notify_all();
    q_not_empty.notify_all();
    reader.join();
    for (auto &th : pool) th.join();

    final_stats.done = done_count.load();
    final_stats.total = count;
    final_stats.errors = err_count.load();
    final_stats.bytes_written = bytes_written.load();
    final_stats.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    final_stats.fps = final_stats.elapsed_sec > 0.001 ? (final_stats.done / final_stats.elapsed_sec) : 0.0;
    final_stats.mb_per_sec = final_stats.elapsed_sec > 0.001 ? (final_stats.bytes_written / 1048576.0 / final_stats.elapsed_sec) : 0.0;
    if (on_progress) on_progress(final_stats);

    if (cancel_flag.load())
    {
        err_out = "Cancelled";
        return false;
    }
    if (final_stats.errors > 0)
    {
        err_out = last_err.empty() ? "Decode/write errors occurred" : last_err;
        return false;
    }
    return true;
}

} // namespace crx_engine
