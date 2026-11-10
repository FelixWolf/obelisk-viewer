#pragma once

// Multi-producer / single-consumer queue: the way threads talk to each other.
// Threads never share mutable state directly; they send messages.
//
// This is a simple mutex + swap implementation. The consumer drains the whole
// queue at once, so contention is one lock per drain rather than per message.
// Replace with a lock-free queue if profiling says so; the interface stays.

#include <mutex>
#include <utility>
#include <vector>

namespace viewer
{

template <typename T>
class MessageQueue
{
public:
    void push(T message)
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(message));
    }

    // Moves every queued message into `out` (which is cleared first).
    // Returns false if there was nothing to drain.
    bool drain(std::vector<T>& out)
    {
        out.clear();
        std::lock_guard lock(mutex_);
        if(pending_.empty())
        {
            return false;
        }
        std::swap(out, pending_);
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<T> pending_;
};

} // namespace viewer
