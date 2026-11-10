#pragma once

// Fixed pool of worker threads for CPU-heavy work (decoding, compression, disk
// I/O). Owned by App and passed by reference to whoever submits work; there is
// no global pool.
//
// Jobs must not touch state owned by another thread. They receive their inputs
// by value and report results through a MessageQueue.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace viewer
{

class JobSystem
{
public:
    using Job = std::function<void()>;

    // threadCount == 0 picks hardware_concurrency() - 1 (leaving a core for
    // the main thread), with a minimum of one.
    explicit JobSystem(unsigned threadCount = 0);

    // Discards jobs that have not started, then waits for running jobs.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void submit(Job job);

    // Blocks until the queue is empty and no job is running.
    void waitIdle();

    unsigned threadCount() const { return static_cast<unsigned>(workers_.size()); }
    size_t pendingCount() const;

private:
    void workerLoop(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any workAvailable_;
    std::condition_variable idle_;
    std::deque<Job> queue_;
    size_t running_ = 0;
    std::vector<std::jthread> workers_; // last: joined before the rest is destroyed
};

} // namespace viewer
