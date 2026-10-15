#include "kernel/file_mods.hpp"

#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>

namespace portablekit::file_mods {
namespace {

// How long a look at the folder is trusted before the next one.
constexpr std::chrono::seconds kRescanAfter{2};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

struct Index {
    std::mutex mutex;
    bool scanned{};
    std::chrono::steady_clock::time_point scanned_at;
    // Lower-case path inside mods/files, '/' separated -> the file.
    std::map<std::string, std::filesystem::path> files;
};

Index &index() {
    static Index state;
    return state;
}

// Under the index's lock.
void rescan_if_stale(Index &state) {
    const auto now = std::chrono::steady_clock::now();
    if (state.scanned && now - state.scanned_at < kRescanAfter) return;
    state.scanned = true;
    state.scanned_at = now;
    state.files.clear();
    const std::filesystem::path root = folder();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec)) continue;
        std::string relative = install::path_to_utf8(it->path().lexically_relative(root));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        // The first of two names that differ only in case wins, the same one
        // every time.
        state.files.emplace(lower(relative), it->path());
    }
}

} // namespace

std::filesystem::path folder() {
    try {
        return install::user_data_directory() / "mods" / "files";
    } catch (const std::exception &) {
        return {};
    }
}

std::optional<std::filesystem::path> find(const std::string &disc_path) {
    if (!settings::current().file_mods || disc_path.empty()) return std::nullopt;
    Index &state = index();
    const std::lock_guard lock(state.mutex);
    rescan_if_stale(state);
    if (state.files.empty()) return std::nullopt;
    const auto found = state.files.find(lower(disc_path));
    if (found == state.files.end()) return std::nullopt;
    return found->second;
}

std::size_t count() {
    Index &state = index();
    const std::lock_guard lock(state.mutex);
    rescan_if_stale(state);
    return state.files.size();
}

} // namespace portablekit::file_mods
