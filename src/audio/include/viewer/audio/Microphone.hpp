#pragma once

// A microphone (miniaudio): mono samples at 48 kHz while one of these
// exists. All but the capturing itself from one thread.

#include "viewer/audio/FrameRing.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace viewer::audio
{

class Microphone
{
public:
    static constexpr uint32_t kSampleRate = 48000;

    // Those there are (not what plays, listened in on). May take a moment.
    static std::vector<std::string> names();
    // Which of these `name` means: the one called that, else the first with
    // it in its name (whatever the case).
    static std::optional<size_t> match(const std::vector<std::string>& names,
                                       const std::string& name);
    // Opens that one; `name` empty, or none such: the system's default. Null
    // if it can't.
    static std::unique_ptr<Microphone> open(const std::string& name);
    ~Microphone();
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;

    FrameRing& samples() { return *ring_; }
    const std::string& name() const { return name_; }

private:
    struct Impl;
    Microphone();
    std::unique_ptr<FrameRing> ring_;
    std::string name_;
    std::unique_ptr<Impl> impl_; // last: stops before the ring goes
};

} // namespace viewer::audio
