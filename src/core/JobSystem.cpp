#include "viewer/core/JobSystem.hpp"

#include <algorithm>

namespace viewer
{

JobSystem::JobSystem(unsigned threadCount)
{
    if(threadCount == 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        threadCount = std::max(1u, hw > 1 ? hw - 1 : 1u);
    }
    workers_.reserve(threadCount);
    for(unsigned i = 0; i < threadCount; i++)
    {
        workers_.emplace_back([this](std::stop_token stop) { workerLoop(stop); });
    }
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
    }
    for(auto& worker : workers_)
    {
        worker.request_stop();
    }
    workAvailable_.notify_all();
    workers_.clear(); // joins
}

void JobSystem::submit(Job job)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(job));
    }
    workAvailable_.notify_one();
}

void JobSystem::waitIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

size_t JobSystem::pendingCount() const
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void JobSystem::workerLoop(std::stop_token stop)
{
    while(true)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if(!workAvailable_.wait(lock, stop, [this] { return !queue_.empty(); }))
            {
                return; // stop requested
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            running_++;
        }

        job();

        {
            std::lock_guard lock(mutex_);
            running_--;
            if(queue_.empty() && running_ == 0)
            {
                idle_.notify_all();
            }
        }
    }
}

} // namespace viewer
