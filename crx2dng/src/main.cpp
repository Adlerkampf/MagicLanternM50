// crx2dng - Fast multi-threaded Canon EOS M50 Magic Lantern CRX -> CinemaDNG CLI
// Uses shared crx_engine (converter_core.h) with automatic dropped-frame A/V sync handling.

#include <iomanip>
#include <iostream>
#include <string>

#include "converter_core.h"

namespace fs = std::filesystem;

static void print_usage()
{
    std::cout <<
        "crx2dng - Fast Multi-Threaded Canon EOS M50 CRX -> CinemaDNG Converter\n"
        "Produces native CinemaDNG sequences for Blackmagic DaVinci Resolve.\n\n"
        "Usage:\n"
        "  crx2dng <input.CRX> [options]\n\n"
        "Options:\n"
        "  -o <dir>          Output folder (default: <input_stem>_dng next to input)\n"
        "  -j <N>            Worker threads (default: all CPU threads)\n"
        "  --pack            Write tightly packed 12/14-bit DNG (25% smaller, 100% lossless)\n"
        "  --full            Keep full encoded sensor frame (no active-area crop)\n"
        "  --no-fill-drops   Do not duplicate previous frame across dropped-frame gaps\n"
        "  --start <N>       First recorded frame index to convert (0-based)\n"
        "  --count <N>       Number of recorded frames to convert\n"
        "  --wb <r,g,b>      Custom WB multipliers (default: 2.04,1.00,1.61 D65 daylight)\n"
        "  --info            Print clip info (including dropped-frame count) and exit\n"
        "  -h, --help        Show this help\n";
}

int main(int argc, char **argv)
{
    std::string input_path;
    std::string output_dir;
    bool info_only = false;
    crx_engine::ConvertOptions opt{};

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "-o" && i + 1 < argc) output_dir = argv[++i];
        else if (a == "-j" && i + 1 < argc) opt.threads = atoi(argv[++i]);
        else if (a == "--start" && i + 1 < argc) opt.start_frame = atoi(argv[++i]);
        else if (a == "--count" && i + 1 < argc) opt.max_frames = atoi(argv[++i]);
        else if (a == "--pack") opt.pack_bits = true;
        else if (a == "--full") opt.full_frame = true;
        else if (a == "--no-fill-drops") opt.fill_drops = false;
        else if (a == "--info") info_only = true;
        else if (a == "--wb" && i + 1 < argc)
        {
            double r = 2.04, g = 1.0, b = 1.61;
            if (sscanf(argv[++i], "%lf,%lf,%lf", &r, &g, &b) == 3)
            {
                opt.wb_r = r; opt.wb_g = g; opt.wb_b = b;
            }
        }
        else if (a[0] != '-' && input_path.empty()) input_path = a;
        else { std::cerr << "Unknown option: " << a << "\n"; return 1; }
    }

    if (input_path.empty()) { print_usage(); return 1; }

    fs::path in_path(input_path);
    crx_engine::ClipInfo clip;
    std::string err;
    if (!crx_engine::inspect_crx_clip(in_path, clip, err))
    {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    uint32_t cx0 = 0, cy0 = 0, cw = 0, ch = 0;
    crx_engine::compute_crop(clip.hdr, opt.full_frame, cx0, cy0, cw, ch);
    double fps = clip.hdr.fps_x1000 / 1000.0;
    double dur_s = fps > 0.1 ? (clip.timeline_frames / fps) : 0.0;
    double avg_mb = (clip.total_in_bytes / 1048576.0) / std::max<size_t>(1, clip.frames.size());
    double mbps = dur_s > 0.01 ? (clip.total_in_bytes / 1048576.0) / dur_s : 0.0;

    std::cout << "=== Canon EOS M50 CRX Clip ===\n"
              << "  File:       " << in_path.filename().string()
              << " (" << clip.chunks.size() << " chunk" << (clip.chunks.size() > 1 ? "s" : "")
              << ", " << std::fixed << std::setprecision(1) << (clip.total_in_bytes / 1048576.0) << " MB)\n"
              << "  Codec:      " << (clip.hdr.mode == 4 ? "Canon C-RAW (lossy)" : "Canon RAW (lossless)")
              << " " << clip.hdr.bits << "-bit\n"
              << "  Encoded:    " << clip.hdr.enc_w << "x" << clip.hdr.enc_h
              << "  ->  Output DNG: " << cw << "x" << ch
              << (opt.pack_bits ? (" (" + std::to_string(clip.hdr.bits) + "-bit packed)") : " (16-bit uncompressed)") << "\n"
              << "  Levels:     black=" << clip.hdr.black_level << " white=" << clip.hdr.white_level << "\n"
              << "  Frames:     " << clip.frames.size() << " recorded";
    if (clip.dropped_frames > 0)
        std::cout << " + " << clip.dropped_frames << " dropped = " << clip.timeline_frames
                  << " timeline frames (" << (opt.fill_drops ? "auto-filling drops" : "drops NOT filled") << ")";
    std::cout << " @ " << std::setprecision(3) << fps << " fps ("
              << std::setprecision(2) << dur_s << " s, "
              << avg_mb << " MB/frame, " << std::setprecision(1) << mbps << " MB/s)\n";

    if (info_only) return 0;

    opt.output_dir = output_dir.empty()
        ? (in_path.parent_path() / (in_path.stem().string() + "_dng"))
        : fs::path(output_dir);

    std::cout << "  Output:     " << opt.output_dir.string() << "\n";

    std::atomic<bool> cancel{false};
    crx_engine::ProgressInfo stats{};
    bool ok = crx_engine::convert_crx_clip(
        clip, opt, cancel,
        [](const crx_engine::ProgressInfo &pi) {
            int pct = pi.total ? static_cast<int>((pi.done * 100) / pi.total) : 0;
            std::cout << "\r  [" << std::setw(3) << pct << "%] "
                      << pi.done << "/" << pi.total << " frames | "
                      << std::fixed << std::setprecision(1) << pi.fps << " fps | "
                      << pi.mb_per_sec << " MB/s   " << std::flush;
        },
        stats, err);

    std::cout << "\r  [100%] " << stats.done << "/" << stats.total << " frames in "
              << std::fixed << std::setprecision(2) << stats.elapsed_sec << " s ("
              << std::setprecision(1) << stats.fps << " fps, "
              << stats.mb_per_sec << " MB/s, "
              << (stats.bytes_written / 1048576.0) << " MB written)\n";

    if (!ok)
    {
        std::cerr << "Error: " << err << "\n";
        return 2;
    }
    std::cout << "Done! Import folder '" << opt.output_dir.string() << "' directly into DaVinci Resolve.\n";
    return 0;
}
