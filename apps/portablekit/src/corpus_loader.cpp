#include "corpus_loader.hpp"

#include "corpus.hpp"
#include "text_format.hpp"

#include "corpus_abi.hpp"

#include "psprecomp/runtime.hpp"

#if defined(PORTABLEKIT_HAS_RENDERER)
#include "imgui.h"
#include "ui/layer.hpp"
#include "ui/ui.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace portablekit::app {
namespace {

using Clock = std::chrono::steady_clock;

struct LoaderState {
    std::optional<GameRecord> game;
    CorpusChoice choice;
    psprecomp::Runtime *runtime{};
    LoadedCorpus loaded;
    Clock::time_point last_check{};
    Clock::time_point switched_at{};
    std::mutex line_lock;
    std::string line;          // what the overlay shows
    bool compiling{};
};

LoaderState &state() {
    static LoaderState value;
    return value;
}

using AbiFn = const char *(*)();
using RegisterFn = void (*)(psprecomp::CorpusRuntime &);

void *open_library(const std::filesystem::path &path, std::string &error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (module == nullptr) error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void *>(module);
#else
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) error = dlerror();
    return handle;
#endif
}

void *symbol(void *library, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

// A corpus library opened and checked, not yet registered.
struct OpenedCorpus {
    int opt_level{};
    std::filesystem::path library;
    RegisterFn register_all{};
};

// Opens one corpus library and checks it is for this build and this game.
// Safe on any thread: nothing reaches the runtime. Libraries are never
// unloaded: a replaced corpus's code may still be what a return address points
// into until the guest leaves it.
std::optional<OpenedCorpus> open_corpus(const ReadyCorpus &corpus) {
    LoaderState &s = state();
    std::string error;
    void *library = open_library(corpus.library, error);
    if (library == nullptr) {
        std::cerr << "[corpus] cannot load " << corpus.library.string() << ": " << error << "\n";
        return std::nullopt;
    }
    const auto abi = reinterpret_cast<AbiFn>(symbol(library, "portablekit_corpus_abi"));
    const auto executable = reinterpret_cast<AbiFn>(symbol(library, "portablekit_corpus_executable"));
    const auto register_all = reinterpret_cast<RegisterFn>(symbol(library, "portablekit_corpus_register"));
    if (abi == nullptr || executable == nullptr || register_all == nullptr) {
        std::cerr << "[corpus] " << corpus.library.string() << " is not a corpus library\n";
        return std::nullopt;
    }
    if (std::string(abi()) != kCorpusAbi) {
        std::cerr << "[corpus] " << corpus.library.string() << " was compiled for another build\n";
        return std::nullopt;
    }
    if (std::string(executable()) != s.game->executable_sha256) {
        std::cerr << "[corpus] " << corpus.library.string() << " was compiled from another executable\n";
        return std::nullopt;
    }
    return OpenedCorpus{corpus.opt_level, corpus.library, register_all};
}

// Registers an opened corpus's code: at start, or at a dispatch boundary.
void register_corpus(psprecomp::Runtime &runtime, const OpenedCorpus &corpus) {
    LoaderState &s = state();
    const auto start = Clock::now();
    corpus.register_all(runtime);
    s.loaded.opt_level = corpus.opt_level;
    s.loaded.register_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "[corpus] loaded -O" << corpus.opt_level << " code from " << corpus.library.string() << " ("
              << runtime.function_count() << " functions, registered in " << s.loaded.register_seconds << " s)"
              << std::endl;
}

bool load(psprecomp::Runtime &runtime, const ReadyCorpus &corpus) {
    const auto opened = open_corpus(corpus);
    if (!opened) return false;
    register_corpus(runtime, *opened);
    return true;
}

// Filled by the watcher thread, taken at a dispatch boundary.
std::mutex g_pending_lock;
std::optional<OpenedCorpus> g_pending;
std::atomic<bool> g_pending_ready{false};
std::atomic<int> g_opened_level{-1};

void refresh_line() {
    LoaderState &s = state();
    std::string line;
    bool compiling = false;
    for (const int level : corpus_levels()) {
        if (level <= s.loaded.opt_level) break;
        const CorpusStatus status = current_status(*s.game, level);
        if (status.state == CorpusState::Generating) {
            line = "Preparing to compile the game";
            compiling = true;
        } else if (status.state == CorpusState::Compiling && status.units_total != 0u) {
            char text[96];
            std::snprintf(text, sizeof(text), "Compiling the game: %u%%",
                          status.units_done * 100u / status.units_total);
            line = text;
            compiling = true;
        } else if (status.state == CorpusState::Linking) {
            line = "Compiling the game: linking";
            compiling = true;
        }
        if (compiling) break;
    }
    if (compiling)
        line += s.loaded.opt_level < 0 ? " - playing under the interpreter (slow) until it is done"
                                        : " - playing compiled code; a faster build follows";
    const std::lock_guard<std::mutex> guard(s.line_lock);
    s.line = line;
    s.compiling = compiling;
}

// Every few thousand dispatches, at a dispatch boundary: no generated code is
// on the stack and ctx.pc is the next address to run, so registering a corpus
// here makes the very next dispatch use it.
std::atomic<bool> g_stop_requested{false};
bool g_watching = false;

void heartbeat(std::uint64_t, std::uint32_t) {
    LoaderState &s = state();
    // A bounded run (--seconds) ends here, at a dispatch boundary, so the
    // port reports its threads and the interpreter's profile as it stops.
    if (g_stop_requested.load(std::memory_order_relaxed) && s.runtime != nullptr) {
        s.runtime->stop("run time limit reached (--seconds)");
        return;
    }
    if (!g_watching) return;
    // Opening a library of hundreds of megabytes takes seconds (measured: a
    // 4.4 s frame when it was done here), so the watcher thread opens it and
    // this only registers it.
    if (g_pending_ready.load(std::memory_order_acquire) && s.runtime != nullptr) {
        std::optional<OpenedCorpus> corpus;
        {
            const std::lock_guard<std::mutex> guard(g_pending_lock);
            corpus = std::move(g_pending);
            g_pending.reset();
            g_pending_ready = false;
        }
        if (corpus && corpus->opt_level > s.loaded.opt_level) {
            register_corpus(*s.runtime, *corpus);
            ++s.loaded.switches;
            s.switched_at = Clock::now();
        }
    }
    const auto now = Clock::now();
    if (now - s.last_check < std::chrono::seconds(1)) return;
    s.last_check = now;
    refresh_line();
}

// Looks at the cache once a second and opens a better corpus when a compile
// has finished one.
void watch() {
    LoaderState &s = state();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_pending_ready.load(std::memory_order_acquire)) continue;
        const std::optional<ReadyCorpus> ready = ready_corpus(*s.game);
        if (!ready || ready->opt_level <= g_opened_level.load()) continue;
        if (auto opened = open_corpus(*ready)) {
            g_opened_level = opened->opt_level;
            const std::lock_guard<std::mutex> guard(g_pending_lock);
            g_pending = std::move(opened);
            g_pending_ready.store(true, std::memory_order_release);
        } else {
            g_opened_level = ready->opt_level;  // do not try a broken library again
        }
        if (g_opened_level >= corpus_levels().front()) return;
    }
}

#if defined(PORTABLEKIT_HAS_RENDERER)
bool overlay_visible() {
    LoaderState &s = state();
    if (s.switched_at != Clock::time_point{} && Clock::now() - s.switched_at < std::chrono::seconds(5)) return true;
    const std::lock_guard<std::mutex> guard(s.line_lock);
    return s.compiling;
}

void overlay_draw() {
    LoaderState &s = state();
    std::string text;
    if (s.switched_at != Clock::time_point{} && Clock::now() - s.switched_at < std::chrono::seconds(5)) {
        text = "Switched to compiled code (-O" + std::to_string(s.loaded.opt_level) + ")";
    } else {
        const std::lock_guard<std::mutex> guard(s.line_lock);
        text = s.line;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float pad = ui::Layer::get().font_size() * 0.5f;
    ImGui::SetNextWindowPos({viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad}, 0,
                            {0.0f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##portablekit-status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}
#endif

} // namespace

void request_stop() { g_stop_requested = true; }

void prepare_corpus_loading(const GameRecord &game, const CorpusChoice &choice) {
    state().game = game;
    state().choice = choice;
}

LoadedCorpus loaded_corpus() { return state().loaded; }

std::string corpus_status_line() {
    refresh_line();
    const std::lock_guard<std::mutex> guard(state().line_lock);
    return state().line;
}

} // namespace portablekit::app

namespace psprecomp {

// The framework calls this once, before the game runs (host/main.cpp). The
// program links no generated code of its own: it loads the player's.
void register_generated_functions(CorpusRuntime &corpus) {
    using namespace portablekit::app;
    auto &runtime = static_cast<Runtime &>(corpus);
    LoaderState &s = state();
    s.runtime = &runtime;
    // Checked every few thousand dispatches, which is many times a second
    // whether the interpreter or compiled code runs; heartbeat() itself looks
    // at the cache at most once a second.
    set_runtime_heartbeat_hook(&heartbeat, 4096u);
    if (!s.game || s.choice.interpreter_only) {
        std::cout << "[corpus] not loading compiled code; the game runs under the interpreter\n";
        return;
    }
    if (const auto corpus = ready_corpus(*s.game)) (void)load(runtime, *corpus);
    if (!s.choice.watch || s.loaded.opt_level >= corpus_levels().front()) return;
    g_opened_level = s.loaded.opt_level;
    g_watching = true;
    refresh_line();
    std::thread(&watch).detach();
#if defined(PORTABLEKIT_HAS_RENDERER)
    portablekit::ui::set_status_overlay(&overlay_visible, &overlay_draw);
#endif
}

} // namespace psprecomp
