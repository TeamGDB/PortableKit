#include "app_home.hpp"

// The framework's: where the executable is.
#include "app_paths.hpp"
#include "install/user_data.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace portablekit::app {
namespace {

std::filesystem::path from_environment(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return {};
    return install::path_from_utf8(value);
}

// The app is portable: everything it writes lives in one folder beside it,
// never in the user's system folders, so the whole installation moves or goes
// away as one folder. An explicit PORTABLEKIT_HOME (or --data-dir, which sets
// it) overrides that.
std::filesystem::path portable_directory() {
    const std::filesystem::path exe_dir = portablekit::executable_directory();
    if (exe_dir.empty()) throw std::runtime_error("Cannot tell where the program is, so it cannot find its data folder. "
                                                  "Give one with --data-dir <folder>.");
#if defined(__APPLE__)
    // Inside an .app bundle (Contents/MacOS/portablekit): the bundle is signed
    // and must not be written into, so the folder goes next to the bundle.
    const std::filesystem::path contents = exe_dir.parent_path();
    const std::filesystem::path bundle = contents.parent_path();
    if (exe_dir.filename() == "MacOS" && contents.filename() == "Contents" && bundle.extension() == ".app") {
        // Opened from a quarantined download, macOS runs a copy from a random
        // read-only location ("App Translocation"); a folder next to that copy
        // would be lost. Only moving the app fixes it.
        if (bundle.string().find("/AppTranslocation/") != std::string::npos)
            throw std::runtime_error("macOS is running PortableKit from a temporary read-only copy, because it was "
                                     "opened where it was downloaded. Move PortableKit.app into a folder of its own "
                                     "(for example your Applications folder, or a folder in Documents) with the "
                                     "Finder, then open it again.");
        return bundle.parent_path() / "PortableKit Data";
    }
#endif
    return exe_dir / "data";
}

// Creates the folder if needed and proves it can be written to.
void check_writable(const std::filesystem::path &folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path probe = folder / ".write-test";
    {
        std::ofstream out(probe, std::ios::trunc);
        if (out && std::filesystem::is_directory(folder, ec)) {
            out << "ok";
            out.close();
            if (out) {
                std::filesystem::remove(probe, ec);
                return;
            }
        }
    }
    throw std::runtime_error("PortableKit keeps everything in a folder next to itself, and cannot write to " +
                             path_text(folder) + ". Move PortableKit to a folder you can write to (not Program "
                             "Files, not a read-only disk), or give a data folder with --data-dir <folder>.");
}

} // namespace

std::filesystem::path home_directory() {
    static const std::filesystem::path home = [] {
        std::filesystem::path folder = from_environment("PORTABLEKIT_HOME");
        if (folder.empty()) folder = portable_directory();
        check_writable(folder);
        return folder;
    }();
    return home;
}

std::filesystem::path cache_root() {
    if (auto cache = from_environment("PORTABLEKIT_CACHE"); !cache.empty()) return cache;
    return home_directory() / "cache";
}

std::filesystem::path games_directory() { return home_directory() / "games"; }
std::filesystem::path keys_file_path() { return home_directory() / "keys.txt"; }

std::filesystem::path recompiler_path() {
#if defined(_WIN32)
    return portablekit::executable_directory() / "psp_recomp.exe";
#else
    return portablekit::executable_directory() / "psp_recomp";
#endif
}

std::filesystem::path corpus_include_directory() {
    const std::filesystem::path beside = portablekit::executable_directory() / "share" / "portablekit" / "include";
#if defined(__APPLE__)
    // Inside an .app bundle: Contents/MacOS/portablekit, Contents/Resources/include.
    const std::filesystem::path bundled = portablekit::executable_directory().parent_path() / "Resources" / "include";
    std::error_code ec;
    if (!std::filesystem::exists(beside, ec) && std::filesystem::exists(bundled, ec)) return bundled;
#endif
    return beside;
}

std::string path_text(const std::filesystem::path &path) { return install::path_to_utf8(path); }

} // namespace portablekit::app
