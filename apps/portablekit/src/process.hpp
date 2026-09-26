#pragma once

// Starting the recompiler and the compiler, and waiting for them. Output goes
// to a log file; nothing is read back but the exit status.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::app {

struct ProcessResult {
    bool started{};
    int exit_code{-1};
    std::string error;  // why it could not start
};

// Runs argv[0] with the arguments and waits. stdout and stderr are appended
// to `log` (none: inherited). `lower_priority` asks the system to run it
// behind the game.
ProcessResult run_process(const std::vector<std::string> &argv, const std::filesystem::path &log,
                          bool lower_priority = true);

// Runs a command and returns its first line of output, or nothing when it
// failed: for asking `xcrun --find clang++` and the like.
std::optional<std::string> capture_first_line(const std::vector<std::string> &argv);

// Starts this program again, detached, with the arguments; for a compile that
// outlives the window that asked for it.
bool spawn_detached(const std::vector<std::string> &argv, const std::filesystem::path &log);

// Physical memory in bytes, 0 when unknown.
[[nodiscard]] std::uint64_t physical_memory();
[[nodiscard]] unsigned logical_cpus();

// Process id of this program, and whether a process id is still running.
[[nodiscard]] long current_process_id();
[[nodiscard]] bool process_alive(long pid);

// Finds a program on PATH.
[[nodiscard]] std::optional<std::filesystem::path> find_on_path(const std::string &name);

} // namespace portablekit::app
