#include "process.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
extern char **environ;
#endif

namespace portablekit::app {

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string &text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// Quoting as CommandLineToArgvW reads it back.
std::wstring command_line(const std::vector<std::string> &argv) {
    std::wstring line;
    for (const std::string &argument : argv) {
        const std::wstring wide = widen(argument);
        if (!line.empty()) line += L' ';
        if (!wide.empty() && wide.find_first_of(L" \t\"") == std::wstring::npos) {
            line += wide;
            continue;
        }
        line += L'"';
        std::size_t backslashes = 0;
        for (const wchar_t c : wide) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            if (c == L'"') line.append(backslashes * 2 + 1, L'\\');
            else line.append(backslashes, L'\\');
            backslashes = 0;
            line += c;
        }
        line.append(backslashes * 2, L'\\');
        line += L'"';
    }
    return line;
}

ProcessResult run_windows(const std::vector<std::string> &argv, const std::filesystem::path &log, bool lower_priority,
                          bool wait, std::string *first_line) {
    ProcessResult result;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE read_end = nullptr;
    if (first_line != nullptr) {
        if (!CreatePipe(&read_end, &output, &inherit, 0)) return result;
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    } else if (!log.empty()) {
        output = CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (output != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION info{};
    std::wstring line = command_line(argv);
    DWORD flags = CREATE_NO_WINDOW;
    if (lower_priority) flags |= BELOW_NORMAL_PRIORITY_CLASS;
    if (!wait) flags |= DETACHED_PROCESS;
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup,
                                        &info);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) {
        result.error = "cannot start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")";
        if (read_end != nullptr) CloseHandle(read_end);
        return result;
    }
    result.started = true;
    if (first_line != nullptr) {
        std::string text;
        char buffer[512];
        DWORD got = 0;
        while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) text.append(buffer, got);
        CloseHandle(read_end);
        *first_line = text.substr(0, text.find_first_of("\r\n"));
    }
    if (wait) {
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        result.exit_code = static_cast<int>(code);
    } else {
        result.exit_code = 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

} // namespace

ProcessResult run_process(const std::vector<std::string> &argv, const std::filesystem::path &log, bool lower_priority) {
    return run_windows(argv, log, lower_priority, true, nullptr);
}

std::optional<std::string> capture_first_line(const std::vector<std::string> &argv) {
    std::string line;
    const ProcessResult result = run_windows(argv, {}, false, true, &line);
    if (!result.started || result.exit_code != 0) return std::nullopt;
    return line;
}

bool spawn_detached(const std::vector<std::string> &argv, const std::filesystem::path &log) {
    return run_windows(argv, log, true, false, nullptr).started;
}

std::uint64_t physical_memory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0u;
}

long current_process_id() { return static_cast<long>(GetCurrentProcessId()); }

bool process_alive(long pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

#else

namespace {

pid_t start_posix(const std::vector<std::string> &argv, int output_fd, bool lower_priority, bool new_session,
                  std::string &error) {
    std::vector<char *> args;
    for (const std::string &argument : argv) args.push_back(const_cast<char *>(argument.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO);
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#if defined(POSIX_SPAWN_SETSID)
    if (new_session) posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#else
    (void)new_session;
#endif
    pid_t pid = -1;
    const int status = posix_spawnp(&pid, argv[0].c_str(), &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
        error = "cannot start " + argv[0] + ": " + std::strerror(status);
        return -1;
    }
    // Behind the game: a compile must never make it stutter.
    if (lower_priority) setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
    return pid;
}

int wait_for(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

ProcessResult run_process(const std::vector<std::string> &argv, const std::filesystem::path &log, bool lower_priority) {
    ProcessResult result;
    int fd = -1;
    if (!log.empty()) {
        fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            result.error = "cannot write " + log.string();
            return result;
        }
    }
    const pid_t pid = start_posix(argv, fd, lower_priority, false, result.error);
    if (fd >= 0) ::close(fd);
    if (pid < 0) return result;
    result.started = true;
    result.exit_code = wait_for(pid);
    return result;
}

std::optional<std::string> capture_first_line(const std::vector<std::string> &argv) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return std::nullopt;
    std::string error;
    const pid_t pid = start_posix(argv, pipe_fds[1], false, false, error);
    ::close(pipe_fds[1]);
    if (pid < 0) {
        ::close(pipe_fds[0]);
        return std::nullopt;
    }
    std::string text;
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) text.append(buffer, static_cast<std::size_t>(got));
    ::close(pipe_fds[0]);
    if (wait_for(pid) != 0) return std::nullopt;
    return text.substr(0, text.find_first_of("\r\n"));
}

bool spawn_detached(const std::vector<std::string> &argv, const std::filesystem::path &log) {
    int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    std::string error;
    const pid_t pid = start_posix(argv, fd, true, true, error);
    if (fd >= 0) ::close(fd);
    if (pid < 0) return false;
    // Reaped by a thread so it never lingers as a zombie while we run.
    std::thread([pid] { (void)wait_for(pid); }).detach();
    return true;
}

std::uint64_t physical_memory() {
#if defined(__APPLE__)
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0u;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page_size > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size) : 0u;
#endif
}

long current_process_id() { return static_cast<long>(::getpid()); }

bool process_alive(long pid) { return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }

#endif

unsigned logical_cpus() {
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : count;
}

std::optional<std::filesystem::path> find_on_path(const std::string &name) {
    const char *path = std::getenv("PATH");
    if (path == nullptr) return std::nullopt;
#if defined(_WIN32)
    const char separator = ';';
    const std::vector<std::string> suffixes{".exe", ""};
#else
    const char separator = ':';
    const std::vector<std::string> suffixes{""};
#endif
    std::string entries = path;
    std::size_t start = 0;
    while (start <= entries.size()) {
        const std::size_t end = entries.find(separator, start);
        const std::string directory = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (const std::string &suffix : suffixes) {
            std::error_code ec;
            const std::filesystem::path candidate = std::filesystem::path(directory) / (name + suffix);
            if (!directory.empty() && std::filesystem::is_regular_file(candidate, ec)) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

} // namespace portablekit::app
