#pragma once

// Memory two processes see: one makes it under a name, the other opens it by
// that name. Named so the host can find it; the name is dropped once the
// host has it open (forget()), so nothing is left behind once both close
// it, and nobody else can open it then.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace viewer::media
{

class SharedMemory
{
public:
    // A new one of `size` bytes (zeroed), under a name of its own made from
    // `prefix`; null if it can't be made.
    static std::unique_ptr<SharedMemory> create(const std::string& prefix, size_t size);
    // The one with this name, `size` bytes of it; null if there's none.
    static std::unique_ptr<SharedMemory> open(const std::string& name, size_t size);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    const std::string& name() const { return name_; }
    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    // Its name is no longer needed (both have it open): nobody else can
    // open it now. (Windows: its name goes with the last handle anyway.)
    void forget();

private:
    SharedMemory() = default;
    std::string name_;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool named_ = false;
#if defined(_WIN32)
    void* handle_ = nullptr;
#endif
};

} // namespace viewer::media
