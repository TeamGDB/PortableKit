#include "corpus_library.hpp"

#include "app_paths.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace portablekit {
namespace {

void *open_library(const std::filesystem::path &path, std::string &error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (module == nullptr) error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
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

} // namespace

std::optional<CorpusLibrary> open_corpus_library(const std::filesystem::path &path, std::string_view executable_sha256,
                                                 std::string &error) {
    void *handle = open_library(path, error);
    if (handle == nullptr) return std::nullopt;
    using VersionFn = std::uint32_t (*)();
    using TextFn = const char *(*)();
    const auto version = reinterpret_cast<VersionFn>(symbol(handle, "portablekit_corpus_abi_version"));
    const auto executable = reinterpret_cast<TextFn>(symbol(handle, "portablekit_corpus_executable"));
    const auto register_all =
        reinterpret_cast<void (*)(psprecomp::CorpusRuntime &)>(symbol(handle, "portablekit_corpus_register"));
    if (version == nullptr || executable == nullptr || register_all == nullptr) {
        error = "not a corpus library";
        return std::nullopt;
    }
    if (version() != psprecomp::kCorpusAbiVersion) {
        error = "built for corpus ABI " + std::to_string(version()) + ", this program speaks " +
                std::to_string(psprecomp::kCorpusAbiVersion);
        return std::nullopt;
    }
    if (!executable_sha256.empty() && executable_sha256 != executable()) {
        error = "generated from another executable (" + std::string(executable()) + ")";
        return std::nullopt;
    }
    return CorpusLibrary{path, register_all, handle};
}

std::filesystem::path variant_corpus_path(const ProfileVariant &variant) {
#if defined(_WIN32)
    const char *extension = ".dll";
#elif defined(__APPLE__)
    const char *extension = ".dylib";
#else
    const char *extension = ".so";
#endif
    return executable_directory() / "corpora" / (std::string(variant.key) + extension);
}

} // namespace portablekit
