#include "viewer/media/ChildProcess.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace viewer::media
{

#if defined(_WIN32)

namespace
{

// Quoted as CommandLineToArgvW reads it.
std::wstring quoted(const std::wstring& arg)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for(wchar_t c : arg)
    {
        if(c == L'\\')
        {
            slashes++;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

std::wstring wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& program,
                                                  const std::vector<std::string>& args)
{
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr, ourIn = nullptr, ourOut = nullptr, childOut = nullptr;
    if(!CreatePipe(&childIn, &ourIn, &inherit, 0) || !CreatePipe(&ourOut, &childOut, &inherit, 0))
    {
        return nullptr;
    }
    SetHandleInformation(ourIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);
    std::wstring line = quoted(program.wstring());
    for(const std::string& a : args)
    {
        line += L" " + quoted(wide(a));
    }
    // Only these handles go to it (each must be inheritable: our standard
    // error, a copy of it).
    HANDLE error = nullptr;
    const HANDLE ours = GetStdHandle(STD_ERROR_HANDLE);
    if(ours && ours != INVALID_HANDLE_VALUE)
    {
        DuplicateHandle(GetCurrentProcess(), ours, GetCurrentProcess(), &error, 0, TRUE,
                        DUPLICATE_SAME_ACCESS);
    }
    HANDLE handles[] = {childIn, childOut, error};
    const DWORD count = error ? 3 : 2;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> list(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                              count * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn;
    si.StartupInfo.hStdOutput = childOut;
    si.StartupInfo.hStdError = error;
    si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    // In a job that ends with us: it, and the processes it starts, go when
    // we do, however we go. Started suspended, so it's in before it starts
    // any.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if(job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    const BOOL ok =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                       program.parent_path().wstring().c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if(error)
    {
        CloseHandle(error);
    }
    if(!ok)
    {
        CloseHandle(ourIn);
        CloseHandle(ourOut);
        if(job)
        {
            CloseHandle(job);
        }
        return nullptr;
    }
    if(job && !AssignProcessToJobObject(job, pi.hProcess))
    {
        CloseHandle(job); // (ours in a job that won't nest it: it ends by its input)
        job = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::unique_ptr<ChildProcess> p(new ChildProcess);
    p->job_ = job;
    p->process_ = pi.hProcess;
    p->input_ = ourIn;
    p->output_ = ourOut;
    return p;
}

void ChildProcess::finish()
{
    closeInput();
    if(process_ && WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0)
    {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 3000);
    }
}

ChildProcess::~ChildProcess()
{
    finish();
    if(output_)
    {
        CloseHandle(output_);
    }
    if(process_)
    {
        CloseHandle(process_);
    }
    if(job_)
    {
        CloseHandle(job_); // (it's gone by now; anything it left goes too)
    }
}

bool ChildProcess::write(std::span<const uint8_t> bytes)
{
    std::lock_guard lock(writing_);
    while(!bytes.empty() && input_)
    {
        DWORD wrote = 0;
        if(!WriteFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr))
        {
            return false;
        }
        bytes = bytes.subspan(wrote);
    }
    return bytes.empty();
}

size_t ChildProcess::read(std::span<uint8_t> into)
{
    DWORD got = 0;
    if(!ReadFile(output_, into.data(), static_cast<DWORD>(into.size()), &got, nullptr))
    {
        return 0;
    }
    return got;
}

void ChildProcess::closeInput()
{
    std::lock_guard lock(writing_);
    if(input_)
    {
        CloseHandle(input_);
        input_ = nullptr;
    }
}

bool ChildProcess::exited()
{
    return !process_ || WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
}

#else

std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& program,
                                                  const std::vector<std::string>& args)
{
    // Writing to a child that's gone mustn't end us: an error instead.
    static const bool ignored = (std::signal(SIGPIPE, SIG_IGN), true);
    (void)ignored;
    int in[2], out[2];
    if(pipe2(in, O_CLOEXEC) != 0)
    {
        return nullptr;
    }
    if(pipe2(out, O_CLOEXEC) != 0)
    {
        close(in[0]);
        close(in[1]);
        return nullptr;
    }
    // Its standard input and output are the pipes' ends (dup2 clears
    // close-on-exec on them); everything else of ours is close-on-exec.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    const std::string path = program.string();
    std::vector<std::string> owned{path};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for(std::string& a : owned)
    {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int error = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if(error != 0)
    {
        close(in[1]);
        close(out[0]);
        return nullptr;
    }
    std::unique_ptr<ChildProcess> p(new ChildProcess);
    p->pid_ = pid;
    p->input_ = in[1];
    p->output_ = out[0];
    return p;
}

void ChildProcess::finish()
{
    closeInput();
    using namespace std::chrono_literals;
    for(int i = 0; i < 60 && !exited(); i++)
    {
        std::this_thread::sleep_for(50ms);
    }
    if(!exited())
    {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        reaped_ = true;
    }
}

ChildProcess::~ChildProcess()
{
    finish();
    if(output_ >= 0)
    {
        close(output_);
    }
}

bool ChildProcess::write(std::span<const uint8_t> bytes)
{
    std::lock_guard lock(writing_);
    while(!bytes.empty() && input_ >= 0)
    {
        // A gone child is EPIPE here (start() ignores SIGPIPE).
        const ssize_t n = ::write(input_, bytes.data(), bytes.size());
        if(n < 0 && errno == EINTR)
        {
            continue;
        }
        if(n <= 0)
        {
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(n));
    }
    return bytes.empty();
}

size_t ChildProcess::read(std::span<uint8_t> into)
{
    for(;;)
    {
        const ssize_t n = ::read(output_, into.data(), into.size());
        if(n < 0 && errno == EINTR)
        {
            continue;
        }
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
}

void ChildProcess::closeInput()
{
    std::lock_guard lock(writing_);
    if(input_ >= 0)
    {
        close(input_);
        input_ = -1;
    }
}

bool ChildProcess::exited()
{
    if(reaped_)
    {
        return true;
    }
    if(pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) == pid_)
    {
        reaped_ = true;
    }
    return reaped_;
}

#endif

} // namespace viewer::media
