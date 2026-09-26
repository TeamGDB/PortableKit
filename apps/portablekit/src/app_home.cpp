#include "app_home.hpp"

// The framework's: where the executable is.
#include "app_paths.hpp"
#include "install/user_data.hpp"

#include <cstdlib>

namespace portablekit::app {
namespace {

std::filesystem::path from_environment(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return {};
    return install::path_from_utf8(value);
}

std::filesystem::path user_home() {
#if defined(_WIN32)
    return from_environment("USERPROFILE");
#else
    return from_environment("HOME");
#endif
}

} // namespace

std::filesystem::path home_directory() {
    if (auto home = from_environment("PORTABLEKIT_HOME"); !home.empty()) return home;
#if defined(_WIN32)
    // Local, not roaming: the cache beside it is gigabytes.
    if (auto local = from_environment("LOCALAPPDATA"); !local.empty()) return local / "PortableKit";
    return user_home() / "AppData" / "Local" / "PortableKit";
#elif defined(__APPLE__)
    return user_home() / "Library" / "Application Support" / "PortableKit";
#else
    if (auto data = from_environment("XDG_DATA_HOME"); !data.empty()) return data / "PortableKit";
    return user_home() / ".local" / "share" / "PortableKit";
#endif
}

std::filesystem::path cache_root() {
    if (auto cache = from_environment("PORTABLEKIT_CACHE"); !cache.empty()) return cache;
    if (auto home = from_environment("PORTABLEKIT_HOME"); !home.empty()) return home / "cache";
#if defined(_WIN32)
    return home_directory() / "cache";
#elif defined(__APPLE__)
    return user_home() / "Library" / "Caches" / "PortableKit";
#else
    if (auto cache = from_environment("XDG_CACHE_HOME"); !cache.empty()) return cache / "PortableKit";
    return user_home() / ".cache" / "PortableKit";
#endif
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
