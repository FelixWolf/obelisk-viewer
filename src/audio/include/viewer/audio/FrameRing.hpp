#pragma once

// Mono samples from one thread to another without locks (one writer, one
// reader): the microphone's, and a copy of what's played.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace viewer::audio
{

class FrameRing
{
public:
    explicit FrameRing(size_t capacity) : samples_(capacity) {}

    // The writer's: what doesn't fit is dropped.
    void write(std::span<const float> in)
    {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const size_t room = samples_.size() - static_cast<size_t>(head - tail);
        const size_t n = in.size() < room ? in.size() : room;
        for(size_t i = 0; i < n; i++)
        {
            samples_[static_cast<size_t>((head + i) % samples_.size())] = in[i];
        }
        head_.store(head + n, std::memory_order_release);
    }

    // The reader's: all of `out` or nothing (false).
    bool read(std::span<float> out)
    {
        if(available() < out.size())
        {
            return false;
        }
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        for(size_t i = 0; i < out.size(); i++)
        {
            out[i] = samples_[static_cast<size_t>((tail + i) % samples_.size())];
        }
        tail_.store(tail + out.size(), std::memory_order_release);
        return true;
    }
    size_t available() const
    {
        return static_cast<size_t>(head_.load(std::memory_order_acquire) -
                                   tail_.load(std::memory_order_relaxed));
    }
    // Forgets the oldest but this many.
    void keep(size_t n)
    {
        const size_t have = available();
        if(have > n)
        {
            tail_.fetch_add(have - n, std::memory_order_release);
        }
    }

private:
    std::vector<float> samples_;
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
};

} // namespace viewer::audio
