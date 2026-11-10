#pragma once

// JPEG 2000 decoding behind an interface, so the library doing the work
// (currently OpenJPEG) can be swapped without touching callers.
//
// To add a backend: implement J2kDecoder in a new .cpp under src/image,
// and switch which file defines createJ2kDecoder() in src/image/CMakeLists.txt.
// tools/j2kbench can compare any two implementations of this interface.

#include "viewer/image/Image.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace viewer::image {

struct J2kDecodeOptions {
    // Skip this many of the highest resolution levels: 0 = full size,
    // 1 = half width and height, and so on. Matches Second Life's
    // "discard level".
    uint32_t discardLevels = 0;

    // 1 = decode entirely on the calling thread (what the worker pool wants).
    // >1 = allow the backend to parallelize one image internally.
    uint32_t threads = 1;
};

class J2kDecoder {
public:
    virtual ~J2kDecoder() = default;

    virtual std::string_view name() const = 0;

    // Decodes a J2K codestream or JP2 file held in memory.
    //
    // Thread-safe: one instance may be shared by every worker thread.
    // `out` is overwritten; reusing the same Image across calls avoids
    // reallocating its pixel buffer.
    // Returns false if the data could not be decoded (contents of `out` are
    // then unspecified).
    virtual bool decode(std::span<const uint8_t> data, const J2kDecodeOptions& options,
                        Image& out) const = 0;
};

// Creates the viewer's default JPEG 2000 decoder.
std::unique_ptr<J2kDecoder> createJ2kDecoder();

} // namespace viewer::image
