#include "corpus.hpp"

#include "app_home.hpp"
#include "process.hpp"
#include "text_format.hpp"

#include "corpus_abi.hpp"

#include "app_paths.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace portablekit::app {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Exit codes (app_main.cpp lists them all).
constexpr int kNoToolchain = 20;
constexpr int kRecompileFailed = 21;
constexpr int kCompileFailed = 22;
constexpr int kLinkFailed = 23;
constexpr int kAlreadyCompiling = 24;

std::string abi_prefix() { return std::string(kCorpusAbi).substr(0, 16); }

std::string now_text() {
    const std::time_t now = std::time(nullptr);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return text;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

const char *state_key(CorpusState state) { return state_name(state); }

CorpusState state_from(const std::string &text) {
    for (CorpusState state : {CorpusState::Generating, CorpusState::Compiling, CorpusState::Linking,
                              CorpusState::Ready, CorpusState::Failed})
        if (text == state_name(state)) return state;
    return CorpusState::None;
}

void write_status(const CorpusPaths &paths, const CorpusStatus &status) {
    KeyValues values{
        {"state", state_key(status.state)},
        {"opt_level", std::to_string(status.opt_level)},
        {"jobs", std::to_string(status.jobs)},
        {"units_total", std::to_string(status.units_total)},
        {"units_done", std::to_string(status.units_done)},
        {"pid", std::to_string(status.pid)},
        {"generate_seconds", std::to_string(status.generate_seconds)},
        {"compile_seconds", std::to_string(status.compile_seconds)},
        {"link_seconds", std::to_string(status.link_seconds)},
        {"library_bytes", std::to_string(status.library_bytes)},
        {"peak_unit_rss", std::to_string(status.peak_unit_rss)},
        {"message", status.message},
        {"toolchain", status.toolchain},
        {"abi", kCorpusAbi},
        {"updated", now_text()},
    };
    (void)write_key_values(paths.status, values, "# Written by portablekit compile; read by the running game.\n");
}

std::uint64_t children_peak_rss() {
#if defined(_WIN32)
    return 0u;
#else
    rusage usage{};
    if (getrusage(RUSAGE_CHILDREN, &usage) != 0) return 0u;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

std::uint64_t directory_bytes(const fs::path &root) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code size_ec;
        if (it->is_regular_file(size_ec)) total += it->file_size(size_ec);
    }
    return total;
}

// The command prefix that runs the compiler, and the flags every unit gets.
struct CompilerCommand {
    std::vector<std::string> prefix;
    bool msvc_style{};
};

CompilerCommand command_for(const Toolchain &toolchain) {
    CompilerCommand command;
#if defined(__APPLE__)
    if (toolchain.kind == "apple-clang") {
        // xcrun picks the SDK, which a bare clang++ from the tools would not find.
        command.prefix = {"/usr/bin/xcrun", "clang++"};
        return command;
    }
#endif
    command.prefix = {toolchain.compiler};
    command.msvc_style = toolchain.kind == "clang-cl";
    return command;
}

std::vector<std::string> compile_arguments(const CompilerCommand &command, int opt_level, const fs::path &source,
                                           const fs::path &object, const fs::path &generated) {
    std::vector<std::string> args = command.prefix;
    const std::string include = path_text(corpus_include_directory());
    if (command.msvc_style) {
        args.insert(args.end(), {"/nologo", "/std:c++20", "/EHsc", "/bigobj", "/c"});
        args.push_back(opt_level == 0 ? "/Od" : "/O" + std::to_string(std::min(opt_level, 2)));
        args.push_back("/I" + include);
        args.push_back("/I" + path_text(generated));
        for (const char *const *definition = kCorpusDefinitions; *definition != nullptr; ++definition)
            args.push_back(std::string("/D") + (*definition + 2));
        args.push_back("/Dregister_generated_functions=portablekit_corpus_register_all");
        args.push_back("/DPSPRECOMP_IMPORT_HOST_SYMBOLS=1");
        args.push_back(path_text(source));
        args.push_back("/Fo" + path_text(object));
        return args;
    }
    args.insert(args.end(), {"-std=c++20", "-O" + std::to_string(opt_level), "-g0", "-fvisibility=hidden",
                             "-w", "-I", include, "-I", path_text(generated)});
#if !defined(_WIN32)
    args.push_back("-fPIC");
#endif
    for (const char *const *definition = kCorpusDefinitions; *definition != nullptr; ++definition)
        args.push_back(*definition);
    // The corpus's registry is called through the library's own entry point,
    // never by this name, which the program itself defines as the loader.
    args.push_back("-Dregister_generated_functions=portablekit_corpus_register_all");
#if defined(_WIN32)
    args.push_back("-DPSPRECOMP_IMPORT_HOST_SYMBOLS=1");
#endif
    args.insert(args.end(), {"-c", path_text(source), "-o", path_text(object)});
    return args;
}

std::vector<std::string> link_arguments(const CompilerCommand &command, const fs::path &library,
                                        const std::vector<fs::path> &objects) {
    std::vector<std::string> args = command.prefix;
#if defined(__APPLE__)
    // A bundle whose undefined symbols (the runtime) bind to the executable
    // that loads it.
    args.insert(args.end(), {"-bundle", "-bundle_loader", path_text(portablekit::executable_path())});
#elif defined(_WIN32)
    // Against the program's import library, shipped beside it.
    if (command.msvc_style) {
        args.insert(args.end(), {"/LD", "/Fe" + path_text(library)});
    } else {
        args.insert(args.end(), {"-shared"});
    }
#else
    args.insert(args.end(), {"-shared"});
#endif
    if (!command.msvc_style) args.insert(args.end(), {"-o", path_text(library)});
    for (const fs::path &object : objects) args.push_back(path_text(object));
#if defined(_WIN32)
    const fs::path import_library = portablekit::executable_directory() /
                                    (command.msvc_style ? "portablekit.lib" : "libportablekit.dll.a");
    args.push_back(path_text(import_library));
#endif
    return args;
}

// The library's entry points: which build and which game it was made for,
// and the call that registers its code.
std::string entry_source(const GameRecord &game) {
    return std::string(
               "// Written by portablekit compile. The corpus library's entry points.\n"
               "#include \"psprecomp/runtime.hpp\"\n\n"
               "namespace psprecomp {\n"
               "void portablekit_corpus_register_all(Runtime &runtime);\n"
               "}\n\n"
               "#if defined(_WIN32)\n#define PORTABLEKIT_CORPUS_EXPORT __declspec(dllexport)\n"
               "#else\n#define PORTABLEKIT_CORPUS_EXPORT __attribute__((visibility(\"default\")))\n#endif\n\n"
               "extern \"C\" PORTABLEKIT_CORPUS_EXPORT const char *portablekit_corpus_abi() { return \"") +
           kCorpusAbi +
           "\"; }\n"
           "extern \"C\" PORTABLEKIT_CORPUS_EXPORT const char *portablekit_corpus_executable() { return \"" +
           game.executable_sha256 +
           "\"; }\n"
           "extern \"C\" PORTABLEKIT_CORPUS_EXPORT void portablekit_corpus_register(psprecomp::Runtime &runtime) {\n"
           "    psprecomp::portablekit_corpus_register_all(runtime);\n}\n";
}

} // namespace

const char *state_name(CorpusState state) {
    switch (state) {
    case CorpusState::None: return "none";
    case CorpusState::Generating: return "generating";
    case CorpusState::Compiling: return "compiling";
    case CorpusState::Linking: return "linking";
    case CorpusState::Ready: return "ready";
    case CorpusState::Failed: return "failed";
    }
    return "none";
}

std::string library_extension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

Toolchain find_toolchain() {
    Toolchain toolchain;
    if (const char *forced = std::getenv("PORTABLEKIT_CXX"); forced != nullptr && *forced != '\0') {
        toolchain.found = true;
        toolchain.compiler = forced;
        toolchain.kind = std::string(forced).find("clang-cl") != std::string::npos ? "clang-cl" : "clang";
        toolchain.version = capture_first_line({forced, "--version"}).value_or("");
        return toolchain;
    }
#if defined(__APPLE__)
    // Apple's Command Line Tools (or Xcode) are the compiler on a Mac. Asking
    // xcrun without them opens the system's install prompt, so ask
    // xcode-select first, which only answers.
    if (!capture_first_line({"/usr/bin/xcode-select", "-p"})) {
        toolchain.problem = "Compiling needs Apple's Command Line Tools, which are free. Install them with "
                            "\"xcode-select --install\" in Terminal (or from the prompt this program can open), "
                            "then try again.";
        return toolchain;
    }
    if (const auto path = capture_first_line({"/usr/bin/xcrun", "--find", "clang++"})) {
        toolchain.found = true;
        toolchain.kind = "apple-clang";
        toolchain.compiler = *path;
        toolchain.version = capture_first_line({"/usr/bin/xcrun", "clang++", "--version"}).value_or("");
        return toolchain;
    }
    toolchain.problem = "The Command Line Tools are installed but have no clang++. Reinstall them with "
                        "\"xcode-select --install\".";
    return toolchain;
#elif defined(_WIN32)
    // A toolchain shipped beside the program (llvm-mingw, see
    // docs/DESKTOP_APP.md), then one on PATH.
    const fs::path bundled = portablekit::executable_directory() / "toolchain" / "bin" / "clang++.exe";
    std::error_code ec;
    if (fs::is_regular_file(bundled, ec)) {
        toolchain.found = true;
        toolchain.kind = "llvm-mingw";
        toolchain.compiler = path_text(bundled);
        toolchain.version = capture_first_line({toolchain.compiler, "--version"}).value_or("");
        return toolchain;
    }
    for (const char *name : {"clang++", "clang-cl"}) {
        if (const auto found = find_on_path(name)) {
            toolchain.found = true;
            toolchain.kind = std::string(name) == "clang-cl" ? "clang-cl" : "clang";
            toolchain.compiler = path_text(*found);
            toolchain.version = capture_first_line({toolchain.compiler, "--version"}).value_or("");
            return toolchain;
        }
    }
    toolchain.problem = "No compiler was found. This build expects its own toolchain in toolchain\\ next to "
                        "portablekit.exe.";
    return toolchain;
#else
    for (const char *name : {"c++", "g++", "clang++"}) {
        if (const auto found = find_on_path(name)) {
            toolchain.found = true;
            toolchain.compiler = path_text(*found);
            toolchain.version = capture_first_line({toolchain.compiler, "--version"}).value_or("");
            toolchain.kind = toolchain.version.find("clang") != std::string::npos ? "clang" : "gcc";
            return toolchain;
        }
    }
    toolchain.problem = "No C++ compiler was found. Install your distribution's (for example "
                        "\"sudo apt install g++\"), then try again.";
    return toolchain;
#endif
}

CorpusPaths corpus_paths(const GameRecord &game, int opt_level) {
    CorpusPaths paths;
    paths.root = cache_root() / game.id / (abi_prefix() + "-O" + std::to_string(opt_level));
    paths.generated = paths.root / "generated";
    paths.objects = paths.root / "objects";
    paths.library = paths.root / ("corpus" + library_extension());
    paths.status = paths.root / "status.txt";
    paths.log = paths.root / "build.log";
    return paths;
}

std::vector<int> corpus_levels() { return {2, 1, 0}; }

CorpusStatus read_status(const CorpusPaths &paths) {
    CorpusStatus status;
    const KeyValues values = read_key_values(paths.status);
    if (values.empty()) return status;
    const auto get = [&](const char *key) {
        const auto it = values.find(key);
        return it != values.end() ? it->second : std::string();
    };
    status.state = state_from(get("state"));
    status.opt_level = std::atoi(get("opt_level").c_str());
    status.jobs = static_cast<unsigned>(std::atoi(get("jobs").c_str()));
    status.units_total = static_cast<unsigned>(std::atoi(get("units_total").c_str()));
    status.units_done = static_cast<unsigned>(std::atoi(get("units_done").c_str()));
    status.pid = std::atol(get("pid").c_str());
    status.generate_seconds = std::atof(get("generate_seconds").c_str());
    status.compile_seconds = std::atof(get("compile_seconds").c_str());
    status.link_seconds = std::atof(get("link_seconds").c_str());
    status.library_bytes = std::strtoull(get("library_bytes").c_str(), nullptr, 10);
    status.peak_unit_rss = std::strtoull(get("peak_unit_rss").c_str(), nullptr, 10);
    status.message = get("message");
    status.toolchain = get("toolchain");
    status.updated = get("updated");
    if (get("abi") != kCorpusAbi) status.state = CorpusState::None;
    return status;
}

CorpusStatus current_status(const GameRecord &game, int opt_level) {
    const CorpusPaths paths = corpus_paths(game, opt_level);
    CorpusStatus status = read_status(paths);
    const bool running = status.state == CorpusState::Generating || status.state == CorpusState::Compiling ||
                         status.state == CorpusState::Linking;
    if (running && !process_alive(status.pid)) {
        status.state = CorpusState::Failed;
        status.message = "The compile was interrupted. Start it again to continue where it stopped.";
    }
    std::error_code ec;
    if (status.state == CorpusState::Ready && !fs::is_regular_file(paths.library, ec)) status.state = CorpusState::None;
    return status;
}

std::optional<ReadyCorpus> ready_corpus(const GameRecord &game) {
    // PORTABLEKIT_MAX_OPT caps the level loaded, for comparing levels.
    const char *cap = std::getenv("PORTABLEKIT_MAX_OPT");
    const int max_level = cap != nullptr && *cap != '\0' ? std::atoi(cap) : 99;
    for (const int level : corpus_levels()) {
        if (level > max_level) continue;
        if (current_status(game, level).state == CorpusState::Ready)
            return ReadyCorpus{level, corpus_paths(game, level).library};
    }
    return std::nullopt;
}

unsigned default_jobs() {
    // A unit's compiler peaked at 0.5 GB at -O2 with clang (measured,
    // docs/DESKTOP_APP.md); allow a gigabyte each, and leave room for the
    // game and the system. Half the cores, so the game keeps its own.
    const std::uint64_t memory = physical_memory();
    const std::uint64_t per_job = 1ull << 30;
    std::uint64_t by_memory = memory > (3ull << 30) ? (memory - (3ull << 30)) / per_job : 1u;
    if (by_memory < 1u) by_memory = 1u;
    const unsigned cpus = logical_cpus();
    const unsigned by_cpus = cpus >= 2u ? cpus / 2u : 1u;
    return static_cast<unsigned>(std::min<std::uint64_t>(by_memory, by_cpus));
}

int compile_corpus(const GameRecord &game, const CompileOptions &options) {
    const CorpusPaths paths = corpus_paths(game, options.opt_level);
    std::error_code ec;
    fs::create_directories(paths.root, ec);

    CorpusStatus status = current_status(game, options.opt_level);
    const bool running = status.state == CorpusState::Generating || status.state == CorpusState::Compiling ||
                         status.state == CorpusState::Linking;
    if (running && status.pid != current_process_id()) {
        status.message = "Another compile of this game is already running (process " + std::to_string(status.pid) + ").";
        if (options.progress) options.progress(status);
        return kAlreadyCompiling;
    }
    status = CorpusStatus{};
    status.opt_level = options.opt_level;
    status.pid = current_process_id();
    status.jobs = options.jobs != 0u ? options.jobs : default_jobs();
    std::mutex status_lock;
    const auto publish = [&] {
        write_status(paths, status);
        if (options.progress) options.progress(status);
    };
    const auto fail = [&](int code, const std::string &message) {
        status.state = CorpusState::Failed;
        status.message = message;
        publish();
        return code;
    };

    const Toolchain toolchain = find_toolchain();
    if (!toolchain.found) return fail(kNoToolchain, toolchain.problem);
    status.toolchain = toolchain.kind + ": " + toolchain.version;
    {
        std::ofstream log(paths.log, std::ios::app);
        log << "=== " << now_text() << " compile " << game.id << " at -O" << options.opt_level << " with "
            << status.jobs << " job(s)\n=== " << status.toolchain << "\n";
    }

    // 1. The recompiler: the executable to C++.
    status.state = CorpusState::Generating;
    status.message = "Recompiling the game's code to C++";
    publish();
    const auto generate_start = Clock::now();
    if (!options.reuse_generated.empty() && fs::is_directory(options.reuse_generated, ec) &&
        !fs::exists(paths.generated, ec)) {
        fs::rename(options.reuse_generated, paths.generated, ec);
    }
    if (!fs::exists(paths.generated / "generated_registry.cpp", ec)) {
        fs::create_directories(paths.generated, ec);
        const ProcessResult generated = run_process(
            {path_text(recompiler_path()), path_text(game.data_dir / "EBOOT.ELF"), "--auto", path_text(paths.generated)},
            paths.log);
        if (!generated.started) return fail(kRecompileFailed, generated.error);
        if (generated.exit_code != 0)
            return fail(kRecompileFailed, "The recompiler failed (exit " + std::to_string(generated.exit_code) +
                                              "); see " + path_text(paths.log) + ".");
    }
    status.generate_seconds = seconds_since(generate_start);
    {
        std::ofstream entry(paths.generated / "portablekit_corpus_entry.cpp", std::ios::trunc);
        entry << entry_source(game);
    }

    // 2. The compiler: one object per unit, the largest first so the last
    // ones to finish are short.
    std::vector<fs::path> sources;
    for (const auto &entry : fs::directory_iterator(paths.generated, ec))
        if (entry.path().extension() == ".cpp") sources.push_back(entry.path());
    std::sort(sources.begin(), sources.end(), [](const fs::path &a, const fs::path &b) {
        std::error_code size_ec;
        return fs::file_size(a, size_ec) > fs::file_size(b, size_ec);
    });
    fs::create_directories(paths.objects, ec);
    status.state = CorpusState::Compiling;
    status.units_total = static_cast<unsigned>(sources.size());
    status.message = "Compiling";
    publish();
    const CompilerCommand command = command_for(toolchain);
    const auto compile_start = Clock::now();
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::string failure;
    std::vector<fs::path> objects(sources.size());
    const auto worker = [&] {
        for (;;) {
            const std::size_t index = next.fetch_add(1);
            if (index >= sources.size() || failed) return;
            const fs::path &source = sources[index];
            const fs::path object = paths.objects / (source.stem().string() + ".o");
            objects[index] = object;
            std::error_code time_ec;
            // An object newer than its source is kept: an interrupted compile
            // continues where it stopped, and psp_recomp rewrites only units
            // whose text changed.
            const bool current = fs::exists(object, time_ec) &&
                                 fs::last_write_time(object, time_ec) >= fs::last_write_time(source, time_ec);
            if (!current) {
                const ProcessResult result =
                    run_process(compile_arguments(command, options.opt_level, source, object, paths.generated),
                                paths.log);
                if (!result.started || result.exit_code != 0) {
                    const std::lock_guard<std::mutex> guard(status_lock);
                    if (!failed) failure = result.started ? "Compiling " + source.filename().string() + " failed; see " + path_text(paths.log) + "." : result.error;
                    failed = true;
                    fs::remove(object, time_ec);
                    return;
                }
            }
            const std::lock_guard<std::mutex> guard(status_lock);
            ++status.units_done;
            status.compile_seconds = seconds_since(compile_start);
            status.peak_unit_rss = std::max(status.peak_unit_rss, children_peak_rss());
            publish();
        }
    };
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < status.jobs; ++i) workers.emplace_back(worker);
    for (std::thread &thread : workers) thread.join();
    status.compile_seconds = seconds_since(compile_start);
    if (failed) return fail(kCompileFailed, failure);

    // 3. The linker: one library.
    status.state = CorpusState::Linking;
    status.message = "Linking";
    publish();
    const auto link_start = Clock::now();
    fs::path partial = paths.library;
    partial += ".part";
    const ProcessResult linked = run_process(link_arguments(command, partial, objects), paths.log);
    status.link_seconds = seconds_since(link_start);
    if (!linked.started || linked.exit_code != 0)
        return fail(kLinkFailed, linked.started ? "Linking failed; see " + path_text(paths.log) + "." : linked.error);
    fs::rename(partial, paths.library, ec);
    if (ec) return fail(kLinkFailed, "Cannot move the library into place: " + ec.message());
    status.library_bytes = fs::file_size(paths.library, ec);

    if (!options.keep_intermediates) {
        fs::remove_all(paths.objects, ec);
        if (!options.keep_generated) fs::remove_all(paths.generated, ec);
    }
    status.state = CorpusState::Ready;
    status.pid = 0;
    status.message = "Ready";
    publish();
    // Corpora of other builds of the program can never be loaded again by
    // this one: once it has its own, their space is given back.
    for (const auto &entry : fs::directory_iterator(cache_root() / game.id, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && !name.starts_with(abi_prefix())) fs::remove_all(entry.path(), ec);
    }
    return 0;
}

std::uint64_t cache_bytes(const GameRecord &game) { return directory_bytes(cache_root() / game.id); }

bool clear_cache(const GameRecord &game, std::string &error) {
    for (const int level : corpus_levels()) {
        const CorpusStatus status = current_status(game, level);
        if (status.state == CorpusState::Generating || status.state == CorpusState::Compiling ||
            status.state == CorpusState::Linking) {
            error = "A compile of this game is running (process " + std::to_string(status.pid) + ").";
            return false;
        }
    }
    std::error_code ec;
    fs::remove_all(cache_root() / game.id, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

} // namespace portablekit::app
