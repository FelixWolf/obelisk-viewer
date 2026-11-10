#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"

#include <doctest/doctest.h>

#include <atomic>

TEST_CASE("JobSystem runs every submitted job")
{
    viewer::JobSystem jobs(4);
    CHECK(jobs.threadCount() == 4);

    std::atomic<int> count{0};
    for(int i = 0; i < 1000; i++)
    {
        jobs.submit([&count] { count++; });
    }
    jobs.waitIdle();
    CHECK(count == 1000);
}

TEST_CASE("JobSystem destructor discards queued jobs without hanging")
{
    std::atomic<int> count{0};
    {
        viewer::JobSystem jobs(1);
        for(int i = 0; i < 100; i++)
        {
            jobs.submit([&count] { count++; });
        }
    }
    CHECK(count <= 100);
}

TEST_CASE("MessageQueue delivers messages from many producers")
{
    viewer::MessageQueue<int> queue;
    {
        viewer::JobSystem jobs(4);
        for(int i = 0; i < 500; i++)
        {
            jobs.submit([&queue, i] { queue.push(i); });
        }
        jobs.waitIdle();
    }

    std::vector<int> drained;
    CHECK(queue.drain(drained));
    CHECK(drained.size() == 500);
    CHECK_FALSE(queue.drain(drained));
    CHECK(drained.empty());
}
