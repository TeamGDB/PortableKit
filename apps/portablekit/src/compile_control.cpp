#include "compile_control.hpp"

#include "app_home.hpp"
#include "process.hpp"
#include "text_format.hpp"

#include "app_paths.hpp"

#include <chrono>
#include <ctime>
#include <thread>

#if defined(_WIN32)
#else
#include <csignal>
#include <sys/types.h>
#endif

namespace portablekit::app {
namespace {

bool is_running(CorpusState state) {
    return state == CorpusState::Generating || state == CorpusState::Compiling || state == CorpusState::Linking;
}

std::string elapsed_text(const CorpusStatus &status) {
    if (status.started == 0) return {};
    const std::int64_t seconds = static_cast<std::int64_t>(std::time(nullptr)) - status.started;
    if (seconds < 0) return {};
    const std::int64_t minutes = seconds / 60, hours = minutes / 60;
    char text[32];
    if (hours != 0) std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                                  static_cast<long long>(minutes % 60), static_cast<long long>(seconds % 60));
    else std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(minutes), static_cast<long long>(seconds % 60));
    return text;
}

} // namespace

CompileActivity compile_activity(const GameRecord &game) {
    CompileActivity activity;
    const std::vector<int> levels = corpus_levels();  // best first
    for (const int level : levels) {
        const CorpusStatus status = current_status(game, level);
        if (is_running(status.state)) {
            activity.running = true;
            activity.status = status;
        } else if (status.state == CorpusState::Ready && !activity.ready_level) {
            activity.ready_level = level;
        }
    }
    activity.ready_best = activity.ready_level && !levels.empty() && *activity.ready_level == levels.front();
    if (activity.running) return activity;
    // A failure matters unless a level at least as good is ready.
    for (const int level : levels) {
        if (activity.ready_level && *activity.ready_level >= level) break;
        const CorpusStatus status = current_status(game, level);
        if (status.state == CorpusState::Failed) {
            activity.status = status;
            activity.failure = status.message.empty() ? std::string("The compile failed.") : status.message;
            break;
        }
    }
    return activity;
}

std::string activity_text(const CompileActivity &activity) {
    const CorpusStatus &status = activity.status;
    std::string text;
    switch (status.state) {
    case CorpusState::Generating: text = "Recompiling to C++"; break;
    case CorpusState::Compiling:
        text = "Compiling " + std::to_string(status.units_done) + "/" + std::to_string(status.units_total);
        break;
    case CorpusState::Linking: text = "Linking"; break;
    default: return {};
    }
    text += " at -O" + std::to_string(status.opt_level);
    const std::string elapsed = elapsed_text(status);
    if (!elapsed.empty()) text += ", " + elapsed;
    return text;
}

bool start_background_compile(const GameRecord &game, const std::string &level, unsigned jobs) {
    const std::filesystem::path self = portablekit::executable_path();
    if (self.empty()) return false;
    std::vector<std::string> command{path_text(self), "compile", game.id, "--opt", level};
    if (jobs != 0u) {
        command.push_back("--jobs");
        command.push_back(std::to_string(jobs));
    }
    const std::filesystem::path log_dir = cache_root() / game.id;
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    return spawn_detached(command, log_dir / "compile-process.log");
}

bool stop_compile(const GameRecord &game, std::string &error) {
    const CompileActivity activity = compile_activity(game);
    if (!activity.running || activity.status.pid <= 0) {
        error = "No compile of this game is running.";
        return false;
    }
    const long pid = activity.status.pid;
#if defined(_WIN32)
    // The compile, the recompiler and the compilers it started.
    const ProcessResult result =
        run_process({"taskkill", "/PID", std::to_string(pid), "/T", "/F"}, {}, false);
    if (!result.started) {
        error = result.error;
        return false;
    }
#else
    // The compile was started in a session of its own (spawn_detached): its
    // process group is its id, and holds everything it started.
    (void)::kill(-static_cast<pid_t>(pid), SIGTERM);
    for (int i = 0; i < 20 && process_alive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (process_alive(pid)) (void)::kill(-static_cast<pid_t>(pid), SIGKILL);
#endif
    for (int i = 0; i < 40 && process_alive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (process_alive(pid)) {
        error = "The compile (process " + std::to_string(pid) + ") did not stop.";
        return false;
    }
    return true;
}

} // namespace portablekit::app
