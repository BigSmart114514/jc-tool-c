// 视频链路诊断工具：用产品代码本身录制 / 回放 H.264。
//
//   VideoPipelineTest encode --out clip [--seconds 5] [--fps 30] [--width N]
//                            [--capture-output \\.\DISPLAY4 | auto-virtual] [--cpu]
//   VideoPipelineTest decode --in clip.h264 [--save-dir dir] [--save-every N]
//                            [--max-frames N]
//
// encode: ScreenCapture + MediaEncoder（与 DesktopService 完全相同的代码），
//         输出标准 Annex-B 的 clip.h264 和帧索引 clip.h264.idx。
// decode: MediaDecoder（与客户端完全相同的代码），逐帧解码并保存 BMP。
//
// 日志里的 [MediaEncoder]/[MediaDecoder] 行就是产品里那套诊断输出。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../server/screen_capture.h"
#include "../server/media_encoder.h"
#include "../server/h264_nal.h"
#include "../client/media_decoder.h"

namespace fs = std::filesystem;

// ===================== H.264 辅助（SPS 尺寸 / 帧切分） =====================

struct BitReader {
    BitReader(const uint8_t* d, std::size_t n) : data_(d), size_(n) {}

    bool bit(int& v) {
        if (bitPos_ >= size_ * 8) return false;
        v = (data_[bitPos_ >> 3] >> (7 - (bitPos_ & 7))) & 1;
        ++bitPos_;
        return true;
    }
    bool ue(uint32_t& v) {
        int b = 0;
        int zeros = 0;
        while (true) {
            if (!bit(b)) return false;
            if (b) break;
            if (++zeros > 31) return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < zeros; ++i) {
            if (!bit(b)) return false;
            value = (value << 1) | static_cast<uint32_t>(b);
        }
        v = (uint32_t(1) << zeros) - 1 + value;
        return true;
    }
    bool se(int32_t& v) {
        uint32_t k = 0;
        if (!ue(k)) return false;
        v = (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
        return true;
    }
    void skipBits(int n) { bitPos_ += static_cast<std::size_t>(n); }

private:
    const uint8_t* data_;
    std::size_t size_;
    std::size_t bitPos_ = 0;
};

struct SpsInfo {
    bool ok = false;
    int profile = 0;
    int level = 0;
    int codedWidth = 0;
    int codedHeight = 0;
};

static std::vector<uint8_t> removeEmulationPrevention(const uint8_t* p, std::size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2;
            continue;
        }
        out.push_back(p[i]);
    }
    return out;
}

static void skipScalingList(BitReader& br, int size) {
    int lastScale = 8, nextScale = 8;
    for (int j = 0; j < size; ++j) {
        if (nextScale != 0) {
            int32_t delta = 0;
            if (!br.se(delta)) return;
            nextScale = (lastScale + delta + 256) % 256;
        }
        lastScale = nextScale == 0 ? lastScale : nextScale;
    }
}

// nal 指向 NAL header 起（不含起始码）。
static SpsInfo parseSps(const uint8_t* nal, std::size_t len) {
    SpsInfo info;
    if (len < 4) return info;
    const auto rbsp = removeEmulationPrevention(nal + 1, len - 1);
    if (rbsp.size() < 4) return info;
    BitReader br(rbsp.data(), rbsp.size());

    const int profile = rbsp[0];
    uint32_t u = 0;
    br.skipBits(8);   // profile_idc
    br.skipBits(8);   // constraint_set flags
    br.skipBits(8);   // level_idc
    if (!br.ue(u)) return info;         // seq_parameter_set_id

    int chromaFormatIdc = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        if (!br.ue(u)) return info;
        chromaFormatIdc = static_cast<int>(u);
        if (chromaFormatIdc == 3) br.skipBits(1);   // separate_colour_plane_flag
        if (!br.ue(u)) return info;                 // bit_depth_luma_minus8
        if (!br.ue(u)) return info;                 // bit_depth_chroma_minus8
        br.skipBits(1);                             // qpprime_y_zero_transform_bypass_flag
        int scalingMatrixPresent = 0;
        if (!br.bit(scalingMatrixPresent)) return info;
        if (scalingMatrixPresent) {
            const int count = (chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < count; ++i) {
                int present = 0;
                if (!br.bit(present)) return info;
                if (present) skipScalingList(br, i < 6 ? 16 : 64);
            }
        }
    }

    uint32_t log2MaxFrameNumMinus4 = 0;
    if (!br.ue(log2MaxFrameNumMinus4)) return info;
    uint32_t picOrderCntType = 0;
    if (!br.ue(picOrderCntType)) return info;
    if (picOrderCntType == 0) {
        if (!br.ue(u)) return info;
    } else if (picOrderCntType == 1) {
        br.skipBits(1);
        int32_t se = 0;
        if (!br.se(se)) return info;
        if (!br.se(se)) return info;
        uint32_t n = 0;
        if (!br.ue(n)) return info;
        for (uint32_t i = 0; i < n; ++i)
            if (!br.se(se)) return info;
    }
    if (!br.ue(u)) return info;   // max_num_ref_frames
    br.skipBits(1);               // gaps_in_frame_num_value_allowed_flag

    uint32_t picWidthInMbsMinus1 = 0;
    uint32_t picHeightInMapUnitsMinus1 = 0;
    if (!br.ue(picWidthInMbsMinus1)) return info;
    if (!br.ue(picHeightInMapUnitsMinus1)) return info;

    int frameMbsOnly = 0;
    if (!br.bit(frameMbsOnly)) return info;
    if (!frameMbsOnly) br.skipBits(1);   // mb_adaptive_frame_field_flag
    br.skipBits(1);                       // direct_8x8_inference_flag

    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    int frameCropping = 0;
    if (!br.bit(frameCropping)) return info;
    if (frameCropping) {
        if (!br.ue(cropLeft) || !br.ue(cropRight) ||
            !br.ue(cropTop) || !br.ue(cropBottom)) return info;
    }

    int widthMb = static_cast<int>(picWidthInMbsMinus1) + 1;
    int heightMapUnits = static_cast<int>(picHeightInMapUnitsMinus1) + 1;
    int cropUnitX = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2 : 1;
    int cropUnitY = ((chromaFormatIdc == 1) ? 2 : 1) * (2 - frameMbsOnly);

    info.profile = profile;
    info.level = rbsp[2];
    info.codedWidth = widthMb * 16 - static_cast<int>(cropLeft + cropRight) * cropUnitX;
    info.codedHeight = heightMapUnits * 16 * (2 - frameMbsOnly) -
                       static_cast<int>(cropTop + cropBottom) * cropUnitY;
    info.ok = info.codedWidth > 0 && info.codedHeight > 0;
    return info;
}

struct NalRef {
    std::size_t codeOffset = 0;   // 起始码第一个字节
    std::size_t dataOffset = 0;   // NAL header
    std::size_t size = 0;
    int type = 0;
};

static std::vector<NalRef> findNals(const std::vector<uint8_t>& b) {
    std::vector<NalRef> nals;
    const std::size_t n = b.size();
    if (n < 4) return nals;

    // 定位每个起始码里的 0x01 字节。负载里的 00 00 01 会被防竞争字节转义，
    // 所以这里不会误判。
    std::vector<std::size_t> ones;
    for (std::size_t i = 2; i < n; ++i) {
        if (b[i] == 1 && b[i - 1] == 0 && b[i - 2] == 0) ones.push_back(i);
    }

    auto codeStart = [&](std::size_t onePos) {
        return (onePos >= 3 && b[onePos - 3] == 0) ? onePos - 3 : onePos - 2;
    };

    for (std::size_t k = 0; k < ones.size(); ++k) {
        NalRef ref;
        ref.codeOffset = codeStart(ones[k]);
        ref.dataOffset = ones[k] + 1;
        const std::size_t end = (k + 1 < ones.size()) ? codeStart(ones[k + 1]) : n;
        if (end <= ref.dataOffset) continue;
        ref.size = end - ref.dataOffset;
        ref.type = b[ref.dataOffset] & 0x1f;
        nals.push_back(ref);
    }
    return nals;
}

static SpsInfo findSps(const std::vector<uint8_t>& data) {
    for (const auto& nal : findNals(data)) {
        if (nal.type == 7) return parseSps(data.data() + nal.dataOffset, nal.size);
    }
    return {};
}

static uint32_t firstMbInSlice(const std::vector<uint8_t>& data, const NalRef& nal) {
    // first_mb_in_slice 是 slice header 的第一个 ue(v)，位于 NAL header 之后。
    std::size_t begin = nal.dataOffset + 1;
    std::size_t end = std::min(data.size(), begin + 8);
    BitReader br(data.data() + begin, end - begin);
    uint32_t v = 0;
    if (!br.ue(v)) return 0;
    return v;
}

// 无索引文件时按 H.264 规则切分访问单元。
static std::vector<std::pair<std::size_t, std::size_t>> splitAccessUnits(std::vector<uint8_t>& data) {
    std::vector<std::pair<std::size_t, std::size_t>> aus;
    const auto nals = findNals(data);
    std::size_t begin = SIZE_MAX;
    bool hasVcl = false;
    for (const auto& nal : nals) {
        const bool vcl = nal.type >= 1 && nal.type <= 5;
        if (begin == SIZE_MAX) {
            begin = nal.codeOffset;
        } else {
            bool startNew = false;
            if (hasVcl && vcl) startNew = firstMbInSlice(data, nal) == 0;
            else if (hasVcl && (nal.type == 6 || nal.type == 7 || nal.type == 8 || nal.type == 9))
                startNew = true;
            if (startNew) {
                aus.emplace_back(begin, nal.codeOffset - begin);
                begin = nal.codeOffset;
                hasVcl = false;
            }
        }
        if (vcl) hasVcl = true;
    }
    if (begin != SIZE_MAX && begin < data.size()) aus.emplace_back(begin, data.size() - begin);
    return aus;
}

// ===================== BMP 输出 =====================

static void put16(std::ostream& o, uint16_t v) {
    char b[2] = { static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff) };
    o.write(b, 2);
}
static void put32(std::ostream& o, uint32_t v) {
    char b[4] = { static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                  static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff) };
    o.write(b, 4);
}

static bool writeBmp(const fs::path& path, const uint8_t* bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t pixelBytes = static_cast<uint32_t>(w) * h * 4;
    out.write("BM", 2);
    put32(out, 14 + 40 + pixelBytes);
    put16(out, 0);
    put16(out, 0);
    put32(out, 14 + 40);
    put32(out, 40);
    put32(out, static_cast<uint32_t>(w));
    put32(out, static_cast<uint32_t>(h));   // 正数 = bottom-up
    put16(out, 1);
    put16(out, 32);
    put32(out, 0);            // BI_RGB
    put32(out, pixelBytes);
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    for (int y = h - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char*>(bgra + static_cast<std::size_t>(y) * stride), w * 4);
    return static_cast<bool>(out);
}

// ===================== 命令行 =====================

struct Options {
    std::string mode;
    std::string input;
    std::string output;
    std::string captureOutput;
    std::string saveDir = "video-dump";
    int seconds = 5;
    int fps = 30;
    int width = 0;
    int saveEvery = 15;
    int maxFrames = 0;
    bool cpu = false;
};

static void usage() {
    std::cout <<
        "VideoPipelineTest - 用产品代码录制/回放 H.264\n\n"
        "  encode --out clip [--seconds 5] [--fps 30] [--width N]\n"
        "         [--capture-output \\\\.\\DISPLAY4|auto-virtual] [--cpu]\n"
        "  decode --in clip.h264 [--save-dir dir] [--save-every 15] [--max-frames N]\n";
}

static bool parseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2) return false;
    opt.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        auto needInt = [&](int& dst) {
            if (i + 1 >= argc) return false;
            dst = std::atoi(argv[++i]);
            return true;
        };
        if (a == "--out" || a == "--output") { if (!need(opt.output)) return false; }
        else if (a == "--in" || a == "--input") { if (!need(opt.input)) return false; }
        else if (a == "--capture-output") { if (!need(opt.captureOutput)) return false; }
        else if (a == "--save-dir") { if (!need(opt.saveDir)) return false; }
        else if (a == "--seconds") { if (!needInt(opt.seconds)) return false; }
        else if (a == "--fps") { if (!needInt(opt.fps)) return false; }
        else if (a == "--width") { if (!needInt(opt.width)) return false; }
        else if (a == "--save-every") { if (!needInt(opt.saveEvery)) return false; }
        else if (a == "--max-frames") { if (!needInt(opt.maxFrames)) return false; }
        else if (a == "--cpu") { opt.cpu = true; }
        else if (a == "-h" || a == "--help") { return false; }
        else {
            std::cerr << "Unknown option: " << a << "\n";
            return false;
        }
    }
    return true;
}

// ===================== encode =====================

static int runEncode(const Options& opt) {
    if (opt.output.empty()) {
        std::cerr << "encode: --out is required\n";
        return 2;
    }
    if (opt.fps <= 0 || opt.seconds <= 0) {
        std::cerr << "encode: --fps/--seconds must be positive\n";
        return 2;
    }

    ScreenCapture capture;
    if (!opt.captureOutput.empty()) {
        std::wstring name(opt.captureOutput.begin(), opt.captureOutput.end());
        capture.setCaptureOutput(name);
    }
    if (!capture.init()) {
        std::cerr << "[Test] ScreenCapture init failed\n";
        return 3;
    }

    const int capW = capture.getWidth();
    const int capH = capture.getHeight();
    if (capW <= 0 || capH <= 0) {
        std::cerr << "[Test] Invalid capture size\n";
        return 3;
    }

    // 与 DesktopService::onMessage(StreamConfig) 相同的目标尺寸算法。
    int targetW = capW;
    int targetH = capH;
    if (opt.width > 0) {
        targetW = std::max(opt.width, 270);
        targetH = capH * targetW / capW;
        targetW = (targetW + 15) & ~15;
        targetH = (targetH + 15) & ~15;
    }

    MediaEncoder encoder;
    const int bitrate = std::max(10000000, targetW * targetH * 4);
    const bool useGpu = !opt.cpu && capture.getDevice() != nullptr;
    if (!encoder.init(useGpu ? capture.getDevice() : nullptr,
                      capW, capH, targetW, targetH, opt.fps, bitrate)) {
        std::cerr << "[Test] MediaEncoder init failed\n";
        return 3;
    }

    std::cout << "[Test] encode src=" << capW << "x" << capH
              << " dst=" << encoder.encodedWidth() << "x" << encoder.encodedHeight()
              << " fps=" << opt.fps << " gpu=" << (useGpu && encoder.hasGPUPath()) << "\n";

    const fs::path outPath(opt.output);
    if (outPath.has_parent_path()) fs::create_directories(outPath.parent_path());
    std::ofstream h264(outPath, std::ios::binary | std::ios::trunc);
    std::ofstream idx(std::string(opt.output) + ".idx", std::ios::trunc);
    if (!h264) {
        std::cerr << "[Test] Cannot open " << opt.output << "\n";
        return 4;
    }
    idx << "# " << encoder.encodedWidth() << ' ' << encoder.encodedHeight()
        << ' ' << opt.fps << '\n';

    const int totalFrames = opt.fps * opt.seconds;
    const int kfInterval = std::max(1, opt.fps * 2);
    std::vector<MediaEncoder::Frame> encoded;
    int64_t pts = 0;
    int framesWritten = 0;
    int keyframes = 0;
    std::size_t reportedFrames = 0;

    auto next = std::chrono::steady_clock::now();
    for (int f = 0; f < totalFrames; ++f) {
        const bool wantKeyframe = (f % kfInterval == 0);
        encoded.clear();
        bool ok = false;
        if (useGpu && encoder.hasGPUPath()) {
            ID3D11Texture2D* tex = nullptr;
            if (capture.captureTexture(&tex, true))
                ok = encoder.encodeFromTexture(tex, pts, encoded, wantKeyframe);
        } else {
            bool hasNew = false;
            const uint8_t* bgra = capture.capture(hasNew);
            if (bgra)
                ok = encoder.encode(bgra, pts, encoded, wantKeyframe);
        }

        if (!ok)
            std::cerr << "[Test] encode failed at pts=" << pts << "\n";

        for (const auto& frame : encoded) {
            const auto offset = static_cast<std::uint64_t>(h264.tellp());
            h264.write(reinterpret_cast<const char*>(frame.data.data()),
                       static_cast<std::streamsize>(frame.data.size()));
            idx << offset << ' ' << frame.data.size() << ' ' << (frame.keyframe ? 1 : 0) << '\n';
            ++framesWritten;
            if (frame.keyframe) ++keyframes;
            if (reportedFrames < 3) {
                ++reportedFrames;
                std::cout << "[Test] tx#" << framesWritten << " pts=" << pts
                          << " bytes=" << frame.data.size()
                          << " kf=" << frame.keyframe << "\n";
            }
        }

        ++pts;
        next += std::chrono::milliseconds(1000 / opt.fps);
        std::this_thread::sleep_until(next);
    }
    h264.flush();
    idx.flush();
    h264.close();
    idx.close();

    // 回读文件头，确认落盘的 SPS 尺寸与编码器声明一致。
    std::ifstream check(outPath, std::ios::binary);
    std::vector<uint8_t> head((std::istreambuf_iterator<char>(check)),
                              std::istreambuf_iterator<char>());
    const auto sps = findSps(head);
    if (sps.ok) {
        std::cout << "[Test] file SPS " << sps.codedWidth << "x" << sps.codedHeight
                  << " profile=" << sps.profile << " level=" << sps.level
                  << " (encoder says " << encoder.encodedWidth() << "x"
                  << encoder.encodedHeight() << ")\n";
    } else {
        std::cout << "[Test] file SPS not found in first keyframe\n";
    }

    std::cout << "[Test] wrote " << framesWritten << " frames (" << keyframes
              << " keyframes), " << fs::file_size(outPath) << " bytes -> "
              << outPath.string() << "\n";
    std::cout << "[Test] frame index -> " << (opt.output + ".idx") << "\n";

    encoder.cleanup();
    capture.cleanup();
    return framesWritten > 0 ? 0 : 5;
}

// ===================== decode =====================

struct BandStats {
    double mean = 0.0;
    double blackPct = 0.0;
};

static BandStats bandStats(const uint8_t* bgra, int w, int h, int stride, int y0, int y1) {
    BandStats s;
    if (!bgra || w <= 0 || y1 <= y0) return s;
    uint64_t sum = 0, count = 0, black = 0;
    for (int y = y0; y < y1; y += 3) {
        const uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; x += 3) {
            const uint8_t* p = row + x * 4;
            const int luma = (29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8;
            sum += static_cast<uint64_t>(luma);
            black += (luma <= 20);
            ++count;
        }
    }
    if (count) {
        s.mean = double(sum) / count;
        s.blackPct = double(black) * 100.0 / count;
    }
    return s;
}

static int runDecode(const Options& opt) {
    if (opt.input.empty()) {
        std::cerr << "decode: --in is required\n";
        return 2;
    }
    std::ifstream in(opt.input, std::ios::binary);
    if (!in) {
        std::cerr << "[Test] Cannot open " << opt.input << "\n";
        return 4;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (data.size() < 8) {
        std::cerr << "[Test] Input too small\n";
        return 4;
    }

    struct Span { std::size_t offset = 0; std::size_t size = 0; bool keyframe = false; };
    std::vector<Span> frames;

    struct IndexHeader { int w = 0, h = 0, fps = 0; } header;
    bool haveIndex = false;
    const std::string indexPath = opt.input + ".idx";
    if (std::filesystem::exists(indexPath)) {
        std::ifstream idx(indexPath);
        std::string line;
        std::getline(idx, line);
        if (!line.empty() && line[0] == '#') {
            std::istringstream hs(line.substr(1));
            if (hs >> header.w >> header.h >> header.fps) haveIndex = true;
        }
        std::size_t offset = 0, size = 0;
        int kf = 0;
        while (idx >> offset >> size >> kf) {
            if (offset + size <= data.size() && size > 0)
                frames.push_back({offset, size, kf != 0});
        }
    }

    if (frames.empty()) {
        const auto spans = splitAccessUnits(data);
        const auto nals = findNals(data);
        for (const auto& s : spans) {
            bool kf = false;
            for (const auto& nal : nals) {
                if (nal.codeOffset >= s.first && nal.codeOffset < s.first + s.second &&
                    nal.type == 5) {
                    kf = true;
                    break;
                }
            }
            frames.push_back({s.first, s.second, kf});
        }
        std::cout << "[Test] no .idx, split into " << frames.size()
                  << " access units (assumes one slice group per frame)\n";
    } else {
        std::cout << "[Test] loaded " << frames.size() << " frames from index\n";
    }
    if (frames.empty()) {
        std::cerr << "[Test] No frames to decode\n";
        return 5;
    }

    if (!haveIndex || header.w <= 0 || header.h <= 0) {
        const auto sps = findSps(data);
        if (!sps.ok) {
            std::cerr << "[Test] Cannot determine frame size (no index, no SPS)\n";
            return 5;
        }
        header.w = sps.codedWidth;
        header.h = sps.codedHeight;
        std::cout << "[Test] frame size from SPS: " << header.w << "x" << header.h << "\n";
    }

    MediaDecoder decoder;
    if (!decoder.init(header.w, header.h)) {
        std::cerr << "[Test] MediaDecoder init failed for " << header.w << 'x' << header.h << "\n";
        return 3;
    }

    fs::create_directories(opt.saveDir);
    std::vector<uint8_t> bgra;
    int fed = 0, ready = 0, needInput = 0, errors = 0, saved = 0;
    const int limit = opt.maxFrames > 0 ? std::min<int>(opt.maxFrames, frames.size())
                                        : static_cast<int>(frames.size());
    for (int i = 0; i < limit; ++i) {
        const auto& span = frames[static_cast<std::size_t>(i)];
        auto result = decoder.decode(data.data() + span.offset,
                                     static_cast<int>(span.size), bgra);
        ++fed;
        if (result == MediaDecoder::DecodeResult::FrameReady) {
            ++ready;
            const int w = decoder.getWidth();
            const int h = decoder.getHeight();
            const int stride = decoder.getStride();
            const auto top = bandStats(bgra.data(), w, h, stride, 0, h / 2);
            const auto mid = bandStats(bgra.data(), w, h, stride, h / 2, h * 3 / 4);
            const auto bottom = bandStats(bgra.data(), w, h, stride, h * 3 / 4, h);
            std::cout << "[Test] dec#" << i << " size=" << span.size
                      << " kf=" << span.keyframe
                      << " " << w << "x" << h
                      << " luma top=" << top.mean << "(" << top.blackPct << "%)"
                      << " mid=" << mid.mean << "(" << mid.blackPct << "%)"
                      << " bottom=" << bottom.mean << "(" << bottom.blackPct << "%)\n";

            if (opt.saveEvery > 0 && (i % opt.saveEvery == 0 || i == limit - 1)) {
                char name[64];
                std::snprintf(name, sizeof(name), "dec_%05d.bmp", i);
                const fs::path p = fs::path(opt.saveDir) / name;
                if (writeBmp(p, bgra.data(), w, h, stride)) {
                    ++saved;
                    std::cout << "[Test] saved " << p.string() << "\n";
                }
            }
        } else if (result == MediaDecoder::DecodeResult::NeedInput) {
            ++needInput;
        } else {
            ++errors;
        }
    }

    std::cout << "[Test] fed=" << fed << " decoded=" << ready
              << " needInput=" << needInput << " errors=" << errors
              << " bmp=" << saved << " dir=" << opt.saveDir << "\n";
    decoder.cleanup();
    return errors == 0 ? 0 : 6;
}

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt) || opt.mode == "-h" || opt.mode == "--help") {
        usage();
        return 1;
    }
    if (opt.mode == "encode") return runEncode(opt);
    if (opt.mode == "decode") return runDecode(opt);
    usage();
    return 1;
}
