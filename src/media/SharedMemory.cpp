#include "viewer/media/SharedMemory.hpp"

#include <atomic>
#include <format>
#include <random>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace viewer::media
{

namespace
{

std::string uniqueName(const std::string& prefix)
{
    static std::atomic<uint32_t> counter{0};
    std::random_device random;
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
    return std::format("Local\\{}-{}-{}-{:08x}", prefix, pid, counter++, random());
#else
    return std::format("/{}-{}-{}-{:08x}", prefix, static_cast<long>(getpid()), counter++,
                       random());
#endif
}

} // namespace

#if defined(_WIN32)

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& prefix, size_t size)
{
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->name_ = uniqueName(prefix);
    m->size_ = size;
    const std::wstring wide(m->name_.begin(), m->name_.end()); // ASCII
    m->handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    static_cast<DWORD>(uint64_t{size} >> 32),
                                    static_cast<DWORD>(size & 0xFFFFFFFFu), wide.c_str());
    if(!m->handle_ || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return nullptr;
    }
    m->data_ = static_cast<uint8_t*>(MapViewOfFile(m->handle_, FILE_MAP_ALL_ACCESS, 0, 0, size));
    return m->data_ ? std::move(m) : nullptr;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, size_t size)
{
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->name_ = name;
    m->size_ = size;
    const std::wstring wide(name.begin(), name.end());
    m->handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
    if(!m->handle_)
    {
        return nullptr;
    }
    m->data_ = static_cast<uint8_t*>(MapViewOfFile(m->handle_, FILE_MAP_ALL_ACCESS, 0, 0, size));
    return m->data_ ? std::move(m) : nullptr;
}

SharedMemory::~SharedMemory()
{
    if(data_)
    {
        UnmapViewOfFile(data_);
    }
    if(handle_)
    {
        CloseHandle(handle_);
    }
}

void SharedMemory::forget() {}

#else

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& prefix, size_t size)
{
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->name_ = uniqueName(prefix);
    const int fd = shm_open(m->name_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if(fd < 0)
    {
        return nullptr;
    }
    m->named_ = true;
    if(ftruncate(fd, static_cast<off_t>(size)) != 0)
    {
        close(fd);
        return nullptr; // (the destructor forgets the name)
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(data == MAP_FAILED)
    {
        return nullptr;
    }
    m->data_ = static_cast<uint8_t*>(data);
    m->size_ = size;
    return m;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, size_t size)
{
    // Only ours: not a path anywhere else.
    if(name.size() < 2 || name[0] != '/' || name.find('/', 1) != std::string::npos)
    {
        return nullptr;
    }
    const int fd = shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    if(fd < 0)
    {
        return nullptr;
    }
    // No bigger than it is (asking for more would fault later).
    struct stat st{};
    if(fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < size)
    {
        close(fd);
        return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(data == MAP_FAILED)
    {
        return nullptr;
    }
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->name_ = name;
    m->data_ = static_cast<uint8_t*>(data);
    m->size_ = size;
    m->named_ = true;
    return m;
}

SharedMemory::~SharedMemory()
{
    if(data_)
    {
        munmap(data_, size_);
    }
    forget();
}

void SharedMemory::forget()
{
    if(named_)
    {
        shm_unlink(name_.c_str());
        named_ = false;
    }
}

#endif

} // namespace viewer::media
