#include "platform/android_documents.hpp"
#include "profile.hpp"

#include "platform/android_jni.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <utility>
#include <cerrno>
#include <fstream>
#include <vector>

namespace portablekit::android {
namespace {

namespace fs = std::filesystem;

// A readable name for a picked tree: the last part of its document id
// ("primary:Download/saves" gives "Download/saves").
std::string describe_tree(const std::string &uri) {
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            decoded += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += uri[i];
        }
    }
    const std::size_t colon = decoded.rfind(':');
    return colon == std::string::npos ? decoded : decoded.substr(colon + 1);
}

bool copy_document_to_file(const std::string &uri, const fs::path &target, std::string &error) {
    const int fd = open_document(uri, "r");
    if (fd < 0) {
        error = "cannot read " + target.filename().string();
        return false;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    std::array<char, 1 << 16> buffer{};
    bool ok = static_cast<bool>(out);
    while (ok) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = got == 0;
            break;
        }
        out.write(buffer.data(), got);
        ok = static_cast<bool>(out);
    }
    ::close(fd);
    if (!ok) error = "cannot copy " + target.filename().string();
    return ok;
}

bool copy_file_to_document(const fs::path &source, const std::string &folder_uri, std::string &error) {
    const std::optional<std::string> created = create(folder_uri, source.filename().string(), false);
    if (!created) {
        error = "cannot create " + source.filename().string() + " in the chosen folder";
        return false;
    }
    const int fd = open_document(*created, "w");
    if (fd < 0) {
        error = "cannot write " + source.filename().string();
        return false;
    }
    std::ifstream in(source, std::ios::binary);
    std::array<char, 1 << 16> buffer{};
    bool ok = static_cast<bool>(in);
    while (ok && in) {
        in.read(buffer.data(), buffer.size());
        std::streamsize left = in.gcount();
        const char *at = buffer.data();
        while (left > 0) {
            const ssize_t put = ::write(fd, at, static_cast<std::size_t>(left));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) {
                ok = false;
                break;
            }
            at += put;
            left -= put;
        }
    }
    ok = ::close(fd) == 0 && ok;
    if (!ok) error = "cannot write " + source.filename().string();
    return ok;
}

// Copies the files of a folder document (not its subfolders) into `target`.
bool copy_save_folder(const std::string &uri, const fs::path &target, std::string &error) {
    const auto entries = list_folder(uri);
    if (!entries) {
        error = "cannot read the chosen folder";
        return false;
    }
    std::error_code ec;
    fs::create_directories(target, ec);
    for (const Entry &entry : *entries)
        if (!entry.directory && !copy_document_to_file(entry.uri, target / entry.name, error)) return false;
    return true;
}

bool holds_param_sfo(const std::vector<Entry> &entries) {
    for (const Entry &entry : entries)
        if (!entry.directory && entry.name == "PARAM.SFO") return true;
    return false;
}

// Looks for save folders (those holding a PARAM.SFO) under a folder document
// and copies each into `savedata`/<name>: in the folder itself, its SAVEDATA
// and PSP folders, and one level of other folders, so a memory stick's root,
// an export ("MHP3rd saves <time>/PSP/SAVEDATA") or a folder holding one all
// work. The first save of a name wins.
bool find_and_copy(const std::string &uri, const std::string &name, const fs::path &savedata, int depth,
                   std::string &error) {
    const auto entries = list_folder(uri);
    if (!entries) return true;
    if (holds_param_sfo(*entries)) {
        std::error_code ec;
        if (fs::exists(savedata / name, ec)) return true;
        return copy_save_folder(uri, savedata / name, error);
    }
    if (depth >= 5) return true;
    for (const Entry &entry : *entries) {
        if (!entry.directory) continue;
        const bool obvious = entry.name == "PSP" || entry.name == "SAVEDATA" || name == "SAVEDATA";
        if ((obvious || depth < 2) && !find_and_copy(entry.uri, entry.name, savedata, depth + 1, error))
            return false;
    }
    return true;
}

bool copy_tree(const fs::path &local, const std::string &folder_uri, std::string &error) {
    const std::optional<std::string> made = create(folder_uri, local.filename().string(), true);
    if (!made) {
        error = "cannot create " + local.filename().string() + " in the chosen folder";
        return false;
    }
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(local, ec)) {
        if (entry.is_directory(ec)) {
            if (!copy_tree(entry.path(), *made, error)) return false;
        } else if (!copy_file_to_document(entry.path(), *made, error)) {
            return false;
        }
    }
    if (ec) error = "cannot read " + local.string();
    return !ec;
}

} // namespace

namespace {

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

const Entry *child_named(const std::vector<Entry> &entries, const std::string &name, bool directory) {
    const std::string wanted = lower(name);
    for (const Entry &entry : entries)
        if (entry.directory == directory && lower(entry.name) == wanted) return &entry;
    return nullptr;
}

bool holds_ini(const std::string &uri) {
    const auto entries = list_folder(uri);
    return entries && child_named(*entries, "textures.ini", false) != nullptr;
}

// The folder under `uri` reached through `path`, whatever its case; empty
// when it is not there.
std::string descend(std::string uri, const std::vector<std::string> &path) {
    for (const std::string &part : path) {
        const auto entries = list_folder(uri);
        if (!entries) return {};
        const Entry *entry = child_named(*entries, part, true);
        if (entry == nullptr) return {};
        uri = entry->uri;
    }
    return uri;
}

// The first folder within `depth` levels holding textures.ini, breadth first.
bool search(const std::string &uri, int depth, std::string &found_uri, std::string &found_name) {
    const auto entries = list_folder(uri);
    if (!entries) return false;
    std::vector<const Entry *> folders;
    for (const Entry &entry : *entries)
        if (entry.directory && entry.name[0] != '.') folders.push_back(&entry);
    for (const Entry *entry : folders)
        if (holds_ini(entry->uri)) {
            found_uri = entry->uri;
            found_name = entry->name;
            return true;
        }
    if (depth <= 1) return false;
    for (const Entry *entry : folders)
        if (search(entry->uri, depth - 1, found_uri, found_name)) return true;
    return false;
}

void list_pack(const std::string &uri, const std::string &prefix, bool (*skipped)(const std::string &),
               std::vector<PackFile> &files, std::string &error) {
    const auto entries = list_folder(uri);
    if (!entries) {
        error = "cannot read " + (prefix.empty() ? std::string("the pack's folder") : prefix);
        return;
    }
    for (const Entry &entry : *entries) {
        if (skipped(entry.name)) continue;
        const std::string relative = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.directory) {
            list_pack(entry.uri, relative, skipped, files, error);
            if (!error.empty()) return;
        } else {
            files.push_back({entry.uri, relative, entry.size});
        }
    }
}

} // namespace

std::optional<FoundPack> find_texture_pack(const std::string &tree, const std::vector<std::string> &names,
                                           bool (*skipped)(const std::string &), std::string &error) {
    FoundPack pack;
    pack.where = describe_tree(tree);
    const std::string root = tree_root(tree);
    if (!list_folder(root)) {
        error = std::string("Android would not let ") + portablekit::game().project_name + " read that folder.";
        return std::nullopt;
    }
    std::string uri;
    if (holds_ini(root)) {
        uri = root;
        pack.name = pack.where.substr(pack.where.rfind('/') + 1);
        pack.layout = "The folder holds textures.ini.";
    }
    for (std::size_t i = 0; uri.empty() && i < names.size(); ++i) {
        const std::string &name = names[i];
        const std::pair<std::vector<std::string>, const char *> places[] = {
            {{"textures", name}, "Found in its textures folder."},
            {{name}, "Found in the folder."},
            {{"PSP", "TEXTURES", name}, "Found in PPSSPP's PSP/TEXTURES folder."},
        };
        for (const auto &[path, layout] : places) {
            const std::string folder = descend(root, path);
            if (folder.empty() || !holds_ini(folder)) continue;
            uri = folder;
            pack.name = name;
            pack.layout = layout;
            break;
        }
    }
    if (uri.empty()) {
        std::string found_name;
        if (search(root, 3, uri, found_name)) {
            pack.name = found_name;
            pack.layout = "Found as " + found_name + ".";
        }
    }
    if (uri.empty()) {
        error = "No texture pack (a folder holding textures.ini) was found in " + pack.where + ".";
        return std::nullopt;
    }
    if (pack.name.empty()) pack.name = "pack";
    list_pack(uri, {}, skipped, pack.files, error);
    if (!error.empty()) return std::nullopt;
    return pack;
}

bool copy_document(const std::string &uri, const fs::path &target, std::string &error) {
    return copy_document_to_file(uri, target, error);
}

std::optional<PickedImport> pick_saves_to_import(const fs::path &staging) {
    const std::optional<std::string> tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedImport picked;
    std::error_code ec;
    fs::remove_all(staging, ec);
    const std::string where = describe_tree(*tree);
    // The staged copy carries the picked folder's name, which the review
    // screen shows; the saves go into its SAVEDATA, where find_saves() looks.
    const fs::path root = staging / where.substr(where.rfind('/') + 1);
    picked.staged = root;
    fs::create_directories(root / "SAVEDATA", ec);
    const std::string root_uri = tree_root(*tree);
    if (!list_folder(root_uri)) {
        picked.error = std::string("Android would not let ") + portablekit::game().project_name + " read that folder.";
        return picked;
    }
    std::string error;
    const std::string root_name = where.substr(where.rfind('/') + 1);
    if (!find_and_copy(root_uri, root_name, root / "SAVEDATA", 0, error))
        picked.error = "Copying from the chosen folder failed: " + error + ".";
    return picked;
}

std::optional<PickedExport> pick_folder_and_copy(const fs::path &local) {
    const std::optional<std::string> tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedExport result;
    result.where = describe_tree(*tree);
    std::string error;
    if (!copy_tree(local, tree_root(*tree), error)) result.error = error;
    return result;
}

} // namespace portablekit::android
