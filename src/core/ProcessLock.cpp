#include "viewer/core/ProcessLock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace viewer
{

#if defined(_WIN32)

ProcessLock::ProcessLock(const std::filesystem::path& file)
{
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(handle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    OVERLAPPED where{};
    if(LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &where))
    {
        handle_ = handle;
        state_ = State::Held;
        return;
    }
    state_ = GetLastError() == ERROR_LOCK_VIOLATION ? State::Busy : State::Failed;
    CloseHandle(handle);
}

ProcessLock::~ProcessLock()
{
    if(handle_)
    {
        OVERLAPPED where{};
        UnlockFileEx(handle_, 0, 1, 0, &where);
        CloseHandle(handle_);
    }
}

#else

ProcessLock::ProcessLock(const std::filesystem::path& file)
{
    // Close-on-exec: programs we start mustn't keep it held.
    const int descriptor = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if(descriptor < 0)
    {
        return;
    }
    // flock, not fcntl: its locks belong to this opening of the file, not
    // to the whole process.
    if(::flock(descriptor, LOCK_EX | LOCK_NB) == 0)
    {
        descriptor_ = descriptor;
        state_ = State::Held;
        return;
    }
    state_ = errno == EWOULDBLOCK || errno == EINTR ? State::Busy : State::Failed;
    ::close(descriptor);
}

ProcessLock::~ProcessLock()
{
    if(descriptor_ >= 0)
    {
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
    }
}

#endif

} // namespace viewer
