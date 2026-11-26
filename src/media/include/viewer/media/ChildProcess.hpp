#pragma once

// A program we start and talk to through its standard input and output
// (blocking pipes: SDL's are non-blocking). Its standard error is ours (its
// log goes with ours). Nothing else of ours is handed to it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace viewer::media
{

class ChildProcess
{
public:
    // Null if it can't be started.
    static std::unique_ptr<ChildProcess> start(const std::filesystem::path& program,
                                               const std::vector<std::string>& args);
    // finish(), then lets go of its output.
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Any thread (one at a time). False once it's gone.
    bool write(std::span<const uint8_t> bytes);
    // One thread: what it says, waiting for it; 0 once its output ended.
    size_t read(std::span<uint8_t> into);
    // No more input: it sees the end.
    void closeInput();
    // Whether it has exited (without waiting).
    bool exited();
    // Ends its input (it should exit), waits up to three seconds, then
    // kills it. Its output ends then, so a read() waiting returns.
    void finish();

private:
    ChildProcess() = default;
    std::mutex writing_;
#if defined(_WIN32)
    void* process_ = nullptr;
    void* input_ = nullptr;
    void* output_ = nullptr;
#else
    int pid_ = -1;
    int input_ = -1;
    int output_ = -1;
    bool reaped_ = false;
#endif
};

} // namespace viewer::media
