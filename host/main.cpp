#include "mhp3rd_profile.hpp"

#include "kernel/kernel.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr const char *kSupportedSha256 =
    "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";

std::uint64_t configured_max_dispatches() {
    constexpr std::uint64_t default_limit = 4'000'000'000ull;
    const char *text = std::getenv("PSPRECOMP_MAX_DISPATCHES");
    if (text == nullptr || *text == '\0') return default_limit;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0u)
        throw psprecomp::Error(std::string("Invalid PSPRECOMP_MAX_DISPATCHES value: ") + text);
    return static_cast<std::uint64_t>(parsed);
}

std::filesystem::path default_game_directory() {
    if (const char *dir = std::getenv("MHP3RD_GAME_DIR"); dir != nullptr && *dir != '\0') return dir;
    return MHP3RD_DEFAULT_GAME_DIR;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc > 2) {
            std::cerr << "usage: MHP3rdNative [game_dir]\n"
                      << "  game_dir contains EBOOT.ELF, disc.iso and ms0/\n";
            return 2;
        }
        const std::filesystem::path game_dir = argc == 2 ? std::filesystem::path(argv[1]) : default_game_directory();
        const std::filesystem::path executable = game_dir / "EBOOT.ELF";
        mhp3rd::ProfilePaths paths;
        paths.disc_image = game_dir / "disc.iso";
        paths.memory_stick = game_dir / "ms0";
        if (!std::filesystem::exists(paths.disc_image)) {
            std::cerr << "warning: " << paths.disc_image.string() << " not found; disc0: is unavailable\n";
            paths.disc_image.clear();
        }
        if (!std::filesystem::is_regular_file(executable))
            throw psprecomp::Error("Missing " + executable.string() + " (run profiles/mhp3rd/scripts/prepare_game.sh)");

        const std::string sha256 = psprecomp::sha256_file(executable);
        if (sha256 != kSupportedSha256)
            std::cerr << "warning: unsupported executable hash " << sha256 << "\n";

        const psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(executable);
        if (elf.required_ram_size(mhp3rd::kLoadBase) != mhp3rd::kGuestRamBytes)
            throw psprecomp::Error("Executable does not match the 64 MiB MHP3rd HD layout");

        psprecomp::Runtime runtime(mhp3rd::kGuestRamBytes);
        runtime.nids().load_csv(MHP3RD_NIDS_CSV);
        (void)elf.load_and_relocate(runtime.memory(), mhp3rd::kLoadBase);
        psprecomp::register_generated_functions(runtime);
        mhp3rd::install_profile(runtime, elf, paths);

        std::cout << "MHP3rdNative PSP bootstrap\n"
                  << "Executable: " << executable.string() << "\n"
                  << "SHA-256:    " << sha256 << "\n"
                  << "Disc image: " << (paths.disc_image.empty() ? "<none>" : paths.disc_image.string()) << "\n"
                  << "Entry:      " << psprecomp::hex32(elf.runtime_entry(mhp3rd::kLoadBase)) << "\n"
                  << "Functions:  " << runtime.function_count() << "\n";
        if (runtime.function_count() == 0u) {
            std::cout << "No generated functions are linked. Run profiles/mhp3rd/scripts/generate.sh and rebuild.\n";
            return 3;
        }

        runtime.run(elf.runtime_entry(mhp3rd::kLoadBase), configured_max_dispatches());
        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";
        std::cout << mhp3rd::kernel().describe_threads() << "\n";
        runtime.report_hle_histogram();
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &e) {
        std::cerr << "MHP3rdNative error: " << e.what() << "\n";
        return 1;
    }
}
