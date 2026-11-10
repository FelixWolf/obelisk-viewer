// j2kbench: benchmarks J2kDecoder implementations on a corpus of real files.
//
// Usage: j2kbench [options] <file-or-directory>...
//   --threads N    worker/decoder threads for the parallel modes (default: all cores)
//   --reduce LIST  comma-separated resolution reductions to test (default: 0)
//   --iters N      repetitions per measurement; best is reported (default: 3)
//   --decoder NAME only run the decoder with this name()
//
// Files are detected by content (J2K codestream or JP2 signature), so you can
// point it at a directory of mixed files.
//
// Modes:
//   serial   1 thread, decoder single-threaded   -> per-image latency
//   pool     N threads, each decoder 1-threaded  -> throughput, how the viewer's
//                                                   worker pool would use it
//   internal 1 caller, decoder uses N threads    -> latency of one big image

#include "viewer/image/J2kDecoder.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using viewer::image::Image;
using viewer::image::J2kDecodeOptions;
using viewer::image::J2kDecoder;

namespace {

struct InputFile {
    fs::path path;
    std::vector<uint8_t> bytes;
};

bool looksLikeJpeg2000(const std::vector<uint8_t>& b)
{
    static constexpr uint8_t kCodestream[] = {0xFF, 0x4F, 0xFF, 0x51};
    static constexpr uint8_t kJp2[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20};
    return (b.size() >= 4 && std::memcmp(b.data(), kCodestream, 4) == 0) ||
           (b.size() >= 8 && std::memcmp(b.data(), kJp2, 8) == 0);
}

void loadFile(const fs::path& path, std::vector<InputFile>& out)
{
    constexpr uintmax_t kMaxSize = 64u << 20;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size < 16 || size > kMaxSize) {
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return;
    }
    if (looksLikeJpeg2000(bytes)) {
        out.push_back({path, std::move(bytes)});
    }
}

std::vector<InputFile> loadCorpus(const std::vector<std::string>& inputs)
{
    std::vector<InputFile> files;
    for (const auto& input : inputs) {
        const fs::path p(input);
        if (fs::is_directory(p)) {
            for (const auto& entry : fs::recursive_directory_iterator(
                     p, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    loadFile(entry.path(), files);
                }
            }
        } else {
            loadFile(p, files);
        }
    }
    std::ranges::sort(files, {}, &InputFile::path);
    return files;
}

struct RunResult {
    double seconds = 0;
    uint64_t pixels = 0; // output pixels decoded
    size_t failures = 0;
};

using Clock = std::chrono::steady_clock;

RunResult runSerial(const J2kDecoder& dec, const std::vector<InputFile>& files, uint32_t reduce,
                    uint32_t decoderThreads)
{
    RunResult r;
    Image img;
    const auto start = Clock::now();
    for (const auto& f : files) {
        if (dec.decode(f.bytes, {.discardLevels = reduce, .threads = decoderThreads}, img)) {
            r.pixels += static_cast<uint64_t>(img.width) * img.height;
        } else {
            ++r.failures;
        }
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return r;
}

RunResult runPool(const J2kDecoder& dec, const std::vector<InputFile>& files, uint32_t reduce,
                  uint32_t threads)
{
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> pixels{0};
    std::atomic<size_t> failures{0};

    const auto start = Clock::now();
    {
        std::vector<std::jthread> workers;
        for (uint32_t t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                Image img;
                uint64_t localPixels = 0;
                size_t localFailures = 0;
                for (size_t i = next++; i < files.size(); i = next++) {
                    if (dec.decode(files[i].bytes, {.discardLevels = reduce, .threads = 1}, img)) {
                        localPixels += static_cast<uint64_t>(img.width) * img.height;
                    } else {
                        ++localFailures;
                    }
                }
                pixels += localPixels;
                failures += localFailures;
            });
        }
    }
    RunResult r;
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.pixels = pixels;
    r.failures = failures;
    return r;
}

template <typename Fn>
RunResult best(uint32_t iters, Fn&& fn)
{
    RunResult bestResult;
    bestResult.seconds = 1e300;
    for (uint32_t i = 0; i < iters; ++i) {
        RunResult r = fn();
        if (r.seconds < bestResult.seconds) {
            bestResult = r;
        }
    }
    return bestResult;
}

std::vector<uint32_t> parseList(std::string_view s)
{
    std::vector<uint32_t> out;
    while (!s.empty()) {
        const auto comma = s.find(',');
        const auto part = s.substr(0, comma);
        uint32_t v = 0;
        std::from_chars(part.data(), part.data() + part.size(), v);
        out.push_back(v);
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
    }
    return out;
}

void printUsage()
{
    std::println(stderr, "usage: j2kbench [--threads N] [--reduce 0,1,2] [--iters N] "
                         "[--decoder NAME] <file-or-dir>...");
}

} // namespace

int main(int argc, char** argv)
{
    uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    uint32_t iters = 3;
    std::vector<uint32_t> reduces = {0};
    std::string onlyDecoder;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                printUsage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--threads") {
            threads = std::max(1u, parseList(value()).at(0));
        } else if (arg == "--iters") {
            iters = std::max(1u, parseList(value()).at(0));
        } else if (arg == "--reduce") {
            reduces = parseList(value());
        } else if (arg == "--decoder") {
            onlyDecoder = value();
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            inputs.emplace_back(arg);
        }
    }
    if (inputs.empty()) {
        printUsage();
        return 2;
    }

    const auto files = loadCorpus(inputs);
    if (files.empty()) {
        std::println(stderr, "no JPEG 2000 files found");
        return 1;
    }
    size_t totalBytes = 0;
    for (const auto& f : files) {
        totalBytes += f.bytes.size();
    }
    std::println("corpus: {} files, {:.1f} MiB compressed; threads={}, iters={}", files.size(),
                 static_cast<double>(totalBytes) / (1024.0 * 1024.0), threads, iters);

    std::vector<std::unique_ptr<J2kDecoder>> decoders;
    decoders.push_back(viewer::image::createJ2kDecoder());
    // Add alternative J2kDecoder implementations here to compare them.
    std::erase_if(decoders, [&](const auto& d) {
        return !onlyDecoder.empty() && d->name() != onlyDecoder;
    });

    // Cross-check: decode every file once with each decoder and compare pixels
    // against the first decoder. Lossy (9/7) files may legitimately differ by
    // a few levels; lossless (5/3) files should match exactly.
    if (decoders.size() > 1) {
        for (size_t d = 1; d < decoders.size(); ++d) {
            size_t identical = 0, differing = 0, shapeMismatch = 0, failed = 0;
            int worstDiff = 0;
            std::string worstFile;
            for (const auto& f : files) {
                Image a, b;
                if (!decoders[0]->decode(f.bytes, {}, a) || !decoders[d]->decode(f.bytes, {}, b)) {
                    ++failed;
                    continue;
                }
                if (a.width != b.width || a.height != b.height || a.components != b.components) {
                    ++shapeMismatch;
                    continue;
                }
                int maxDiff = 0;
                for (size_t i = 0; i < a.pixels.size(); ++i) {
                    maxDiff = std::max(maxDiff, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
                }
                if (maxDiff == 0) {
                    ++identical;
                } else {
                    ++differing;
                    if (maxDiff > worstDiff) {
                        worstDiff = maxDiff;
                        worstFile = f.path.filename().string();
                    }
                }
            }
            std::println("cross-check {} vs {}: {} identical, {} differ (max abs diff {}{}{}), "
                         "{} shape mismatches, {} failures",
                         decoders[0]->name(), decoders[d]->name(), identical, differing, worstDiff,
                         worstFile.empty() ? "" : " in ", worstFile, shapeMismatch, failed);
        }
    }

    std::println("");
    std::println("{:<10} {:>6} {:<9} {:>10} {:>10} {:>10} {:>6}", "decoder", "reduce", "mode",
                 "seconds", "files/s", "MPix/s", "fails");
    for (uint32_t reduce : reduces) {
        for (auto& dec : decoders) {
            auto report = [&](std::string_view mode, const RunResult& r) {
                std::println("{:<10} {:>6} {:<9} {:>10.3f} {:>10.1f} {:>10.1f} {:>6}", dec->name(),
                             reduce, mode, r.seconds,
                             static_cast<double>(files.size()) / r.seconds,
                             static_cast<double>(r.pixels) / 1e6 / r.seconds, r.failures);
            };
            report("serial", best(iters, [&] { return runSerial(*dec, files, reduce, 1); }));
            if (threads > 1) {
                report("pool", best(iters, [&] { return runPool(*dec, files, reduce, threads); }));
                report("internal",
                       best(iters, [&] { return runSerial(*dec, files, reduce, threads); }));
            }
        }
    }
    return 0;
}
