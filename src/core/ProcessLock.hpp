#pragma once

// A lock held by one holder at a time across processes (and within one: each
// instance is its own holder). The system releases it if its process dies,
// so it's never left behind; the file itself stays.

#include <cstdint>
#include <filesystem>

namespace viewer
{

class ProcessLock
{
public:
    enum class State : uint8_t
    {
        Held,
        Busy,   // someone else has it
        Failed, // the file can't be opened, or can't be locked there
    };

    // Takes the lock if it's free; doesn't wait.
    explicit ProcessLock(const std::filesystem::path& file);
    ~ProcessLock();

    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;

    State state() const { return state_; }

private:
    State state_ = State::Failed;
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};

} // namespace viewer
