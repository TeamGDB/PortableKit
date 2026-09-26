// portablekit: plays the PSP games the player owns by recompiling them on the
// player's machine. The window (no arguments) and the commands below are two
// front ends over the same functions; every command has --json for scripts.
//
// Exit codes, for scripts:
//    0  done
//    1  an unexpected error
//    2  the command line is wrong
//    3  no such game in the library
//   10  the file is not a PSP game disc image
//   11  the executable is encrypted and the keys to decrypt it are missing
//   12  the executable is not one this program can load
//   13  a file could not be read or written
//   20  no compiler was found
//   21  the recompiler failed
//   22  a unit did not compile
//   23  the library did not link
//   24  a compile of this game is already running
//   25  the export failed
//   30  the keys file was not accepted
//   40+ the game stopped with an error (40 + the port's own exit code)

#include "app_home.hpp"
#include "auto_profile.hpp"
#include "corpus.hpp"
#include "corpus_loader.hpp"
#include "game_import.hpp"
#include "keys_file.hpp"
#include "launcher_ui.hpp"
#include "process.hpp"
#include "text_format.hpp"

#include "app_paths.hpp"
#include "corpus_abi.hpp"
#include "install/user_data.hpp"

#include "psprecomp/interpreter.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

int portablekit_host_main(int argc, char **argv);

namespace portablekit::app {
namespace {

constexpr int kUsage = 2;
constexpr int kNoSuchGame = 3;
constexpr int kKeysRejected = 30;

const char *const kUsageText =
    "usage: portablekit                        open the library window\n"
    "       portablekit add <image.iso> [--executable <EBOOT>] [--boot-bin]\n"
    "       portablekit list\n"
    "       portablekit info <game>\n"
    "       portablekit status <game>\n"
    "       portablekit compile <game> [--opt 0|1|2|tiered] [--jobs N] [--background] [--keep]\n"
    "       portablekit run <game> [--interpreter] [--no-compile] [--opt 0|1|2|tiered] [--headless] [--seconds N]\n"
    "       portablekit export <game> <folder> [--source-only | --library-only]\n"
    "       portablekit keys import <file>\n"
    "       portablekit keys status\n"
    "       portablekit cache clear <game>\n"
    "       portablekit toolchain\n"
    "  <game> is a game's id, its disc id (UCES01059 or UCES-01059), or a unique prefix.\n"
    "  Every command takes --json and then prints one JSON object on stdout, and\n"
    "  --data-dir <folder> to use another data folder than the one next to the program.\n";

struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;  // --name value, or "1" for a switch
    bool has(const std::string &name) const { return options.contains(name); }
    std::string get(const std::string &name, const std::string &fallback = {}) const {
        const auto it = options.find(name);
        return it != options.end() ? it->second : fallback;
    }
};

// Options that take a value; everything else starting with -- is a switch.
bool takes_value(const std::string &name) {
    return name == "executable" || name == "opt" || name == "jobs" || name == "seconds" || name == "data-dir";
}

Arguments parse(int argc, char **argv, int first) {
    Arguments args;
    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("--")) {
            const std::string name = arg.substr(2);
            if (takes_value(name) && i + 1 < argc) args.options[name] = argv[++i];
            else args.options[name] = "1";
        } else {
            args.positional.push_back(arg);
        }
    }
    return args;
}

bool g_json = false;

int report_error(int code, const std::string &message) {
    if (g_json) std::cout << Json().field("ok", false).field("exit_code", code).field("error", message).str() << "\n";
    else std::cerr << "portablekit: " << message << "\n";
    return code;
}

std::optional<GameRecord> game_argument(const Arguments &args, int &code) {
    if (args.positional.size() < 2) {
        code = report_error(kUsage, "which game? Give its id or disc id (portablekit list shows them).");
        return std::nullopt;
    }
    auto game = find_game(args.positional[1]);
    if (!game) code = report_error(kNoSuchGame, "no game in the library matches \"" + args.positional[1] + "\".");
    return game;
}

Json status_json(const GameRecord &game) {
    std::vector<Json> levels;
    for (const int level : corpus_levels()) {
        const CorpusStatus status = current_status(game, level);
        if (status.state == CorpusState::None) continue;
        Json entry;
        entry.field("opt_level", level)
            .field("state", state_name(status.state))
            .field("units_done", status.units_done)
            .field("units_total", status.units_total)
            .field("jobs", status.jobs)
            .field("generate_seconds", status.generate_seconds)
            .field("compile_seconds", status.compile_seconds)
            .field("link_seconds", status.link_seconds)
            .field("library_bytes", status.library_bytes)
            .field("peak_unit_rss", status.peak_unit_rss)
            .field("message", status.message)
            .field("toolchain", status.toolchain)
            .field("library", path_text(corpus_paths(game, level).library));
        levels.push_back(entry);
    }
    const auto ready = ready_corpus(game);
    Json out;
    out.field("id", game.id).array("corpora", levels).field("cache_bytes", cache_bytes(game));
    if (ready) out.field("ready_opt_level", ready->opt_level);
    else out.null_field("ready_opt_level");
    return out;
}

Json game_json(const GameRecord &game) {
    const HandProfile *hand = hand_profile_for(game.executable_sha256);
    Json out;
    out.field("id", game.id)
        .field("disc_id", game.disc_id)
        .field("title", game.title)
        .field("iso", path_text(game.iso))
        .field("data_dir", path_text(game.data_dir))
        .field("executable_sha256", game.executable_sha256)
        .field("executable_source", game.executable_source)
        .field("profile", hand != nullptr ? hand->name : "auto");
    const auto ready = ready_corpus(game);
    if (ready) out.field("compiled_opt_level", ready->opt_level);
    else out.null_field("compiled_opt_level");
    return out;
}

void print_status_text(const GameRecord &game) {
    bool any = false;
    for (const int level : corpus_levels()) {
        const CorpusStatus status = current_status(game, level);
        if (status.state == CorpusState::None) continue;
        any = true;
        std::cout << "  -O" << level << ": " << state_name(status.state);
        if (status.units_total) std::cout << " " << status.units_done << "/" << status.units_total << " units";
        if (status.compile_seconds > 0.0)
            std::cout << ", generate " << static_cast<int>(status.generate_seconds) << " s, compile "
                      << static_cast<int>(status.compile_seconds) << " s, link " << static_cast<int>(status.link_seconds)
                      << " s";
        if (status.library_bytes) std::cout << ", " << human_bytes(status.library_bytes);
        if (!status.message.empty() && status.state != CorpusState::Ready) std::cout << " - " << status.message;
        std::cout << "\n";
    }
    if (!any) std::cout << "  not compiled\n";
    std::cout << "  cache: " << human_bytes(cache_bytes(game)) << " in " << path_text(cache_root() / game.id) << "\n";
}

int command_add(const Arguments &args) {
    if (args.positional.size() < 2) return report_error(kUsage, "add needs the disc image: portablekit add <image.iso>");
    ImportOptions options;
    options.iso = install::path_from_utf8(args.positional[1]);
    if (args.has("executable")) options.executable = install::path_from_utf8(args.get("executable"));
    options.prefer_boot_bin = args.has("boot-bin");
    try {
        const GameRecord record = import_game(options, [](const std::string &stage) {
            if (!g_json) std::cout << stage << "\n";
        });
        if (g_json) {
            std::cout << game_json(record).field("ok", true).str() << "\n";
        } else {
            const HandProfile *hand = hand_profile_for(record.executable_sha256);
            std::cout << "Added " << record.title << " (" << display_disc_id(record.disc_id) << ") as " << record.id
                      << "\n  executable: " << record.executable_source << ", SHA-256 " << record.executable_sha256
                      << "\n  profile:    " << (hand != nullptr ? std::string(hand->name) + " (hand-written)" : "automatic")
                      << "\nPlay it with: portablekit run " << record.disc_id << "\n";
        }
        return 0;
    } catch (const ImportError &e) {
        return report_error(e.code(), e.what());
    }
}

int command_list() {
    const std::vector<GameRecord> games = list_games();
    if (g_json) {
        std::vector<Json> items;
        for (const GameRecord &game : games) items.push_back(game_json(game));
        std::cout << Json().field("ok", true).array("games", items).str() << "\n";
        return 0;
    }
    if (games.empty()) std::cout << "No games yet. Add one: portablekit add <image.iso>\n";
    for (const GameRecord &game : games) {
        const auto ready = ready_corpus(game);
        std::cout << game.id << "  " << game.title << "  ("
                  << (ready ? "compiled -O" + std::to_string(ready->opt_level) : std::string("not compiled")) << ")\n";
    }
    return 0;
}

int command_info(const GameRecord &game) {
    const std::string profile = activate_game_profile(game);
    if (g_json) {
        std::vector<Json> decisions;
        for (const auto &[what, why] : describe_active_profile()) decisions.push_back(Json().field("what", what).field("decision", why));
        std::cout << game_json(game).field("ok", true).array("profile_decisions", decisions)
                         .raw("status", status_json(game).str()).str() << "\n";
        return 0;
    }
    std::cout << game.title << " (" << display_disc_id(game.disc_id) << ")\n"
              << "  id:          " << game.id << "\n"
              << "  disc image:  " << path_text(game.iso) << "\n"
              << "  data:        " << path_text(game.data_dir) << "\n"
              << "  executable:  " << game.executable_source << ", SHA-256 " << game.executable_sha256 << "\n"
              << "Profile:\n";
    for (const auto &[what, why] : describe_active_profile()) std::cout << "  " << what << ": " << why << "\n";
    std::cout << "Compiled code:\n";
    print_status_text(game);
    return 0;
}

int command_status(const GameRecord &game) {
    if (g_json) {
        std::cout << status_json(game).field("ok", true).str() << "\n";
        return 0;
    }
    std::cout << game.title << " (" << game.id << ")\n";
    print_status_text(game);
    return 0;
}

std::string self_path(char **argv) {
    const std::filesystem::path self = portablekit::executable_path();
    return self.empty() ? std::string(argv[0]) : path_text(self);
}

int start_background_compile(const GameRecord &game, const std::string &level, unsigned jobs, char **argv) {
    std::vector<std::string> command{self_path(argv), "compile", game.id, "--opt", level};
    if (jobs != 0u) {
        command.push_back("--jobs");
        command.push_back(std::to_string(jobs));
    }
    const std::filesystem::path log_dir = cache_root() / game.id;
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    return spawn_detached(command, log_dir / "compile-process.log") ? 0 : 1;
}

int command_compile(const GameRecord &game, const Arguments &args, char **argv) {
    CompileOptions options;
    const bool tiered = args.get("opt", "2") == "tiered";
    options.opt_level = tiered ? 0 : std::atoi(args.get("opt", "2").c_str());
    if (options.opt_level < 0 || options.opt_level > 2)
        return report_error(kUsage, "--opt takes 0, 1, 2 or tiered");
    options.jobs = static_cast<unsigned>(std::atoi(args.get("jobs", "0").c_str()));
    options.keep_intermediates = args.has("keep");
    if (args.has("background")) {
        if (start_background_compile(game, args.get("opt", "2"), options.jobs, argv) != 0)
            return report_error(1, "cannot start the background compile");
        if (g_json) std::cout << Json().field("ok", true).field("started", true).field("opt_level", options.opt_level).str() << "\n";
        else std::cout << "Compiling " << game.title << " in the background; follow it with: portablekit status " << game.disc_id << "\n";
        return 0;
    }
    unsigned last_percent = 101;
    std::string last_message;
    options.progress = [&](const CorpusStatus &status) {
        if (g_json) return;
        const unsigned percent = status.units_total ? status.units_done * 100u / status.units_total : 0u;
        if (status.message != last_message || (status.state == CorpusState::Compiling && percent != last_percent)) {
            std::cout << "[" << state_name(status.state) << "] ";
            if (status.state == CorpusState::Compiling)
                std::cout << status.units_done << "/" << status.units_total << " units (" << percent << "%), "
                          << static_cast<int>(status.compile_seconds) << " s";
            else
                std::cout << status.message;
            std::cout << std::endl;
            last_percent = percent;
            last_message = status.message;
        }
    };
    // Tiered: -O0 first, which the running game switches to within minutes,
    // then -O2 from the same C++, which it switches to when that is done.
    options.keep_generated = tiered;
    int code = 0;
    // A tier already compiled is not compiled again.
    if (!tiered || current_status(game, 0).state != CorpusState::Ready) code = compile_corpus(game, options);
    if (tiered && code == 0) {
        options.reuse_generated = corpus_paths(game, 0).generated;
        options.keep_generated = false;
        options.opt_level = 2;
        code = compile_corpus(game, options);
    }
    const CorpusStatus status = current_status(game, options.opt_level);
    if (g_json) {
        std::cout << status_json(game).field("ok", code == 0).field("exit_code", code).str() << "\n";
    } else if (code == 0) {
        std::cout << "Compiled in " << static_cast<int>(status.generate_seconds + status.compile_seconds + status.link_seconds)
                  << " s (recompile " << static_cast<int>(status.generate_seconds) << " s, compile "
                  << static_cast<int>(status.compile_seconds) << " s with " << status.jobs << " job(s), link "
                  << static_cast<int>(status.link_seconds) << " s); library " << human_bytes(status.library_bytes)
                  << "; largest compiler process " << human_bytes(status.peak_unit_rss) << "\n";
    } else {
        std::cerr << "portablekit: " << status.message << "\n";
    }
    return code;
}

void set_environment(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Where the interpreter spent its time, for the next compile to cover what it
// missed (docs/DESKTOP_APP.md, "Growing the corpus").
void save_interpreter_profile(const GameRecord &game) {
    const auto profile = psprecomp::interpreter_entry_profile();
    if (profile.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(cache_root() / game.id, ec);
    std::ofstream out(cache_root() / game.id / "interpreted.txt", std::ios::trunc);
    out << "# Guest addresses the interpreter entered, and the instructions it ran from each.\n";
    for (const auto &[address, instructions] : profile) out << hex32_text(address) << " " << instructions << "\n";
}

int command_run(const GameRecord &game, const Arguments &args, char **argv) {
    const std::string profile = activate_game_profile(game);
    set_environment("PORTABLEKIT_DATA_DIR", path_text(game.data_dir));
    if (args.has("headless")) {
        set_environment("PORTABLEKIT_NO_RENDER", "1");
        set_environment("PORTABLEKIT_NO_AUDIO", "1");
    }
    CorpusChoice choice;
    choice.interpreter_only = args.has("interpreter");
    const auto ready = ready_corpus(game);
    // Nothing compiled at the best level and nothing compiling: start it now,
    // behind the game.
    if (!choice.interpreter_only && !args.has("no-compile") && (!ready || ready->opt_level < 2)) {
        bool running = false;
        for (const int level : corpus_levels()) {
            const CorpusState state = current_status(game, level).state;
            running = running || state == CorpusState::Generating || state == CorpusState::Compiling ||
                      state == CorpusState::Linking;
        }
        if (!running && find_toolchain().found) {
            std::cout << "[portablekit] compiling " << game.title << " in the background\n";
            (void)start_background_compile(game, args.get("opt", "tiered"), 0u, argv);
        }
    }
    prepare_corpus_loading(game, choice);
    std::cout << "[portablekit] " << game.title << " (" << game.id << "), profile " << profile << "\n";
    if (args.has("seconds")) {
        // A bounded run, for tests: the game is stopped at a dispatch
        // boundary after this long, and the port reports its threads.
        const int seconds = std::atoi(args.get("seconds").c_str());
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::cout << "[portablekit] --seconds " << seconds << " reached; stopping" << std::endl;
            request_stop();
            // Should the game not reach a dispatch boundary (a hang in host
            // code), leave anyway a little later.
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::cout << "[portablekit] the game did not stop; leaving" << std::endl;
            save_interpreter_profile(*find_game(std::getenv("PORTABLEKIT_RUN_GAME")));
            std::_Exit(0);
        }).detach();
        set_environment("PORTABLEKIT_RUN_GAME", game.id);
    }
    std::vector<char *> host_argv{argv[0], nullptr};
    const int code = portablekit_host_main(1, host_argv.data());
    save_interpreter_profile(game);
    const LoadedCorpus loaded = loaded_corpus();
    std::cout << "[portablekit] ran " << (loaded.opt_level < 0 ? "under the interpreter" : "compiled code (-O" + std::to_string(loaded.opt_level) + ")")
              << "; switched " << loaded.switches << " time(s) while running\n";
    return code == 0 ? 0 : 40 + code;
}

int command_keys(const Arguments &args) {
    const std::string action = args.positional.size() > 1 ? args.positional[1] : "status";
    if (action == "import") {
        if (args.positional.size() < 3) return report_error(kUsage, "keys import needs the file");
        KeysReport report;
        const bool ok = import_keys_file(install::path_from_utf8(args.positional[2]), report);
        if (g_json) {
            std::vector<Json> problems;
            for (const std::string &problem : report.problems) problems.push_back(Json().field("problem", problem));
            std::cout << Json().field("ok", ok).field("path", path_text(keys_file_path()))
                             .field("executables", report.can_decrypt_executables)
                             .field("saves", report.can_encrypt_saves)
                             .field("tags", static_cast<std::uint64_t>(report.tag_count))
                             .array("problems", problems).str() << "\n";
        } else {
            for (const std::string &problem : report.problems) std::cerr << "  " << problem << "\n";
            if (ok) std::cout << "Keys saved to " << path_text(keys_file_path()) << "\n";
        }
        return ok ? 0 : kKeysRejected;
    }
    if (action != "status") return report_error(kUsage, "keys takes import or status");
    const KeysReport &report = active_keys();
    if (g_json) {
        std::vector<Json> problems;
        for (const std::string &problem : report.problems) problems.push_back(Json().field("problem", problem));
        std::cout << Json().field("ok", true).field("path", path_text(report.path)).field("found", report.file_found)
                         .field("executables", report.can_decrypt_executables)
                         .field("saves", report.can_encrypt_saves)
                         .field("tags", static_cast<std::uint64_t>(report.tag_count))
                         .array("problems", problems).str() << "\n";
        return 0;
    }
    std::cout << "Keys file: " << path_text(report.path) << (report.file_found ? "" : " (none)") << "\n"
              << "  decrypt executables: " << (report.can_decrypt_executables ? "yes" : "no") << "\n"
              << "  encrypt saves:       " << (report.can_encrypt_saves ? "yes" : "no (saves are kept unencrypted)") << "\n"
              << "  tag keys:            " << report.tag_count << "\n";
    for (const std::string &problem : report.problems) std::cout << "  problem: " << problem << "\n";
    return 0;
}

int command_toolchain() {
    const Toolchain toolchain = find_toolchain();
    if (g_json) {
        std::cout << Json().field("ok", toolchain.found).field("kind", toolchain.kind).field("compiler", toolchain.compiler)
                         .field("version", toolchain.version).field("problem", toolchain.problem)
                         .field("default_jobs", default_jobs()).field("abi", kCorpusAbi).str() << "\n";
    } else if (toolchain.found) {
        std::cout << toolchain.kind << ": " << toolchain.compiler << "\n  " << toolchain.version
                  << "\n  jobs by default: " << default_jobs() << "\n  corpus ABI: " << kCorpusAbi << "\n";
    } else {
        std::cout << toolchain.problem << "\n";
    }
    return toolchain.found ? 0 : 20;
}

int dispatch(int argc, char **argv) {
    const Arguments args = parse(argc, argv, 1);
    g_json = args.has("json");
    if (args.has("data-dir")) set_environment("PORTABLEKIT_HOME", args.get("data-dir"));
    if (args.has("help") || (!args.positional.empty() && args.positional[0] == "help")) {
        std::cout << kUsageText;
        return 0;
    }
    if (args.positional.empty()) {
        activate_launcher_profile();
        set_environment("PORTABLEKIT_DATA_DIR", path_text(home_directory() / "launcher"));
        const auto choice = run_launcher();
        if (!choice) return 0;
        // The game runs as a process of its own, with its own settings: exec
        // this program again as `portablekit run <game>`.
        std::vector<std::string> command{self_path(argv), "run", choice->game_id};
        if (choice->interpreter_only) command.push_back("--interpreter");
#if defined(_WIN32)
        return spawn_detached(command, home_directory() / "last-run.log") ? 0 : 1;
#else
        std::vector<char *> exec_argv;
        for (std::string &part : command) exec_argv.push_back(part.data());
        exec_argv.push_back(nullptr);
        execv(exec_argv[0], exec_argv.data());
        return report_error(1, "cannot start the game");
#endif
    }
    const std::string &command = args.positional[0];
    if (command == "add") return command_add(args);
    if (command == "list") return command_list();
    if (command == "keys") return command_keys(args);
    if (command == "toolchain") return command_toolchain();
    int code = 0;
    if (command == "cache") {
        if (args.positional.size() < 3 || args.positional[1] != "clear")
            return report_error(kUsage, "cache takes: clear <game>");
        Arguments shifted = args;
        shifted.positional.erase(shifted.positional.begin());
        const auto game = game_argument(shifted, code);
        if (!game) return code;
        std::string error;
        if (!clear_cache(*game, error)) return report_error(1, error);
        if (g_json) std::cout << Json().field("ok", true).str() << "\n";
        else std::cout << "Removed the compiled code of " << game->title << "\n";
        return 0;
    }
    if (command == "export") {
        if (args.positional.size() < 3) return report_error(kUsage, "export needs the game and a folder");
        const auto game = game_argument(args, code);
        if (!game) return code;
        ExportOptions options;
        options.source = !args.has("library-only");
        options.library = !args.has("source-only");
        if (!g_json) std::cout << kExportNotice << "\n";
        std::string message;
        const int result = export_corpus(*game, install::path_from_utf8(args.positional[2]), options, message);
        if (result != 0) return report_error(result, message);
        if (g_json) std::cout << Json().field("ok", true).field("folder", args.positional[2]).field("notice", kExportNotice).str() << "\n";
        else std::cout << message << "\n";
        return 0;
    }
    if (command == "info" || command == "status" || command == "compile" || command == "run") {
        const auto game = game_argument(args, code);
        if (!game) return code;
        if (command == "info") return command_info(*game);
        if (command == "status") return command_status(*game);
        if (command == "compile") return command_compile(*game, args, argv);
        return command_run(*game, args, argv);
    }
    return report_error(kUsage, "unknown command \"" + command + "\"\n" + kUsageText);
}

} // namespace
} // namespace portablekit::app

int main(int argc, char **argv) {
    try {
        return portablekit::app::dispatch(argc, argv);
    } catch (const std::exception &e) {
        return portablekit::app::report_error(1, e.what());
    }
}
