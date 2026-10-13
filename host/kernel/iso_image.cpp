#include "iso_image.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <system_error>

namespace portablekit {
namespace {

std::uint32_t read_le32(const std::vector<std::uint8_t> &data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16u) | (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

} // namespace

IsoImage::IsoImage(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        host_directory_ = true;
        host_root_ = std::filesystem::absolute(path, ec);
        if (ec) host_root_ = path;
        entries_[""] = Entry{0u, 0u, true, ""};
        std::vector<std::filesystem::path> paths;
        for (std::filesystem::recursive_directory_iterator it(
                 host_root_, std::filesystem::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto status = it->symlink_status(ec);
            if (ec || std::filesystem::is_symlink(status)) {
                ec.clear();
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (std::filesystem::is_directory(status) || std::filesystem::is_regular_file(status))
                paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end(), [&](const auto &a, const auto &b) {
            return a.lexically_relative(host_root_).generic_string() <
                   b.lexically_relative(host_root_).generic_string();
        });
        std::uint64_t next_lba = 32u;
        for (const auto &host_path : paths) {
            const std::string relative = host_path.lexically_relative(host_root_).generic_string();
            const std::string key = normalize(relative);
            const std::string basename = host_path.filename().string();
            const auto status = std::filesystem::status(host_path, ec);
            if (ec) { ec.clear(); continue; }
            if (std::filesystem::is_directory(status)) {
                entries_[key] = Entry{0u, 0u, true, basename};
                continue;
            }
            const std::uint64_t size = std::filesystem::file_size(host_path, ec);
            if (ec) { ec.clear(); continue; }
            if (size > std::numeric_limits<std::uint32_t>::max() ||
                next_lba > std::numeric_limits<std::uint32_t>::max())
                throw psprecomp::Error("Extracted game folder is too large for the PSP file interface");
            entries_[key] = Entry{static_cast<std::uint32_t>(next_lba), static_cast<std::uint32_t>(size), false, basename};
            host_files_[next_lba * kSectorSize] = HostFile{host_path, size};
            next_lba += std::max<std::uint64_t>(1u, (size + kSectorSize - 1u) / kSectorSize);
        }
        size_bytes_ = next_lba * kSectorSize;
        if (!entries_.contains("PSP_GAME/PARAM.SFO") || !entries_.contains("PSP_GAME/SYSDIR/EBOOT.BIN"))
            throw psprecomp::Error("Extracted folder must contain PSP_GAME/PARAM.SFO and PSP_GAME/SYSDIR/EBOOT.BIN");
        return;
    }
    file_.open(path, std::ios::binary);
    if (!file_) throw psprecomp::Error("Cannot open disc image: " + path.string());
    size_bytes_ = std::filesystem::file_size(path);
    std::vector<std::uint8_t> primary(kSectorSize);
    if (read(16u * kSectorSize, primary) != primary.size() || primary[1] != 'C' || primary[2] != 'D' ||
        primary[3] != '0' || primary[4] != '0' || primary[5] != '1')
        throw psprecomp::Error("Not an ISO9660 image: " + path.string());
    const std::uint32_t root_lba = read_le32(primary, 156u + 2u);
    const std::uint32_t root_size = read_le32(primary, 156u + 10u);
    entries_[""] = Entry{root_lba, root_size, true};
    scan_directory(root_lba, root_size, "", 0);
}

std::string IsoImage::normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && (result.empty() || result.back() == '/')) continue;
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(path[i]))));
    }
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

void IsoImage::scan_directory(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth) {
    if (depth > 16) return;
    std::vector<std::uint8_t> data(size);
    if (read(static_cast<std::uint64_t>(lba) * kSectorSize, data) != data.size()) return;
    std::size_t offset = 0u;
    while (offset < data.size()) {
        const std::uint8_t length = data[offset];
        if (length == 0u) {
            offset = (offset / kSectorSize + 1u) * kSectorSize;
            continue;
        }
        if (offset + length > data.size() || length < 34u) break;
        const std::uint8_t name_length = data[offset + 32u];
        const std::uint32_t entry_lba = read_le32(data, offset + 2u);
        const std::uint32_t entry_size = read_le32(data, offset + 10u);
        const bool directory = (data[offset + 25u] & 2u) != 0u;
        std::string name(reinterpret_cast<const char *>(data.data() + offset + 33u), name_length);
        offset += length;
        if (name_length == 1u && (name[0] == '\0' || name[0] == '\1')) continue;
        if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
        const std::string full = normalize(prefix.empty() ? name : prefix + "/" + name);
        entries_[full] = Entry{entry_lba, entry_size, directory, name};
        if (directory) scan_directory(entry_lba, entry_size, full, depth + 1);
    }
}

std::optional<IsoImage::Entry> IsoImage::find(std::string path) const {
    const auto found = entries_.find(normalize(std::move(path)));
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

std::size_t IsoImage::read(std::uint64_t offset, std::span<std::uint8_t> output) {
    if (offset >= size_bytes_) return 0u;
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), size_bytes_ - offset));
    if (host_directory_) {
        // File-backed virtual disc for ordinary IoFileMgr paths. Raw ISO-sector
        // reads do not receive a byte-for-byte ISO representation.
        std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(count), 0u);
        const std::uint64_t end = offset + count;
        auto it = host_files_.upper_bound(offset);
        if (it != host_files_.begin()) {
            auto previous = std::prev(it);
            if (previous->first + previous->second.size > offset) it = previous;
        }
        for (; it != host_files_.end() && it->first < end; ++it) {
            const std::uint64_t file_start = it->first;
            const std::uint64_t overlap_start = std::max(offset, file_start);
            const std::uint64_t overlap_end = std::min(end, file_start + it->second.size);
            if (overlap_start >= overlap_end) continue;
            std::ifstream in(it->second.path, std::ios::binary);
            if (!in) continue;
            in.seekg(static_cast<std::streamoff>(overlap_start - file_start));
            const auto destination = static_cast<std::size_t>(overlap_start - offset);
            const auto length = static_cast<std::size_t>(overlap_end - overlap_start);
            in.read(reinterpret_cast<char *>(output.data() + destination), static_cast<std::streamsize>(length));
        }
        return count;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(file_.gcount());
}

std::vector<std::string> IsoImage::list(std::string directory) const {
    const std::string prefix = normalize(std::move(directory));
    std::vector<std::string> names;
    for (const auto &[path, entry] : entries_) {
        if (path.empty() || path.size() <= prefix.size()) continue;
        if (!prefix.empty() && (path.compare(0, prefix.size(), prefix) != 0 || path[prefix.size()] != '/')) continue;
        const std::string rest = prefix.empty() ? path : path.substr(prefix.size() + 1u);
        if (rest.find('/') == std::string::npos) names.push_back(entry.name.empty() ? rest : entry.name);
    }
    return names;
}

} // namespace portablekit
